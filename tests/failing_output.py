# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""An output path that a tool opens but cannot write: the renderers' tests use it to reach a write failure after a
successful open (a full disk, a lost device), which a missing directory, refused at the open, does not.

- Where /dev/full exists (Linux, the host CI): every write to it fails with ENOSPC.
- On Windows: an empty file whose first MiB another handle of this process holds a byte-range lock on
  (msvcrt.locking). The tool opens and truncates it, and its writes there fail with ERROR_LOCK_VIOLATION.
- Elsewhere: None, and the test skips.
"""

from __future__ import annotations

import contextlib
import os
from pathlib import Path
import sys
from typing import Iterator

# More than either renderer's test output, so every byte it writes lies in the lock.
LOCKED_BYTES = 1 << 20
FULL_DEVICE = Path('/dev/full')


@contextlib.contextmanager
def failing_output(folder: Path, name: str = 'locked.wav') -> Iterator[Path | None]:
    if FULL_DEVICE.exists():
        yield FULL_DEVICE
        return
    if sys.platform != 'win32':
        yield None
        return
    import msvcrt  # Windows only

    path = folder / name
    path.write_bytes(b'')
    handle = os.open(path, os.O_RDWR | os.O_BINARY)
    try:
        msvcrt.locking(handle, msvcrt.LK_NBLCK, LOCKED_BYTES)
        try:
            yield path
        finally:
            os.lseek(handle, 0, os.SEEK_SET)
            msvcrt.locking(handle, msvcrt.LK_UNLCK, LOCKED_BYTES)
    finally:
        os.close(handle)
