# Mobility layer: SUMO bridge (raw-socket TraCI client) + mock feed.
add_library(sumo_integration STATIC sumo_bridge.cpp)
target_include_directories(sumo_integration PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(WIN32)
    target_link_libraries(sumo_integration PUBLIC ws2_32)
endif()
