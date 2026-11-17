add_library(bytecode_plugin SHARED bytecode_plugin.c)
target_include_directories(bytecode_plugin PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(bytecode_plugin PRIVATE neverd_shared)
set_target_properties(bytecode_plugin PROPERTIES
  PREFIX ""
  FOLDER "NeverD/Plugins"
  C_STANDARD 11
  C_STANDARD_REQUIRED YES
  C_EXTENSIONS NO)
set_neverd_plugin_output_directories(bytecode_plugin)
