cmake_minimum_required(VERSION 3.16)
project(sul_index CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

include_directories(${CMAKE_SOURCE_DIR}/include)

# ============================================================================
# 第一部分：明文索引（无外部依赖）
# ============================================================================
set(SUL_SOURCES
    src/z_order.cpp
    src/art_tree.cpp
    src/gpl_builder.cpp
    src/sul_index.cpp
    src/util/csv_loader.cpp
)
add_library(sul_index STATIC ${SUL_SOURCES})

add_executable(sul_demo main.cpp)
target_link_libraries(sul_demo PRIVATE sul_index)

# ============================================================================
# 第二部分：密文索引（依赖 ophelib + NTL + GMP）
# 开关：默认开启；未安装依赖时用 -DBUILD_CIPHER=OFF 跳过
# ============================================================================
option(BUILD_CIPHER "Build SUL-cipher-index (requires ophelib, NTL, GMP)" ON)

if(BUILD_CIPHER)
    set(OPHELIB_ROOT ${CMAKE_SOURCE_DIR}/third_party/ophelib)

    if(NOT EXISTS ${OPHELIB_ROOT}/lib/libophelib.a)
        message(WARNING
            "ophelib static lib not found at ${OPHELIB_ROOT}/lib/libophelib.a. "
            "Run setup_cipher.sh or set -DBUILD_CIPHER=OFF.")
    endif()

    set(SUL_CIPHER_SOURCES
        src/cipher/crypto_context.cpp
        src/cipher/enc_art_tree.cpp
        src/cipher/sul_cipher_index.cpp
        agreements/agreements/OSM.cpp
        agreements/agreements/SIC.cpp
        agreements/agreements/SPI.cpp
    )

    add_library(sul_cipher_index STATIC ${SUL_CIPHER_SOURCES})
    target_include_directories(sul_cipher_index
        PUBLIC ${OPHELIB_ROOT}/include
        PUBLIC ${CMAKE_SOURCE_DIR}/agreements/agreements)
    target_link_libraries(sul_cipher_index
        PUBLIC sul_index
        PUBLIC ${OPHELIB_ROOT}/lib/libophelib.a
        PUBLIC ntl gmpxx gmp gomp pthread)

    add_executable(sul_cipher_demo main_cipher.cpp)
    target_link_libraries(sul_cipher_demo PRIVATE sul_cipher_index)

    # 明文 vs 密文对比 demo（同一查询、双索引、逐项比对）
    add_executable(sul_compare_demo main_compare.cpp)
    target_link_libraries(sul_compare_demo PRIVATE sul_cipher_index)
endif()
