#!/usr/bin/env python3
"""Restore SLASH's AVED changes directly into the source submodule."""

import argparse
from pathlib import Path
import shutil
import subprocess


DESIGN = "amd_v80_gen4x16_25.1"
OVERLAY = Path(__file__).resolve().parent


def apply_patches(ami_root: Path) -> None:
    for line in (OVERLAY / "series").read_text().splitlines():
        name = line.strip()
        if not name or name.startswith("#"):
            continue
        # Patch paths are a/sw/AMI/...; apply relative to the AMI directory.
        command = ["patch", "--batch", "--fuzz=0", "-p3", "-d", str(ami_root),
                   "-i", str(OVERLAY / name)]
        # --force prevents patch from silently switching a reverse check forward.
        reverse = subprocess.run(command + ["--dry-run", "--reverse", "--force"],
                                 capture_output=True, text=True)
        if reverse.returncode == 0:
            print(f"Already applied: {name}")
            continue
        subprocess.run(command + ["--forward"], check=True)


def apply_aved(aved_root: Path) -> None:
    apply_patches(aved_root / "sw" / "AMI")
    # This is a complete independent design, not a transformation of Gen5.
    shutil.copytree(OVERLAY / DESIGN, aved_root / "hw" / DESIGN,
                    dirs_exist_ok=True,
                    ignore=shutil.ignore_patterns("build", ".Xil", "__pycache__",
                                                  "*.log", "*.jou"))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--aved-root", type=Path,
                        default=OVERLAY.parents[1] / "submodules" / "AVED",
                        help="AVED source tree; defaults to this checkout's submodules/AVED")
    args = parser.parse_args()
    apply_aved(args.aved_root.resolve())


if __name__ == "__main__":
    main()
