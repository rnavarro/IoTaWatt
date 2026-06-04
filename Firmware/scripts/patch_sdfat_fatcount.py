#!/usr/bin/env python3
"""Backport the SdFat 2.3.1 single-FAT fix into the ESP8266 core's SdFat 2.1.1.

Why
---
ESP8266 Arduino core 3.x ships SdFat 2.x, whose FatPartition::init() refuses
to mount FAT volumes whose BPB advertises one FAT table (bpb->fatCount != 2).
SdFat 1.x (core 2.4.0) accepted any nonzero count, and some factory-formatted
SD cards shipped with IoTaWatt units (Oct-Dec 2022 batch) use a single FAT.
This is the failure that forced the 02_08_00 -> 02_08_01 rollback in Dec 2022.

greiman/SdFat fixed this upstream in 2.3.1 (commit cda05731, 2025-08-13), but
the fix has not propagated to earlephilhower/ESP8266SdFat (frozen at 2.2.2),
esp8266/Arduino (latest release 3.1.2 ships 2.1.1), or any released
platformio espressif8266 platform. Until it does, this script backports the
fix into the installed framework package at build time.

What it changes (mirrors greiman 2.3.1 exactly)
-----------------------------------------------
FatLib/FatPartition.cpp:
  1. BPB validation: accept fatCount 1 or 2 (was: exactly 2), store m_fatCount
  2. Root dir start: m_fatCount * m_sectorsPerFat (was: hardcoded 2 *)
FatLib/FatPartition.h:
  3. fatCount() accessor returns m_fatCount (was: hardcoded 2)
  4. fatCachePrepare(): only set CACHE_STATUS_MIRROR_FAT when m_fatCount == 2
     (on a single-FAT card the mirror write would land past the FAT, inside
     the root directory / data region, corrupting the filesystem)
  5. Add uint8_t m_fatCount member

All five edits are required together: relaxing the mount check without (2)
and (4) mounts the card with the wrong layout and corrupts it on first write.

Usage
-----
As a PlatformIO extra_script (patches before compile):
    extra_scripts = pre:scripts/patch_sdfat_fatcount.py

Standalone:
    python3 scripts/patch_sdfat_fatcount.py            # apply
    python3 scripts/patch_sdfat_fatcount.py --restore  # undo (from .orig backups)

Notes
-----
- Idempotent: re-running on a patched tree is a no-op.
- Fails loudly if the expected code is not found and the tree is not already
  patched (e.g. after a platform upgrade changes SdFat) so a silent mismatch
  cannot slip through.
- The PlatformIO package directory is shared machine-wide; the patch therefore
  affects every project that builds against this framework package. Backups
  (*.orig) are written next to the patched files; --restore puts them back.
"""

import os
import shutil
import sys

SDFAT_REL = os.path.join("libraries", "ESP8266SdFat", "src", "FatLib")

# (filename, old, new) triples. Whitespace must match the installed
# framework-arduinoespressif8266 3.30102.0 (core 3.1.2, SdFat 2.1.1) exactly.
EDITS = [
    (
        "FatPartition.cpp",
        "  if (!pbs || bpb->fatCount != 2 ||\n"
        "    getLe16(bpb->bytesPerSector) != m_bytesPerSector) {\n"
        "    DBG_FAIL_MACRO;\n"
        "    goto fail;\n"
        "  }\n",
        "  if (!pbs || (bpb->fatCount != 1 && bpb->fatCount != 2) ||\n"
        "    getLe16(bpb->bytesPerSector) != m_bytesPerSector) {\n"
        "    DBG_FAIL_MACRO;\n"
        "    goto fail;\n"
        "  }\n"
        "  m_fatCount = bpb->fatCount;\n",
    ),
    (
        "FatPartition.cpp",
        "  m_rootDirStart = m_fatStartSector + 2 * m_sectorsPerFat;\n",
        "  m_rootDirStart = m_fatStartSector + m_fatCount * m_sectorsPerFat;\n",
    ),
    (
        "FatPartition.h",
        "  uint8_t fatCount() const {\n"
        "    return 2;\n"
        "  }\n",
        "  uint8_t fatCount() const {\n"
        "    return m_fatCount;\n"
        "  }\n",
    ),
    (
        "FatPartition.h",
        "  uint8_t* fatCachePrepare(uint32_t sector, uint8_t options) {\n"
        "    options |= FsCache::CACHE_STATUS_MIRROR_FAT;\n"
        "    return m_fatCache.prepare(sector, options);\n"
        "  }\n",
        "  uint8_t* fatCachePrepare(uint32_t sector, uint8_t options) {\n"
        "    if (m_fatCount == 2) {\n"
        "      options |= FsCache::CACHE_STATUS_MIRROR_FAT;\n"
        "    }\n"
        "    return m_fatCache.prepare(sector, options);\n"
        "  }\n",
    ),
    (
        "FatPartition.h",
        "  uint8_t* fatCachePrepare(uint32_t sector, uint8_t options) {\n"
        "    options |= FsCache::CACHE_STATUS_MIRROR_FAT;\n"
        "    return dataCachePrepare(sector, options);\n"
        "  }\n",
        "  uint8_t* fatCachePrepare(uint32_t sector, uint8_t options) {\n"
        "    if (m_fatCount == 2) {\n"
        "      options |= FsCache::CACHE_STATUS_MIRROR_FAT;\n"
        "    }\n"
        "    return dataCachePrepare(sector, options);\n"
        "  }\n",
    ),
    (
        "FatPartition.h",
        "  uint8_t  m_fatType = 0;             // Volume type (12, 16, OR 32).\n",
        "  uint8_t  m_fatType = 0;             // Volume type (12, 16, OR 32).\n"
        "  uint8_t  m_fatCount = 2;            // 1 or 2 FAT tables (SdFat 2.3.1 backport).\n",
    ),
]

MARKER = "m_fatCount"  # present only after patching


def fatlib_dir(framework_dir):
    return os.path.join(framework_dir, SDFAT_REL)


def is_patched(path):
    with open(path) as f:
        return MARKER in f.read()


def apply_patch(framework_dir):
    d = fatlib_dir(framework_dir)
    targets = {e[0] for e in EDITS}
    contents = {}
    for name in targets:
        p = os.path.join(d, name)
        if not os.path.exists(p):
            sys.exit("patch_sdfat_fatcount: %s not found - framework layout changed?" % p)
        with open(p) as f:
            contents[name] = f.read()

    if all(MARKER in contents[n] for n in targets):
        print("patch_sdfat_fatcount: already applied, nothing to do")
        return

    # Verify every edit matches before touching anything.
    for name, old, _ in EDITS:
        if old not in contents[name]:
            sys.exit(
                "patch_sdfat_fatcount: expected code not found in %s.\n"
                "The installed SdFat version differs from 2.1.1 - re-verify the "
                "backport against the new version before building." % name
            )

    for name in targets:
        orig = os.path.join(d, name + ".orig")
        if not os.path.exists(orig):
            shutil.copy2(os.path.join(d, name), orig)

    for name, old, new in EDITS:
        contents[name] = contents[name].replace(old, new, 1)

    for name in targets:
        with open(os.path.join(d, name), "w") as f:
            f.write(contents[name])
    print("patch_sdfat_fatcount: applied SdFat 2.3.1 single-FAT backport (%d edits)" % len(EDITS))


def restore(framework_dir):
    d = fatlib_dir(framework_dir)
    restored = 0
    for name in {e[0] for e in EDITS}:
        orig = os.path.join(d, name + ".orig")
        if os.path.exists(orig):
            shutil.move(orig, os.path.join(d, name))
            restored += 1
    print("patch_sdfat_fatcount: restored %d original files" % restored)


def find_framework_dir():
    # Standalone invocation: locate the package the same way PlatformIO does.
    home = os.environ.get("PLATFORMIO_CORE_DIR", os.path.expanduser("~/.platformio"))
    p = os.path.join(home, "packages", "framework-arduinoespressif8266")
    if not os.path.isdir(p):
        sys.exit("patch_sdfat_fatcount: framework package not found at %s" % p)
    return p


def main_standalone():
    fw = find_framework_dir()
    if "--restore" in sys.argv:
        restore(fw)
    else:
        apply_patch(fw)


# PlatformIO extra_script entry point
try:
    Import("env")  # noqa: F821  (provided by SCons when run by PlatformIO)
except NameError:
    main_standalone()
else:
    _fw = env.PioPlatform().get_package_dir("framework-arduinoespressif8266")  # noqa: F821
    apply_patch(_fw)
