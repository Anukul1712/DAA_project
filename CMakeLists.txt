cmake_minimum_required(VERSION 3.15)
project(iov_edge_scheduling CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_subdirectory(src/scheduling)
add_subdirectory(src/offloading)
add_subdirectory(src/resource_mgmt)
add_subdirectory(simulation/sumo_integration)

# The experiment driver.
add_executable(iov_sim main.cpp)
target_link_libraries(iov_sim PRIVATE scheduling offloading resource_mgmt sumo_integration)
target_include_directories(iov_sim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Unit tests (no external framework): `ctest` or run build/iov_tests directly.
enable_testing()
add_executable(iov_tests tests/run_tests.cpp)
target_link_libraries(iov_tests PRIVATE sumo_integration)
target_include_directories(iov_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME unit_tests COMMAND iov_tests)

# MinGW: link the C++ runtime statically so the .exe runs outside the MSYS2 shell.
if(MINGW)
    target_link_options(iov_sim PRIVATE -static)
    target_link_options(iov_tests PRIVATE -static)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(iov_sim PRIVATE -Wall -Wextra)
    target_compile_options(iov_tests PRIVATE -Wall -Wextra)
endif()
