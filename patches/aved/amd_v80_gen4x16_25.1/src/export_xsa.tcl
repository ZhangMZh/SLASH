# Export the hardware handoff needed by AMC and RP1 without implementing a shell.
set design_name "amd_v80_gen4x16_25.1"

validate_bd_design
generate_target all [get_files top.bd]
write_hw_platform -force -fixed -minimal \
    "[get_property DIRECTORY [current_project]]/${design_name}.xsa"
