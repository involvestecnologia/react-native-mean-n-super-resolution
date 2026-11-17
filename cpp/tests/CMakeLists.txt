cmake_minimum_required(VERSION 3.19)
project(mean_n_super_resolution_native_tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/OpenCVMobile.cmake)
opencv_mobile_find_package()

include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.18.0.tar.gz
  URL_HASH SHA256=6e3191c1455468b3fc35a417fb565c1c5071aee1b7e7f85e30cf48a98d37d8b5
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
# why: keeps gtest on the same MSVC runtime as the rest of the test binary.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()

set(SR_DIR ${CMAKE_CURRENT_LIST_DIR}/../sr)
add_library(sr_core STATIC
  ${SR_DIR}/Align.cpp
  ${SR_DIR}/Burst.cpp
  ${SR_DIR}/ErrorMapping.cpp
  ${SR_DIR}/Paths.cpp
  ${SR_DIR}/Pipeline.cpp
  ${SR_DIR}/Validate.cpp
  ${SR_DIR}/vendor/Ecc.cpp
  TempDirHost.cpp
)
target_include_directories(sr_core PUBLIC ${SR_DIR})
target_link_libraries(sr_core PUBLIC ${OpenCV_LIBS})

add_executable(sr_tests
  smoke_test.cpp
  paths_test.cpp
  validate_test.cpp
  fixtures.cpp
  pipeline_test.cpp
  error_mapping_test.cpp
  ecc_test.cpp
  burst_test.cpp
  warp_test.cpp
)
target_link_libraries(sr_tests PRIVATE sr_core GTest::gtest_main)

# invariant: parity harness only (scripts/parity, lab-parity-mean T11); not a ctest.
add_executable(sr_parity_cli parity_cli.cpp)
target_link_libraries(sr_parity_cli PRIVATE sr_core)

include(GoogleTest)
gtest_discover_tests(sr_tests)
