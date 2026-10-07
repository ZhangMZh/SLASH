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
 * @file slash_dmabuf.h
 *
 * DMA-buf exporter for PCI BAR regions.
 *
 * Provides a dma-buf wrapper around a PCI BAR so that userspace can
 * obtain a file descriptor and mmap the BAR for direct MMIO access.
 * Only userspace mmap is supported; kernel-side device attachment is
 * intentionally rejected: peer attachment mapping is not implemented.
 * Userspace I/O-memory registration uses the mmap path instead.
 */

#ifndef SLASH_DMABUF_H
#define SLASH_DMABUF_H

#include <linux/dma-buf.h>
#include <linux/pci.h>

/**
 * slash_bar_dmabuf_create() - Export a PCI BAR as a dma-buf.
 * @pdev:       PCI device owning the BAR.
 * @bar_number: BAR index (0-5).  Must be a valid MMIO BAR.
 *
 * Creates a dma-buf exporter backed by the physical address range of
 * the specified BAR.  Takes a reference on @pdev (via pci_dev_get())
 * that is released when the dma-buf is freed.
 *
 * Return: Pointer to the new dma_buf on success, ERR_PTR on failure.
 */
struct dma_buf *slash_bar_dmabuf_create(struct pci_dev *pdev, int bar_number);

struct slash_p2p_region;
/* Caller holds region->lock and has verified that the region is live.
 * The returned object owns a region reference and exports only this range.
 * Its mmap eagerly installs PFNs for hipHostRegisterIoMemory. */
struct dma_buf *slash_bar_dmabuf_create_range(struct slash_p2p_region *region,
                                             u64 offset, u64 length);

/**
 * slash_bar_dmabuf_destroy() - Release a BAR dma-buf.
 * @dmabuf: dma-buf returned by slash_bar_dmabuf_create().
 *
 * Drops the driver's reference on the dma-buf.  The underlying
 * resources (PCI device reference, private data) are freed when the
 * last user closes their fd / drops their reference.
 */
void slash_bar_dmabuf_destroy(struct dma_buf *dmabuf);

#endif /* SLASH_DMABUF_H */
