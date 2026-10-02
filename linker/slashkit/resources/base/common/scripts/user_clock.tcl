# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Advanced Micro Devices, Inc.
# Implementation/STA model only. Never write a device image while characterizing
# runtime MMCM modes. OOC synthesis has its own module-input clock.
namespace eval slash_user_clock {
    variable model_file [file normalize [info script]]
    variable wizard top_i/static_region/clk_rst_shell/clk_wizard_slash/inst/clock_primitive_inst
}

proc slash_user_clock::one {objects label} {
    if {[llength $objects] != 1} {
        error "User clock: expected one $label, got '$objects'"
    }
    return [lindex $objects 0]
}

proc slash_user_clock::positive_hz {hz} {
    if {![string is wideinteger -strict $hz] || $hz <= 0 || $hz > 4294967295} {
        error "User clock: expected a positive uint32 frequency in Hz, got '$hz'"
    }
}

proc slash_user_clock::gcd {a b} {
    while {$b != 0} {
        set remainder [expr {$a % $b}]
        set a $b
        set b $remainder
    }
    return $a
}

proc slash_user_clock::same_net {a b} {
    set nets [lsort [get_nets -segments -of_objects $a]]
    return [expr {[llength $nets] && $nets eq [lsort [get_nets -segments -of_objects $b]]}]
}

proc slash_user_clock::apply {hz} {
    variable wizard
    positive_hz $hz
    set mmcm [one [get_cells -quiet $wizard/MMCME5_inst] "user MMCME5"]
    set input [one [get_pins -quiet $mmcm/CLKIN1] "MMCM input"]
    set output [one [get_pins -quiet $mmcm/CLKOUT0] "MMCM output"]
    set buffer_in [one [get_pins -quiet $wizard/BUFG_clkout1_inst/I] "BUFG input"]
    set buffer_out [one [get_pins -quiet $wizard/BUFG_clkout1_inst/O] "BUFG output"]
    set rm_pin [one [get_pins -quiet top_i/slash/user_clk] "RM clock input"]
    if {![same_net $output $buffer_in] || ![same_net $buffer_out $rm_pin]} {
        error "User clock: unexpected MMCM/BUFG/RM topology"
    }

    # Replace an IP-local primary input clock with a 1:1 generated alias of
    # the physical upstream driver. Preserve its name for existing exceptions.
    # Vivado has no remove_clocks command; never reset all timing constraints.
    set legacy_input [one [get_pins -quiet \
        top_i/static_region/clk_rst_shell/clk_wizard_slash/inst/clk_in1] "wizard input"]
    foreach clock [get_clocks -quiet -of_objects $legacy_input] {
        if {![get_property IS_GENERATED $clock] &&
            [get_property SOURCE_PINS $clock] eq $legacy_input} {
            set net [get_nets -segments -of_objects $legacy_input]
            set drivers [get_pins -quiet -leaf -of_objects $net -filter {DIRECTION == OUT}]
            if {![llength $drivers]} {
                set drivers [get_ports -quiet -of_objects $net -filter {DIRECTION == IN}]
            }
            set driver [one $drivers "upstream reference driver"]
            one [get_clocks -quiet -of_objects $driver] "upstream reference clock"
            create_generated_clock -name $clock -source $driver -divide_by 1 $legacy_input
        }
    }
    set reference [one [get_clocks -quiet -of_objects $input] "MMCM reference clock"]
    set ref_hz [expr {round(1e9 / [get_property PERIOD $reference])}]
    if {$ref_hz != 100000000} {
        error "User clock: vrtd expects a 100 MHz reference, got $ref_hz Hz"
    }
    # Do not silently reuse a name now owned by a different clock tree.
    foreach {name pin} [list slash_user_source $output user_clk $rm_pin] {
        set existing [get_clocks -quiet $name]
        if {[llength $existing] && [get_property SOURCE_PINS $existing] ne $pin} {
            error "User clock: $name is rooted at an unexpected pin"
        }
    }
    set divisor [gcd $hz $ref_hz]
    create_generated_clock -name slash_user_source -source $input -multiply_by [expr {$hz / $divisor}] \
        -divide_by [expr {$ref_hz / $divisor}] $output
    create_generated_clock -name user_clk -source $buffer_out -divide_by 1 $rm_pin

    foreach pin [list $buffer_out $rm_pin] {
        set clock [one [get_clocks -quiet -of_objects $pin] "clock at $pin"]
        if {![get_property IS_GENERATED $clock] ||
            abs([get_property PERIOD $clock] - 1e9 / $hz) > 0.001} {
            error "User clock: incorrect generated clock at $pin"
        }
    }
    if {[get_property MASTER_CLOCK [get_clocks user_clk]] ne "slash_user_source"} {
        error "User clock: RM and static user clocks have no common source"
    }
    puts "SLASH_USER_CLOCK: $hz Hz, reference=$reference, origin=$output"
}

# Also used by example-local placement hooks. Wrappers source existing hooks
# first; remembering the previous hook makes repeated configuration idempotent.
proc slash_user_clock::chain_hook {run step directory name commands} {
    file mkdir $directory
    set wrapper [file normalize [file join $directory ${name}.tcl]]
    set saved [file join $directory ${name}.previous]
    lassign [split $step .] stage position
    if {$position ni {PRE POST}} {error "Invalid implementation hook: $step"}
    set property STEPS.${stage}.TCL.${position}
    set head [get_property $property $run]
    set cursor $head
    set seen {}
    set linked 0
    while {$cursor ne ""} {
        if {$cursor in $seen} {error "Cyclic implementation hook chain: $cursor"}
        lappend seen $cursor
        if {$cursor eq $wrapper} {set linked 1; break}
        set next_file [file rootname $cursor].previous
        if {![file isfile $cursor] || ![file isfile $next_file]} {break}
        set f [open $cursor r]
        try {set marker [gets $f]} finally {close $f}
        if {$marker ne "# SLASH_CHAIN_HOOK"} {break}
        set f [open $next_file r]
        try {set cursor [read $f]} finally {close $f}
    }
    set previous $head
    if {$linked} {
        set previous ""
        if {[file exists $saved]} {
            set f [open $saved r]
            try {set previous [read $f]} finally {close $f}
        }
    } else {
        set f [open $saved w]
        try {puts -nonewline $f $previous} finally {close $f}
    }
    set f [open $wrapper w]
    try {
        puts $f "# SLASH_CHAIN_HOOK"
        if {$previous ne ""} {puts $f [list source $previous]}
        foreach command $commands {puts $f $command}
    } finally {close $f}
    if {!$linked} {set_property $property $wrapper $run}
    return $wrapper
}

proc slash_user_clock::configure_run {hz directory} {
    variable model_file
    positive_hz $hz
    file mkdir $directory
    set model [file normalize [file join $directory user_clock.tcl]]
    if {$model_file ne $model} {file copy -force $model_file $model}
    chain_hook [get_runs impl_1] OPT_DESIGN.PRE $directory user_clock_impl \
        [list [list source $model] [list slash_user_clock::apply $hz]]
}

# A conservative superset of the driver's first 50 candidates: include every
# tie at the cutoff. Ranking and bounds mirror vrt/vrtd/src/clock.c; tests compare
# this enumeration with that C implementation, rather than a Python re-copy.
proc slash_user_clock::compare_modes {a b} {
    foreach index {0 1 2 3 4} {
        set x [lindex $a $index]
        set y [lindex $b $index]
        if {$x < $y} {return -1}
        if {$x > $y} {return 1}
    }
    return 0
}

proc slash_user_clock::runtime_modes {hz} {
    positive_hz $hz
    set modes {}
    for {set m 5} {$m <= 432} {incr m} {
        # D > 10 violates the minimum PFD frequency for the 100 MHz reference.
        for {set d 1} {$d <= 10} {incr d} {
            set vco [expr {100000000 * wide($m) / $d}]
            if {$vco < 2160000000 || $vco > 4320000000} {continue}
            set estimate [expr {min(511, max(2, ($vco + $hz / 2) / $hz))}]
            for {set o [expr {max(2, $estimate - 6)}]} {$o <= min(511, $estimate + 6)} {incr o} {
                set achieved [expr {$vco / $o}]
                if {$achieved > $hz} {continue}
                lappend modes [list [expr {$hz - $achieved}] [expr {$o % 4 != 0}] \
                    [expr {$o % 2 != 0}] [expr {-$vco}] $o $m $d $achieved]
            }
        }
    }
    set modes [lsort -command slash_user_clock::compare_modes $modes]
    if {![llength $modes]} {error "No round-down runtime MMCM mode for $hz Hz"}
    set result [lrange $modes 0 49]
    foreach mode [lrange $modes 50 end] {
        if {[compare_modes $mode [lindex $modes 49]] != 0} {break}
        lappend result $mode
    }
    return $result
}

proc slash_user_clock::check_coverage {report_file} {
    set f [open $report_file r]
    try {set text [read $f]} finally {close $f}
    foreach check {constant_clock pulse_width_clock multiple_clock generated_clocks loops latch_loops} {
        if {![regexp [format {checking %s \(([0-9]+)\)} $check] $text -> count] || $count != 0} {
            error "User clock coverage failed: $check; see $report_file"
        }
    }
    if {![regexp {There are ([0-9]+) pins that are not constrained for maximum delay\.} $text -> active] || $active != 0} {
        error "User clock: missing coverage or active unconstrained endpoints; see $report_file"
    }
    # Known inactive PS MDIO clock pins are unrelated to the PL user clock.
    if {![regexp {checking no_clock \(([0-9]+)\)} $text -> count]} {
        error "User clock: missing no_clock report"
    }
    set known 0
    foreach suffix {EMIOENET0MDIOMDCINT EMIOENET1MDIOMDCINT} {
        set pin top_i/static_region/aved/cips/inst/pspmc_0/inst/PS9_inst/$suffix
        if {[lsearch -exact [split $text \n] $pin] >= 0} {incr known}
    }
    if {$known != $count} {error "User clock: new no-clock endpoints; see $report_file"}
    # Inactive CPM GT adaptation pins are the only accepted constant-clock
    # endpoints. Match names AND count; a newly untimed RM pin must fail closed.
    if {![regexp {There are ([0-9]+) pins that are not constrained for maximum delay due to constant clock\.} $text -> count]} {
        error "User clock: missing constant-clock endpoint coverage"
    }
    set known 0
    foreach line [split $text \n] {
        if {[regexp {^top_i/static_region/aved/cips/inst/cpm_0/inst/gt_quad_inst[0-3]/inst/quad_inst/CH[0-3]_(PHYESMADAPTSAVE|RXDLYALIGNREQ|RXMLDCHAINDONE|RXMLDCHAINREQ|RXPHALIGNREQ|RXPHSETINITREQ|RXPHSHIFT180)$} [string trim $line]]} {incr known}
    }
    if {$known != $count} {error "User clock: unexpected constant-clock endpoints; see $report_file"}
}

proc slash_user_clock::report {directory} {
    file mkdir $directory
    report_clocks -file [file join $directory clocks.rpt]
    report_clock_interaction -file [file join $directory clock_interaction.rpt]
    report_methodology -file [file join $directory methodology.rpt]
    report_pulse_width -file [file join $directory pulse_width.rpt]
    check_timing -verbose -file [file join $directory check_timing.rpt]
    report_timing_summary -delay_type min_max -check_timing_verbose -max_paths 100 \
        -file [file join $directory timing.rpt]
    set static [get_cells -hierarchical -filter {NAME =~ top_i/static_region/* && IS_SEQUENTIAL}]
    set rm [get_cells -hierarchical -filter {NAME =~ top_i/slash/* && IS_SEQUENTIAL}]
    foreach {name from to} [list static $static $static static_to_rm $static $rm \
                                rm_to_static $rm $static rm $rm $rm] {
        if {[llength $from] && [llength $to]} {
            report_timing -from $from -to $to -delay_type min_max -max_paths 100 \
                -file [file join $directory ${name}.rpt]
        }
    }
    set f [open [file join $directory coverage.txt] w]
    puts $f "Reports cover only timing paths retained in the loaded checkpoint."
    puts $f "An RM linked against an abstract shell is NOT a full static-platform signoff."
    puts $f "Full signoff also needs the matching complete parent checkpoint, at the same target and runtime modes."
    close $f
    # Keep diagnostics even when the clock model has incomplete coverage.
    check_coverage [file join $directory check_timing.rpt]
}

proc slash_user_clock::characterize_runtime {hz directory} {
    variable wizard
    positive_hz $hz
    set mmcm [one [get_cells $wizard/MMCME5_inst] "user MMCME5"]
    set saved {}
    foreach property {CLKFBOUT_MULT DIVCLK_DIVIDE CLKOUT0_DIVIDE CLKFBOUT_FRACT} {
        lappend saved $property [get_property $property $mmcm]
    }
    file mkdir $directory
    set f [open [file join $directory runtime_modes.tsv] w]
    puts $f "m\td\to\tachieved_hz\tsetup_ns\thold_ns\tpulse_width_violated"
    set worst_setup 1e9
    set worst_hold 1e9
    set worst_setup_mode {}
    set worst_hold_mode {}
    try {
        foreach mode [runtime_modes $hz] {
            lassign $mode diff o4 o2 nvco o m d achieved
            set_property -dict [list CLKFBOUT_MULT $m DIVCLK_DIVIDE $d CLKOUT0_DIVIDE $o CLKFBOUT_FRACT 0] $mmcm
            apply $achieved
            set setup [get_property SLACK [one [get_timing_paths -delay_type max \
                -slack_lesser_than 1000000 -max_paths 1] "setup path"]]
            set hold [get_property SLACK [one [get_timing_paths -delay_type min \
                -slack_lesser_than 1000000 -max_paths 1] "hold path"]]
            set pulse [report_pulse_width -all_violators -no_header -return_string]
            # Even with -no_header a clean report can contain section labels.
            set bad_pulse [regexp {Slack\s*\(VIOLATED\)} $pulse]
            puts $f "$m\t$d\t$o\t$achieved\t$setup\t$hold\t$bad_pulse"
            if {$setup < $worst_setup} {set worst_setup $setup; set worst_setup_mode $mode}
            if {$hold < $worst_hold} {set worst_hold $hold; set worst_hold_mode $mode}
            if {$bad_pulse} {
                set pf [open [file join $directory pulse_M${m}_D${d}_O${o}.rpt] w]
                try {puts $pf $pulse} finally {close $pf}
            }
        }
        foreach {label mode} [list setup $worst_setup_mode hold $worst_hold_mode] {
            lassign $mode diff o4 o2 nvco o m d achieved
            set_property -dict [list CLKFBOUT_MULT $m DIVCLK_DIVIDE $d CLKOUT0_DIVIDE $o CLKFBOUT_FRACT 0] $mmcm
            apply $achieved
            report [file join $directory worst_$label]
        }
    } finally {
        # Even a failed STA must leave the boot MMCM configuration intact.
        close $f
        set_property -dict $saved $mmcm
        apply $hz
    }
    return [list $worst_setup $worst_hold]
}
