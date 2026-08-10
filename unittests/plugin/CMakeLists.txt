add_library(neverd-native-plugin-fixture SHARED
  Inputs/native_fixture_plugin.c
)

add_library(neverd-no-export-plugin-fixture SHARED
  Inputs/no_export_plugin.c
)

add_library(neverd-null-name-plugin-fixture SHARED
  Inputs/null_name_plugin.c
)

foreach(_fixture_target IN ITEMS
    neverd-native-plugin-fixture
    neverd-no-export-plugin-fixture
    neverd-null-name-plugin-fixture)
  target_include_directories(${_fixture_target} PRIVATE
    ${CMAKE_SOURCE_DIR}/include
  )
  set_target_properties(${_fixture_target} PROPERTIES
    PREFIX ""
    FOLDER "NeverD/Tests/Fixtures"
  )
endforeach()

set_target_properties(neverd-native-plugin-fixture PROPERTIES
  OUTPUT_NAME "neverd_native_plugin_fixture"
)
set_target_properties(neverd-no-export-plugin-fixture PROPERTIES
  OUTPUT_NAME "neverd_no_export_plugin_fixture")
set_target_properties(neverd-null-name-plugin-fixture PROPERTIES
  OUTPUT_NAME "neverd_null_name_plugin_fixture")

set(_neverd_plugin_runtime_test_sources
  ${CMAKE_SOURCE_DIR}/lib/sdk/PluginManager.cpp)
set(_neverd_plugin_runtime_test_libs ${CMAKE_DL_LIBS})
if(NEVERD_PYTHON_PLUGIN_HOST_ENABLED)
  list(APPEND _neverd_plugin_runtime_test_sources
    ${CMAKE_SOURCE_DIR}/lib/sdk/PythonPluginRuntime.cpp)
  list(APPEND _neverd_plugin_runtime_test_libs Python3::Python)
endif()

add_library(neverd-plugin-runtime-test-support STATIC
  ${_neverd_plugin_runtime_test_sources})
target_include_directories(neverd-plugin-runtime-test-support PUBLIC
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/lib/sdk)
target_compile_features(neverd-plugin-runtime-test-support PUBLIC cxx_std_20)
target_link_libraries(neverd-plugin-runtime-test-support PUBLIC
  ${_neverd_plugin_runtime_test_libs})
set_target_properties(neverd-plugin-runtime-test-support PROPERTIES
  FOLDER "NeverD/Tests/Support")
if(NEVERD_PYTHON_PLUGIN_HOST_ENABLED)
  target_compile_definitions(neverd-plugin-runtime-test-support PRIVATE
    NEVERD_ENABLE_PYTHON_PLUGINS=1
    NEVERD_PYTHON_SDK_SOURCE_DIR="${CMAKE_SOURCE_DIR}/pluginsdk/python"
    NEVERD_PYTHON_SDK_VERSION="${PROJECT_VERSION}")
endif()

add_neverd_unittest(NeverDPluginRuntimeTests
  PluginRuntimeTests.cpp
  LINK_LIBS neverd-plugin-runtime-test-support
)
add_dependencies(NeverDPluginRuntimeTests
  neverd-native-plugin-fixture
  neverd-no-export-plugin-fixture
  neverd-null-name-plugin-fixture)
target_compile_definitions(NeverDPluginRuntimeTests PRIVATE
  NEVERD_NATIVE_PLUGIN_FIXTURE="$<TARGET_FILE:neverd-native-plugin-fixture>"
  NEVERD_NO_EXPORT_PLUGIN_FIXTURE="$<TARGET_FILE:neverd-no-export-plugin-fixture>"
  NEVERD_NULL_NAME_PLUGIN_FIXTURE="$<TARGET_FILE:neverd-null-name-plugin-fixture>"
  NEVERD_PYTHON_FILE_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/Inputs/RuntimeLifecyclePlugin.py"
  NEVERD_TEST_PYTHON_PLUGINS_ENABLED=$<BOOL:${NEVERD_PYTHON_PLUGIN_HOST_ENABLED}>)

if(NEVERD_BUILD_SHARED)
  add_neverd_unittest(NeverDPluginTests
    PluginManagerTests.cpp
    LINK_LIBS neverd_shared
  )

  add_dependencies(NeverDPluginTests neverd-native-plugin-fixture)
  target_compile_definitions(NeverDPluginTests PRIVATE
    NEVERD_NATIVE_PLUGIN_FIXTURE="$<TARGET_FILE:neverd-native-plugin-fixture>"
  )
endif()

if(NEVERD_PYTHON_PLUGIN_HOST_ENABLED)
  add_neverd_unittest(NeverDPythonPluginTests
    PythonPluginTests.cpp
    LINK_LIBS neverd_shared
  )

  target_compile_definitions(NeverDPythonPluginTests PRIVATE
    NEVERD_PYTHON_LIFECYCLE_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/Inputs/LifecyclePlugin.py"
    NEVERD_PYTHON_RAISING_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/Inputs/RaisingPlugin.py"
  )

  # A focused adapter test remains link-independent from the decompiler core.
  # This makes GIL/lifetime/traceback regressions cheap to isolate.
  add_neverd_unittest(NeverDPythonRuntimeTests
    PythonRuntimeTests.cpp
    LINK_LIBS neverd-plugin-runtime-test-support
  )
  target_compile_definitions(NeverDPythonRuntimeTests PRIVATE
    NEVERD_PYTHON_RUNTIME_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/Inputs/RuntimeLifecyclePlugin.py"
    NEVERD_PYTHON_RAISING_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/Inputs/RaisingPlugin.py"
    NEVERD_PYTHON_STALE_PROBE_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/Inputs/StaleSessionProbePlugin.py"
    NEVERD_PYTHON_RELOAD_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/Inputs/ReloadStatePlugin.py"
    NEVERD_NATIVE_PLUGIN_FIXTURE="$<TARGET_FILE:neverd-native-plugin-fixture>"
  )
  add_dependencies(NeverDPythonRuntimeTests neverd-native-plugin-fixture)
endif()
