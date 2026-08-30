cmake_minimum_required(VERSION 3.15)
project(iov_edge_scheduling CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Each member's module builds as its own object/static library so work
# stays independent until integration.
add_subdirectory(src/scheduling)
add_subdirectory(src/offloading)
add_subdirectory(src/resource_mgmt)
add_subdirectory(simulation/sumo_integration)

# End-to-end demo: runs the full pipeline (scheduling -> offloading ->
# resource_mgmt) driven by the mock mobility feed. This is what to run
# live at the mid-eval.
add_executable(iov_demo main.cpp)
target_link_libraries(iov_demo scheduling offloading resource_mgmt sumo_integration)
target_include_directories(iov_demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
