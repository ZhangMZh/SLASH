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
 * @file slash_pcie.c
 *
 * PCI driver for the SLASH control function (PF2).
 *
 * This driver binds to PCI device 10EE:50C2 (the V80 SLASH control
 * function), or the legacy 10EE:50B6 used by pre-compute-platform
 * designs.  On probe it creates a control device (slash_ctldev) that
 * exposes BAR information and dma-buf-backed BAR mappings to userspace,
 * and registers the device with the hotplug subsystem so that it can
 * be removed/reset/rescanned during FPGA reconfiguration.
 *
 * PF1 (the QDMA function, device 10EE:50C1, legacy 10EE:50B5, or AVED
 * 10EE:50BD) is handled by a separate PCI driver in slash_qdma.c.
 */

#include "slash_pcie.h"

#include <linux/module.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/mm.h>

#include "slash.h"
#include "slash_ctldev.h"
#include "slash_dmabuf.h"
#include "slash_hotplug_driver.h"

static int slash_pcie_probe(struct pci_dev *pdev, const struct pci_device_id *id);
static void slash_pcie_remove(struct pci_dev *pdev);

/* Match the SLASH control function (PF2): new 0x50C2, or legacy 0x50B6. */
static const struct pci_device_id slash_pcie_ids[] = {
    {PCI_DEVICE(SLASH_PCIE_VENDOR_ID, SLASH_PCIE_DEVICE_ID)},
    {PCI_DEVICE(SLASH_PCIE_VENDOR_ID, SLASH_PCIE_DEVICE_ID_LEGACY)},
    {PCI_DEVICE(SLASH_PCIE_VENDOR_ID, SLASH_P2P_DEVICE_ID)},
    {0,}
};
MODULE_DEVICE_TABLE(pci, slash_pcie_ids);

static struct pci_driver slash_pcie_driver = {
    .name = SLASH_PCIE_DRV_NAME,
    .id_table = slash_pcie_ids,
    .probe = slash_pcie_probe,
    .remove = slash_pcie_remove,
};

static void slash_p2p_region_release(struct kref *ref)
{
    struct slash_p2p_region *region = container_of(ref, struct slash_p2p_region, ref);

    pci_dev_put(region->pdev);
    kfree(region);
}

void slash_p2p_region_put(struct slash_p2p_region *region)
{
    kref_put(&region->ref, slash_p2p_region_release);
}

static int slash_p2p_probe(struct pci_dev *pdev)
{
    struct slash_p2p_region *region;
    int err;

    if (PCI_FUNC(pdev->devfn) != SLASH_P2P_PF)
        return -EINVAL;
    if (!(pci_resource_flags(pdev, SLASH_P2P_BAR) & IORESOURCE_MEM) ||
        !pci_resource_start(pdev, SLASH_P2P_BAR) ||
        pci_resource_len(pdev, SLASH_P2P_BAR) < SLASH_P2P_HBM_SIZE)
        return -ENODEV;

    region = kzalloc(sizeof(*region), GFP_KERNEL);
    if (!region)
        return -ENOMEM;
    err = pci_enable_device_mem(pdev);
    if (err)
        goto err_free;
    err = pci_request_region(pdev, SLASH_P2P_BAR, "slash_p2p");
    if (err)
        goto err_disable;

    kref_init(&region->ref);
    mutex_init(&region->lock);
    region->pdev = pci_dev_get(pdev);
    region->start = pci_resource_start(pdev, SLASH_P2P_BAR);
    region->len = pci_resource_len(pdev, SLASH_P2P_BAR);
    pci_set_drvdata(pdev, region);
    /* PF3 is only a target; it does not require bus mastering. BAR size
     * alone cannot prove its translation. The application must verify
     * BAR/HBM aliasing before registering the range with a GPU. */
    dev_info(&pdev->dev, "PF3 BAR2 ready for experimental range exports\n");
    return 0;

err_disable:
    pci_disable_device(pdev);
err_free:
    kfree(region);
    return err;
}

static void slash_p2p_remove(struct pci_dev *pdev)
{
    struct slash_p2p_region *region = pci_get_drvdata(pdev);

    mutex_lock(&region->lock);
    region->dead = true;
    pci_set_drvdata(pdev, NULL);
    pci_release_region(pdev, SLASH_P2P_BAR);
    pci_disable_device(pdev);
    mutex_unlock(&region->lock);
    /* Existing GPU mappings cannot be revoked by this minimal exporter.
     * Users must unregister them before unbinding/resetting the board. */
    slash_p2p_region_put(region);
}

struct dma_buf *slash_pcie_export_p2p(struct pci_dev *pf2,
                                     u64 hbm_addr, u64 length)
{
    struct pci_dev *pf3;
    struct slash_p2p_region *region;
    struct dma_buf *buf = ERR_PTR(-ENODEV);
    u64 offset;

    if (hbm_addr < SLASH_P2P_HBM_BASE || !length ||
        !IS_ALIGNED(hbm_addr, PAGE_SIZE) || !IS_ALIGNED(length, PAGE_SIZE))
        return ERR_PTR(-EINVAL);
    offset = hbm_addr - SLASH_P2P_HBM_BASE;
    if (offset >= SLASH_P2P_HBM_SIZE || length > SLASH_P2P_HBM_SIZE - offset ||
        length > SLASH_P2P_BANK_SIZE - (offset % SLASH_P2P_BANK_SIZE))
        return ERR_PTR(-EINVAL);

    pf3 = pci_get_domain_bus_and_slot(pci_domain_nr(pf2->bus), pf2->bus->number,
                                      PCI_DEVFN(PCI_SLOT(pf2->devfn), SLASH_P2P_PF));
    if (!pf3)
        return buf;
    /* Serialize lookup with PF3 probe/remove; no probe-order dependency. */
    device_lock(&pf3->dev);
    if (pf3->driver == &slash_pcie_driver &&
        pf3->vendor == SLASH_PCIE_VENDOR_ID && pf3->device == SLASH_P2P_DEVICE_ID) {
        region = pci_get_drvdata(pf3);
        if (region) {
            mutex_lock(&region->lock);
            if (!region->dead)
                buf = slash_bar_dmabuf_create_range(region, offset, length);
            mutex_unlock(&region->lock);
        }
    }
    device_unlock(&pf3->dev);
    pci_dev_put(pf3);
    return buf;
}

/**
 * slash_pcie_probe() - Bind to a SLASH control function.
 * @pdev: PCI device being probed.
 * @id:   Matching device ID entry (unused).
 *
 * Verifies that this is the expected physical function (PF2), enables
 * the PCI device, creates the control character device, and registers
 * with the hotplug subsystem.
 *
 * Takes an extra reference on @pdev (pci_dev_get) because the control
 * device and hotplug subsystem hold pointers to it beyond this function.
 * The reference is released in slash_pcie_remove().
 *
 * Return: 0 on success, negative errno on failure.
 */
static int slash_pcie_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    int err;

    (void) id; /* Unused */

    if (pdev->device == SLASH_P2P_DEVICE_ID)
        return slash_p2p_probe(pdev);

    dev_info(&pdev->dev, "slash: probe start for %s\n", pci_name(pdev));
    dev_dbg(&pdev->dev, "slash: vendor=0x%04x device=0x%04x fn=%u\n", pdev->vendor, pdev->device, PCI_FUNC(pdev->devfn));

    /*
     * The SLASH design places the control interface on PF2.  Reject
     * any other function — this guards against unexpected device ID
     * collisions or misconfigured FPGA designs.
     */
    if (PCI_FUNC(pdev->devfn) != SLASH_PCIE_PF) {
        dev_err(&pdev->dev, "slash: expected PF %u, got %u\n", SLASH_PCIE_PF, PCI_FUNC(pdev->devfn));
        return -EINVAL;
    }

    /* Hold a reference for the lifetime of the driver binding. */
    pci_dev_get(pdev);

    err = pci_enable_device(pdev);
    if (err) {
        dev_err(&pdev->dev, "slash: pci_enable_device() failed: %d\n", err);
        goto err_put_device;
    }

    /* Bus mastering is required for the device to perform DMA. */
    pci_set_master(pdev);
    dev_dbg(&pdev->dev, "slash: bus mastering enabled\n");

    err = slash_ctldev_create(pdev);
    if (err) {
        dev_err(&pdev->dev, "slash: control device create failed: %d\n", err);
        goto err_disable_device;
    }

    dev_info(&pdev->dev, "slash: probe successful\n");
    return 0;

err_disable_device:
    pci_clear_master(pdev);
    pci_disable_device(pdev);

err_put_device:
    pci_dev_put(pdev);

    return err;
}

/**
 * slash_pcie_remove() - Unbind from a SLASH control function.
 * @pdev: PCI device being removed.
 *
 * Tears down resources in reverse probe order: hotplug unregister,
 * control device destroy, then PCI cleanup.
 */
static void slash_pcie_remove(struct pci_dev *pdev)
{
    if (pdev->device == SLASH_P2P_DEVICE_ID) {
        slash_p2p_remove(pdev);
        return;
    }
    dev_info(&pdev->dev, "slash: remove start for %s\n", pci_name(pdev));

    slash_ctldev_destroy(pdev);
    pci_clear_master(pdev);
    pci_disable_device(pdev);
    pci_dev_put(pdev);

    dev_info(&pdev->dev, "slash: remove complete\n");
}

int __init slash_pcie_init(void)
{
    int err;

    pr_info("slash: registering PCIe driver '%s'\n", SLASH_NAME);
    err = pci_register_driver(&slash_pcie_driver);

    if (err) {
        pr_err("slash: pci_register_driver failed: %d\n", err);
        return err;
    }

    pr_info("slash: driver '%s' registered\n", SLASH_NAME);
    return 0;
}

void __exit slash_pcie_exit(void)
{
    pr_info("slash: unregistering PCIe driver '%s'\n", SLASH_NAME);
    pci_unregister_driver(&slash_pcie_driver);
    pr_info("slash: driver '%s' unregistered\n", SLASH_NAME);
}
