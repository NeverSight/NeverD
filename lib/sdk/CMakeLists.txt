# libneverd shared library — core engine for CLI, GUI, and plugins.
# All NeverD frontends link this single shared library.
set(NEVERD_SDK_SOURCES
    capi/NeverDCAPI.cpp
    capi/NeverDCAPIBench.cpp
    capi/NeverDCAPIBenchDecode.cpp
    capi/NeverDCAPIDecompile.cpp
    capi/NeverDCAPIDisasm.cpp
    capi/NeverDCAPIGraph.cpp
    capi/NeverDCAPIInfo.cpp
    capi/NeverDCAPIOptimize.cpp
    capi/NeverDCAPIPatch.cpp
    capi/NeverDCAPIPersist.cpp
    capi/NeverDCAPIRoundtrip.cpp
    capi/NeverDCAPISafety.cpp
    capi/NeverDCAPISearch.cpp
    capi/NeverDCAPISigs.cpp
    capi/NeverDCAPISimplify.cpp
    capi/NeverDCAPISynth.cpp
    capi/NeverDCAPISymbolic.cpp
    capi/NeverDCAPITargetConfig.cpp
    capi/NeverDCAPITranslate.cpp
    plugin/PluginManager.cpp)

# The Python host translation units include <Python.h> unconditionally, so they
# only compile when the CPython development headers are available.  PluginManager
# guards its Python usage with NEVERD_ENABLE_PYTHON_PLUGINS and stays in the base
# set so the manager builds without a Python toolchain.
if(NEVERD_PYTHON_PLUGIN_HOST_ENABLED)
    list(APPEND NEVERD_SDK_SOURCES
        plugin/PythonPluginBridge.cpp
        plugin/PythonPluginError.cpp
        plugin/PythonPluginInterpreter.cpp
        plugin/PythonPluginMarshal.cpp
        plugin/PythonPluginRuntime.cpp)
endif()

add_library(neverd_shared SHARED ${NEVERD_SDK_SOURCES})

target_include_directories(neverd_shared
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    PUBLIC  ${CMAKE_SOURCE_DIR}/include
            ${LLVM_INCLUDE_DIRS}
            $<TARGET_PROPERTY:capstone_static,INTERFACE_INCLUDE_DIRECTORIES>)

target_compile_features(neverd_shared PUBLIC cxx_std_20)

# PRIVATE: all component libraries and LLVM are linked into the dylib but
# NOT propagated to consumers.  This prevents LLVM PassManager templates
# from being instantiated in consumer executables, which on macOS causes
# dyld weak_def coalescing to split AnalysisKey identity and crash the
# pass pipeline.
#
# Tools that need LLVM command-line infra (InitLLVM, cl::opt, WithColor)
# resolve those symbols from libneverd.dylib at link time.
target_link_libraries(neverd_shared PRIVATE
    NeverDPipeline
    NeverDBackendC
    NeverDBackendLLVM
    NeverDCodeGen
    NeverDDecode
    NeverDLift
    NeverDLoader
    NeverDSupport
    NeverDPassIR
    NeverDPassMIR
    NeverDLibC
    NeverDDebugInfo
    NeverDSolver
    NeverDSymbolic
    NeverDSafety
    NeverDTranslate
    NeverDSigs
    NeverDSBF
    ${CMAKE_DL_LIBS}
    ${NEVERD_COMMON_LIBS})

if(NEVERD_PYTHON_PLUGIN_HOST_ENABLED)
    target_link_libraries(neverd_shared PRIVATE Python3::Python)
    target_compile_definitions(neverd_shared PRIVATE
        NEVERD_ENABLE_PYTHON_PLUGINS=1
        NEVERD_PYTHON_SDK_SOURCE_DIR="${CMAKE_SOURCE_DIR}/pluginsdk/python"
        NEVERD_PYTHON_SDK_VERSION="${PROJECT_VERSION}")
endif()

# The aggregate IR archive and its subcomponent archives reference each other.
# GNU-style linkers scan a static archive only once unless the archives are in
# a rescan group, which otherwise leaves symbols such as NdType::str unresolved
# in libneverd.so.
if(UNIX AND NOT APPLE)
    get_property(_neverd_component_libs GLOBAL PROPERTY NEVERD_COMPONENT_LIBS)
    target_link_libraries(neverd_shared PRIVATE
        "-Wl,--start-group"
        ${_neverd_component_libs}
        "-Wl,--end-group")
endif()

# On macOS, shared-backed frontends resolve LLVM support APIs from libneverd.
# Static archives only contribute referenced objects, so force the complete
# support archive into the dylib (notably InitLLVM and the single command-line
# registry).
if(APPLE AND TARGET LLVMSupport)
    get_target_property(_neverd_llvm_support_type LLVMSupport TYPE)
    if(_neverd_llvm_support_type STREQUAL "STATIC_LIBRARY")
        target_link_options(neverd_shared PRIVATE
            "LINKER:-force_load,$<TARGET_FILE:LLVMSupport>")
    endif()
endif()

target_compile_definitions(neverd_shared PRIVATE NEVERD_EXPORTS)

set_target_properties(neverd_shared PROPERTIES
    OUTPUT_NAME "neverd"
    FOLDER "NeverD/Lib")

if(APPLE)
    set_target_properties(neverd_shared PROPERTIES
        INSTALL_RPATH "@loader_path")
elseif(UNIX)
    set_target_properties(neverd_shared PROPERTIES
        INSTALL_RPATH "$ORIGIN")
endif()

# Copy public SDK headers alongside the output binaries.  Keep the historical
# flat entry points, and also preserve the include hierarchy used by every
# header so consumers can compile with `-I<libneverd_dir>/sdk` without reaching
# back into the source tree, including with multi-configuration generators.
set(NEVERD_SDK_HEADERS
    ${CMAKE_SOURCE_DIR}/include/neverd/sdk/NeverDCAPI.h
    ${CMAKE_SOURCE_DIR}/include/neverd/sdk/NeverDCAPITypes.h
    ${CMAKE_SOURCE_DIR}/include/neverd/sdk/NeverDCAPISession.h
    ${CMAKE_SOURCE_DIR}/include/neverd/sdk/NeverDCAPIDisasm.h
    ${CMAKE_SOURCE_DIR}/include/neverd/sdk/NeverDCAPIQuery.h
    ${CMAKE_SOURCE_DIR}/include/neverd/sdk/NeverDCAPISymbolic.h
    ${CMAKE_SOURCE_DIR}/include/neverd/sdk/NeverDCAPISafety.h
    ${CMAKE_SOURCE_DIR}/include/neverd/sdk/NeverDCAPIPatch.h
    ${CMAKE_SOURCE_DIR}/include/neverd/sdk/NeverDCAPIPersist.h
    ${CMAKE_SOURCE_DIR}/include/neverd/sdk/NeverDCAPISigs.h
    ${CMAKE_SOURCE_DIR}/include/neverd/sdk/NeverDCAPIPlugin.h
    ${CMAKE_SOURCE_DIR}/include/neverd/sdk/NeverDCAPIOptimize.h
    ${CMAKE_SOURCE_DIR}/include/neverd/sdk/NeverDCAPISimplify.h
    ${CMAKE_SOURCE_DIR}/include/neverd/sdk/NeverDCAPISynth.h
    ${CMAKE_SOURCE_DIR}/include/neverd/sdk/NeverDCAPITranslate.h
    ${CMAKE_SOURCE_DIR}/include/neverd/sdk/NeverDPlugin.h)

set(NEVERD_STAGED_SDK_DIRECTORY
    "$<TARGET_FILE_DIR:neverd_shared>/sdk")

add_custom_command(TARGET neverd_shared POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${NEVERD_STAGED_SDK_DIRECTORY}"
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${NEVERD_STAGED_SDK_DIRECTORY}/neverd/sdk"
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${NEVERD_STAGED_SDK_DIRECTORY}/neverd"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${NEVERD_SDK_HEADERS}
        "${NEVERD_STAGED_SDK_DIRECTORY}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${NEVERD_SDK_HEADERS}
        "${NEVERD_STAGED_SDK_DIRECTORY}/neverd/sdk"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/include/neverd/OutputLanguages.def"
        "${NEVERD_STAGED_SDK_DIRECTORY}/neverd"
    COMMENT "Copying SDK headers next to libneverd")

if(NEVERD_PYTHON_PLUGIN_HOST_ENABLED)
  set(NEVERD_PYTHON_SDK_ROOT_FILES
    "${CMAKE_SOURCE_DIR}/pluginsdk/python/LICENSE"
    "${CMAKE_SOURCE_DIR}/pluginsdk/python/README.md"
    "${CMAKE_SOURCE_DIR}/pluginsdk/python/pyproject.toml")
  set(NEVERD_PYTHON_SDK_PACKAGE_FILES
    "${CMAKE_SOURCE_DIR}/pluginsdk/python/neverd_plugin/__init__.py"
    "${CMAKE_SOURCE_DIR}/pluginsdk/python/neverd_plugin/_version.py"
    "${CMAKE_SOURCE_DIR}/pluginsdk/python/neverd_plugin/abi.py"
    "${CMAKE_SOURCE_DIR}/pluginsdk/python/neverd_plugin/api.py"
    "${CMAKE_SOURCE_DIR}/pluginsdk/python/neverd_plugin/ffi.py"
    "${CMAKE_SOURCE_DIR}/pluginsdk/python/neverd_plugin/py.typed")
  set(NEVERD_PYTHON_SDK_EXAMPLE_FILES
    "${CMAKE_SOURCE_DIR}/pluginsdk/python/examples/analysis_report.py"
    "${CMAKE_SOURCE_DIR}/pluginsdk/python/examples/minimal.py"
    "${CMAKE_SOURCE_DIR}/pluginsdk/python/examples/semantic_optimizer.py")
  add_custom_command(TARGET neverd_shared POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${NEVERD_STAGED_SDK_DIRECTORY}/python"
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${NEVERD_STAGED_SDK_DIRECTORY}/python/neverd_plugin"
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${NEVERD_STAGED_SDK_DIRECTORY}/python/examples"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${NEVERD_PYTHON_SDK_ROOT_FILES}
        "${NEVERD_STAGED_SDK_DIRECTORY}/python"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${NEVERD_PYTHON_SDK_PACKAGE_FILES}
        "${NEVERD_STAGED_SDK_DIRECTORY}/python/neverd_plugin"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${NEVERD_PYTHON_SDK_EXAMPLE_FILES}
        "${NEVERD_STAGED_SDK_DIRECTORY}/python/examples"
    COMMENT "Copying Python plugin SDK next to libneverd")
endif()
