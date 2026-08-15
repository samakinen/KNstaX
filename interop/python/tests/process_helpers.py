# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen

from __future__ import annotations

import asyncio
import os
import subprocess
from typing import Any

try:
    import fcntl
except ImportError:  # pragma: no cover - fcntl is not available on Windows
    fcntl = None


def _set_nonblocking(fd: int) -> None:
    try:
        os.set_blocking(fd, False)
    except AttributeError:
        if fcntl is None:
            return
        flags = fcntl.fcntl(fd, fcntl.F_GETFL)
        fcntl.fcntl(fd, fcntl.F_SETFL, flags | os.O_NONBLOCK)


def _read_available(fd: int) -> bytes:
    try:
        return os.read(fd, 8192)
    except BlockingIOError:
        return b""
    except OSError:  # pragma: no cover - best-effort drain only
        return b""


async def wait_for_line(
    proc: subprocess.Popen[bytes],
    needle: bytes,
    timeout_s: float = 3.0,
    *,
    raise_on_missing: bool = True,
    poll_interval: float = 0.02,
) -> bool:
    if proc.stdout is None:
        raise AssertionError("process stdout not available")

    fd = proc.stdout.fileno()
    _set_nonblocking(fd)

    loop = asyncio.get_running_loop()
    deadline = loop.time() + timeout_s
    buf = bytearray()

    while loop.time() < deadline:
        chunk = _read_available(fd)
        if chunk:
            buf.extend(chunk)
            if needle in buf:
                return True

        if proc.poll() is not None:
            # Attempt to drain any final bytes before giving up.
            tail = _read_available(fd)
            if tail:
                buf.extend(tail)
                if needle in buf:
                    return True
            break

        await asyncio.sleep(poll_interval)

    found = needle in buf
    if not found and raise_on_missing:
        raise AssertionError(f"did not see {needle!r} in peer output. got:\n{buf.decode(errors='replace')}")
    return found


# Backwards-compatible alias: some tests call `_wait_for_line`.
_wait_for_line = wait_for_line


def drain_process_output(proc: subprocess.Popen[bytes]) -> str:
    if proc.stdout is None:
        return ""

    fd = proc.stdout.fileno()
    _set_nonblocking(fd)
    buf = bytearray()

    while True:
        chunk = _read_available(fd)
        if not chunk:
            break
        buf.extend(chunk)

    return buf.decode(errors="replace")
