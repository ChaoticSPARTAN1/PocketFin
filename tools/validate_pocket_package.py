# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms and permissions: ADDITIONAL-TERMS.md
"""Check a core.json against the Analogue Pocket (APF) limits the Pocket enforces."""
from __future__ import annotations

from pathlib import Path


def check_core(core: dict, directory: Path):
    if core.get("magic") != "APF_VER_1":
        raise ValueError("core.json has an invalid APF magic.")
    limits = {"shortname": 31, "description": 63, "author": 31,
              "url": 63, "version": 31, "date_release": 10}
    metadata = core["metadata"]
    expected_directory = metadata["author"] + "." + metadata["shortname"]
    if directory.name != expected_directory:
        raise ValueError("Core folder does not match the author.shortname identity used by Pocket.")
    for key, maximum in limits.items():
        if not isinstance(metadata.get(key), str) or len(metadata[key].encode("utf-8")) > maximum:
            raise ValueError(f"core metadata {key} exceeds its APF limit or has the wrong type.")
    if len(metadata["platform_ids"]) > 4:
        raise ValueError("Too many APF platforms.")
    framework = core["framework"]
    if framework["target_product"] != "Analogue Pocket":
        raise ValueError("Invalid target product.")
    # Analogue documents the loader here; working cores on this Pocket use it here too.
    loader = framework.get("chip32_vm")
    if not loader or len(loader) > 15 or not (directory / loader).is_file():
        raise ValueError("Declare the existing Chip32 loader in core.framework.chip32_vm.")
    bitstreams = core["cores"]
    if not 1 <= len(bitstreams) <= 8:
        raise ValueError("Invalid APF bitstream count.")
    for bitstream in bitstreams:
        if "chip32_vm" in bitstream:
            raise ValueError("Chip32 loader belongs in framework, not the bitstream entry.")
        for key in ("name", "filename"):
            if len(bitstream.get(key, "")) > 15:
                raise ValueError("Bitstream name/filename exceeds APF limits.")
        if not (directory / bitstream["filename"]).is_file():
            raise ValueError("Referenced bitstream is missing.")
