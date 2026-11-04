cmake_minimum_required(VERSION 3.24)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(NeverDPluginHost LANGUAGES CXX)
  include(CTest)
endif()

find_package(Threads REQUIRED)
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/NeverDJson.cmake")
add_library(neverd_plugin_host STATIC PluginHost.cpp Executor.cpp Tasks.cpp Process.cpp)
if(UNIX AND NOT APPLE)
  target_compile_definitions(neverd_plugin_host PRIVATE _GNU_SOURCE)
endif()
target_compile_features(neverd_plugin_host PUBLIC cxx_std_20)
target_include_directories(neverd_plugin_host PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/../../include")
target_link_libraries(neverd_plugin_host PUBLIC Threads::Threads)
set_target_properties(neverd_plugin_host PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(neverd_plugin_protocol STATIC Manifest.cpp Protocol.cpp Artifacts.cpp PluginCatalog.cpp
  Schema.cpp Actions.cpp ProcessActions.cpp Services.cpp Events.cpp HostServices.cpp)
target_link_libraries(neverd_plugin_protocol PUBLIC neverd_plugin_host nlohmann_json::nlohmann_json)
set_target_properties(neverd_plugin_protocol PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(WIN32)
  target_link_libraries(neverd_plugin_protocol PRIVATE bcrypt)
elseif(NOT APPLE)
  find_package(OpenSSL REQUIRED COMPONENTS Crypto)
  target_link_libraries(neverd_plugin_protocol PRIVATE OpenSSL::Crypto)
endif()

if(BUILD_TESTING OR NEVERD_BUILD_TESTS)
  add_executable(neverd-plugin-process-tests
    "${CMAKE_CURRENT_SOURCE_DIR}/../../unittests/plugin/ProcessTests.cpp")
  target_link_libraries(neverd-plugin-process-tests PRIVATE neverd_plugin_host)
  foreach(case streaming diagnostic_streaming lifetime diagnostic_lifetime
      backlog diagnostic_backlog discard)
    add_test(NAME NeverDPluginProcess.${case}
      COMMAND neverd-plugin-process-tests ${case})
    set_tests_properties(NeverDPluginProcess.${case} PROPERTIES TIMEOUT 30)
  endforeach()
  add_executable(neverd-plugin-service-tests
    "${CMAKE_CURRENT_SOURCE_DIR}/../../unittests/plugin/ServiceEventTests.cpp")
  target_link_libraries(neverd-plugin-service-tests PRIVATE neverd_plugin_protocol)
  foreach(case services contexts lifetime reentrant events replacement bridge)
    add_test(NAME NeverDPluginServices.${case} COMMAND neverd-plugin-service-tests ${case})
    set_tests_properties(NeverDPluginServices.${case} PROPERTIES TIMEOUT 15)
  endforeach()
  add_executable(neverd-plugin-catalog-tests
    "${CMAKE_CURRENT_SOURCE_DIR}/../../unittests/plugin/PluginCatalogTests.cpp")
  target_link_libraries(neverd-plugin-catalog-tests PRIVATE neverd_plugin_protocol)
  foreach(case intent repair generation records cleanup queue)
    add_test(NAME NeverDPluginCatalog.${case}
      COMMAND neverd-plugin-catalog-tests ${case})
    set_tests_properties(NeverDPluginCatalog.${case} PROPERTIES TIMEOUT 15)
  endforeach()
  add_executable(neverd-plugin-host-tests
    "${CMAKE_CURRENT_SOURCE_DIR}/../../unittests/plugin/PluginHostTests.cpp")
  target_link_libraries(neverd-plugin-host-tests PRIVATE neverd_plugin_protocol)
  foreach(case activation retirement replacement cleanup projects dispatch
      disposal_barrier late_cleanup task_completion task_retirement receipt_failure configuration)
    add_test(NAME NeverDPluginHost.${case}
      COMMAND neverd-plugin-host-tests ${case})
    set_tests_properties(NeverDPluginHost.${case} PROPERTIES TIMEOUT 30)
  endforeach()
endif()

if(BUILD_TESTING OR NEVERD_BUILD_TESTS)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME NeverDProcessPluginSDK
      COMMAND "${Python3_EXECUTABLE}" -m unittest discover
        -s "${CMAKE_CURRENT_SOURCE_DIR}/../../pluginsdk/gui/tests" -v)
    set_tests_properties(NeverDProcessPluginSDK PROPERTIES TIMEOUT 60)
  endif()
endif()

