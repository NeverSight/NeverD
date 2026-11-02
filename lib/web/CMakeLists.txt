add_neverd_component_library(NeverDWeb
  ArtifactStore.cpp
  SourceLimits.cpp
  packages/PackageReader.cpp
  packages/PackageDiff.cpp
  packages/PackageArchive.cpp
  archives/PathIndex.cpp
  archives/Zip.cpp
  archives/ZipPayload.cpp
  packages/PackageIntegrity.cpp
  packages/SHA512.cpp
  packages/evidence/Descriptor.cpp
  packages/evidence/Publication.cpp
  packages/advisories/SemVer.cpp
  packages/advisories/Reader.cpp
  packages/advisories/Match.cpp
  packages/advisories/Publication.cpp
  SessionPackageAdvisories.cpp
  packages/attestations/Encoding.cpp
  packages/attestations/Reader.cpp
  packages/attestations/Registry.cpp
  packages/attestations/Statement.cpp
  packages/attestations/Provenance.cpp
  packages/attestations/Digest.cpp
  packages/attestations/Publication.cpp
  SessionPackageAttestations.cpp
  packages/assessment/Compose.cpp
  packages/assessment/Integrity.cpp
  packages/assessment/Metadata.cpp
  packages/assessment/Behavior.cpp
  packages/assessment/Disposition.cpp
  packages/assessment/Publication.cpp
  packages/assessment/Report.cpp
  SessionPackageAssessments.cpp
  SessionPackageReviewRestore.cpp
  interfaces/Endpoint.cpp
  interfaces/HAR.cpp
  interfaces/Correlation.cpp
  SessionInterfaces.cpp
  SessionInterfaceRelations.cpp
  SessionPassiveReport.cpp
  interfaces/report/Compose.cpp
  interfaces/report/Shapes.cpp
  interfaces/report/ObservedStructure.cpp
  interfaces/report/Publication.cpp
  streams/Framing.cpp
  streams/Records.cpp
  streams/ProtocolMethods.cpp
  streams/Relations.cpp
  SessionStreams.cpp
  SessionPackages.cpp
  SessionNodeBehavior.cpp
  SessionPackageBehavior.cpp
  SessionPackageBehaviorDiff.cpp
  packages/behavior/DiffPublication.cpp
  SessionPackageInstall.cpp
  packages/install/Publication.cpp
  packages/behavior/Publication.cpp
  SessionPackageArchive.cpp
  SessionPackageIntegrity.cpp
  HTML.cpp
  HTMLReferences.cpp
  HTMLLinks.cpp
  HTMLImportMaps.cpp
  ImportMap.cpp
  SessionHTML.cpp
  ArtifactView.cpp
  Asar.cpp
  SessionAsar.cpp
  ElectronManifest.cpp
  desktop/Manifest.cpp
  desktop/NWManifest.cpp
  desktop/VSIXManifest.cpp
  desktop/ResourceManifest.cpp
  desktop/DataPack.cpp
  SessionDataPack.cpp
  SessionDataPackExport.cpp
  compression/Gzip.cpp
  desktop/wails/ELF.cpp
  desktop/wails/PE.cpp
  desktop/wails/Mappings.cpp
  desktop/wails/Profiles.cpp
  desktop/wails/BuildInfo.cpp
  desktop/wails/Tables.cpp
  desktop/wails/Contents.cpp
  desktop/wails/Reader.cpp
  SessionWails.cpp
  SessionWailsExport.cpp
  desktop/tauri/PHF.cpp
  desktop/tauri/Reader.cpp
  desktop/bridge/Publication.cpp
  features/configuration/Publication.cpp
  features/configuration/FilesPublication.cpp
  SessionTauri.cpp
  SessionTauriExport.cpp
  compression/Brotli.cpp
  SessionDesktop.cpp
  SessionDesktopExport.cpp
  SessionElectron.cpp
  SessionElectronIPC.cpp
  SessionElectronEntries.cpp
  PathPolicy.cpp
  BlobStore.cpp
  Bun.cpp
  BunContainer.cpp
  sea/Blob.cpp
  sea/Container.cpp
  sea/ELF.cpp
  sea/MachO.cpp
  sea/PE.cpp
  SessionSEA.cpp
  nexe/Reader.cpp
  nexe/Index.cpp
  SessionNexe.cpp
  pkg/Locator.cpp
  pkg/Index.cpp
  pkg/Reader.cpp
  SessionPkg.cpp
  BunSourceMap.cpp
  compression/Zstd.cpp
  SessionBun.cpp
  SessionBunExport.cpp
  SessionReadableExport.cpp
  InputRoot.cpp
  JsonReader.cpp
  SourceLocation.cpp
  SourceMaps.cpp
  Session.cpp
  SessionMaps.cpp
  SessionBindings.cpp
  SessionSemantics.cpp
  SessionRewrite.cpp
  SessionModules.cpp
  SessionBundles.cpp
  SessionView.cpp
  SessionNavigation.cpp
  features/Publication.cpp
  SessionAnchor.cpp
  SessionNative.cpp
  LLVM_COMPONENTS Support
)
include("${CMAKE_SOURCE_DIR}/cmake/NeverDWebURL.cmake")
if(WIN32)
  target_sources(NeverDWeb PRIVATE platform/WindowsFile.cpp)
  target_link_libraries(NeverDWeb PRIVATE advapi32 bcrypt)
endif()
include("${CMAKE_SOURCE_DIR}/cmake/NeverDWebBrotli.cmake")
neverd_add_web_brotli()
include(${CMAKE_SOURCE_DIR}/cmake/NeverDZlib.cmake)
target_link_libraries(NeverDWeb PRIVATE ZLIB::ZLIB)
target_compile_definitions(NeverDWeb PRIVATE NEVERD_WEB_ZLIB=1)
# LLVM builds with Zstd remain the preferred decoder. A prebuilt LLVM package
# may omit it; use only the pinned static SDK as an optional in-process fallback.
# The existing format reader still owns framing and resource admission.
if(NOT LLVM_ENABLE_ZSTD)
  find_package(zstd 1.5.7 EXACT CONFIG QUIET)
  if(zstd_FOUND AND TARGET zstd::libzstd_static)
    target_link_libraries(NeverDWeb PRIVATE zstd::libzstd_static)
    set_property(SOURCE compression/Zstd.cpp APPEND PROPERTY
      COMPILE_DEFINITIONS NEVERD_WEB_ZSTD=1)
    message(STATUS "Web Zstd: using native static SDK 1.5.7 (LLVM omits Zstd)")
  else()
    message(STATUS "Bun source maps unavailable: LLVM Zstd or static Zstd SDK 1.5.7 required")
  endif()
endif()
neverd_add_web_url_parser()
target_link_libraries(NeverDWeb PRIVATE neverd_web_url)
install(DIRECTORY "${CMAKE_SOURCE_DIR}/LICENSES/ada/"
        DESTINATION share/neverd/licenses/ada)
option(NEVERD_ENABLE_WEB_ASAR "Build ASAR extraction with pinned native Unicode policy" ON)
set_property(TARGET NeverDWeb PROPERTY NEVERD_WEB_ICU_AVAILABLE FALSE)
if(NEVERD_ENABLE_WEB_ASAR)
  find_package(ICU 77.1 EXACT COMPONENTS uc QUIET)
  if(ICU_FOUND)
    target_link_libraries(NeverDWeb PRIVATE ICU::uc)
    target_compile_definitions(NeverDWeb PRIVATE NEVERD_WEB_ICU=1)
    set_property(TARGET NeverDWeb PROPERTY NEVERD_WEB_ICU_AVAILABLE TRUE)
  else()
    message(STATUS "ASAR unavailable: ICU 77.1 uc library is required (set ICU_ROOT)")
  endif()
endif()
install(DIRECTORY "${CMAKE_SOURCE_DIR}/LICENSES/bun/"
        DESTINATION share/neverd/licenses/bun)
install(DIRECTORY "${CMAKE_SOURCE_DIR}/LICENSES/zstd/"
        DESTINATION share/neverd/licenses/zstd)
install(DIRECTORY "${CMAKE_SOURCE_DIR}/LICENSES/node/"
        DESTINATION share/neverd/licenses/node)
install(DIRECTORY "${CMAKE_SOURCE_DIR}/LICENSES/asar/"
        DESTINATION share/neverd/licenses/asar)
install(DIRECTORY "${CMAKE_SOURCE_DIR}/LICENSES/whatwg/"
        DESTINATION share/neverd/licenses/whatwg)
if(NEVERD_ENABLE_WEB_ASAR)
  install(DIRECTORY "${CMAKE_SOURCE_DIR}/LICENSES/icu/"
          DESTINATION share/neverd/licenses/icu)
endif()
option(NEVERD_ENABLE_WEB_JAVASCRIPT "Build the embedded C++ JavaScript parser" ON)
if(NEVERD_ENABLE_WEB_JAVASCRIPT)
  include("${CMAKE_SOURCE_DIR}/cmake/NeverDJavaScriptParser.cmake")
  neverd_add_javascript_parser()
  target_sources(NeverDWeb PRIVATE SourceParser.cpp ParserBudget.cpp
    BunProject.cpp BunProjectSource.cpp BunProjectRequire.cpp BunSemanticIndex.cpp
    desktop/bridge/Source.cpp
    features/configuration/Analysis.cpp
    features/configuration/Files.cpp
    packages/behavior/Rules.cpp
    packages/behavior/Source.cpp
    packages/behavior/Package.cpp
    packages/behavior/Diff.cpp
    packages/install/Command.cpp
    packages/install/Trace.cpp
    interfaces/SourceInterfaces.cpp
    interfaces/SourceContext.cpp
    interfaces/SourceFetch.cpp
    interfaces/SourceWrappers.cpp
    interfaces/SourceXHR.cpp
    interfaces/RelationContext.cpp
    interfaces/RequestLinks.cpp
    interfaces/ResultLinks.cpp
    interfaces/TransformationLinks.cpp
    interfaces/SourceRelations.cpp
    interfaces/report/Source.cpp
    interfaces/report/SourceStructure.cpp
    interfaces/report/URLStructure.cpp
    SourceRecovery.cpp
    rewrite/Plan.cpp
    rewrite/Rename.cpp
    rewrite/RenameValidate.cpp
    rewrite/Branches.cpp
    rewrite/BranchValidate.cpp
    rewrite/Primitive.cpp
    rewrite/Apply.cpp
    rewrite/Validate.cpp
    rewrite/Export.cpp
    SourceModel.cpp SourceBindings.cpp SourceEvaluate.cpp SourceEffects.cpp SourceModules.cpp HTMLModules.cpp
    SourceBundles.cpp SourceView.cpp SourceNavigation.cpp
    features/Analysis.cpp features/Recognize.cpp
    SourceOrigins.cpp SourcePaths.cpp ElectronSource.cpp ElectronSelection.cpp ElectronIPC.cpp ElectronEntries.cpp PrimitiveNumbers.cpp)
  target_link_libraries(NeverDWeb PRIVATE neverd_js_parser)
  target_compile_definitions(NeverDWeb PRIVATE NEVERD_ENABLE_WEB_JAVASCRIPT=1)
  if(MSVC)
    # HermesModel forwards variadic AST field lists through nested macros.
    # The traditional MSVC preprocessor does not expand these as C++ requires.
    target_compile_options(NeverDWeb PRIVATE /Zc:preprocessor)
  endif()
  # The parser's build directory is EXCLUDE_FROM_ALL, including its install
  # rules. Ship both embedded-source license sets from the owning feature.
  install(DIRECTORY "${CMAKE_SOURCE_DIR}/LICENSES/hermes/"
          DESTINATION share/neverd/licenses/hermes)
  install(DIRECTORY "${CMAKE_SOURCE_DIR}/LICENSES/webpack/"
          DESTINATION share/neverd/licenses/webpack)
endif()
