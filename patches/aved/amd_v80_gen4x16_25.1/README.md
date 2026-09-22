# V80 Gen4 x16 hardware variant

This design is based on `amd_v80_gen5x8_25.1`. It selects CPM5 PCIe
Controller 0 in QDMA mode at 16 GT/s x16 and leaves the original Gen5 x8
directory intact. The PF and BAR settings are kept compatible with the
original design.

The matching SLASH service and compute Tcl designs are in the parent
repository. Build all three images from the same revision. A Gen5 x8 PDI
or XSA is not a substitute for an artifact built from this variant.

`src/create_design.tcl` followed by `src/export_xsa.tcl` exports the
minimal XSA needed to compile AMC and RP1 firmware. `build_all.sh` performs
the complete AVED image build when a shell image is required. Vivado 2025.1
must have the V80 SMBus IP installed in the project's IP catalog, as with
the source Gen5 x8 design.

The V80 host slot must provide 16 electrical lanes as one PCIe port. After
programming, verify `LnkSta` reports `Speed 16GT/s, Width x16`.
