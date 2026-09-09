cmake_minimum_required(VERSION 3.24)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(NeverDDesktop VERSION 3389.0.1 LANGUAGES CXX)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
  include(CTest)
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../neverd-worker" "${CMAKE_BINARY_DIR}/worker")
endif()

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Network LinguistTools Svg)
# Qt 6.8 exports private targets from the public module packages. Newer Qt
# releases ship separate private packages; KDDockWidgets needs the matching
# private headers in either layout.
foreach(_neverd_private_module Gui Quick)
  if(NOT TARGET Qt6::${_neverd_private_module}Private)
    find_package(Qt6 6.8 REQUIRED COMPONENTS ${_neverd_private_module}Private)
  endif()
endforeach()
qt_standard_project_setup(REQUIRES 6.8)
include(FetchContent)
set(KDDockWidgets_FRONTENDS "qtquick" CACHE STRING "" FORCE)
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

add_library(neverd_gui_core STATIC EngineClient.cpp PageModel.cpp Workbench.cpp
  mcp/McpConnectionManager.cpp mcp/GuiSessionBroker.cpp)
target_include_directories(neverd_gui_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(neverd_gui_core PUBLIC cxx_std_20)
target_link_libraries(neverd_gui_core PUBLIC Qt6::Core Qt6::Gui Qt6::Qml Qt6::Network)

qt_add_executable(neverd-gui main.cpp NativeCodeHighlighter.cpp NativeGraphItem.cpp)
target_link_libraries(neverd-gui PRIVATE neverd_gui_core Qt6::Quick Qt6::QuickControls2 Qt6::Svg KDAB::kddockwidgets)
file(GLOB NEVERD_QML_FILES CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" "qml/*.qml")
set_source_files_properties(qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
qt_add_qml_module(neverd-gui URI NeverD.Workbench VERSION 1.0 QML_FILES ${NEVERD_QML_FILES})
qt_add_resources(neverd-gui "brand_assets" PREFIX /brand
  BASE "${CMAKE_CURRENT_SOURCE_DIR}/../../docs/assets"
  FILES "${CMAKE_CURRENT_SOURCE_DIR}/../../docs/assets/neverd-logo-dark.svg")

file(GLOB NEVERD_TRANSLATIONS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/i18n/*.ts")
if(NEVERD_TRANSLATIONS)
  qt_add_translations(neverd-gui TS_FILES ${NEVERD_TRANSLATIONS}
    QM_FILES_OUTPUT_VARIABLE NEVERD_QM_FILES)
  qt_add_resources(neverd-gui "app_translations" PREFIX /i18n
    BASE "${CMAKE_CURRENT_BINARY_DIR}" FILES ${NEVERD_QM_FILES})
endif()
add_dependencies(neverd-gui neverd-worker)
install(TARGETS neverd-gui RUNTIME DESTINATION bin BUNDLE DESTINATION .)
# Qt's deployment script gathers QML plugins as well as the native libraries.
qt_generate_deploy_qml_app_script(TARGET neverd-gui OUTPUT_SCRIPT deploy_script NO_UNSUPPORTED_PLATFORM_ERROR)
install(SCRIPT ${deploy_script})
install(PROGRAMS "${CMAKE_CURRENT_SOURCE_DIR}/../neverd-mcp/neverd-mcp" DESTINATION bin)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../neverd-mcp/server.py"
              "${CMAKE_CURRENT_SOURCE_DIR}/../neverd-mcp/transport.py" DESTINATION bin)

if(BUILD_TESTING)
  target_sources(neverd-gui PRIVATE tests/DockingProbe.cpp tests/AnalysisProbe.cpp)
  target_compile_definitions(neverd-gui PRIVATE NEVERD_GUI_TEST_PROBES=1)
  find_package(Qt6 6.8 REQUIRED COMPONENTS Test)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  if(TARGET neverd-worker-test-server)
    qt_add_executable(neverd-gui-tests tests/WorkbenchTests.cpp)
    target_link_libraries(neverd-gui-tests PRIVATE neverd_gui_core Qt6::Test)
    target_compile_definitions(neverd-gui-tests PRIVATE TEST_WORKER="$<TARGET_FILE:neverd-worker-test-server>")
    add_dependencies(neverd-gui-tests neverd-worker-test-server)
    add_test(NAME NeverDGuiController COMMAND neverd-gui-tests)
    set_tests_properties(NeverDGuiController PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
  endif()
  qt_add_executable(neverd-gui-graph-tests tests/NativeGraphItemTests.cpp NativeGraphItem.cpp)
  target_link_libraries(neverd-gui-graph-tests PRIVATE Qt6::Quick Qt6::Test)
  target_include_directories(neverd-gui-graph-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  add_test(NAME NeverDGuiGraph COMMAND neverd-gui-graph-tests)
  set_tests_properties(NeverDGuiGraph PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 15)
  qt_add_executable(neverd-gui-page-tests tests/PageModelRetryTests.cpp)
  target_link_libraries(neverd-gui-page-tests PRIVATE neverd_gui_core Qt6::Test)
  add_test(NAME NeverDGuiPageRetries COMMAND neverd-gui-page-tests)
  set_tests_properties(NeverDGuiPageRetries PROPERTIES TIMEOUT 15)
  qt_add_executable(neverd-mcp-qt-tests mcp/tests/test_mcp.cpp)
  target_link_libraries(neverd-mcp-qt-tests PRIVATE neverd_gui_core Qt6::Test)
  target_include_directories(neverd-mcp-qt-tests PRIVATE mcp)
  target_compile_definitions(neverd-mcp-qt-tests PRIVATE
    TEST_PYTHON="${Python3_EXECUTABLE}" TEST_SERVER="${CMAKE_CURRENT_SOURCE_DIR}/mcp/tests/fake_mcp.py")
  add_test(NAME NeverDMcpQt COMMAND neverd-mcp-qt-tests)
  set_tests_properties(NeverDMcpQt PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
  add_test(NAME NeverDMcpProtocol COMMAND "${Python3_EXECUTABLE}" -m unittest discover
    -s "${CMAKE_CURRENT_SOURCE_DIR}/../neverd-mcp/tests" -v)
  if(NEVERD_WORKER_REAL_ENGINE_TESTS)
    add_test(NAME NeverDGuiAnalysis COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/analysis_probe_test.py"
      "$<TARGET_FILE:neverd-gui>" "$<TARGET_FILE:neverd_shared>")
    set_tests_properties(NeverDGuiAnalysis PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30 SKIP_RETURN_CODE 77)
  endif()
  add_test(NAME NeverDGuiSmoke COMMAND neverd-gui --smoke-test)
  set_tests_properties(NeverDGuiSmoke PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 15)
  add_test(NAME NeverDGuiDocking COMMAND neverd-gui --docking-test)
  set_tests_properties(NeverDGuiDocking PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/LanguageSwitchTests.cpp")
    qt_add_executable(neverd-gui-language-tests tests/LanguageSwitchTests.cpp)
    target_link_libraries(neverd-gui-language-tests PRIVATE neverd_gui_core Qt6::Test)
    qt_add_resources(neverd-gui-language-tests "test_translations" PREFIX /i18n
      BASE "${CMAKE_CURRENT_BINARY_DIR}" FILES ${NEVERD_QM_FILES})
    add_test(NAME NeverDGuiLanguages COMMAND neverd-gui-language-tests)
    set_tests_properties(NeverDGuiLanguages PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 20)
  endif()
endif()
