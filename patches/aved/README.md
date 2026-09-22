# SLASH AVED sources and patches

This directory owns SLASH's AVED additions independently of the AVED submodule.
The current upstream baseline is `cc2b3f9b7bd3eb2ae63cdc9da1025c966c5409e4`.

- `amd_v80_gen4x16_25.1/` is the complete standalone hardware design: Tcl,
  constraints, IP sources, FPT tools and build scripts. It is copied directly
  into `submodules/AVED/hw/amd_v80_gen4x16_25.1/`. No Gen5 directory is
  read, copied or transformed to produce it.
- `0001-ami-include-vmalloc.patch` adds `linux/vmalloc.h` to four AMI driver
  sources. `series` defines the patch application order.
- `apply.py` modifies the source submodule directly. It requires
  Python 3.9+ and GNU `patch`. Already-applied patches are skipped; patch errors
  stop preparation. It does not invoke any build tools.

Generated `build/`, `.Xil/`, logs and journals are excluded. The exported firmware
XSA is maintained separately at
`linker/slashkit/resources/aved/amd_v80_gen4x16_25.1.xsa`.

## Apply to the submodule

After initializing the AVED submodule, run once from the SLASH root:

```sh
zsh -lic 'python3 patches/aved/apply.py'
```

This applies the AMI patch and copies the independent Gen4 design directly into
`submodules/AVED`. Existing build outputs are preserved. Use
`--aved-root /path/to/AVED` to select another source tree.

Shell builds and AMI packaging then use the modified submodule through their
existing workflows. Neither applies these patches itself.

The standalone hardware scripts expect the AVED layout because firmware
is located at `../../fw/AMC`. Run them from
`submodules/AVED/hw/amd_v80_gen4x16_25.1/`. To export
the minimal firmware XSA, source `src/create_design.tcl` and then
`src/export_xsa.tcl` in the same Vivado session. This remains an AVED firmware
handoff; SLASH's service/compute shell Tcl owns its four-PF PCIe configuration.

An installed slashkit wheel does not carry the repository-level `patches/`
directory. Apply these changes to AVED first, then pass that tree with
`slashkit install --aved-source /path/to/prepared/AVED ...`.
