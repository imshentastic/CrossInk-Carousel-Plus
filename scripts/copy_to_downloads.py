"""
PlatformIO post-build script: archive each firmware.bin to ~/Downloads under a
name unique to that build, so successive test builds accumulate instead of
overwriting each other.

Opt-in. Does nothing unless the environment sets

    custom_downloads_copy = yes

which belongs in platformio.local.ini (gitignored personal overrides), not in
platformio.ini. Set CROSSPOINT_DOWNLOADS_DIR to archive somewhere other than
~/Downloads.

Name format:

    firmware-<env>-<version>-<YYYYMMDD-HHMMSS>-<shorthash>[-dirty].bin
    firmware-tiny-cjk-4.7.1-20260805-214800-9ab8fe86-dirty.bin

The timestamp guarantees uniqueness and sorts chronologically; the short hash
plus the -dirty marker say which code it was, which matters when a build is
made from an uncommitted working tree (the common case while iterating).

Never fails the build: any problem here is reported and ignored, because
losing an archive copy is not a reason to fail a firmware build.
"""

import os
import shutil
import subprocess
import sys
from datetime import datetime


def _truthy(value):
    return str(value).strip().lower() in {'1', 'true', 'yes', 'on'}


def _git(project_dir, *args, fallback=''):
    try:
        return subprocess.check_output(
            ['git', *args], text=True, stderr=subprocess.DEVNULL, cwd=project_dir
        ).strip()
    except Exception:
        return fallback


def _version(env):
    try:
        return env.GetProjectConfig().get('crosspoint', 'crumble_version').strip()
    except Exception:
        return 'dev'


def copy_to_downloads(source, target, env):
    try:
        try:
            enabled = env.GetProjectOption('custom_downloads_copy')
        except Exception:
            return
        if not _truthy(enabled):
            return

        project_dir = env['PROJECT_DIR']
        dest_dir = os.environ.get('CROSSPOINT_DOWNLOADS_DIR') or os.path.expanduser('~/Downloads')
        if not os.path.isdir(dest_dir):
            print(f'copy_to_downloads: {dest_dir} does not exist; skipping')
            return

        short_hash = _git(project_dir, 'rev-parse', '--short', 'HEAD', fallback='nogit')
        dirty = '-dirty' if _git(project_dir, 'status', '--porcelain') else ''
        stamp = datetime.now().strftime('%Y%m%d-%H%M%S')

        name = f"firmware-{env['PIOENV']}-{_version(env)}-{stamp}-{short_hash}{dirty}.bin"
        dest = os.path.join(dest_dir, name)
        shutil.copy(str(target[0]), dest)
        print(f'copy_to_downloads: {dest} ({os.path.getsize(dest)} bytes)')
    except Exception as exc:  # never break a build over an archive copy
        print(f'copy_to_downloads: skipped ({exc})', file=sys.stderr)


try:
    Import('env')                                           # noqa: F821  # type: ignore[name-defined]
    env.AddPostAction(                                      # noqa: F821  # type: ignore[name-defined]
        '$BUILD_DIR/${PROGNAME}.bin',
        copy_to_downloads,
    )
except NameError:
    print('copy_to_downloads.py: must be run via PlatformIO', file=sys.stderr)
