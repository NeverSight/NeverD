add_neverd_unittest(NeverDWebArtifactTests
  ArtifactTests.cpp
  PackageTests.cpp
  PackageArchiveTests.cpp
  PackageIntegrityTests.cpp
  InterfaceTests.cpp
  StreamTests.cpp
  AsarTests.cpp
  ElectronManifestTests.cpp
  HTMLTests.cpp
  ImportMapTests.cpp
  BlobStoreTests.cpp
  BunTests.cpp
  SEATests.cpp
  JsonReaderTests.cpp
  LINK_COMPONENTS Web
)
target_include_directories(NeverDWebArtifactTests PRIVATE "${CMAKE_SOURCE_DIR}/lib/web")
target_compile_definitions(NeverDWebArtifactTests PRIVATE
  NEVERD_WEB_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
# Manual, development-only recorder; no production or test dependency on it.
add_executable(neverd-web-bun-fixture-record EXCLUDE_FROM_ALL fixtures/bun/Record.cpp)
add_executable(neverd-web-sea-fixture-record EXCLUDE_FROM_ALL fixtures/sea/Record.cpp)
target_include_directories(neverd-web-sea-fixture-record PRIVATE ${LLVM_INCLUDE_DIRS})
target_link_libraries(neverd-web-sea-fixture-record PRIVATE LLVMSupport)
target_include_directories(neverd-web-bun-fixture-record PRIVATE ${LLVM_INCLUDE_DIRS})
target_link_libraries(neverd-web-bun-fixture-record PRIVATE LLVMSupport)
add_executable(neverd-web-html-reference-record EXCLUDE_FROM_ALL fixtures/html/RecordReferences.cpp)
target_include_directories(neverd-web-html-reference-record PRIVATE ${LLVM_INCLUDE_DIRS})
target_link_libraries(neverd-web-html-reference-record PRIVATE LLVMSupport)
add_neverd_unittest(NeverDWebSourceMapTests
  SourceMapTests.cpp
  BunSourceMapTests.cpp
  LINK_COMPONENTS Web
)
target_include_directories(NeverDWebSourceMapTests PRIVATE "${CMAKE_SOURCE_DIR}/lib/web")
target_compile_definitions(NeverDWebSourceMapTests PRIVATE
  NEVERD_WEB_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
if(NEVERD_ENABLE_WEB_JAVASCRIPT)
  add_neverd_unittest(NeverDWebSourceTests
    SourceInterfaceTests.cpp
    HTMLModuleTests.cpp
    ElectronIPCTests.cpp
    ElectronEntryTests.cpp
    ElectronSourceTests.cpp
    SourceTests.cpp
    SourceRecoveryTests.cpp
    SourceBindingTests.cpp
    SourceValueTests.cpp
    SourceEffectTests.cpp
    SourceModuleTests.cpp
    SourceBundleTests.cpp
    SourceViewTests.cpp
    SourceNavigationTests.cpp
    LINK_COMPONENTS Web
  )
  target_compile_definitions(NeverDWebSourceTests PRIVATE
    NEVERD_WEB_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
  set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/RecoveryTestProperties.cmake")
endif()
if(TARGET neverd_shared)
  add_neverd_unittest(NeverDWebNativeTests
    NativeTests.cpp
    LINK_LIBS neverd_shared LLVMSupport
  )
  target_include_directories(NeverDWebNativeTests PRIVATE
    "${CMAKE_SOURCE_DIR}/include" ${LLVM_INCLUDE_DIRS})
  add_neverd_unittest(NeverDWebSDKTests
    SDKTests.cpp
    PackageSDKTests.cpp
    PackageArchiveSDKTests.cpp
    PackageIntegritySDKTests.cpp
    InterfaceSDKTests.cpp
    StreamSDKTests.cpp
    SEASDKTests.cpp
    AsarSDKTests.cpp
    ElectronSDKTests.cpp
    HTMLSDKTests.cpp
    LINK_LIBS neverd_shared LLVMSupport
  )
  target_include_directories(NeverDWebSDKTests PRIVATE
    "${CMAKE_SOURCE_DIR}/include" ${LLVM_INCLUDE_DIRS})
  target_compile_definitions(NeverDWebSDKTests PRIVATE
    NEVERD_WEB_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    NEVERD_TEST_WEB_JAVASCRIPT=$<BOOL:${NEVERD_ENABLE_WEB_JAVASCRIPT}>)
  if(TARGET neverd)
    add_dependencies(NeverDWebNativeTests neverd)
    target_compile_definitions(NeverDWebNativeTests PRIVATE
      NEVERD_WEB_TEST_CLI="$<TARGET_FILE:neverd>")
    add_dependencies(NeverDWebSDKTests neverd)
    target_compile_definitions(NeverDWebSDKTests PRIVATE
      NEVERD_WEB_TEST_CLI="$<TARGET_FILE:neverd>")
  endif()
endif()
