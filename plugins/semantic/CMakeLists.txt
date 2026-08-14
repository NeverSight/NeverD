# Proof-gated semantic optimization plugin example.

add_library(semantic_plugin SHARED semantic_plugin.c)

target_include_directories(semantic_plugin PRIVATE
  ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(semantic_plugin PRIVATE neverd_shared)

set_target_properties(semantic_plugin PROPERTIES
  PREFIX ""
  OUTPUT_NAME "semantic_plugin"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/plugins"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins"
  FOLDER "NeverD/Plugins"
)
