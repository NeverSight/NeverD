# Proof-gated semantic optimization plugin example.

add_library(semantic_plugin SHARED semantic_plugin.c)

target_include_directories(semantic_plugin PRIVATE
  ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(semantic_plugin PRIVATE neverd_shared)

set_target_properties(semantic_plugin PROPERTIES
  PREFIX ""
  OUTPUT_NAME "semantic_plugin"
  FOLDER "NeverD/Plugins"
)

set_neverd_plugin_output_directories(semantic_plugin)
