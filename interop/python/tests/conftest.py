# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen

from __future__ import annotations

import os
import sys
from pathlib import Path
import pytest


# Make repository root importable so `interop` is a top-level package, and
# also make `interop/python` importable so `knip_gateway` can be imported
# as a top-level package depending on test imports.
REPO_ROOT = Path(__file__).resolve().parents[2]
PY_ROOT = Path(__file__).resolve().parents[1]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))
if str(PY_ROOT) not in sys.path:
    sys.path.insert(0, str(PY_ROOT))


def pytest_addoption(parser: pytest.Parser) -> None:
    parser.addoption(
        "--knstax-tunnel-peer-bin",
        action="store",
        default=None,
        help=("Path to the KNstaX tunnelling peer binary used by interop tests. "
              "Can also be provided via KNSTAX_TUNNEL_PEER_BIN environment variable."),
    )
    parser.addoption(
        "--knstax-routing-peer-bin",
        action="store",
        default=None,
        help=("Path to the KNstaX routing peer binary used by interop tests. "
              "Can also be provided via KNSTAX_ROUTING_PEER_BIN environment variable."),
    )


def pytest_configure(config: pytest.Config) -> None:
    # Prefer explicit CLI options; fall back to legacy KNSTAX_PEER_BIN mapping
    tunnel_opt = config.getoption("--knstax-tunnel-peer-bin")
    routing_opt = config.getoption("--knstax-routing-peer-bin")

    # If explicit tunnel option provided, set env var
    if tunnel_opt:
        os.environ["KNSTAX_TUNNEL_PEER_BIN"] = tunnel_opt
    # If explicit routing option provided, set env var
    if routing_opt:
        os.environ["KNSTAX_ROUTING_PEER_BIN"] = routing_opt

    # Backwards compatibility: if legacy KNSTAX_PEER_BIN is set and explicit
    # tunnel var is not, map it to tunnel peer.
    legacy = os.environ.get("KNSTAX_PEER_BIN")
    if legacy and not os.environ.get("KNSTAX_TUNNEL_PEER_BIN"):
        os.environ["KNSTAX_TUNNEL_PEER_BIN"] = legacy
