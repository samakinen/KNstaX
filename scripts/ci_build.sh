#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen
set -euo pipefail

# CI / developer helper for building and running tests on Linux host
# Usage: ./scripts/ci_build.sh [--no-secure] [--no-tests]

BUILD_DIR=build_test
KNX_SECURE=ON
BUILD_TESTS=ON

for arg in "$@"; do
  case "$arg" in
    --no-secure) KNX_SECURE=OFF ;; 
    --no-tests) BUILD_TESTS=OFF ;; 
    -h|--help)
      echo "Usage: $0 [--no-secure] [--no-tests]"
      exit 0
      ;;
  esac
done

cmake -S . -B "$BUILD_DIR" -DKNX_PLATFORM=linux -DKNX_SECURE_ENABLED=${KNX_SECURE} -DBUILD_TESTS=${BUILD_TESTS}
cmake --build "$BUILD_DIR" -j

if [ "$BUILD_TESTS" = "ON" ]; then
  ctest --test-dir "$BUILD_DIR" --output-on-failure
else
  echo "Tests skipped (--no-tests)"
fi
