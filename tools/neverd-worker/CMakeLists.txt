cmake_minimum_required(VERSION 3.24)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(NeverDWorker LANGUAGES C CXX)
  include(CTest)
endif()

if(NOT TARGET neverd_tool_web)
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../common" common)
endif()
find_package(Threads REQUIRED)

if(NOT TARGET neverd_shared)
  set(_neverd_worker_imported_engine TRUE)
  set(NEVERD_ENGINE_LIBRARY "" CACHE FILEPATH "Matching NeverD shared C ABI library")
  if(NOT EXISTS "${NEVERD_ENGINE_LIBRARY}")
    message(FATAL_ERROR "Set NEVERD_ENGINE_LIBRARY to a matching libneverd shared library")
  endif()
  add_library(neverd_shared SHARED IMPORTED GLOBAL)
  set_target_properties(neverd_shared PROPERTIES IMPORTED_LOCATION "${NEVERD_ENGINE_LIBRARY}")
  if(WIN32)
    set(NEVERD_ENGINE_IMPLIB "" CACHE FILEPATH "NeverD import library")
    set_target_properties(neverd_shared PROPERTIES IMPORTED_IMPLIB "${NEVERD_ENGINE_IMPLIB}")
  endif()
endif()

add_library(neverd_worker_protocol STATIC Protocol.cpp)
target_compile_features(neverd_worker_protocol PUBLIC cxx_std_20)
target_link_libraries(neverd_worker_protocol PUBLIC neverd_tool_json)
target_include_directories(neverd_worker_protocol PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../include")
add_library(neverd_worker_graph STATIC GraphSnapshot.cpp GraphLayout.cpp)
add_library(neverd_worker_format STATIC OperandFormat.cpp OperandFormatARM.cpp
  OperandFormatSBF.cpp OperandFormatEVM.cpp CodeEdits.cpp)
target_link_libraries(neverd_worker_format PUBLIC neverd_worker_protocol)
# Header-only tables shared with the engine, such as the wide characters.
target_include_directories(neverd_worker_format PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../../include")
target_link_libraries(neverd_worker_graph PUBLIC neverd_worker_protocol)
# The engine the worker serves, which neverd-mcp also runs in its own process.
add_library(neverd_worker_engine STATIC Engine.cpp EnginePatch.cpp ComparisonService.cpp LineageService.cpp Listing.cpp BrowseCache.cpp StringReferences.cpp TextFold.cpp ProjectHistory.cpp Contributions.cpp)
target_compile_features(neverd_worker_engine PUBLIC cxx_std_20)
target_include_directories(neverd_worker_engine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../../include")
# The C ABI is the only engine dependency. Never add LLVM to this target.
target_link_libraries(neverd_worker_engine PUBLIC neverd_worker_graph neverd_worker_format neverd_tool_web neverd_shared Threads::Threads ${CMAKE_DL_LIBS})
add_executable(neverd-worker main.cpp)
target_link_libraries(neverd-worker PRIVATE neverd_worker_engine)
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/NeverDWorkerAllocator.cmake")
neverd_worker_allocator(neverd-worker)
set_target_properties(neverd-worker PROPERTIES
  INSTALL_RPATH "$ORIGIN/../lib;@loader_path/../lib")
install(TARGETS neverd-worker RUNTIME DESTINATION bin)
neverd_stage_json_notice(neverd-worker)

if(BUILD_TESTING OR NEVERD_BUILD_TESTS)
  get_target_property(_neverd_worker_allocator neverd-worker NEVERD_WORKER_ALLOCATOR)
  if(_neverd_worker_allocator STREQUAL "mimalloc")
    add_executable(neverd-worker-allocator-tests tests/allocator_tests.cpp)
    target_compile_features(neverd-worker-allocator-tests PRIVATE cxx_std_20)
    target_include_directories(neverd-worker-allocator-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../include")
    neverd_link_mimalloc(neverd-worker-allocator-tests)
    target_link_libraries(neverd-worker-allocator-tests PRIVATE neverd_shared Threads::Threads ${CMAKE_DL_LIBS})
    add_test(NAME NeverDWorkerAllocator COMMAND neverd-worker-allocator-tests)
  endif()
  add_executable(neverd-worker-protocol-tests tests/protocol_tests.cpp)
  target_link_libraries(neverd-worker-protocol-tests PRIVATE neverd_worker_protocol)
  add_test(NAME NeverDWorkerProtocol COMMAND neverd-worker-protocol-tests)
  if(NEVERD_ENABLE_SIMDJSON)
    add_executable(neverd-worker-protocol-fallback-tests tests/protocol_tests.cpp
      ../common/transport/Json.cpp)
    target_compile_definitions(neverd-worker-protocol-fallback-tests PRIVATE NEVERD_USE_SIMDJSON=0)
    target_link_libraries(neverd-worker-protocol-fallback-tests PRIVATE neverd_worker_protocol)
    add_test(NAME NeverDWorkerProtocolFallback COMMAND neverd-worker-protocol-fallback-tests)
  endif()
  add_executable(neverd-worker-format-tests tests/format_tests.cpp)
  target_link_libraries(neverd-worker-format-tests PRIVATE neverd_worker_format)
  add_test(NAME NeverDWorkerFormat COMMAND neverd-worker-format-tests)
  add_executable(neverd-worker-code-edits-tests tests/code_edits_tests.cpp)
  target_link_libraries(neverd-worker-code-edits-tests PRIVATE neverd_worker_format)
  add_test(NAME NeverDWorkerCodeEdits COMMAND neverd-worker-code-edits-tests)
  add_executable(neverd-worker-history-tests tests/history_tests.cpp ProjectHistory.cpp)
  target_link_libraries(neverd-worker-history-tests PRIVATE neverd_worker_format)
  add_test(NAME NeverDWorkerHistory COMMAND neverd-worker-history-tests)
  add_executable(neverd-worker-graph-tests tests/graph_tests.cpp)
  target_link_libraries(neverd-worker-graph-tests PRIVATE neverd_worker_graph)
  add_test(NAME NeverDWorkerGraph COMMAND neverd-worker-graph-tests)
  if(_neverd_worker_imported_engine)
    option(NEVERD_WORKER_WEB_ENGINE_TESTS "Test an imported engine with the web C API" OFF)
  else()
    option(NEVERD_WORKER_WEB_ENGINE_TESTS "Test the web C API worker adapter" ON)
  endif()
  if(NEVERD_WORKER_WEB_ENGINE_TESTS)
    add_executable(neverd-worker-web-tests tests/web_engine_tests.cpp)
    target_include_directories(neverd-worker-web-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../include")
    target_link_libraries(neverd-worker-web-tests PRIVATE neverd_worker_protocol neverd_tool_web neverd_shared ${CMAKE_DL_LIBS})
    add_dependencies(neverd-worker-web-tests neverd-worker)
    add_test(NAME NeverDWorkerWeb COMMAND neverd-worker-web-tests "$<TARGET_FILE:neverd-worker>")
    set_tests_properties(NeverDWorkerWeb PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
  endif()
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_executable(neverd-worker-test-server main.cpp Engine.cpp EnginePatch.cpp ComparisonService.cpp LineageService.cpp Listing.cpp BrowseCache.cpp StringReferences.cpp TextFold.cpp ProjectHistory.cpp Contributions.cpp tests/mock_engine.cpp)
    set_target_properties(neverd-worker-test-server PROPERTIES ENABLE_EXPORTS ON)
    add_test(NAME NeverDWorkerPatch
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/byte_patch_test.py"
              "$<TARGET_FILE:neverd-worker-test-server>")
    set_tests_properties(NeverDWorkerPatch PROPERTIES TIMEOUT 60)
    target_compile_features(neverd-worker-test-server PRIVATE cxx_std_20)
    target_compile_definitions(neverd-worker-test-server PRIVATE NEVERD_EXPORTS)
    target_include_directories(neverd-worker-test-server PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../include")
    target_link_libraries(neverd-worker-test-server PRIVATE neverd_worker_graph neverd_worker_format neverd_tool_web Threads::Threads ${CMAKE_DL_LIBS})
    add_test(NAME NeverDWorkerTransport
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/transport_test.py"
                "$<TARGET_FILE:neverd-worker-test-server>")
    add_test(NAME NeverDWorkerUnwindQuery
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/unwind_query_test.py"
              "$<TARGET_FILE:neverd-worker-test-server>")
    add_test(NAME NeverDWorkerAnalysisSnapshot
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/analysis_snapshot_test.py"
              "$<TARGET_FILE:neverd-worker-test-server>")
    set_tests_properties(NeverDWorkerAnalysisSnapshot PROPERTIES TIMEOUT 30)
    if(APPLE)
      # The worker records canonical paths. The fixture compares that path to
      # its temporary input; avoid macOS's /var and /tmp symlink aliases.
      file(REAL_PATH "/tmp" _worker_canonical_tmp)
      set_tests_properties(NeverDWorkerAnalysisSnapshot PROPERTIES
        ENVIRONMENT "TMPDIR=${_worker_canonical_tmp}")
    endif()
    set_tests_properties(NeverDWorkerTransport PROPERTIES TIMEOUT 40)
    # This query uses the same protocol/lifecycle harness and allowance.
    get_property(_neverd_worker_transport_timeout TEST NeverDWorkerTransport
      PROPERTY TIMEOUT)
    set_tests_properties(NeverDWorkerUnwindQuery PROPERTIES
      TIMEOUT "${_neverd_worker_transport_timeout}")
    add_executable(neverd-worker-discovery-test-server main.cpp Engine.cpp EnginePatch.cpp ComparisonService.cpp LineageService.cpp Listing.cpp BrowseCache.cpp StringReferences.cpp TextFold.cpp ProjectHistory.cpp Contributions.cpp tests/mock_engine.cpp)
    set_target_properties(neverd-worker-discovery-test-server PROPERTIES ENABLE_EXPORTS ON)
    target_compile_features(neverd-worker-discovery-test-server PRIVATE cxx_std_20)
    target_compile_definitions(neverd-worker-discovery-test-server PRIVATE NEVERD_EXPORTS NEVERD_TEST_STEPPED_DISCOVERY)
    target_include_directories(neverd-worker-discovery-test-server PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../include")
    target_link_libraries(neverd-worker-discovery-test-server PRIVATE neverd_worker_graph neverd_worker_format neverd_tool_web Threads::Threads ${CMAKE_DL_LIBS})
    add_test(NAME NeverDWorkerDiscoveryReady
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/discovery_ready_test.py"
              "$<TARGET_FILE:neverd-worker-discovery-test-server>")
    add_test(NAME NeverDWorkerDiscoveryReadyLegacy
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/discovery_ready_test.py"
              "$<TARGET_FILE:neverd-worker-test-server>")
    set_tests_properties(NeverDWorkerDiscoveryReady NeverDWorkerDiscoveryReadyLegacy PROPERTIES TIMEOUT 30)
    add_test(NAME NeverDWorkerSourceCache
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/source_cache_test.py"
              "$<TARGET_FILE:neverd-worker-test-server>")
    set_tests_properties(NeverDWorkerSourceCache PROPERTIES TIMEOUT 40)
    add_test(NAME NeverDWorkerInterpreterRecovery
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/interpreter_recovery_test.py"
              "$<TARGET_FILE:neverd-worker-test-server>")
    set_tests_properties(NeverDWorkerInterpreterRecovery PROPERTIES TIMEOUT 40)
    add_test(NAME NeverDWorkerLocalNameCache
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/local_name_cache_test.py"
              "$<TARGET_FILE:neverd-worker-test-server>")
    set_tests_properties(NeverDWorkerLocalNameCache PROPERTIES TIMEOUT 40)
    add_test(NAME NeverDWorkerWarmFunctions
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/warm_functions_test.py"
              "$<TARGET_FILE:neverd-worker-test-server>")
    set_tests_properties(NeverDWorkerWarmFunctions PROPERTIES TIMEOUT 60)
    add_test(NAME NeverDWorkerMakeCode
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/make_code_test.py"
              "$<TARGET_FILE:neverd-worker-test-server>")
    set_tests_properties(NeverDWorkerMakeCode PROPERTIES TIMEOUT 40)
    add_test(NAME NeverDWorkerGraphTransport
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/graph_transport_test.py"
              "$<TARGET_FILE:neverd-worker-test-server>")
    set_tests_properties(NeverDWorkerGraphTransport PROPERTIES TIMEOUT 40)
    add_test(NAME NeverDWorkerHistoryContributions
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/history_contributions_test.py"
              "$<TARGET_FILE:neverd-worker-test-server>")
    set_tests_properties(NeverDWorkerHistoryContributions PROPERTIES TIMEOUT 40)
    add_test(NAME NeverDWorkerFunctionalRegressions
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/functional_regressions_test.py"
              "$<TARGET_FILE:neverd-worker-test-server>")
    set_tests_properties(NeverDWorkerFunctionalRegressions PROPERTIES TIMEOUT 60)
    option(NEVERD_WORKER_REAL_ENGINE_TESTS "Run smoke tests against the real engine" ON)
    if(NEVERD_WORKER_REAL_ENGINE_TESTS)
      if(_neverd_worker_allocator STREQUAL "mimalloc")
        add_test(NAME NeverDWorkerIdleMemory
          COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/idle_memory_test.py"
                  "$<TARGET_FILE:neverd-worker>")
        set_tests_properties(NeverDWorkerIdleMemory PROPERTIES TIMEOUT 60)
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
          add_test(NAME NeverDWorkerTHPPolicy
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/thp_policy_test.py"
                    "$<TARGET_FILE:neverd-worker>" "$<TARGET_FILE:neverd_shared>")
          set_tests_properties(NeverDWorkerTHPPolicy PROPERTIES
            TIMEOUT 60 SKIP_RETURN_CODE 77 LABELS "NeverDAllocatorTests")
        endif()
      endif()
      # Generated C uses Clang C extensions, including scalar bit casts.
      # Match the native unit fixtures' frontend when the SDK is built by cl.
      # GCC's C front end lacks them too; it remains the fallback only when
      # no Clang is installed beside it, and tests needing Clang then skip.
      set(_neverd_worker_source_cc "${CMAKE_C_COMPILER}")
      if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        find_program(_neverd_worker_source_clang NAMES clang
          HINTS "${LLVM_TOOLS_BINARY_DIR}" REQUIRED)
        set(_neverd_worker_source_cc "${_neverd_worker_source_clang}")
      elseif(CMAKE_C_COMPILER_ID STREQUAL "GNU")
        find_program(_neverd_worker_source_clang NAMES clang
          HINTS "${LLVM_TOOLS_BINARY_DIR}")
        if(_neverd_worker_source_clang)
          set(_neverd_worker_source_cc "${_neverd_worker_source_clang}")
        endif()
      endif()
      add_test(NAME NeverDWorkerNativePatch
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/byte_patch_test.py"
                "$<TARGET_FILE:neverd-worker>" --real)
      set_tests_properties(NeverDWorkerNativePatch PROPERTIES TIMEOUT 90)
      add_test(NAME NeverDWorkerSourceBytePatch
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/source_byte_patch_test.py"
                "$<TARGET_FILE:neverd-worker>" "${_neverd_worker_source_cc}")
      set_tests_properties(NeverDWorkerSourceBytePatch PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77)
      add_test(NAME NeverDWorkerNativeMakeCode
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/make_code_test.py"
                "$<TARGET_FILE:neverd-worker>" --real)
      set_tests_properties(NeverDWorkerNativeMakeCode PROPERTIES TIMEOUT 40)
      add_test(NAME NeverDWorkerConstantBranch
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/constant_branch_test.py"
                "$<TARGET_FILE:neverd-worker>")
      set_tests_properties(NeverDWorkerConstantBranch PROPERTIES TIMEOUT 40)
      add_test(NAME NeverDWorkerScalarBranchValues
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/scalar_branch_value_test.py"
                "$<TARGET_FILE:neverd-worker>" "${_neverd_worker_source_cc}")
      set_tests_properties(NeverDWorkerScalarBranchValues PROPERTIES
        TIMEOUT 120 SKIP_RETURN_CODE 77)
      add_test(NAME NeverDWorkerRealEngine
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/real_engine_test.py"
                "$<TARGET_FILE:neverd-worker>")
      set_tests_properties(NeverDWorkerRealEngine PROPERTIES TIMEOUT 60)
      set(_neverd_x87_cli_args)
      if(TARGET neverd)
        list(APPEND _neverd_x87_cli_args --cli "$<TARGET_FILE:neverd>")
      endif()
      add_test(NAME NeverDWorkerX87Capabilities
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/x87_capabilities_test.py"
                "$<TARGET_FILE:neverd-worker>" "$<TARGET_FILE:neverd_shared>"
                --legacy-worker "$<TARGET_FILE:neverd-worker-test-server>"
                ${_neverd_x87_cli_args})
      set_tests_properties(NeverDWorkerX87Capabilities PROPERTIES TIMEOUT 60)
      add_test(NAME NeverDWorkerUnicodePaths
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/unicode_path_test.py"
                "$<TARGET_FILE:neverd-worker>")
      set_tests_properties(NeverDWorkerUnicodePaths PROPERTIES TIMEOUT 90)
      add_test(NAME NeverDWorkerPEBrowse
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/pe_browse_test.py"
                "$<TARGET_FILE:neverd-worker>")
      set_tests_properties(NeverDWorkerPEBrowse PROPERTIES TIMEOUT 60)
      add_test(NAME NeverDWorkerBrowseCache
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/browse_cache_test.py"
                "$<TARGET_FILE:neverd-worker>")
      set_tests_properties(NeverDWorkerBrowseCache PROPERTIES TIMEOUT 120)
      add_test(NAME NeverDWorkerPEListingFacts
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/pe_listing_facts_test.py"
                "$<TARGET_FILE:neverd-worker>")
      set_tests_properties(NeverDWorkerPEListingFacts PROPERTIES TIMEOUT 120)
      add_test(NAME NeverDMetadataVisitor
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/metadata_visitor_test.py"
                "$<TARGET_FILE:neverd_shared>")
      set_tests_properties(NeverDMetadataVisitor PROPERTIES TIMEOUT 60)
      add_test(NAME NeverDWorkerPDBLoad
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/pdb_load_test.py"
                "$<TARGET_FILE:neverd-worker>")
      set_tests_properties(NeverDWorkerPDBLoad PROPERTIES TIMEOUT 90)
      add_test(NAME NeverDWorkerAnalysisSections
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/analysis_sections_test.py"
                "$<TARGET_FILE:neverd-worker>" "$<TARGET_FILE:neverd_shared>")
      set_tests_properties(NeverDWorkerAnalysisSections PROPERTIES TIMEOUT 90)
      add_test(NAME NeverDWorkerLocalTypes
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/local_types_test.py"
                "$<TARGET_FILE:neverd-worker>")
      set_tests_properties(NeverDWorkerLocalTypes PROPERTIES TIMEOUT 90)
      add_test(NAME NeverDWorkerCachedCallABI
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/cached_call_abi_test.py"
                "$<TARGET_FILE:neverd-worker>" --cc "${_neverd_worker_source_cc}")
      set_tests_properties(NeverDWorkerCachedCallABI PROPERTIES TIMEOUT 90)
      add_test(NAME NeverDWorkerParameterTypePropagation
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/parameter_type_propagation_test.py"
                "$<TARGET_FILE:neverd-worker>")
      set_tests_properties(NeverDWorkerParameterTypePropagation PROPERTIES TIMEOUT 90)
      add_test(NAME NeverDWorkerSourceVariableTypes
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/source_variable_types_test.py"
                "$<TARGET_FILE:neverd-worker>" --cc "${_neverd_worker_source_cc}")
      set_tests_properties(NeverDWorkerSourceVariableTypes PROPERTIES TIMEOUT 120)
      add_test(NAME NeverDWorkerSourceVariableEdits
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/source_variable_edits_test.py" $<TARGET_FILE:neverd-worker> --cc "${_neverd_worker_source_cc}")
      set_tests_properties(NeverDWorkerSourceVariableEdits PROPERTIES TIMEOUT 120)
      add_test(NAME NeverDWorkerDataPresentation
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/data_presentation_test.py" $<TARGET_FILE:neverd-worker>)
        set_tests_properties(NeverDWorkerDataPresentation PROPERTIES TIMEOUT 90)
      add_test(NAME NeverDWorkerStackFrame
          COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/stack_frame_test.py" $<TARGET_FILE:neverd-worker> --cc "${_neverd_worker_source_cc}")
      set_tests_properties(NeverDWorkerStackFrame PROPERTIES TIMEOUT 120)
      add_test(NAME NeverDWorkerStackText
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/stack_text_test.py"
                "$<TARGET_FILE:neverd-worker>" --cc "${_neverd_worker_source_cc}")
      set_tests_properties(NeverDWorkerStackText PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
      add_test(NAME NeverDWorkerHeaderRecords
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/header_records_test.py"
                "$<TARGET_FILE:neverd-worker>" --cc "${CMAKE_C_COMPILER}")
      set_tests_properties(NeverDWorkerHeaderRecords PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77)
      add_test(NAME NeverDWorkerProgramMain
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/program_main_test.py"
                "$<TARGET_FILE:neverd-worker>" --cc "${CMAKE_C_COMPILER}")
      set_tests_properties(NeverDWorkerProgramMain PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77)
      add_test(NAME NeverDWorkerCallSiteTypes
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/call_site_types_test.py" $<TARGET_FILE:neverd-worker> --cc "${_neverd_worker_source_cc}")
      set_tests_properties(NeverDWorkerCallSiteTypes PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77)
      add_test(NAME NeverDWorkerDwarfReference
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/dwarf_reference_test.py"
                "$<TARGET_FILE:neverd-worker>")
      set_tests_properties(NeverDWorkerDwarfReference PROPERTIES TIMEOUT 90)
      add_test(NAME NeverDWorkerMapReference
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/map_reference_test.py"
                "$<TARGET_FILE:neverd-worker>")
      set_tests_properties(NeverDWorkerMapReference PROPERTIES TIMEOUT 90)
      add_test(NAME NeverDWorkerSymbolTableReference
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/symbol_table_reference_test.py"
                "$<TARGET_FILE:neverd-worker>")
      set_tests_properties(NeverDWorkerSymbolTableReference PROPERTIES TIMEOUT 90)
      add_test(NAME NeverDWorkerPEFunctionParts
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/pe_function_parts_test.py"
                "$<TARGET_FILE:neverd-worker>")
      set_tests_properties(NeverDWorkerPEFunctionParts PROPERTIES TIMEOUT 60)
      add_test(NAME NeverDWorkerListingStability
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/listing_stability_test.py"
                "$<TARGET_FILE:neverd-worker>")
      set_tests_properties(NeverDWorkerListingStability PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
      add_test(NAME NeverDWorkerNativeMapping
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_mapping_test.py"
                "$<TARGET_FILE:neverd-worker>")
      set_tests_properties(NeverDWorkerNativeMapping PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
      add_test(NAME NeverDWorkerImportNames
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/import_names_test.py"
                "$<TARGET_FILE:neverd-worker>")
      set_tests_properties(NeverDWorkerImportNames PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
      add_test(NAME NeverDWorkerListingData
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/listing_data_test.py"
                "$<TARGET_FILE:neverd-worker>")
      set_tests_properties(NeverDWorkerListingData PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
      add_test(NAME NeverDWorkerNamesRebuild
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/names_rebuild_test.py"
                "$<TARGET_FILE:neverd-worker>")
      set_tests_properties(NeverDWorkerNamesRebuild PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
      add_test(NAME NeverDWorkerBinaryFile
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/binary_file_test.py"
                "$<TARGET_FILE:neverd-worker>")
      set_tests_properties(NeverDWorkerBinaryFile PROPERTIES TIMEOUT 60)
      add_test(NAME NeverDWorkerBinaryPlatform
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/binary_platform_test.py"
                "$<TARGET_FILE:neverd-worker>")
      set_tests_properties(NeverDWorkerBinaryPlatform PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
      add_test(NAME NeverDWorkerBinaryISA
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/binary_isa_test.py"
                "$<TARGET_FILE:neverd-worker>")
      set_tests_properties(NeverDWorkerBinaryISA PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
      add_test(NAME NeverDWorkerFunctionEdits
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/function_edits_test.py"
                "$<TARGET_FILE:neverd-worker>")
      set_tests_properties(NeverDWorkerFunctionEdits PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
      add_test(NAME NeverDWorkerGitCollaboration
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/git_collaboration_test.py"
                "$<TARGET_FILE:neverd-worker>")
      set_tests_properties(NeverDWorkerGitCollaboration PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
      set(NEVERD_WORKER_CLI_EXECUTABLE "" CACHE FILEPATH "Optional real CLI for cross-process writer tests")
      if(TARGET neverd)
        set(_neverd_worker_test_cli "$<TARGET_FILE:neverd>")
      elseif(EXISTS "${NEVERD_WORKER_CLI_EXECUTABLE}")
        set(_neverd_worker_test_cli "${NEVERD_WORKER_CLI_EXECUTABLE}")
      endif()
      set(_neverd_source_synthesis_args)
      if(_neverd_worker_test_cli)
        list(APPEND _neverd_source_synthesis_args --cli "${_neverd_worker_test_cli}")
      endif()
      add_test(NAME NeverDWorkerImmediatesRegister
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/immediates_register_test.py"
                "$<TARGET_FILE:neverd-worker>" ${_neverd_source_synthesis_args})
      set_tests_properties(NeverDWorkerImmediatesRegister PROPERTIES TIMEOUT 120)
      add_test(NAME NeverDWorkerSourceSynthesis
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/source_synthesis_test.py"
                "$<TARGET_FILE:neverd-worker>" ${_neverd_source_synthesis_args})
      # Each fixture compiles and executes every CLI/worker source variant at
      # O0 and O2. Allow the expanded corpus to finish on shared CI runners.
      set_tests_properties(NeverDWorkerSourceSynthesis PROPERTIES TIMEOUT 360)
      add_test(NAME NeverDWorkerInterpreterRecoveryReal
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/interpreter_recovery_test.py"
                "$<TARGET_FILE:neverd-worker>" --real ${_neverd_source_synthesis_args})
      set_tests_properties(NeverDWorkerInterpreterRecoveryReal PROPERTIES TIMEOUT 120)
      if(_neverd_worker_test_cli)
        add_test(NAME NeverDWorkerCLIPEFunctionParts
          COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/pe_function_parts_test.py"
                  "$<TARGET_FILE:neverd-worker>" --cli "${_neverd_worker_test_cli}")
        set_tests_properties(NeverDWorkerCLIPEFunctionParts PROPERTIES TIMEOUT 60)
        add_test(NAME NeverDWorkerCLIDataPresentation
          COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/data_presentation_cli_test.py"
                  "$<TARGET_FILE:neverd-worker>" "${_neverd_worker_test_cli}" --engine "$<TARGET_FILE:neverd_shared>")
        set_tests_properties(NeverDWorkerCLIDataPresentation PROPERTIES TIMEOUT 180)
        add_test(NAME NeverDWorkerComparison
          COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/comparison_test.py"
            $<TARGET_FILE:neverd-worker> --neverd "${_neverd_worker_test_cli}"
            --cc "${_neverd_worker_source_cc}")
        set_tests_properties(NeverDWorkerComparison PROPERTIES TIMEOUT 120 LABELS "NeverDBinaryDiff")
        add_test(NAME NeverDWorkerLineage
          COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/lineage_test.py"
            $<TARGET_FILE:neverd-worker> --neverd "${_neverd_worker_test_cli}"
            --cc "${_neverd_worker_source_cc}")
        set_tests_properties(NeverDWorkerLineage PROPERTIES TIMEOUT 240 LABELS "NeverDBinaryDiff")
        add_test(NAME NeverDWorkerCLIInstructionFlow
          COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/instruction_flow_cli_test.py"
                  "$<TARGET_FILE:neverd-worker>" "${_neverd_worker_test_cli}")
        set_tests_properties(NeverDWorkerCLIInstructionFlow PROPERTIES TIMEOUT 120)
        add_test(NAME NeverDWorkerCLIStackFrame
          COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/stack_frame_cli_test.py"
                  "$<TARGET_FILE:neverd-worker>" "${_neverd_worker_test_cli}" --cc "${_neverd_worker_source_cc}")
        set_tests_properties(NeverDWorkerCLIStackFrame PROPERTIES TIMEOUT 180)
        add_test(NAME NeverDWorkerCLIWriterLock
          COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_writer_lock_test.py"
                  "$<TARGET_FILE:neverd-worker>" "${_neverd_worker_test_cli}")
        set_tests_properties(NeverDWorkerCLIWriterLock PROPERTIES TIMEOUT 60)
        add_test(NAME NeverDWorkerCLISidecarParity
          COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sidecar_parity_test.py"
                  "$<TARGET_FILE:neverd-worker>" "${_neverd_worker_test_cli}")
        set_tests_properties(NeverDWorkerCLISidecarParity PROPERTIES TIMEOUT 60)
        add_test(NAME NeverDWorkerCLIGitMerge
          COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/git_collaboration_test.py"
                  "$<TARGET_FILE:neverd-worker>" "${_neverd_worker_test_cli}")
        set_tests_properties(NeverDWorkerCLIGitMerge PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77)
      endif()
    endif()
  endif()
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/NeverDHypervisor.cmake")
if(_neverd_worker_imported_engine)
  option(NEVERD_WORKER_SIGN_HVF "Sign a standalone worker for a potentially HVF-enabled engine" ON)
  if(NEVERD_WORKER_SIGN_HVF)
    neverd_sign_hypervisor(neverd-worker IMPORTED_ENGINE)
  endif()
else()
  neverd_sign_hypervisor(neverd-worker)
endif()
