#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen
# Small helper to run the interop pytest suite in a virtualenv.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VENV_DIR="$ROOT_DIR/interop/.venv"

usage() {
  cat <<EOF
Usage: $0 [--build-dir BUILD_DIR]

If BUILD_DIR is provided (default: ../build_test), the script will look
for interop binaries in BUILD_DIR/interop/bin and set KNSTAX_TUNNEL_PEER_BIN
and KNSTAX_ROUTING_PEER_BIN accordingly. Otherwise you must set those env
variables yourself.
EOF
}

BUILD_DIR="${1:-${ROOT_DIR}/build_test}"

if [[ "$1" == "-h" || "$1" == "--help" ]]; then
  usage
  exit 0
fi

python3 -m venv "$VENV_DIR"
source "$VENV_DIR/bin/activate"
pip install --upgrade pip wheel
if [[ -f "$ROOT_DIR/interop/python/requirements.txt" ]]; then
  pip install -r "$ROOT_DIR/interop/python/requirements.txt"
fi

# If not provided via env, prefer binaries built into build dir
if [[ -z "${KNSTAX_TUNNEL_PEER_BIN:-}" ]]; then
  candidate="$BUILD_DIR/interop/bin/knstax_tunnel_peer"
  if [[ -x "$candidate" ]]; then
    export KNSTAX_TUNNEL_PEER_BIN="$candidate"
  fi
fi
if [[ -z "${KNSTAX_ROUTING_PEER_BIN:-}" ]]; then
  candidate="$BUILD_DIR/interop/bin/knstax_routing_peer"
  if [[ -x "$candidate" ]]; then
    export KNSTAX_ROUTING_PEER_BIN="$candidate"
  fi
fi

if [[ -z "${KNSTAX_TUNNEL_PEER_BIN:-}" ]]; then
  echo "ERROR: KNSTAX_TUNNEL_PEER_BIN must be set (or build with ENABLE_INTEROP_TESTS to place it in $BUILD_DIR/interop/bin)" >&2
  exit 2
fi

echo "Using KNSTAX_TUNNEL_PEER_BIN=$KNSTAX_TUNNEL_PEER_BIN"
echo "Using KNSTAX_ROUTING_PEER_BIN=${KNSTAX_ROUTING_PEER_BIN:-<not-set>}"

pytest -q "$ROOT_DIR/interop/python/tests"
