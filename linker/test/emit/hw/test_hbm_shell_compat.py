"""Exercise wide HBM generation alongside the fixed RP control window."""

from importlib import resources
from pathlib import Path
import re
from types import SimpleNamespace

import pytest

from slashkit.core.bd_ports import BdPort, BlockDesignPorts
from slashkit.core.command_config import ShellType
from slashkit.core.connectivity import ConnectivityConfig
from slashkit.core.kernel import Kernel, KernelInstance
from slashkit.core.port import BusType, Port
from slashkit.core.regs import AddressBlock, MemoryMap
from slashkit.emit.hw.tcl_gen import generate_tcl
from slashkit.emit.hw.user_region.hbm_ctx import build_hbm_smartconnect_context


def hbm_design(widths):
    instances = {}
    for i, width in enumerate(widths):
        kernel = Kernel(
            name=f"kernel_{i}", component_xml_path=Path("component.xml"),
            vlnv=f"test:kernel:kernel_{i}:1.0",
            ports={
                "mem": Port("mem", BusType.AXI4FULL, width=width),
                "s_axi_control": Port("s_axi_control", BusType.AXILITE, width=32),
                "ap_clk": Port("ap_clk", BusType.CLOCK),
                "ap_rst_n": Port("ap_rst_n", BusType.RESET),
            },
            memory_maps=[MemoryMap("s_axi_control", [
                AddressBlock("Reg", 0, 0x10000, 32, usage="register")])],
        )
        inst = KernelInstance(f"kernel_{i}_0", kernel, params={
            "mem_sp": {"mem": {"domain": "HBM", "index": 0}},
            "clock_hz": 400_000_000,
        })
        instances[inst.name] = inst
    bd = BlockDesignPorts()
    bd.add_many(BdPort(f"HBM{i}", BusType.AXI4FULL,
                       rtl_name=f"HBM_AXI_{i:02d}", width=256,
                       domain="HBM", index=i) for i in range(2))
    return instances, bd


@pytest.mark.parametrize("widths", [[512], [64], [256, 512], [512] * 17])
def test_hbm_reduction_reaches_every_source_once(widths):
    instances, bd = hbm_design(widths)
    ctx = build_hbm_smartconnect_context(instances, bd)
    root, = ctx["hbm_root_create"]
    assert root["width"] == max(256, *widths)
    assert root["merged"] == (len(widths) == 1 and widths[0] >= 256)
    nodes = {n["name"]: n for n in ctx["hbm_reduce_nodes"]}

    def leaves(pin):
        cell = pin.split("/")[0]
        if cell in instances:
            return [pin]
        node = nodes[cell]
        assert 1 <= node["num_si"] <= 16
        if len(widths) > 1:
            assert node["num_si"] > 1
        return [leaf for si in node["si"] for leaf in leaves(si["src"])]

    reached = leaves(ctx["hbm_root_in"][0]["src_pin"])
    assert sorted(reached) == sorted(f"{name}/mem" for name in instances)
    assert ctx["hbm_root_out"][0]["dst_port"] == "HBM_AXI_00"
    if root["merged"]:
        assert ctx["hbm_root_out"][0]["src_pin"].endswith("_out/M_AXI")
    elif root["width"] > 256:
        assert root["dwidth_name"] is not None


@pytest.mark.parametrize("shell", list(ShellType))
@pytest.mark.parametrize("widths", [[512], [64], [256, 512], [512] * 17])
def test_generate_hbm_with_anchor_and_local_reset_distribution(tmp_path, shell, widths):
    instances, bd = hbm_design(widths)
    config = SimpleNamespace(
        block_design_ports=bd, configuration=ConnectivityConfig(),
        kernel_instances=list(instances.values()),
        kernels=[i.kernel for i in instances.values()],
        shell_type=shell, project_name="compat", build_dir=tmp_path,
        clock_hz=400_000_000,
    )
    generate_tcl(config)
    tcl = "\n".join(line for line in (tmp_path / "slash.tcl").read_text().splitlines()
                    if not line.lstrip().startswith("#"))
    assert "connect_bd_net [get_bd_pins slash_window_anchor/s_axi_aresetn] [get_bd_pins rst_repl_misc/Q]" in tcl
    assert "connect_bd_net [get_bd_pins slash_window_anchor/s_axi_aclk] [get_bd_pins user_clk]" in tcl
    assert "[get_bd_pins kernel_0_0/ap_rst_n] [get_bd_pins rst_repl_kernels/Q]" in tcl
    assert "-offset 0x020207FF0000 -range 0x00010000" in tcl
    assert "[get_bd_addr_segs slash_window_anchor/S_AXI/Reg]" in tcl
    assert "[get_bd_intf_pins slash_window_anchor/S_AXI]" in tcl
    assert "-net user_clk_net" not in tcl
    assert "BUFG_FABRIC" not in tcl
    # HBM1 is unused: all channels must remain combinational bypasses.
    for channel in ("AW", "AR", "W", "R", "B"):
        assert re.search(rf"CONFIG.REG_{channel}\s+\{{0\}}", tcl)
    assert "CONFIG.USE_AUTOPIPELINING {0}" in tcl
    assert "slash_check_hbm_cdc hbm_sc_00" in tcl
    assert (tmp_path / "system_map.xml").is_file()


@pytest.mark.parametrize("shell", ["compute", "service"])
def test_pf2_msix_lives_in_bar2_on_the_active_gen4_controller(shell):
    tcl = resources.files(
        f"slashkit.resources.base.{shell}.scripts").joinpath("top.tcl").read_text()
    pairs = re.findall(r"\b(CPM_PCIE\w+)\s+\{([^{}]*)\}", tcl)
    config = dict(pairs)
    assert len(config) == len(pairs)
    expected = {
        "CAP_PBA_BIR": "BAR_3:2", "CAP_PBA_OFFSET": "1FFF000",
        "CAP_TABLE_BIR": "BAR_3:2", "CAP_TABLE_OFFSET": "1FFE000",
        "CAP_TABLE_SIZE": "1", "ENABLED": "1",
    }
    for suffix, value in expected.items():
        assert config[f"CPM_PCIE0_PF2_MSIX_{suffix}"] == value
    assert not any(k.startswith("CPM_PCIE1_PF2_") for k in config)
