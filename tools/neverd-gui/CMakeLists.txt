cmake_minimum_required(VERSION 3.24)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(NeverDDesktop VERSION 3389.0.1 LANGUAGES C CXX)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
  include(CTest)
  include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/NeverDZlib.cmake")
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../neverd-worker" "${CMAKE_BINARY_DIR}/worker")
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../neverd-mcp" "${CMAKE_BINARY_DIR}/mcp")
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/NeverDProjectDatabase.cmake)
neverd_add_project_database()

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Network LinguistTools Svg
  Sql Concurrent)
# Qt 6.8 exports private targets from the public module packages. Newer Qt
# releases ship separate private packages; KDDockWidgets needs the matching
# private headers in either layout.
foreach(_neverd_private_module Gui Widgets)
  if(NOT TARGET Qt6::${_neverd_private_module}Private)
    find_package(Qt6 6.8 REQUIRED COMPONENTS ${_neverd_private_module}Private)
  endif()
endforeach()
qt_standard_project_setup(REQUIRES 6.8)
include(FetchContent)
set(KDDockWidgets_FRONTENDS "qtwidgets" CACHE STRING "" FORCE)
set(KDDockWidgets_QT6 ON CACHE BOOL "" FORCE)
set(KDDockWidgets_STATIC ON CACHE BOOL "" FORCE)
set(KDDockWidgets_QML_MODULE OFF CACHE BOOL "" FORCE)
set(KDDockWidgets_EXAMPLES OFF CACHE BOOL "" FORCE)
set(KDDockWidgets_TESTS OFF CACHE BOOL "" FORCE)
set(KDDockWidgets_NO_SPDLOG ON CACHE BOOL "" FORCE)
set(KDDockWidgets_LINTER OFF CACHE BOOL "" FORCE)
FetchContent_Declare(neverd_docking
  URL https://github.com/KDAB/KDDockWidgets/archive/refs/tags/v2.4.1.tar.gz
  URL_HASH SHA256=5cd9495d9316cf6cc937bb328a7fd491c3248ef2469cfa42511f1039f6148aca
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(neverd_docking)
# The application embeds KDDockWidgets; its development SDK is not part of
# the desktop runtime install. Referenced targets still build normally.
set_property(DIRECTORY "${neverd_docking_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL TRUE)

# Worker transport, request dispatch and MCP bridges: no widgets.
add_library(neverd_gui_core STATIC EngineClient.cpp QueryService.cpp
  LibraryCodeView.cpp mcp/McpConnectionManager.cpp mcp/GuiSessionBroker.cpp)
target_include_directories(neverd_gui_core PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../include")
target_compile_features(neverd_gui_core PUBLIC cxx_std_20)
target_link_libraries(neverd_gui_core PUBLIC Qt6::Core Qt6::Gui Qt6::Network)

# Offline package presentation and local journals; all analysis stays in the
# shared worker/C API owner. No LLVM or input runtime enters this target.
add_library(neverd_gui_web STATIC
  web/ReviewJournal.cpp web/PackageSession.cpp web/FindingModel.cpp
  web/PackageReviewDialog.cpp)
target_link_libraries(neverd_gui_web PUBLIC neverd_gui_core Qt6::Widgets)

# The workbench: views, windows and the command set.
file(GLOB NEVERD_GUI_APP_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/app/*.cpp")
add_library(neverd_gui_app STATIC ${NEVERD_GUI_APP_SOURCES})
target_include_directories(neverd_gui_app PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/app"
  # Listing roles and address classes are shared with the worker, and
  # header-only tables such as the wide characters with the engine.
  "${CMAKE_CURRENT_SOURCE_DIR}/../neverd-worker"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../include"
  "${neverd_docking_SOURCE_DIR}/src")
target_link_libraries(neverd_gui_app PUBLIC neverd_gui_core neverd_gui_web Qt6::Widgets Qt6::Svg
  Qt6::Sql Qt6::Concurrent KDAB::kddockwidgets NeverDProjectDatabase)
# A window that draws its own frame tells the Wayland compositor where its
# shadow ends through Qt's private native window interface.
target_link_libraries(neverd_gui_app PRIVATE Qt6::GuiPrivate)
# How a window loses the system's title bar differs by platform
# (app/WindowFramePlatform.h).
if(WIN32)
  target_sources(neverd_gui_app PRIVATE app/platform/WindowFrameWindows.cpp)
  target_link_libraries(neverd_gui_app PRIVATE dwmapi)
elseif(APPLE)
  enable_language(OBJCXX)
  target_sources(neverd_gui_app PRIVATE app/platform/WindowFrameMac.mm)
  target_link_libraries(neverd_gui_app PRIVATE "-framework AppKit")
else()
  target_sources(neverd_gui_app PRIVATE app/platform/WindowFrameUnix.cpp)
endif()
file(GLOB NEVERD_GUI_ICONS CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/*.svg")
file(GLOB NEVERD_GUI_FONTS CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/resources/fonts/*.ttf"
  "${CMAKE_CURRENT_SOURCE_DIR}/resources/fonts/*.ttc"
  "${CMAKE_CURRENT_SOURCE_DIR}/resources/fonts/*-OFL.txt"
  "${CMAKE_CURRENT_SOURCE_DIR}/resources/fonts/README.md")
qt_add_resources(neverd_gui_app "workbench_assets" PREFIX /neverd
  BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources"
  FILES ${NEVERD_GUI_ICONS} ${NEVERD_GUI_FONTS}
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/theme.qss")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/../../docs/assets/neverd-logo-dark.svg"
  PROPERTIES QT_RESOURCE_ALIAS "icons/app.svg")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/../../docs/assets/neverd-logo-light.svg"
  PROPERTIES QT_RESOURCE_ALIAS "brand/logo-light.svg")
qt_add_resources(neverd_gui_app "brand_assets" PREFIX /neverd
  FILES "${CMAKE_CURRENT_SOURCE_DIR}/../../docs/assets/neverd-logo-dark.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../docs/assets/neverd-logo-light.svg")

qt_add_executable(neverd-gui WIN32 MACOSX_BUNDLE main.cpp)
target_link_libraries(neverd-gui PRIVATE neverd_gui_app)
add_custom_command(TARGET neverd-gui POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:neverd-gui>/licenses/sqlite"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/../../LICENSES/sqlite/SOURCE.md"
    "$<TARGET_FILE_DIR:neverd-gui>/licenses/sqlite")
file(GLOB NEVERD_GUI_FONT_NOTICES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/resources/fonts/*-OFL.txt"
  "${CMAKE_CURRENT_SOURCE_DIR}/resources/fonts/README.md")
add_custom_command(TARGET neverd-gui POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:neverd-gui>/licenses/fonts"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${NEVERD_GUI_FONT_NOTICES}
    "$<TARGET_FILE_DIR:neverd-gui>/licenses/fonts")
if(APPLE)
  install(FILES ${NEVERD_GUI_FONT_NOTICES}
    DESTINATION "neverd-gui.app/Contents/Resources/licenses/fonts")
else()
  install(FILES ${NEVERD_GUI_FONT_NOTICES} DESTINATION share/neverd/licenses/fonts)
endif()

file(GLOB NEVERD_TRANSLATIONS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/i18n/*.ts")
if(NEVERD_TRANSLATIONS)
  qt_add_translations(neverd-gui TS_FILES ${NEVERD_TRANSLATIONS}
    SOURCE_TARGETS neverd-gui neverd_gui_app neverd_gui_core neverd_gui_web
    QM_FILES_OUTPUT_VARIABLE NEVERD_QM_FILES)
  qt_add_resources(neverd-gui "app_translations" PREFIX /i18n
    BASE "${CMAKE_CURRENT_BINARY_DIR}" FILES ${NEVERD_QM_FILES})
endif()
# The MCP server ships beside the workbench, whose sessions it can attach to.
add_dependencies(neverd-gui neverd-worker neverd-mcp)
install(TARGETS neverd-gui RUNTIME DESTINATION bin BUNDLE DESTINATION .)
qt_generate_deploy_app_script(TARGET neverd-gui OUTPUT_SCRIPT deploy_script
  NO_UNSUPPORTED_PLATFORM_ERROR)
install(SCRIPT ${deploy_script})
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/NeverDDesktopPackaging.cmake")

if(BUILD_TESTING)
  option(NEVERD_GUI_CI_DIAGNOSTICS
    "Write file diagnostics for GitHub Actions GUI unit and controller test runners" OFF)
  set(_neverd_controller_test_log_args)
  set(_neverd_units_test_log_args)
  if(NEVERD_GUI_CI_DIAGNOSTICS)
    if(NOT "$ENV{CI}" STREQUAL "true" OR
       NOT "$ENV{GITHUB_ACTIONS}" STREQUAL "true")
      message(FATAL_ERROR "GUI contract diagnostics require GitHub Actions")
    endif()
    set(_neverd_gui_ci_log_dir "${CMAKE_CURRENT_BINARY_DIR}/ci-diagnostics")
    file(MAKE_DIRECTORY "${_neverd_gui_ci_log_dir}")
    set(_neverd_controller_test_log_args
      -o "${_neverd_gui_ci_log_dir}/controller.txt,txt"
      -o "${_neverd_gui_ci_log_dir}/controller.xml,xml"
      -o "-,txt")
    # A crash loses what a pipe buffered; the file keeps each line.
    set(_neverd_units_test_log_args
      -o "${_neverd_gui_ci_log_dir}/units.txt,txt"
      -o "-,txt")
  endif()
  find_package(Qt6 6.8 REQUIRED COMPONENTS Test)
  if(NEVERD_WORKER_REAL_ENGINE_TESTS)
    qt_add_executable(neverd-gui-comparison-tests tests/ComparisonTests.cpp)
    target_link_libraries(neverd-gui-comparison-tests PRIVATE neverd_gui_app Qt6::Test)
    target_compile_definitions(neverd-gui-comparison-tests PRIVATE
      TEST_COMPARISON_WORKER="$<TARGET_FILE:neverd-worker>")
    add_dependencies(neverd-gui-comparison-tests neverd-worker)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
      set(_neverd_diff_effect_fixture "${CMAKE_CURRENT_BINARY_DIR}/comparison-effects.so")
      set(_neverd_diff_effect_source "${CMAKE_CURRENT_SOURCE_DIR}/../../unittests/diff/call_effects_fixture.c")
      add_custom_command(OUTPUT "${_neverd_diff_effect_fixture}"
        COMMAND "${CMAKE_C_COMPILER}" -shared -nostdlib -fPIC -O1 -fno-builtin
          -fno-asynchronous-unwind-tables -fno-stack-protector
          "${_neverd_diff_effect_source}"
          -o "${_neverd_diff_effect_fixture}"
        DEPENDS "${_neverd_diff_effect_source}"
        VERBATIM)
      add_custom_target(neverd-gui-comparison-effects-fixture DEPENDS "${_neverd_diff_effect_fixture}")
      add_dependencies(neverd-gui-comparison-tests neverd-gui-comparison-effects-fixture)
      target_compile_definitions(neverd-gui-comparison-tests PRIVATE
        TEST_COMPARISON_EFFECT_FIXTURE="${_neverd_diff_effect_fixture}")
    endif()
    set_target_properties(neverd-gui-comparison-tests PROPERTIES WIN32_EXECUTABLE FALSE)
    add_test(NAME NeverDGuiComparison COMMAND neverd-gui-comparison-tests)
    set_tests_properties(NeverDGuiComparison PROPERTIES
      ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 120 LABELS "NeverDBinaryDiff")
  endif()

  if(NEVERD_WORKER_WEB_ENGINE_TESTS)
    qt_add_executable(neverd-gui-web-tests tests/WebReviewTests.cpp)
    target_link_libraries(neverd-gui-web-tests PRIVATE neverd_gui_app Qt6::Test)
    target_include_directories(neverd-gui-web-tests PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/../../unittests/web")
    target_compile_definitions(neverd-gui-web-tests PRIVATE
      TEST_WEB_WORKER="$<TARGET_FILE:neverd-worker>")
    add_dependencies(neverd-gui-web-tests neverd-worker)
    add_test(NAME NeverDGuiWebReview COMMAND neverd-gui-web-tests)
    set_tests_properties(NeverDGuiWebReview PROPERTIES
      ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 120)
    set_target_properties(neverd-gui-web-tests PROPERTIES WIN32_EXECUTABLE FALSE)
  endif()
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  target_sources(neverd-gui PRIVATE tests/WidgetsProbe.cpp)
  target_compile_definitions(neverd-gui PRIVATE NEVERD_GUI_TEST_PROBES=1)
  target_link_libraries(neverd-gui PRIVATE Qt6::Test)

  qt_add_executable(neverd-gui-query-tests tests/QueryServiceTests.cpp QueryService.cpp)
  target_link_libraries(neverd-gui-query-tests PRIVATE Qt6::Core Qt6::Test)
  target_include_directories(neverd-gui-query-tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../include")
  add_test(NAME NeverDGuiQueries COMMAND neverd-gui-query-tests)
  set_tests_properties(NeverDGuiQueries PROPERTIES TIMEOUT 60)

  qt_add_executable(neverd-gui-library-view-tests tests/LibraryCodeViewTests.cpp)
  target_link_libraries(neverd-gui-library-view-tests PRIVATE neverd_gui_core Qt6::Test)
  add_test(NAME NeverDGuiLibraryView COMMAND neverd-gui-library-view-tests)
  set_tests_properties(NeverDGuiLibraryView PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)

  qt_add_executable(neverd-gui-unit-tests tests/WorkbenchUnitTests.cpp)
  qt_add_executable(neverd-gui-wheel-tests tests/WheelInputTests.cpp)
  target_link_libraries(neverd-gui-wheel-tests PRIVATE neverd_gui_app Qt6::Test)
  set_target_properties(neverd-gui-wheel-tests PROPERTIES WIN32_EXECUTABLE FALSE)
  add_test(NAME NeverDGuiWheelInput COMMAND neverd-gui-wheel-tests)
  set_tests_properties(NeverDGuiWheelInput PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
  qt_add_executable(neverd-gui-appearance-tests tests/AppearanceTests.cpp)
  target_link_libraries(neverd-gui-appearance-tests PRIVATE neverd_gui_app Qt6::Test)
  set_target_properties(neverd-gui-appearance-tests PROPERTIES WIN32_EXECUTABLE FALSE)
  add_test(NAME NeverDGuiAppearance COMMAND neverd-gui-appearance-tests)
  set_tests_properties(NeverDGuiAppearance PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
  qt_add_executable(neverd-gui-x87-tests tests/X87CapabilitiesTests.cpp)
  target_link_libraries(neverd-gui-x87-tests PRIVATE neverd_gui_app Qt6::Test)
  set_target_properties(neverd-gui-x87-tests PROPERTIES WIN32_EXECUTABLE FALSE)
  if(NEVERD_WORKER_REAL_ENGINE_TESTS)
    target_compile_definitions(neverd-gui-x87-tests PRIVATE TEST_REAL_WORKER="$<TARGET_FILE:neverd-worker>")
    add_dependencies(neverd-gui-x87-tests neverd-worker)
  endif()
  add_test(NAME NeverDGuiX87Capabilities COMMAND neverd-gui-x87-tests)
  set_tests_properties(NeverDGuiX87Capabilities PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
  qt_add_executable(neverd-gui-unwind-tests tests/UnwindInfoTests.cpp)
  target_link_libraries(neverd-gui-unwind-tests PRIVATE neverd_gui_app Qt6::Test)
  set_target_properties(neverd-gui-unwind-tests PROPERTIES WIN32_EXECUTABLE FALSE)
  add_test(NAME NeverDGuiUnwindInfo COMMAND neverd-gui-unwind-tests)
  # Reuse the existing read-only inspection dialog test environment/budget.
  get_property(_neverd_gui_inspection_timeout TEST NeverDGuiX87Capabilities
    PROPERTY TIMEOUT)
  get_property(_neverd_gui_inspection_environment TEST NeverDGuiX87Capabilities
    PROPERTY ENVIRONMENT)
  set_tests_properties(NeverDGuiUnwindInfo PROPERTIES
    ENVIRONMENT "${_neverd_gui_inspection_environment}"
    TIMEOUT "${_neverd_gui_inspection_timeout}")
  if(NEVERD_WORKER_REAL_ENGINE_TESTS)
    qt_add_executable(neverd-gui-source-types-tests tests/SourceVariableTypesTests.cpp)
    target_link_libraries(neverd-gui-source-types-tests PRIVATE neverd_gui_app Qt6::Test)
    target_compile_definitions(neverd-gui-source-types-tests PRIVATE TEST_REAL_WORKER="$<TARGET_FILE:neverd-worker>")
    add_dependencies(neverd-gui-source-types-tests neverd-worker)
    set_target_properties(neverd-gui-source-types-tests PROPERTIES WIN32_EXECUTABLE FALSE)
    add_test(NAME NeverDGuiSourceVariableTypes COMMAND neverd-gui-source-types-tests)
    set_tests_properties(NeverDGuiSourceVariableTypes PROPERTIES
      ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 180)
  endif()
  target_link_libraries(neverd-gui-unit-tests PRIVATE neverd_gui_app Qt6::Test)
  # A console program on Windows too, so CTest records what failed.
  set_target_properties(neverd-gui-unit-tests PROPERTIES WIN32_EXECUTABLE FALSE)
  add_test(NAME NeverDGuiUnits COMMAND neverd-gui-unit-tests
    ${_neverd_units_test_log_args})
  set_tests_properties(NeverDGuiUnits PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)

  qt_add_executable(neverd-gui-docking-tests tests/DockingTests.cpp)
  target_link_libraries(neverd-gui-docking-tests PRIVATE neverd_gui_app Qt6::Test)
  if(APPLE)
    target_sources(neverd-gui-docking-tests PRIVATE
      tests/platform/NativeWindowStateMac.mm)
    target_sources(neverd-gui-unit-tests PRIVATE
      tests/platform/NativeWindowStateMac.mm)
    # Cocoa window moving/resizing needs real native pointer events, not
    # synthetic Qt events without a current NSEvent.
    target_link_libraries(neverd-gui-docking-tests PRIVATE "-framework ApplicationServices")
  endif()
  set_target_properties(neverd-gui-docking-tests PROPERTIES WIN32_EXECUTABLE FALSE)
  add_test(NAME NeverDGuiDocking COMMAND neverd-gui-docking-tests)
  set_tests_properties(NeverDGuiDocking PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)

  if(TARGET neverd-worker-test-server)
    qt_add_executable(neverd-gui-symbol-tests tests/MicrosoftSymbolsTests.cpp)
    target_link_libraries(neverd-gui-symbol-tests PRIVATE neverd_gui_app Qt6::Test)
    target_compile_definitions(neverd-gui-symbol-tests PRIVATE
      TEST_WORKER="$<TARGET_FILE:neverd-worker-test-server>")
    add_dependencies(neverd-gui-symbol-tests neverd-worker-test-server)
    set_target_properties(neverd-gui-symbol-tests PROPERTIES WIN32_EXECUTABLE FALSE)
    add_test(NAME NeverDGuiMicrosoftSymbols COMMAND neverd-gui-symbol-tests)
    set_tests_properties(NeverDGuiMicrosoftSymbols PROPERTIES
      ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)

    qt_add_executable(neverd-gui-tests tests/WorkbenchTests.cpp)
    target_link_libraries(neverd-gui-tests PRIVATE neverd_gui_app Qt6::Test)
    target_compile_definitions(neverd-gui-tests PRIVATE
      TEST_WORKER="$<TARGET_FILE:neverd-worker-test-server>"
      TEST_GUI="$<TARGET_FILE:neverd-gui>")
    add_dependencies(neverd-gui-tests neverd-worker-test-server neverd-gui)
    add_test(NAME NeverDGuiController COMMAND neverd-gui-tests
      ${_neverd_controller_test_log_args})
    # The full controller inventory takes several minutes on hosted runners;
    # individual waits still use the test's own bounded operation deadlines.
    set_tests_properties(NeverDGuiController PROPERTIES
      ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 600)
    qt_add_executable(neverd-gui-patch-tests tests/PatchTests.cpp)
    target_link_libraries(neverd-gui-patch-tests PRIVATE neverd_gui_app Qt6::Test)
    target_compile_definitions(neverd-gui-patch-tests PRIVATE
      TEST_WORKER="$<TARGET_FILE:neverd-worker-test-server>")
    add_dependencies(neverd-gui-patch-tests neverd-worker-test-server)
    add_test(NAME NeverDGuiPatchKeys COMMAND neverd-gui-patch-tests
      allSecondKeysOwnNativeShortcutDispatch cancelledAndRepeatedKeysNeverPatch
      secondKeyPatchesThroughTheNativePopup patchEditorDismissalDoesNotApply
      patchEditorKeepsFirstPaintGeometry patchEditorDragsAndRetainsPosition
      patchPopupFollowsItsOrigin patchHistoryKeepsUnchangedFunctionRows
      patchColorsStayVisibleAcrossUnrelatedEdits)
    set_tests_properties(NeverDGuiPatchKeys PROPERTIES
      ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 180)
    if(NEVERD_WORKER_REAL_ENGINE_TESTS)
      add_dependencies(neverd-gui-patch-tests neverd-worker)
      add_test(NAME NeverDGuiNativePatchKeys COMMAND neverd-gui-patch-tests
        nativeInstructionKeysApplyUndoAndRedo)
      set_tests_properties(NeverDGuiNativePatchKeys PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;NEVERD_PATCH_NATIVE_WORKER=$<TARGET_FILE:neverd-worker>"
        TIMEOUT 180)
    endif()
    add_test(NAME NeverDGuiWidgets COMMAND neverd-gui --widgets-test
      --worker $<TARGET_FILE:neverd-worker-test-server>)
    set_tests_properties(NeverDGuiWidgets PROPERTIES
      ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
  endif()

  qt_add_executable(neverd-mcp-qt-tests mcp/tests/test_mcp.cpp)
  target_link_libraries(neverd-mcp-qt-tests PRIVATE neverd_gui_core Qt6::Test)
  target_include_directories(neverd-mcp-qt-tests PRIVATE mcp)
  target_compile_definitions(neverd-mcp-qt-tests PRIVATE
    TEST_PYTHON="${Python3_EXECUTABLE}" TEST_SERVER="${CMAKE_CURRENT_SOURCE_DIR}/mcp/tests/fake_mcp.py")
  add_test(NAME NeverDMcpQt COMMAND neverd-mcp-qt-tests)
  set_tests_properties(NeverDMcpQt PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_FORCE_STDERR_LOGGING=1" TIMEOUT 30)
  if(NEVERD_WORKER_REAL_ENGINE_TESTS)
    add_test(NAME NeverDGuiAnalysis COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/analysis_probe_test.py"
      "$<TARGET_FILE:neverd-gui>" "$<TARGET_FILE:neverd_shared>")
    set_tests_properties(NeverDGuiAnalysis PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60 SKIP_RETURN_CODE 77)
  endif()
  add_test(NAME NeverDGuiSmoke COMMAND neverd-gui --smoke-test)
  set_tests_properties(NeverDGuiSmoke PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 15)
  add_test(NAME NeverDGuiLinksCLI COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/link_cli_test.py" "$<TARGET_FILE:neverd-gui>")
  set_tests_properties(NeverDGuiLinksCLI PROPERTIES TIMEOUT 120)
  if(NEVERD_TRANSLATIONS)
    qt_add_executable(neverd-gui-language-tests tests/LanguageSwitchTests.cpp)
    target_link_libraries(neverd-gui-language-tests PRIVATE neverd_gui_app Qt6::Test)
    qt_add_resources(neverd-gui-language-tests "test_translations" PREFIX /i18n
      BASE "${CMAKE_CURRENT_BINARY_DIR}" FILES ${NEVERD_QM_FILES})
    add_test(NAME NeverDGuiLanguages COMMAND neverd-gui-language-tests)
    set_tests_properties(NeverDGuiLanguages PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
  endif()
  # These are console test runners, even though they exercise Qt GUI objects.
  # Keep their diagnostics connected to CTest's stdout on Windows.
  foreach(_neverd_gui_test IN ITEMS neverd-gui-tests neverd-gui-unit-tests
      neverd-gui-patch-tests
      neverd-gui-library-view-tests neverd-gui-query-tests neverd-mcp-qt-tests
      neverd-gui-language-tests)
    if(TARGET ${_neverd_gui_test})
      set_target_properties(${_neverd_gui_test} PROPERTIES WIN32_EXECUTABLE FALSE)
    endif()
  endforeach()
endif()
