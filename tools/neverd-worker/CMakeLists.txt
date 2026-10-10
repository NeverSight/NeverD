cmake_minimum_required(VERSION 3.24)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(NeverDWorker LANGUAGES CXX)
  include(CTest)
endif()

include(FetchContent)
if(NOT TARGET nlohmann_json::nlohmann_json)
  set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(neverd_worker_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(neverd_worker_json)
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
target_link_libraries(neverd_worker_protocol PUBLIC nlohmann_json::nlohmann_json)
target_include_directories(neverd_worker_protocol PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
add_library(neverd_worker_graph STATIC GraphSnapshot.cpp GraphLayout.cpp)
add_library(neverd_worker_format STATIC OperandFormat.cpp CodeEdits.cpp)
target_link_libraries(neverd_worker_format PUBLIC neverd_worker_protocol)
# Header-only tables shared with the engine, such as the wide characters.
target_include_directories(neverd_worker_format PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../../include")
target_link_libraries(neverd_worker_graph PUBLIC neverd_worker_protocol)
add_executable(neverd-worker main.cpp Engine.cpp WebEngine.cpp Listing.cpp StringReferences.cpp TextFold.cpp ProjectHistory.cpp Contributions.cpp)
target_compile_features(neverd-worker PRIVATE cxx_std_20)
target_include_directories(neverd-worker PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../include")
# The C ABI is the only engine dependency. Never add LLVM to this target.
target_link_libraries(neverd-worker PRIVATE neverd_worker_graph neverd_worker_format neverd_shared Threads::Threads ${CMAKE_DL_LIBS})
set_target_properties(neverd-worker PROPERTIES
  INSTALL_RPATH "$ORIGIN/../lib;@loader_path/../lib")
install(TARGETS neverd-worker RUNTIME DESTINATION bin)

if(BUILD_TESTING OR NEVERD_BUILD_TESTS)
  add_executable(neverd-worker-protocol-tests tests/protocol_tests.cpp)
  target_link_libraries(neverd-worker-protocol-tests PRIVATE neverd_worker_protocol)
  add_test(NAME NeverDWorkerProtocol COMMAND neverd-worker-protocol-tests)
  add_executable(neverd-worker-format-tests tests/format_tests.cpp)
  target_link_libraries(neverd-worker-format-tests PRIVATE neverd_worker_format)
  add_test(NAME NeverDWorkerFormat COMMAND neverd-worker-format-tests)
  add_executable(neverd-worker-code-edits-tests tests/code_edits_tests.cpp)
  target_link_libraries(neverd-worker-code-edits-tests PRIVATE neverd_worker_format)
  add_test(NAME NeverDWorkerCodeEdits COMMAND neverd-worker-code-edits-tests)
  add_executable(neverd-worker-graph-tests tests/graph_tests.cpp)
  target_link_libraries(neverd-worker-graph-tests PRIVATE neverd_worker_graph)
  add_test(NAME NeverDWorkerGraph COMMAND neverd-worker-graph-tests)
  if(_neverd_worker_imported_engine)
    option(NEVERD_WORKER_WEB_ENGINE_TESTS "Test an imported engine with the web C API" OFF)
  else()
    option(NEVERD_WORKER_WEB_ENGINE_TESTS "Test the web C API worker adapter" ON)
  endif()
  if(NEVERD_WORKER_WEB_ENGINE_TESTS)
    add_executable(neverd-worker-web-tests tests/web_engine_tests.cpp WebEngine.cpp)
    target_include_directories(neverd-worker-web-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../include")
    target_link_libraries(neverd-worker-web-tests PRIVATE neverd_worker_protocol neverd_shared ${CMAKE_DL_LIBS})
    add_dependencies(neverd-worker-web-tests neverd-worker)
    add_test(NAME NeverDWorkerWeb COMMAND neverd-worker-web-tests "$<TARGET_FILE:neverd-worker>")
    set_tests_properties(NeverDWorkerWeb PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
  endif()
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_executable(neverd-worker-test-server main.cpp Engine.cpp WebEngine.cpp Listing.cpp StringReferences.cpp TextFold.cpp ProjectHistory.cpp Contributions.cpp tests/mock_engine.cpp)
    set_target_properties(neverd-worker-test-server PROPERTIES ENABLE_EXPORTS ON)
    target_compile_features(neverd-worker-test-server PRIVATE cxx_std_20)
    target_compile_definitions(neverd-worker-test-server PRIVATE NEVERD_EXPORTS)
    target_include_directories(neverd-worker-test-server PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../include")
    target_link_libraries(neverd-worker-test-server PRIVATE neverd_worker_graph neverd_worker_format Threads::Threads ${CMAKE_DL_LIBS})
    add_test(NAME NeverDWorkerTransport
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/transport_test.py"
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
    add_test(NAME NeverDWorkerSourceCache
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/source_cache_test.py"
              "$<TARGET_FILE:neverd-worker-test-server>")
    set_tests_properties(NeverDWorkerSourceCache PROPERTIES TIMEOUT 40)
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
    option(NEVERD_WORKER_REAL_ENGINE_TESTS "Run smoke tests against the real engine" ON)
    if(NEVERD_WORKER_REAL_ENGINE_TESTS)
      add_test(NAME NeverDWorkerNativeMakeCode
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/make_code_test.py"
                "$<TARGET_FILE:neverd-worker>" --real)
      set_tests_properties(NeverDWorkerNativeMakeCode PROPERTIES TIMEOUT 40)
      add_test(NAME NeverDWorkerRealEngine
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/real_engine_test.py"
                "$<TARGET_FILE:neverd-worker>")
      set_tests_properties(NeverDWorkerRealEngine PROPERTIES TIMEOUT 60)
      add_test(NAME NeverDWorkerUnicodePaths
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/unicode_path_test.py"
                "$<TARGET_FILE:neverd-worker>")
      set_tests_properties(NeverDWorkerUnicodePaths PROPERTIES TIMEOUT 90)
      add_test(NAME NeverDWorkerPEBrowse
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/pe_browse_test.py"
                "$<TARGET_FILE:neverd-worker>")
      set_tests_properties(NeverDWorkerPEBrowse PROPERTIES TIMEOUT 60)
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
      set(NEVERD_WORKER_CLI_EXECUTABLE "" CACHE FILEPATH "Optional real CLI for cross-process writer tests")
      if(TARGET neverd)
        set(_neverd_worker_test_cli "$<TARGET_FILE:neverd>")
      elseif(EXISTS "${NEVERD_WORKER_CLI_EXECUTABLE}")
        set(_neverd_worker_test_cli "${NEVERD_WORKER_CLI_EXECUTABLE}")
      endif()
      if(_neverd_worker_test_cli)
        add_test(NAME NeverDWorkerCLIWriterLock
          COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_writer_lock_test.py"
                  "$<TARGET_FILE:neverd-worker>" "${_neverd_worker_test_cli}")
        set_tests_properties(NeverDWorkerCLIWriterLock PROPERTIES TIMEOUT 60)
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
