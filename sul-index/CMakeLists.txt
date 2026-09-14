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
    src/util/query_loader.cpp
    src/util/experiment_recorder.cpp
)
add_library(sul_index STATIC ${SUL_SOURCES})

add_executable(sul_demo main.cpp)
target_link_libraries(sul_demo PRIVATE sul_index)

# 查询窗口生成器：从数据集 CSV 生成 5 个不同比例的查询文件
add_executable(sul_query_gen main_query_gen.cpp)
target_link_libraries(sul_query_gen PRIVATE sul_index)

# 数据集划分工具：把 full csv 切成 train_csv + insert_csv（90/10 默认），
# 支持基于 Z-order 桶的分层抽样（依赖 sul_index 的 ZOrderEncoder）
add_executable(sul_split main_split.cpp)
target_link_libraries(sul_split PRIVATE sul_index)

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
        src/cipher/sul_cipher_serde.cpp
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

    # 序列化 / 反序列化 demo：build → save → load → query 比对
    add_executable(sul_serde_demo main_serde_demo.cpp)
    target_link_libraries(sul_serde_demo PRIVATE sul_cipher_index)

    # 场景实验 demo：按读/写比执行 2000 操作
    add_executable(sul_workload main_workload.cpp)
    target_link_libraries(sul_workload PRIVATE sul_cipher_index)

    # 更新对比 demo：按 ul% 批量插入并对比更新后查询
    add_executable(sul_update main_update.cpp)
    target_link_libraries(sul_update PRIVATE sul_cipher_index)

endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(sul_retrain_unit tests/retrain_unit.cpp)
    target_link_libraries(sul_retrain_unit PRIVATE sul_index)
    add_test(NAME retrain_unit COMMAND sul_retrain_unit ${CMAKE_CURRENT_BINARY_DIR}/test_data/retrain_unit)
    if(BUILD_CIPHER)
        add_executable(sul_candidate_count tests/candidate_count.cpp)
        target_link_libraries(sul_candidate_count PRIVATE sul_cipher_index)
        add_test(NAME candidate_count COMMAND sul_candidate_count ${CMAKE_CURRENT_BINARY_DIR}/test_data/candidate_count.scidx)
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        add_test(NAME retrain_cipher COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_retrain.py $<TARGET_FILE:sul_update>)
        set_tests_properties(retrain_cipher PROPERTIES TIMEOUT 180)
        add_test(NAME retrain_ul_matrix COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ul_matrix.py
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_retrain_ul_matrix.py)
    endif()
endif()
