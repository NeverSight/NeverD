# plugins/example/CMakeLists.txt
# Example NeverD plugin (demonstrates the plugin C API).

add_library(example_plugin SHARED example_plugin.c)

target_include_directories(example_plugin PRIVATE
  ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(example_plugin PRIVATE neverd_shared)

set_target_properties(example_plugin PROPERTIES
  PREFIX ""
  OUTPUT_NAME "example_plugin"
  FOLDER "NeverD/Plugins"
)
