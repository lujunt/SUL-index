#!/usr/bin/env bash
# SUL-cipher-index dependency installer (Ubuntu 22.04)
#
# Purpose:
#   1) Install ophelib build dependencies (NTL, GMP, OpenMP, CMake, and others).
#   2) Clone and build ophelib, placing libophelib.a under third_party/ophelib/lib/.
#   3) Verify the installation and show the project build command.
#
# Usage:
#   chmod +x setup_cipher.sh
#   ./setup_cipher.sh
#
# Then build this project:
#   mkdir -p build && cd build
#   cmake .. && make -j

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OPHELIB_DIR="${SCRIPT_DIR}/third_party/ophelib"
OPHELIB_LIB="${OPHELIB_DIR}/lib/libophelib.a"
OPHELIB_INCLUDE="${OPHELIB_DIR}/include/ophelib"
OPHELIB_REPO="https://github.com/abb-iss/ophelib.git"
OPHELIB_SRC_DIR="${OPHELIB_DIR}/_src"

echo "=== Step 1: Install system dependencies (requires sudo) ==="
echo "  Packages: build-essential cmake m4 libtool-bin libgmp-dev libntl-dev"
sudo apt-get update
sudo apt-get install -y build-essential cmake m4 libtool-bin libgmp-dev libntl-dev git

echo
echo "=== Step 2: Verify ophelib headers ==="
if [[ ! -d "${OPHELIB_INCLUDE}" ]]; then
    echo "[ERROR] ${OPHELIB_INCLUDE}/ was not found"
    echo "        This repository vendors the ophelib headers; verify that third_party/ is complete"
    exit 1
fi
echo "  OK: ${OPHELIB_INCLUDE}/"

echo
echo "=== Step 3: Build the ophelib static library ==="
if [[ -f "${OPHELIB_LIB}" ]]; then
    echo "  Already present: ${OPHELIB_LIB}; skipping the build"
else
    echo "  Cloning and building ${OPHELIB_REPO} (first run)..."
    mkdir -p "${OPHELIB_DIR}/lib"
    if [[ ! -d "${OPHELIB_SRC_DIR}" ]]; then
        git clone --depth 1 "${OPHELIB_REPO}" "${OPHELIB_SRC_DIR}"
    fi
    mkdir -p "${OPHELIB_SRC_DIR}/build"
    (cd "${OPHELIB_SRC_DIR}/build" && cmake .. && make -j "$(nproc)")
    cp "${OPHELIB_SRC_DIR}/build/libophelib.a" "${OPHELIB_LIB}"
    echo "  OK: ${OPHELIB_LIB}"
fi

echo
echo "=== Step 4: Build the project (optional) ==="
read -r -p "Build sul_cipher_demo and sul_compare_demo now? [y/N] " ans
if [[ "${ans}" =~ ^[Yy]$ ]]; then
    cd "${SCRIPT_DIR}"
    mkdir -p build && cd build
    cmake ..
    make -j sul_cipher_demo sul_compare_demo
    echo
    echo "Example commands:"
    echo "  ./sul_cipher_demo  /path/to/uniform_*.csv [paillier_key=1024]"
    echo "  ./sul_compare_demo /path/to/uniform_*.csv [paillier_key=1024]"
fi

echo
echo "Done."
