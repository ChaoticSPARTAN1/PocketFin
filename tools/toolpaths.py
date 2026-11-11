# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms and permissions: ADDITIONAL-TERMS.md
"""Where PocketFin's build and test tools live.

Every path can be changed with an environment variable, so nothing here depends on one PC.
Defaults assume Windows with MSYS2 in C:\\msys64 and the other tools unpacked under
local-tools\\ inside this repository (that folder is Git-ignored). See docs/Building-from-Source.md.
"""
from __future__ import annotations

import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LOCAL = Path(os.environ.get('POCKETFIN_LOCAL_TOOLS', ROOT / 'local-tools'))

# MSYS2 UCRT64: native gcc for the PC checks (and libjpeg for check_sd_library).
MSYS = Path(os.environ.get('POCKETFIN_MSYS', 'C:/msys64'))
MSYS_BIN = MSYS / 'ucrt64/bin'
GCC = MSYS_BIN / 'gcc.exe'

# RISC-V cross compiler (riscv64-unknown-elf-gcc 15.1, the MSYS2 packages pinned in upstream.lock.json).
RISCV_BIN = Path(os.environ.get('POCKETFIN_RISCV_BIN', LOCAL / 'riscv/ucrt64/bin'))

# Icarus Verilog: a folder holding bin/iverilog.exe, bin/vvp.exe and lib/ivl.
IVERILOG = Path(os.environ.get('POCKETFIN_IVERILOG', LOCAL / 'iverilog'))

# Intel Quartus Prime Lite 21.1.1 (only for the FPGA build and the bridge test's RAM model).
QUARTUS = Path(os.environ.get('POCKETFIN_QUARTUS', 'C:/intelFPGA_lite/21.1/quartus'))

# Java 17 and sbt 1.10 for generating the VexiiRiscv CPU (only needed to regenerate it).
JAVA_HOME = Path(os.environ.get('POCKETFIN_JAVA_HOME', LOCAL / 'java'))
SBT_HOME = Path(os.environ.get('POCKETFIN_SBT_HOME', LOCAL / 'sbt'))

# Build and test output (Git-ignored).
BUILD = ROOT / 'build'


def msys_env():
    """os.environ with MSYS2's runtime DLLs first on PATH (needed by the gcc-built checks)."""
    env = dict(os.environ)
    env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    return env
