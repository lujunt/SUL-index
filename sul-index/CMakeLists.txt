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
# Part 1: plaintext index (no external dependencies)
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

# Query-window generator: create five selectivity files from a dataset CSV.
add_executable(sul_query_gen main_query_gen.cpp)
target_link_libraries(sul_query_gen PRIVATE sul_index)

# Dataset splitter: divide a full CSV into training and insertion sets (90/10 by default).
# Supports stratified sampling over Z-order buckets through ZOrderEncoder.
add_executable(sul_split main_split.cpp)
target_link_libraries(sul_split PRIVATE sul_index)

# ============================================================================
# Part 2: encrypted index (requires ophelib, NTL, and GMP).
# Enabled by default; use -DBUILD_CIPHER=OFF when these dependencies are unavailable.
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

    # Plaintext-versus-encrypted comparison using identical queries.
    add_executable(sul_compare_demo main_compare.cpp)
    target_link_libraries(sul_compare_demo PRIVATE sul_cipher_index)

    # Serialization demo: build, save, load, and compare query results.
    add_executable(sul_serde_demo main_serde_demo.cpp)
    target_link_libraries(sul_serde_demo PRIVATE sul_cipher_index)

    # Workload demo: execute 2,000 operations at a selected read/write ratio.
    add_executable(sul_workload main_workload.cpp)
    target_link_libraries(sul_workload PRIVATE sul_cipher_index)

    # Update demo: insert a batch selected by ul% and compare post-update queries.
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
