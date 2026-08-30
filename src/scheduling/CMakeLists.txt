add_library(scheduling INTERFACE)
target_include_directories(scheduling INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
# Switch to add_library(scheduling STATIC ...) once .cpp files exist.
