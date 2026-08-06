"""
PlatformIO post-build script: fail the build if firmware.bin exceeds the
environment's `custom_max_firmware_size` project option.

Used by env:tiny-cjk to enforce the stock 6.25 MB slot (6,553,600 bytes)
with release headroom: the env sets custom_max_firmware_size = 6500000.
Environments without the option are unaffected.
"""

import os
import sys


def check_firmware_size(source, target, env):
    limit_raw = None
    try:
        limit_raw = env.GetProjectOption('custom_max_firmware_size')
    except Exception:
        return
    if not limit_raw:
        return
    limit = int(str(limit_raw).strip())

    bin_path = str(target[0])
    size = os.path.getsize(bin_path)
    pct = 100.0 * size / limit
    print(f'check_firmware_size: {os.path.basename(bin_path)} is {size} bytes '
          f'({pct:.1f}% of the {limit}-byte cap)')
    if size > limit:
        print(f'check_firmware_size: FAILED -- firmware.bin ({size} bytes) exceeds '
              f'custom_max_firmware_size ({limit} bytes) by {size - limit} bytes.',
              file=sys.stderr)
        env.Exit(1)


try:
    Import('env')                                           # noqa: F821  # type: ignore[name-defined]
    env.AddPostAction(                                      # noqa: F821  # type: ignore[name-defined]
        '$BUILD_DIR/${PROGNAME}.bin',
        check_firmware_size,
    )
except NameError:
    print('check_firmware_size.py: must be run via PlatformIO', file=sys.stderr)
