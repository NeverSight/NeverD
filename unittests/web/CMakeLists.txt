add_neverd_unittest(NeverDWebArtifactTests
  ArtifactTests.cpp
  WindowsHostTests.cpp
  PackageTests.cpp
  PackageArchiveTests.cpp
  ZipTests.cpp
  PackageIntegrityTests.cpp
  PackageAdvisoryTests.cpp
  PackageAttestationTests.cpp
  PackageAssessmentTests.cpp
  PackageReviewTests.cpp
  InterfaceTests.cpp
  PassiveReportTests.cpp
  PassiveStructureTests.cpp
  StreamTests.cpp
  AsarTests.cpp
  ElectronManifestTests.cpp
  DesktopManifestTests.cpp
  DataPackTests.cpp
  HTMLTests.cpp
  ImportMapTests.cpp
  BlobStoreTests.cpp
  BunTests.cpp
  ClaudeDistributionTests.cpp
  SEATests.cpp
  NexeTests.cpp
  PkgTests.cpp
  ProducerCorpusPolicyTests.cpp
  WailsTests.cpp
  TauriTests.cpp
  JsonReaderTests.cpp
  LINK_COMPONENTS Web
)
target_include_directories(NeverDWebArtifactTests PRIVATE "${CMAKE_SOURCE_DIR}/lib/web")
target_compile_definitions(NeverDWebArtifactTests PRIVATE
  NEVERD_TEST_WEB_JAVASCRIPT=$<BOOL:${NEVERD_ENABLE_WEB_JAVASCRIPT}>
  NEVERD_WEB_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
# Compile the owning payload translation unit without its private zlib macro.
# The archive reader and model are shared; no product omission flag is invented.
add_neverd_unittest(NeverDWebZipWithoutDeflateTests
  ZipNoDeflateTests.cpp
  ../../lib/web/archives/ZipPayload.cpp
  LINK_COMPONENTS Web
)
target_include_directories(NeverDWebZipWithoutDeflateTests PRIVATE "${CMAKE_SOURCE_DIR}/lib/web")
add_neverd_unittest(NeverDWebBrotliWithoutDecoderTests
  BrotliNoDecoderTests.cpp
  ../../lib/web/compression/Brotli.cpp
  LINK_COMPONENTS Web)
target_include_directories(NeverDWebBrotliWithoutDecoderTests PRIVATE
  "${CMAKE_SOURCE_DIR}/lib/web")
add_neverd_unittest(NeverDWebGzipWithoutDecoderTests
  GzipNoDecoderTests.cpp
  ../../lib/web/compression/Gzip.cpp
  LINK_COMPONENTS Web)
target_include_directories(NeverDWebGzipWithoutDecoderTests PRIVATE
  "${CMAKE_SOURCE_DIR}/lib/web")
# Manual, development-only recorder; no production or test dependency on it.
add_executable(neverd-web-bun-fixture-record EXCLUDE_FROM_ALL fixtures/bun/Record.cpp)
add_executable(neverd-web-tauri-fixture-record EXCLUDE_FROM_ALL fixtures/tauri/Record.cpp)
target_include_directories(neverd-web-tauri-fixture-record PRIVATE
  "${CMAKE_SOURCE_DIR}/lib/web")
target_link_libraries(neverd-web-tauri-fixture-record PRIVATE NeverDWeb)
add_executable(neverd-web-sea-fixture-record EXCLUDE_FROM_ALL fixtures/sea/Record.cpp)
add_executable(neverd-web-nexe-fixture-inputs EXCLUDE_FROM_ALL fixtures/nexe/WriteInputs.cpp)
target_include_directories(neverd-web-nexe-fixture-inputs PRIVATE ${LLVM_INCLUDE_DIRS})
target_link_libraries(neverd-web-nexe-fixture-inputs PRIVATE LLVMSupport)
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
  BunSourceMapCorpusTests.cpp
  LINK_COMPONENTS Web
)
target_include_directories(NeverDWebSourceMapTests PRIVATE "${CMAKE_SOURCE_DIR}/lib/web")
target_compile_definitions(NeverDWebSourceMapTests PRIVATE
  NEVERD_WEB_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
add_executable(neverd-web-source-map-model EXCLUDE_FROM_ALL fixtures/bun/SourceMapModel.cpp)
target_include_directories(neverd-web-source-map-model PRIVATE "${CMAKE_SOURCE_DIR}/lib/web")
target_link_libraries(neverd-web-source-map-model PRIVATE NeverDWeb)
if(NEVERD_ENABLE_WEB_JAVASCRIPT)
  # Explicit development driver: export only; never invoke a compiler/runtime.
  add_executable(neverd-web-bun-inventory EXCLUDE_FROM_ALL fixtures/bun/Inventory.cpp)
  target_include_directories(neverd-web-bun-inventory PRIVATE "${CMAKE_SOURCE_DIR}/lib/web")
  target_link_libraries(neverd-web-bun-inventory PRIVATE NeverDWeb)
  add_executable(neverd-web-source-recovery EXCLUDE_FROM_ALL fixtures/bun/SourceRecovery.cpp)
  target_include_directories(neverd-web-source-recovery PRIVATE "${CMAKE_SOURCE_DIR}/lib/web")
  target_link_libraries(neverd-web-source-recovery PRIVATE NeverDWeb)
  add_executable(neverd-web-source-model EXCLUDE_FROM_ALL fixtures/bun/SourceModel.cpp)
  target_include_directories(neverd-web-source-model PRIVATE "${CMAKE_SOURCE_DIR}/lib/web")
  target_link_libraries(neverd-web-source-model PRIVATE NeverDWeb)
  add_executable(neverd-web-bun-project EXCLUDE_FROM_ALL fixtures/bun/Project.cpp)
  target_include_directories(neverd-web-bun-project PRIVATE "${CMAKE_SOURCE_DIR}/lib/web")
  target_link_libraries(neverd-web-bun-project PRIVATE NeverDWeb)
  add_executable(neverd-web-bun-semantic-index EXCLUDE_FROM_ALL fixtures/bun/SemanticIndex.cpp)
  target_include_directories(neverd-web-bun-semantic-index PRIVATE "${CMAKE_SOURCE_DIR}/lib/web")
  target_link_libraries(neverd-web-bun-semantic-index PRIVATE NeverDWeb)
  add_neverd_unittest(NeverDWebSourceTests
    BunProjectSourceTests.cpp
    BunProjectRequireTests.cpp
    BunProjectTests.cpp
    BunSemanticIndexTests.cpp
    NodeBehaviorTests.cpp
    PackageBehaviorTests.cpp
    PackageBehaviorDiffTests.cpp
    PackageInstallTests.cpp
    SourceInterfaceTests.cpp
    SourceWrapperTests.cpp
    InterfaceRelationTests.cpp
    PassiveSourceTests.cpp
    RequestStructureTests.cpp
    SourceXHRTests.cpp
    HTMLModuleTests.cpp
    ElectronIPCTests.cpp
    ElectronEntryTests.cpp
    ElectronSourceTests.cpp
    SourceTests.cpp
    LargeSourceTests.cpp
    SourceRecoveryTests.cpp
    SourceRewriteTests.cpp
    SourceRenameTests.cpp
    SourceBranchTests.cpp
    RewriteExportTests.cpp
    SourceBindingTests.cpp
    SourceDeclarationIndexTests.cpp
    SourceValueTests.cpp
    SourceEffectTests.cpp
    SourceModuleTests.cpp
    SourceBundleTests.cpp
    SourceViewTests.cpp
    SourceNavigationTests.cpp
    SourceFeatureTests.cpp
    DesktopBridgeTests.cpp
    ConfigurationOrderTests.cpp
    ConfigurationFileTests.cpp
    ClientFeatureCorpusTests.cpp
    LINK_COMPONENTS Web
  )
  target_compile_definitions(NeverDWebSourceTests PRIVATE
    NEVERD_WEB_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
  target_include_directories(NeverDWebSourceTests PRIVATE "${CMAKE_SOURCE_DIR}/lib/web")
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
    ReadableExportSDKTests.cpp
    RewriteSDKTests.cpp
    RenameSDKTests.cpp
    BranchSDKTests.cpp
    NodeBehaviorSDKTests.cpp
    PackageBehaviorDiffSDKTests.cpp
    PackageSDKTests.cpp
    PackageArchiveSDKTests.cpp
    PackageIntegritySDKTests.cpp
    PackageAdvisorySDKTests.cpp
    PackageAttestationSDKTests.cpp
    PackageAssessmentSDKTests.cpp
    InterfaceSDKTests.cpp
    StreamSDKTests.cpp
    SEASDKTests.cpp
    NexeSDKTests.cpp
    PkgSDKTests.cpp
    WailsSDKTests.cpp
    TauriSDKTests.cpp
    AsarSDKTests.cpp
    ElectronSDKTests.cpp
    DesktopSDKTests.cpp
    DataPackSDKTests.cpp
    DataPackCorpusSDKTests.cpp
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

if(TARGET NeverDWebSDKTests AND NEVERD_ENABLE_WEB_JAVASCRIPT)
  set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/ProducerCorpusTestProperties.cmake")
  add_custom_target(check-neverd-web-producer-corpus
    COMMAND "${CMAKE_COMMAND}" -E env NEVERD_REQUIRE_WEB_PRODUCER_CORPUS=1
      "${CMAKE_CTEST_COMMAND}" --test-dir "${CMAKE_CURRENT_BINARY_DIR}"
      --output-on-failure --no-tests=error -L "^NeverDWebProducerCorpus$"
    DEPENDS NeverDWebArtifactTests NeverDWebSourceMapTests
            NeverDWebNativeTests NeverDWebSDKTests
    USES_TERMINAL
    # The label regex ends in `$`: without VERBATIM, Ninja on Windows reads
    # it as a bad `$"` escape and rejects the whole build file.
    VERBATIM
    COMMENT "Qualifying all pinned full producer containers without optional skips")
endif()
