add_neverd_component_library(NeverDWeb
  ArtifactStore.cpp
  packages/PackageReader.cpp
  packages/PackageDiff.cpp
  packages/PackageArchive.cpp
  packages/PackageIntegrity.cpp
  packages/SHA512.cpp
  interfaces/Endpoint.cpp
  interfaces/HAR.cpp
  interfaces/Correlation.cpp
  SessionInterfaces.cpp
  streams/Framing.cpp
  streams/Records.cpp
  streams/Relations.cpp
  SessionStreams.cpp
  SessionPackages.cpp
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
  SessionElectron.cpp
  SessionElectronIPC.cpp
  SessionElectronEntries.cpp
  PathPolicy.cpp
  BlobStore.cpp
  Bun.cpp
  BunContainer.cpp
  BunSourceMap.cpp
  SessionBun.cpp
  SessionBunExport.cpp
  InputRoot.cpp
  JsonReader.cpp
  SourceLocation.cpp
  SourceMaps.cpp
  Session.cpp
  SessionMaps.cpp
  SessionBindings.cpp
  SessionSemantics.cpp
  SessionModules.cpp
  SessionBundles.cpp
  SessionView.cpp
  SessionNavigation.cpp
  SessionAnchor.cpp
  SessionNative.cpp
  LLVM_COMPONENTS Support
)
include("${CMAKE_SOURCE_DIR}/cmake/NeverDWebURL.cmake")
find_package(ZLIB QUIET)
if(ZLIB_FOUND)
  target_link_libraries(NeverDWeb PRIVATE ZLIB::ZLIB)
  target_compile_definitions(NeverDWeb PRIVATE NEVERD_WEB_ZLIB=1)
endif()
neverd_add_web_url_parser()
target_link_libraries(NeverDWeb PRIVATE neverd_web_url)
install(DIRECTORY "${CMAKE_SOURCE_DIR}/LICENSES/ada/"
        DESTINATION share/neverd/licenses/ada)
option(NEVERD_ENABLE_WEB_ASAR "Build ASAR extraction with pinned native Unicode policy" ON)
if(NEVERD_ENABLE_WEB_ASAR)
  find_package(ICU 77.1 EXACT COMPONENTS uc QUIET)
  if(ICU_FOUND)
    target_link_libraries(NeverDWeb PRIVATE ICU::uc)
    target_compile_definitions(NeverDWeb PRIVATE NEVERD_WEB_ICU=1)
  else()
    message(STATUS "ASAR unavailable: ICU 77.1 uc library is required (set ICU_ROOT)")
  endif()
endif()
install(DIRECTORY "${CMAKE_SOURCE_DIR}/LICENSES/bun/"
        DESTINATION share/neverd/licenses/bun)
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
    interfaces/SourceInterfaces.cpp
    SourceRecovery.cpp
    SourceModel.cpp SourceBindings.cpp SourceEvaluate.cpp SourceEffects.cpp SourceModules.cpp HTMLModules.cpp
    SourceBundles.cpp SourceView.cpp SourceNavigation.cpp SourceOrigins.cpp SourcePaths.cpp ElectronSource.cpp ElectronSelection.cpp ElectronIPC.cpp ElectronEntries.cpp PrimitiveNumbers.cpp)
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
