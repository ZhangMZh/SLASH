"""Keep both Gen4 shells compatible with the four-function host interface."""

from importlib import resources
import re

import pytest


@pytest.mark.parametrize("shell", ["service", "compute"])
def test_gen4_shell_preserves_all_host_functions(shell):
    top = resources.files(
        f"slashkit.resources.base.{shell}.scripts"
    ).joinpath("top.tcl").read_text()
    pairs = re.findall(r"\b(CPM_PCIE\w+)\s+\{([^{}]*)\}", top)
    config = dict(pairs)
    assert len(config) == len(pairs), "Duplicate CPM configuration keys"
    assert config["CPM_PCIE0_MODES"] == "DMA"
    assert config["CPM_PCIE1_MODES"] == "None"
    assert config["CPM_PCIE0_MAX_LINK_SPEED"] == "16.0_GT/s"
    assert config["CPM_PCIE0_PL_LINK_CAP_MAX_LINK_WIDTH"] == "X16"
    assert config["CPM_PCIE0_TL_PF_ENABLE_REG"] == "4"
    for pf, device in enumerate(["50b4", "50c1", "50c2", "50c3"]):
        assert config[f"CPM_PCIE0_PF{pf}_CFG_DEV_ID"] == device
    # Preserve the original PF3 declarations, including implicit BAR types.
    for bar, address in [(0, "0x4000000000"), (2, "0x60000000000")]:
        prefix = f"CPM_PCIE0_PF3_BAR{bar}_QDMA_"
        assert config[prefix + "64BIT"] == "1"
        assert prefix + "TYPE" not in config
        assert config[prefix + "PREFETCHABLE"] == "1"
        assert config[prefix + "SCALE"] == "Gigabytes"
        assert config[prefix + "SIZE"] == "32"
        assert config[f"CPM_PCIE0_PF3_PCIEBAR2AXIBAR_QDMA_{bar}"] == address
    assert config["CPM_PCIE0_PF3_BAR2_QDMA_ENABLED"] == "1"
    assert "CPM_PCIE0_PF3_BAR0_QDMA_ENABLED" not in config
    assert not any(key.startswith("CPM_PCIE1_") and key != "CPM_PCIE1_MODES"
                   for key in config)
    for old in ["gt_pciea1", "gt_refclk1", "PCIE1_GT", "pcie1_cfg_ext", "dma1_"]:
        assert old not in top
    for port in ["PCIE0_GT", "gt_refclk0", "pcie0_cfg_ext", "dma0_axi_aresetn",
                 "dma0_intrfc_clk", "dma0_intrfc_resetn"]:
        assert f"cips/{port}" in top
