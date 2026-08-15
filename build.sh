#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen
#
# build.sh - Build script for KNX Stack with platform selection
#
# Usage:
#   ./build.sh              - Auto-detect platform and build
#   ./build.sh linux        - Build for Linux (x86)
#   ./build.sh macos        - Build for macOS (x86/ARM64)
#   ./build.sh windows      - Build for Windows (requires MinGW/Cygwin)
#   ./build.sh esp32        - Build for ESP32 (requires ESP-IDF)
#   ./build.sh test         - Incremental fast test run (excludes Long/benchmark)
#   ./build.sh test-all     - Incremental full test run including Long/interop
#   ./build.sh clean        - Clean build directory
#   ./build.sh help         - Show this help message

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
INSTALL_PREFIX="${SCRIPT_DIR}/install"
MIN_CMAKE_VERSION="3.21"
MIN_GCC_VERSION="13"
MIN_CLANG_VERSION="17"
MIN_APPLECLANG_VERSION="16.0"

# Color codes
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# ============================================================================
# Functions
# ============================================================================

print_help() {
    cat "$0" | grep "^#" | tail -n +2 | sed 's/^# //'
}

print_status() {
    echo -e "${BLUE}==>${NC} $1"
}

print_success() {
    echo -e "${GREEN}✓${NC} $1"
}

print_error() {
    echo -e "${RED}✗${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}!${NC} $1"
}

version_ge() {
    [ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -n1)" = "$2" ]
}

is_truthy() {
    case "${1:-}" in
        1|true|TRUE|yes|YES|on|ON)
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

detect_platform() {
    if [[ "$OSTYPE" == "linux-gnu"* ]]; then
        echo "linux"
    elif [[ "$OSTYPE" == "darwin"* ]]; then
        echo "macos"
    elif [[ "$OSTYPE" == "msys" || "$OSTYPE" == "cygwin" ]]; then
        echo "windows"
    else
        print_error "Unknown platform: $OSTYPE"
        exit 1
    fi
}

check_prerequisites() {
    local platform=$1
    local cmake_version
    local compiler_line
    local compiler_version
    
    print_status "Checking prerequisites for $platform..."
    
    # Check CMake
    if ! command -v cmake &> /dev/null; then
        print_error "CMake not found. Please install CMake."
        exit 1
    fi
    cmake_version="$(cmake --version | awk 'NR==1 {print $3}')"
    if ! version_ge "$cmake_version" "$MIN_CMAKE_VERSION"; then
        print_error "CMake $MIN_CMAKE_VERSION or newer is required for KNstaX C++23 builds (found $cmake_version)."
        exit 1
    fi
    print_success "CMake found: $(cmake --version | head -1)"
    
    # Check C++ compiler
    if ! command -v g++ &> /dev/null && ! command -v clang++ &> /dev/null; then
        print_error "C++ compiler not found. Please install GCC or Clang."
        exit 1
    fi
    
    if command -v g++ &> /dev/null; then
        compiler_version="$(g++ -dumpfullversion -dumpversion)"
        if ! version_ge "$compiler_version" "$MIN_GCC_VERSION"; then
            print_error "GCC $MIN_GCC_VERSION or newer is required for KNstaX C++23 builds (found $compiler_version)."
            exit 1
        fi
        print_success "C++ compiler found: $(g++ --version | head -1)"
    else
        compiler_line="$(clang++ --version | head -1)"
        compiler_version="$(printf '%s\n' "$compiler_line" | sed -E 's/.*version ([0-9]+(\.[0-9]+){0,2}).*/\1/')"
        if [[ "$compiler_line" == Apple* ]]; then
            if ! version_ge "$compiler_version" "$MIN_APPLECLANG_VERSION"; then
                print_error "AppleClang $MIN_APPLECLANG_VERSION or newer is required for KNstaX C++23 builds (found $compiler_version)."
                exit 1
            fi
        else
            if ! version_ge "$compiler_version" "$MIN_CLANG_VERSION"; then
                print_error "Clang $MIN_CLANG_VERSION or newer is required for KNstaX C++23 builds (found $compiler_version)."
                exit 1
            fi
        fi
        print_success "C++ compiler found: $compiler_line"
    fi
    
    # Platform-specific checks
    case "$platform" in
        esp32)
            if [ -z "$IDF_PATH" ]; then
                print_error "ESP-IDF not found. Please set IDF_PATH environment variable."
                exit 1
            fi
            print_success "ESP-IDF found: $IDF_PATH"
            ;;
    esac
}

build_platform() {
    local platform=$1
    
    print_status "Building for platform: ${BLUE}$platform${NC}"

    # Create build directory
    rm -rf "$BUILD_DIR"
    mkdir -p "$BUILD_DIR"

    cd "$BUILD_DIR"

    case "$platform" in
        linux|macos|windows)
            print_status "Configuring CMake for $platform..."
            cmake -DKNX_PLATFORM="$platform" \
                  -DCMAKE_BUILD_TYPE=Release \
                  -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" \
                  -DKNX_BUILD_EXAMPLES=OFF \
                  -B . -S "${SCRIPT_DIR}"

            print_status "Building..."
            cmake --build . --config Release -- -j$(nproc)

            print_success "Build completed for $platform"
            ;;

        esp32)
            print_warning "ESP32 builds require ESP-IDF. Using ESP-IDF build system..."
            print_status "Building for ESP32..."

            cd "$SCRIPT_DIR"
            idf.py build

            print_success "Build completed for ESP32"
            ;;

        *)
            print_error "Unknown platform: $platform"
            exit 1
            ;;
    esac
}

run_tests() {
    local platform=${1:-$(detect_platform)}
    local include_long_tests=${2:-OFF}
    local clean_test_build=${KNSTAX_CLEAN_TEST_BUILD:-OFF}
    local test_build_type=${KNSTAX_TEST_BUILD_TYPE:-Debug}
    local test_build_examples=${KNSTAX_TEST_BUILD_EXAMPLES:-ON}
    local default_ctest_timeout=90
    local default_ctest_parallel=$(nproc)
    if [ "$default_ctest_parallel" -gt 4 ]; then
        default_ctest_parallel=4
    fi
    local ctest_parallel=${KNSTAX_CTEST_PARALLEL:-$default_ctest_parallel}
    local ctest_timeout=${KNSTAX_CTEST_TIMEOUT:-$default_ctest_timeout}
    
    print_status "Building and running tests for $platform..."
    
    # Reuse the test build directory by default for fast incremental runs.
    if is_truthy "$clean_test_build"; then
        print_status "Cleaning test build directory..."
        rm -rf "$BUILD_DIR"
    fi
    mkdir -p "$BUILD_DIR"

    cd "$BUILD_DIR"

    # If interop tests exist, prepare a Python venv and ensure pytest is available.
    ENABLE_INTEROP=OFF
    if is_truthy "$include_long_tests" && [ -d "${SCRIPT_DIR}/interop" ]; then
        print_status "Preparing interop Python environment..."

        PYEXEC=$(command -v python3 || command -v python || true)
        if [ -z "$PYEXEC" ]; then
            print_warning "No Python interpreter found; interop tests will be skipped."
        else
            VENV_DIR="${SCRIPT_DIR}/interop/.venv"
            REQUIREMENTS_FILE="${SCRIPT_DIR}/interop/python/requirements.txt"
            STAMP_FILE="$VENV_DIR/.requirements.stamp"
            NEEDS_PYTEST_SETUP=0
            if [ ! -d "$VENV_DIR" ]; then
                print_status "Creating virtualenv at $VENV_DIR"
                "$PYEXEC" -m venv "$VENV_DIR"
                NEEDS_PYTEST_SETUP=1
            fi
            # shellcheck disable=SC1090
            source "$VENV_DIR/bin/activate"

            if [ -f "$REQUIREMENTS_FILE" ] && { [ ! -f "$STAMP_FILE" ] || [ "$REQUIREMENTS_FILE" -nt "$STAMP_FILE" ]; }; then
                NEEDS_PYTEST_SETUP=1
            fi

            if ! python -m pytest --version &> /dev/null; then
                NEEDS_PYTEST_SETUP=1
            fi

            if [ "$NEEDS_PYTEST_SETUP" -eq 1 ]; then
                print_status "Installing/updating interop Python dependencies..."
                pip install --upgrade pip wheel || true
                if [ -f "$REQUIREMENTS_FILE" ]; then
                    pip install -r "$REQUIREMENTS_FILE" || true
                else
                    pip install pytest || true
                fi
            fi

            if python -m pytest --version &> /dev/null; then
                touch "$STAMP_FILE" || true
                print_success "pytest available in venv"
                ENABLE_INTEROP=ON
            else
                print_warning "Unable to ensure pytest; interop tests will be skipped."
            fi
        fi
    elif [ -d "${SCRIPT_DIR}/interop" ]; then
        print_status "Skipping interop environment for fast test run"
    fi

    # Configure CMake with interop tests enabled only when pytest is available
    CMAKE_ARGS=(
        -DKNX_PLATFORM="$platform"
        -DCMAKE_BUILD_TYPE="$test_build_type"
        -DKNX_BUILD_EXAMPLES="$test_build_examples"
        -DBUILD_TESTS=ON
    )
    if [ "$ENABLE_INTEROP" = "ON" ]; then
        CMAKE_ARGS+=( -DENABLE_INTEROP_TESTS=ON )
    fi

    if is_truthy "$test_build_examples"; then
        print_status "Including SDK example-contract builds in test configuration"
    else
        print_status "SDK example-contract builds disabled by KNSTAX_TEST_BUILD_EXAMPLES"
    fi

    print_status "Configuring CMake (${ENABLE_INTEROP} interop)"
    cmake "${SCRIPT_DIR}" -B . "${CMAKE_ARGS[@]}"

    print_status "Building tests (${test_build_type})..."
    cmake --build . --config "$test_build_type" -- -j$(nproc)

    # If interop enabled, export env vars so CTest interop harness can find peers
    if [ "$ENABLE_INTEROP" = "ON" ]; then
        export KNSTAX_TUNNEL_PEER_BIN="${BUILD_DIR}/interop/bin/knstax_tunnel_peer"
        export KNSTAX_ROUTING_PEER_BIN="${BUILD_DIR}/interop/bin/knstax_routing_peer"
        print_status "Interop binaries: $KNSTAX_TUNNEL_PEER_BIN, $KNSTAX_ROUTING_PEER_BIN"
    fi

    CTEST_ARGS=(--output-on-failure --parallel "$ctest_parallel")
    if ! is_truthy "$include_long_tests"; then
        CTEST_ARGS+=( -LE "Long|benchmark" )
        print_status "Running fast test suite (excluding labels: Long, benchmark)..."
    else
        ctest_timeout=${KNSTAX_CTEST_TIMEOUT:-900}
        print_status "Running full test suite..."
    fi
    CTEST_ARGS+=( --timeout "$ctest_timeout" )
    if is_truthy "${KNSTAX_VERBOSE_TESTS:-OFF}"; then
        CTEST_ARGS+=( -V )
    fi
    print_status "CTest timeout per test: ${ctest_timeout}s"
    "${SCRIPT_DIR}/scripts/ci/ctest_with_backtraces.sh" --test-dir "$BUILD_DIR" "${CTEST_ARGS[@]}"

    echo ""
    print_success "Test run completed"
}

clean_build() {
    print_status "Cleaning build artifacts..."
    rm -rf "$BUILD_DIR"
    rm -rf "$INSTALL_PREFIX"
    print_success "Clean completed"
}

# ============================================================================
# Main
# ============================================================================

COMMAND="${1:-auto}"

case "$COMMAND" in
    help|-h|--help)
        print_help
        ;;
    
    clean)
        clean_build
        ;;
    
    test)
        PLATFORM="$(detect_platform)"
        INCLUDE_LONG_TESTS=OFF
        if [ -n "${2:-}" ]; then
            case "$2" in
                linux|macos|windows|esp32)
                    PLATFORM="$2"
                    ;;
                all|full|long)
                    INCLUDE_LONG_TESTS=ON
                    ;;
                *)
                    print_error "Unknown test option/platform: $2"
                    exit 1
                    ;;
            esac
        fi
        if [ -n "${3:-}" ]; then
            case "$3" in
                all|full|long)
                    INCLUDE_LONG_TESTS=ON
                    ;;
                *)
                    print_error "Unknown test option: $3"
                    exit 1
                    ;;
            esac
        fi
        if is_truthy "${KNSTAX_INCLUDE_LONG_TESTS:-OFF}"; then
            INCLUDE_LONG_TESTS=ON
        fi
        check_prerequisites "$PLATFORM"
        run_tests "$PLATFORM" "$INCLUDE_LONG_TESTS"
        ;;

    test-all)
        PLATFORM="${2:-$(detect_platform)}"
        check_prerequisites "$PLATFORM"
        run_tests "$PLATFORM" "ON"
        ;;
    
    linux|macos|windows|esp32)
        PLATFORM="$COMMAND"
        check_prerequisites "$PLATFORM"
        build_platform "$PLATFORM"
        ;;
    
    auto)
        PLATFORM=$(detect_platform)
        print_status "Auto-detected platform: ${BLUE}$PLATFORM${NC}"
        check_prerequisites "$PLATFORM"
        build_platform "$PLATFORM"
        ;;
    
    *)
        print_error "Unknown command: $COMMAND"
        print_help
        exit 1
        ;;
esac

print_status "Build directory: $BUILD_DIR"
print_status "Install prefix:  $INSTALL_PREFIX"
print_success "Done!"
