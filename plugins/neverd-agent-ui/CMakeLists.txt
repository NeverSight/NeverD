cmake_minimum_required(VERSION 3.24)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(NeverDAgentUi LANGUAGES CXX)
  find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets)
endif()

set(agent_dir "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/neverd-agent")
add_library(neverd-agent-ui MODULE AgentUiPlugin.cpp ChatTranscript.cpp
  "${agent_dir}/client/AgentProcess.cpp" "${agent_dir}/platform/AppPaths.cpp")
set_target_properties(neverd-agent-ui PROPERTIES AUTOMOC ON
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_features(neverd-agent-ui PRIVATE cxx_std_20)
target_include_directories(neverd-agent-ui PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../../pluginsdk/gui" "${agent_dir}")
target_link_libraries(neverd-agent-ui PRIVATE Qt6::Widgets)
if(APPLE)
  set_property(TARGET neverd-agent-ui PROPERTY INSTALL_RPATH
    "@executable_path/../Frameworks")
endif()
if(MSVC)
  target_compile_options(neverd-agent-ui PRIVATE /utf-8 /W4)
endif()
file(GENERATE OUTPUT "$<TARGET_FILE_DIR:neverd-agent-ui>/plugin.json"
  INPUT "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json.in")
set(agent_ui_package_dir plugins/neverd-agent-ui)
if(APPLE AND TARGET neverd-gui)
  set(agent_ui_package_dir PlugIns/neverd-agent-ui)
  set(agent_ui_install_dir
    "$<TARGET_BUNDLE_DIR_NAME:neverd-gui>/Contents/${agent_ui_package_dir}")
  set(agent_ui_package_dir "../${agent_ui_package_dir}")
else()
  set(agent_ui_install_dir "bin/${agent_ui_package_dir}")
endif()
# Path from the installed GUI executable's directory.
set_property(TARGET neverd-agent-ui PROPERTY NEVERD_PLUGIN_PACKAGE_DIR
  "${agent_ui_package_dir}")
install(TARGETS neverd-agent-ui
  LIBRARY DESTINATION "${agent_ui_install_dir}"
  RUNTIME DESTINATION "${agent_ui_install_dir}")
install(FILES "$<TARGET_FILE_DIR:neverd-agent-ui>/plugin.json"
  DESTINATION "${agent_ui_install_dir}")
