add_library(sumo_integration INTERFACE)
target_include_directories(sumo_integration INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
# Once sumo_bridge.cpp exists:
# find_package(LibSUMO REQUIRED)  # or point at $ENV{SUMO_HOME}
# target_link_libraries(sumo_integration INTERFACE libsumocpp)
