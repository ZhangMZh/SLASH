# SPDX-License-Identifier: MIT
# Read-only timing characterization; does not synthesize, implement or emit PDI.
# vivado -mode batch -source analyze_user_clock.tcl -tclargs routed.dcp hz reports
if {[llength $argv] != 3} {
    error "Expected: <routed checkpoint> <target Hz> <report directory>"
}
lassign $argv checkpoint hz directory
source [file join [file dirname [info script]] user_clock.tcl]
slash_user_clock::positive_hz $hz
if {![file isfile $checkpoint]} {error "Checkpoint not found: $checkpoint"}
open_checkpoint $checkpoint
try {
    slash_user_clock::apply $hz
    slash_user_clock::report [file join $directory nominal]
    slash_user_clock::characterize_runtime $hz [file join $directory runtime]
} finally {close_design}
