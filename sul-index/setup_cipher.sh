#!/usr/bin/env bash
# SUL-cipher-index 依赖安装脚本（Ubuntu 22.04）
#
# 用途：
#   1) 安装 ophelib 编译依赖（NTL、GMP、OpenMP、cmake 等）
#   2) 自动从 GitHub clone 并构建 ophelib，将 libophelib.a 放到 third_party/ophelib/lib/
#   3) 校验所有就位后给出构建命令
#
# 用法：
#   chmod +x setup_cipher.sh
#   ./setup_cipher.sh
#
# 之后构建本项目：
#   mkdir -p build && cd build
#   cmake .. && make -j

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OPHELIB_DIR="${SCRIPT_DIR}/third_party/ophelib"
OPHELIB_LIB="${OPHELIB_DIR}/lib/libophelib.a"
OPHELIB_INCLUDE="${OPHELIB_DIR}/include/ophelib"
OPHELIB_REPO="https://github.com/abb-iss/ophelib.git"
OPHELIB_SRC_DIR="${OPHELIB_DIR}/_src"

echo "=== Step 1: 安装系统依赖（需要 sudo） ==="
echo "  软件包：build-essential cmake m4 libtool-bin libgmp-dev libntl-dev"
sudo apt-get update
sudo apt-get install -y build-essential cmake m4 libtool-bin libgmp-dev libntl-dev git

echo
echo "=== Step 2: 校验 ophelib 头文件 ==="
if [[ ! -d "${OPHELIB_INCLUDE}" ]]; then
    echo "[ERROR] 未找到 ${OPHELIB_INCLUDE}/"
    echo "        本仓库已 vendored ophelib 头文件，请确认 third_party/ 目录完整"
    exit 1
fi
echo "  OK: ${OPHELIB_INCLUDE}/"

echo
echo "=== Step 3: 构建 ophelib 静态库 ==="
if [[ -f "${OPHELIB_LIB}" ]]; then
    echo "  已存在: ${OPHELIB_LIB}，跳过构建"
else
    echo "  从 ${OPHELIB_REPO} 克隆并构建（首次运行）..."
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
echo "=== Step 4: 构建（可选） ==="
read -r -p "现在构建 sul_cipher_demo / sul_compare_demo? [y/N] " ans
if [[ "${ans}" =~ ^[Yy]$ ]]; then
    cd "${SCRIPT_DIR}"
    mkdir -p build && cd build
    cmake ..
    make -j sul_cipher_demo sul_compare_demo
    echo
    echo "运行示例："
    echo "  ./sul_cipher_demo  /path/to/uniform_*.csv [paillier_key=1024]"
    echo "  ./sul_compare_demo /path/to/uniform_*.csv [paillier_key=1024]"
fi

echo
echo "完成。"
