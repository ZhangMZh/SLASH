/**
 * Copyright (C) 2025 Advanced Micro Devices, Inc. All rights reserved.
 * This program is free software; you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation; version 2.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
 * even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program; if
 * not, write to the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/**
 * @file slash_dmabuf.c
 *
 * DMA-buf exporter for PCI BAR regions.
 *
 * This file implements a dma-buf wrapper around a PCI BAR, allowing
 * userspace to obtain a file descriptor (via the control device ioctl)
 * and mmap the BAR for direct MMIO register access.
 *
 * Only userspace mmap is supported.  Kernel-side device attachments
 * (the normal dma-buf import path) are not implemented. PCI BAR memory
 * can support P2P, but needs a separate peer mapping implementation. The dma-buf
 * framework is used here solely for its fd-based lifetime management
 * and mmap infrastructure.
 *
 * Cache attribute selection:
 *   - **Prefetchable BARs** → write-combine mapping (pgprot_writecombine).
 *     Suitable for frame buffers or bulk data regions where write
 *     coalescing improves throughput.
 *   - **Non-prefetchable BARs** → device/uncached mapping (pgprot_device).
 *     Required for control registers where every write must hit the
 *     device immediately and in order.
 */

#include "slash_dmabuf.h"

#include "slash.h"
#include "slash_compat.h"
#include "slash_pcie.h"

#include <linux/err.h>
#include <linux/mm.h>
#include <linux/pci.h>
#include <linux/printk.h>
#include <linux/slab.h>

/**
 * struct slash_bar_dmabuf_data - Private data attached to each BAR dma-buf.
 * @bar_number: Which PCI BAR (0-5) this dma-buf represents.
 * @len:        Size of the BAR region in bytes.
 * @pdev:       PCI device owning the BAR.  Held via pci_dev_get()
 *              for the lifetime of this struct.
 */
struct slash_bar_dmabuf_data {
    int bar_number;
    resource_size_t len;
    resource_size_t offset;
    struct slash_p2p_region *region;

    struct pci_dev *pdev;
};

/*
 * We only support userspace mmaps of the BAR; importing into other
 * devices is intentionally rejected: peer attachment mapping is not
 * implemented. hipHostRegisterIoMemory uses the userspace VMA instead.
 */
static int slash_bar_dmabuf_attach(struct dma_buf *dmabuf, struct dma_buf_attachment *attach)
{
    dev_warn(attach->dev, "%s: device attachments are not supported for BAR dmabuf", SLASH_NAME);
    return -EOPNOTSUPP;
}

static void slash_bar_dmabuf_detach(struct dma_buf *dmabuf, struct dma_buf_attachment *attach)
{
    dev_dbg(attach->dev, "slash: dmabuf detach (noop)\n");
}

static struct sg_table *slash_bar_dmabuf_map(struct dma_buf_attachment *attach,
                                        enum dma_data_direction dir)
{
    dev_dbg(attach->dev, "slash: dmabuf map requested -> not supported\n");
    return ERR_PTR(-EOPNOTSUPP);
}

static void slash_bar_dmabuf_unmap(struct dma_buf_attachment *attach,
                              struct sg_table *sgl, enum dma_data_direction dir)
{
    dev_dbg(attach->dev, "slash: dmabuf unmap (noop)\n");
}

/**
 * slash_bar_dmabuf_fault() - Page-fault handler for BAR mappings.
 * @vmf: Fault information provided by the kernel.
 *
 * Called on the first access to each page in the VMA.  Computes the
 * physical page frame number (PFN) for the faulting address within
 * the PCI BAR and inserts it into the page tables via vmf_insert_pfn().
 *
 * priv->pdev is held via pci_dev_get() for the lifetime
 * of the dma-buf.  The VMA holds a file reference on the dma-buf, so priv
 * and priv->pdev remain valid for any fault during the VMA's lifetime.
 * This reference preserves the software object, not the PCI resource across
 * device removal. Callers must quiesce accesses before device reconfiguration.
 *
 * Return: VM_FAULT_NOPAGE on success, VM_FAULT_SIGBUS on out-of-range
 *         access or insertion failure.
 */
static vm_fault_t slash_bar_dmabuf_fault(struct vm_fault *vmf)
{
    struct vm_area_struct *vma = vmf->vma;
    struct slash_bar_dmabuf_data *priv = vma->vm_private_data;
    unsigned long page_index;
    unsigned long obj_pgoff;
    resource_size_t bar_start;
    unsigned long pfn;

    /* Page offset within the VMA (0 for the first page of the mapping). */
    page_index = (vmf->address - vma->vm_start) >> PAGE_SHIFT;

    /* BAR-relative page offset: mmap offset + position within mapping. */
    obj_pgoff = vma->vm_pgoff + page_index;

    /* Bounds check: do not map beyond the physical BAR. */
    if ((obj_pgoff << PAGE_SHIFT) >= priv->len)
        return VM_FAULT_SIGBUS;

    bar_start = pci_resource_start(priv->pdev, priv->bar_number);
    pfn = (bar_start >> PAGE_SHIFT) + obj_pgoff;

    return vmf_insert_pfn(vma, vmf->address, pfn);
}

static const struct vm_operations_struct slash_bar_dmabuf_vm_ops = {
    .fault = slash_bar_dmabuf_fault,
};

/**
 * slash_bar_dmabuf_mmap() - Set up a BAR region for fault-based mapping.
 * @dmabuf: The BAR dma-buf being mapped.
 * @vma:    The VMA describing the mapping request.
 *
 * Configures the VMA with appropriate flags, cache attributes, and a
 * custom vm_operations_struct whose .fault handler uses vmf_insert_pfn()
 * to lazily insert PFNs on first access for legacy BAR exports. P2P range
 * exports install PFNs eagerly so GPU registration can use follow_pfn().
 *
 * Cache attribute selection:
 *   - Prefetchable BAR → write-combine (pgprot_writecombine): allows
 *     the CPU to coalesce writes for better throughput on bulk data BARs.
 *   - Non-prefetchable BAR → device/uncached (pgprot_device): strict
 *     ordering for control registers.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int slash_bar_dmabuf_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma)
{
    struct slash_bar_dmabuf_data *priv = dmabuf->priv;
    unsigned long size = vma->vm_end - vma->vm_start;
    u64 offset;
    bool wc;

    if (vma->vm_pgoff > (priv->len >> PAGE_SHIFT))
        return -EINVAL;
    offset = (u64)vma->vm_pgoff << PAGE_SHIFT;

    /* Ensure the requested range lies fully within the BAR. */
    if (offset > priv->len || size > priv->len - offset)
        return -EINVAL;

    /*
     * VM_PFNMAP    — raw PFN mapping, required for vmf_insert_pfn().
     * VM_IO        — I/O memory (blocks /proc/pid/mem, core dump).
     * VM_DONTDUMP  — explicit core-dump exclusion (redundant w/ VM_IO).
     * VM_DONTEXPAND — prevents mremap beyond BAR boundary.
     * VM_DONTCOPY  — do not inherit across fork(); BAR register
     *                mappings should not be silently shared with children.
     */
    slash_vm_flags_set(vma, VM_PFNMAP | VM_IO | VM_DONTDUMP |
                            VM_DONTEXPAND | VM_DONTCOPY);

    wc = !!(pci_resource_flags(priv->pdev, priv->bar_number) & IORESOURCE_PREFETCH);
    vma->vm_page_prot = wc ? pgprot_writecombine(vma->vm_page_prot)
                           : pgprot_device(vma->vm_page_prot);

    if (priv->region) {
        struct slash_p2p_region *region = priv->region;
        int err;

        if (!(vma->vm_flags & VM_SHARED))
            return -EINVAL;
        mutex_lock(&region->lock);
        if (region->dead) {
            err = -ENODEV;
        } else {
            unsigned long pfn = (region->start + priv->offset + offset) >> PAGE_SHIFT;

            err = io_remap_pfn_range(vma, vma->vm_start, pfn, size, vma->vm_page_prot);
        }
        mutex_unlock(&region->lock);
        return err;
    }

    vma->vm_ops = &slash_bar_dmabuf_vm_ops;
    vma->vm_private_data = priv;

    dev_dbg(&priv->pdev->dev,
            "slash: mmap BAR%d wc=%d pgoff=0x%lx len=0x%lx (fault-based)\n",
            priv->bar_number, wc, vma->vm_pgoff, size);

    return 0;
}

/**
 * slash_bar_dmabuf_release() - Free resources when all references are dropped.
 * @dmabuf: The BAR dma-buf being released.
 *
 * Drops the PCI device reference taken in slash_bar_dmabuf_create()
 * and frees the private data.
 */
static void slash_bar_dmabuf_release(struct dma_buf *dmabuf)
{
    struct slash_bar_dmabuf_data *priv = dmabuf->priv;

    dev_dbg(&priv->pdev->dev, "slash: dmabuf release (BAR%d)\n", priv->bar_number);

    pci_dev_put(priv->pdev);
    if (priv->region)
        slash_p2p_region_put(priv->region);
    kfree(priv);
}

static const struct dma_buf_ops slash_bar_dmabuf_ops = {
    .attach        = slash_bar_dmabuf_attach,
    .detach        = slash_bar_dmabuf_detach,
    .map_dma_buf   = slash_bar_dmabuf_map,
    .unmap_dma_buf = slash_bar_dmabuf_unmap,
    .mmap          = slash_bar_dmabuf_mmap,
    .release       = slash_bar_dmabuf_release,
};

/**
 * slash_bar_dmabuf_create() - Export a PCI BAR as a dma-buf.
 * @pdev:       PCI device owning the BAR.
 * @bar_number: BAR index (0-5).  Must be present and MMIO.
 *
 * Allocates private state, takes a reference on @pdev, and registers a
 * dma-buf exporter.  The DEFINE_DMA_BUF_EXPORT_INFO macro initializes
 * the export info struct with sensible defaults; we override ops, size,
 * flags, priv, and exp_name.
 *
 * Return: Pointer to the new dma_buf on success, ERR_PTR on failure.
 */
static struct dma_buf *slash_bar_dmabuf_export(struct pci_dev *pdev, int bar_number,
                                               u64 offset, u64 length,
                                               struct slash_p2p_region *region)
{
    long err;
    resource_size_t len;
    DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
    struct dma_buf *dmabuf;
    struct slash_bar_dmabuf_data *priv;

    if (bar_number < 0 || bar_number >= PCI_STD_NUM_BARS) {
        dev_err(&pdev->dev, "slash: invalid BAR %d\n", bar_number);
        return ERR_PTR(-EINVAL);
    }
    if (!pci_resource_start(pdev, bar_number)) {
        dev_err(&pdev->dev, "slash: BAR%d not present\n", bar_number);
        return ERR_PTR(-ENODEV);
    }
    if ((pci_resource_flags(pdev, bar_number) & IORESOURCE_MEM) == 0) {
        dev_err(&pdev->dev, "slash: BAR%d is not MMIO\n", bar_number);
        return ERR_PTR(-ENODEV);
    }

    len = pci_resource_len(pdev, bar_number);
    if (region) {
        if (!length || !IS_ALIGNED(offset, PAGE_SIZE) ||
            !IS_ALIGNED(length, PAGE_SIZE) || offset > len || length > len - offset)
            return ERR_PTR(-EINVAL);
        len = length;
    }

    dev_dbg(&pdev->dev, "slash: exporting BAR%d as dma-buf (size=%pa)\n", bar_number, &len);

    priv = kzalloc(sizeof(*priv), GFP_KERNEL);
    if (!priv) {
        dev_err(&pdev->dev, "slash: kzalloc(priv) failed\n");
        return ERR_PTR(-ENOMEM);
    }

    priv->bar_number = bar_number;
    priv->len = len;
    priv->offset = offset;
    priv->region = region;
    if (region)
        kref_get(&region->ref);
    /* Hold a PCI device reference for the lifetime of the dma-buf. */
    priv->pdev = pci_dev_get(pdev);

    exp_info.ops = &slash_bar_dmabuf_ops;
    exp_info.size = len;
    exp_info.flags = O_RDWR;
    exp_info.priv = priv;
    exp_info.exp_name = SLASH_NAME;

    dmabuf = dma_buf_export(&exp_info);
    if (IS_ERR(dmabuf)) {
        err = PTR_ERR(dmabuf);
        dev_err(&pdev->dev, "slash: dma_buf_export failed: %ld\n", err);
        goto err_free_priv;
    }

    dev_info(&pdev->dev, "slash: BAR%d exported as dma-buf (size=%pa)\n", bar_number, &len);
    return dmabuf;

err_free_priv:
    pci_dev_put(priv->pdev);
    if (region)
        slash_p2p_region_put(region);
    kfree(priv);

    return ERR_PTR(err);
}

struct dma_buf *slash_bar_dmabuf_create(struct pci_dev *pdev, int bar_number)
{
    return slash_bar_dmabuf_export(pdev, bar_number, 0, 0, NULL);
}

struct dma_buf *slash_bar_dmabuf_create_range(struct slash_p2p_region *region,
                                             u64 offset, u64 length)
{
    lockdep_assert_held(&region->lock);
    if (region->dead)
        return ERR_PTR(-ENODEV);
    return slash_bar_dmabuf_export(region->pdev, SLASH_P2P_BAR, offset, length, region);
}

/**
 * slash_bar_dmabuf_destroy() - Release the driver's reference on a BAR dma-buf.
 * @dmabuf: dma-buf returned by slash_bar_dmabuf_create().
 *
 * Drops one reference.  The dma-buf (and its private data) are actually
 * freed only when the last holder — including any userspace fd — closes.
 */
void slash_bar_dmabuf_destroy(struct dma_buf *dmabuf)
{
    pr_debug("slash: dmabuf_destroy()\n");
    dma_buf_put(dmabuf);
}
