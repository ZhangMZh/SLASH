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
 * @file slash_pcie.h
 *
 * PCI driver registration interface for the SLASH control function (PF2).
 *
 * The PCI driver handles probe/remove callbacks for each V80 SLASH
 * control function discovered on the bus.  It is registered during
 * module init and unregistered during module exit.
 */

#ifndef SLASH_PCIE_H
#define SLASH_PCIE_H

#include <linux/init.h>
#include <linux/kref.h>
#include <linux/mutex.h>
#include <linux/pci.h>

struct dma_buf;

/* The PCI binding and each exported range own a reference. A reference
 * preserves this object, not hardware across reset/unbind. */
struct slash_p2p_region {
    struct kref ref;
    struct mutex lock;
    struct pci_dev *pdev;
    resource_size_t start;
    resource_size_t len;
    bool dead;
};

void slash_p2p_region_put(struct slash_p2p_region *region);
struct dma_buf *slash_pcie_export_p2p(struct pci_dev *pf2,
                                     u64 hbm_addr, u64 length);

/**
 * slash_pcie_init() - Register the SLASH PCI driver with the kernel.
 *
 * Called from module init.  Triggers probe callbacks for any SLASH
 * devices already present on the bus.
 *
 * Return: 0 on success, negative errno on failure.
 */
int __init slash_pcie_init(void);

/**
 * slash_pcie_exit() - Unregister the SLASH PCI driver.
 *
 * Called from module exit.  Triggers remove callbacks for all
 * currently bound devices.
 */
void __exit slash_pcie_exit(void);

#endif /* SLASH_PCIE_H */
