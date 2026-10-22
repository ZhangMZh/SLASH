"""Run hardware guard procedures with bounded Tcl substitutes for Vivado."""

from importlib import resources
import re
import shutil
import subprocess

import pytest


@pytest.fixture
def run_tcl(tmp_path):
    executable = shutil.which("tclsh")
    if executable is None:
        pytest.skip("Tcl interpreter is required for the Vivado guard tests")

    def run(script):
        path = tmp_path / "test.tcl"
        path.write_text(script)
        result = subprocess.run([executable, str(path)], cwd=tmp_path,
                                capture_output=True, text=True, timeout=10)
        assert result.returncode == 0, result.stdout + result.stderr
        return result.stdout
    return run


CDC_STUBS = r"""
set active_design parent
proc current_bd_design {args} {
    if {[llength $args]} {set ::active_design [lindex $args 0]}
    return $::active_design
}
proc get_bd_designs {name} {return $name}
proc get_files {args} {return /ip/component/bd_0/expanded.bd}
proc get_bd_cells {args} {
    set name [lindex $args end]
    if {[regexp {s00_nodes/s00_(.+)_node} $name -> channel]} {
        if {[lsearch -exact $::channels $channel] < 0} {return {}}
    }
    if {[string match *si_converter $name] && !$::converter} {return {}}
    return $name
}
proc get_property {prop cell} {
    if {$prop eq "CONFIG.Component_Name"} {return component}
    if {$prop eq "CONFIG.ACLK_RELATIONSHIP"} {return $::relationship}
    if {$prop eq "CONFIG.SYNCHRONIZATION_STAGES"} {return $::stages}
    if {$prop eq "CONFIG.MAX_PAYLD_BYTES"} {return $::payload}
    if {$prop eq "CONFIG.USER_WIDTH"} {error "USER_WIDTH is not payload width"}
    if {[regexp {CONFIG.(?:MSC000_)?([RW])DATA_WIDTH} $prop -> direction]} {
        set channel [string tolower $direction]
        if {[lsearch -exact $::channels $channel] < 0} {
            error "queried a missing data direction"
        }
        if {[string match CONFIG.MSC000_* $prop]} {return $::target_width}
        return 512
    }
    error "unexpected property $prop"
}
"""


@pytest.mark.parametrize("template", ["slash.tcl", "slash_compute.tcl"])
@pytest.mark.parametrize(
    "channels,converter,relationship,stages,payload,target_width,error",
    [
        ("ar aw w r b", 1, 0, 3, 64, 256, ""),
        ("ar r", 1, 0, 3, 64, 256, ""),
        ("aw w b", 1, 0, 3, 64, 256, ""),
        ("ar r", 0, 0, 3, 64, 256, ""),
        ("", 0, 0, 3, 64, 256, "no AXI channel"),
        ("ar aw b", 0, 0, 3, 64, 256, "no read or write"),
        ("ar r", 1, 1, 3, 64, 256, "asynchronous CDC"),
        ("aw w b", 1, 0, 2, 64, 256, "synchronization stages"),
        ("ar r", 0, 0, 3, 32, 256, "payload width changed"),
        ("ar r", 1, 0, 3, 64, 128, "converter widths"),
    ],
)
def test_hbm_cdc_guards(run_tcl, template, channels, converter,
                        relationship, stages, payload, target_width, error):
    source = resources.files(
        "slashkit.resources.templates").joinpath(template).read_text()
    proc = re.search(r"(?ms)^proc slash_check_hbm_cdc .*?^}\n", source).group()
    setup = f"""
set channels {{{channels}}}
set converter {converter}
set relationship {relationship}
set stages {stages}
set payload {payload}
set target_width {target_width}
set expected_error {{{error}}}
"""
    result = run_tcl(setup + CDC_STUBS + proc + r"""
set failed [catch {slash_check_hbm_cdc hbm_sc_00 512 256} msg]
if {$active_design ne "parent"} {error "outer BD was not restored"}
if {$expected_error eq ""} {
    if {$failed} {error $msg}
} elseif {!$failed || [string first $expected_error $msg] < 0} {
    error "expected '$expected_error', got '$msg' (failed=$failed)"
}
puts VERIFIED
""")
    assert "VERIFIED" in result


@pytest.mark.parametrize("mode", ["pass", "noc", "other"])
def test_pr_verify_keeps_only_images_allowed_by_policy(run_tcl, mode):
    source = resources.files("slashkit.resources.base.common.scripts").joinpath(
        "slash_prverify.tcl").read_text()
    result = run_tcl(f"set mode {mode}\n" + source + r"""
set warnings {}
proc send_msg_id {id level msg} {lappend ::warnings $level}
proc pr_verify {args} {
    set report [lindex $args 2]
    set fh [open $report w]
    if {$::mode eq "noc"} {puts $fh "ERROR: NOC-Validation-16"}
    if {$::mode eq "other"} {puts $fh "ERROR: another verification error"}
    close $fh
    if {$::mode ne "pass"} {error "verification failed"}
}
close [open partial.pdi w]
set failed [catch {slash_pr_verify static.dcp routed.dcp report.txt partial.pdi test} msg]
if {$mode eq "noc"} {
    if {!$failed || [file exists partial.pdi]} {error "bad NoC image survived"}
    if {[string first "not loadable" $msg] < 0} {error $msg}
} else {
    if {$failed || ![file exists partial.pdi]} {error "image was incorrectly rejected: $msg"}
}
if {$mode eq "other"} {
    if {$warnings ne [list "CRITICAL WARNING"]} {error "missing warning"}
} elseif {[llength $warnings]} {error "unexpected warning"}
puts VERIFIED
""")
    assert "VERIFIED" in result


def test_exported_build_scripts_keep_pr_verify_and_clock_hooks(tmp_path):
    from slashkit.emit.render import export_package

    base = tmp_path / "base"
    export_package("slashkit.resources.base", base)
    common = base / "common/scripts"
    assert (common / "slash_prverify.tcl").is_file()
    assert "proc slash_user_clock::chain_hook" in (
        common / "user_clock.tcl").read_text()
    user_build = (common / "slash_project_build.tcl").read_text()
    ordered = ["close_design", "close_project", "slash_pr_verify $static_shell_dcp",
               "open_checkpoint $routed_dcp", "slash_user_clock::characterize_runtime",
               "write_debug_probes"]
    positions = [user_build.index(command) for command in ordered]
    assert positions == sorted(positions)
    assert "source [file join [file dirname [info script]] slash_prverify.tcl]" in user_build
    service_build = (
        base / "service/scripts/service_layer_build.tcl").read_text()
    assert "source [file join [file dirname [info script]] .. .. common scripts slash_prverify.tcl]" in service_build
    assert "slash_pr_verify $static_shell_dcp" in service_build
