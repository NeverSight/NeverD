# plugins/example/CMakeLists.txt
# Example NeverD plugins (demonstrate the plugin C API).

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

set_neverd_plugin_output_directories(example_plugin)

# External bytecode rules and proof-gated semantic optimization examples.
add_subdirectory(bytecode)
add_subdirectory(semantic)
