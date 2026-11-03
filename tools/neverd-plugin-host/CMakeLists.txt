cmake_minimum_required(VERSION 3.24)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(NeverDPluginCLI LANGUAGES CXX)
  include(CTest)
endif()
if(NOT TARGET neverd_plugin_protocol)
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../lib/plugin" plugin-host)
endif()
if(NOT TARGET neverd_tools)
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../lib/tools" tools-layer)
endif()
add_executable(neverd-plugin-host Main.cpp WorkerClient.cpp ToolsCommand.cpp CatalogCommand.cpp)
target_link_libraries(neverd-plugin-host PRIVATE neverd_tools)
if(MSVC)
  target_compile_options(neverd-plugin-host PRIVATE /utf-8)
endif()
install(TARGETS neverd-plugin-host RUNTIME DESTINATION bin)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../../LICENSES/nlohmann-json/LICENSE.MIT"
  DESTINATION share/neverd/licenses/nlohmann-json)
if(BUILD_TESTING OR NEVERD_BUILD_TESTS)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME NeverDPluginCLI
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/process_test.py"
        "$<TARGET_FILE:neverd-plugin-host>" "${CMAKE_CURRENT_SOURCE_DIR}/../../pluginsdk/gui")
    set_tests_properties(NeverDPluginCLI PROPERTIES TIMEOUT 60)
    add_test(NAME NeverDToolsConsumer
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/tools_consumer_test.py"
        "$<TARGET_FILE:neverd-plugin-host>")
    set_tests_properties(NeverDToolsConsumer PROPERTIES TIMEOUT 60)
    if(TARGET neverd-worker AND NEVERD_WORKER_REAL_ENGINE_TESTS)
      add_test(NAME NeverDPluginWorker
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/worker_test.py"
          "$<TARGET_FILE:neverd-plugin-host>" "$<TARGET_FILE:neverd-worker>"
          "$<TARGET_FILE:neverd_shared>")
      set_tests_properties(NeverDPluginWorker PROPERTIES TIMEOUT 60)
      add_test(NAME NeverDToolsWorker
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/tools_worker_test.py"
          "$<TARGET_FILE:neverd-plugin-host>" "$<TARGET_FILE:neverd-worker>"
          "$<TARGET_FILE:neverd_shared>")
      set_tests_properties(NeverDToolsWorker PROPERTIES TIMEOUT 90)
    endif()
  endif()
endif()
