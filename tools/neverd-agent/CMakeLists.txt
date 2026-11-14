cmake_minimum_required(VERSION 3.24)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(NeverDAgent VERSION 0.1.0 LANGUAGES CXX)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
  include(CTest)
endif()

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Quick QuickControls2 Svg Concurrent Network
  LinguistTools)
qt_standard_project_setup(REQUIRES 6.8)

if(NOT TARGET neverd_tools)
  add_subdirectory("../../lib/tools" nd-tools)
endif()
if(NOT TARGET neverd_tool_json)
  add_subdirectory("../common/transport" nd-transport)
endif()

set(NEVERD_AGENT_SOURCES
  project/ProjectStore.cpp project/MemoryStore.cpp project/GoalState.cpp project/ConversationInput.cpp project/EvidenceStore.cpp
  platform/AppPaths.cpp platform/Translation.cpp platform/WorkbenchSettings.cpp
  models/ModelProfile.cpp models/Credentials.cpp models/ProviderCodec.cpp models/ModelClient.cpp
  runtime/NphTools.cpp runtime/McpSession.cpp runtime/ContextUsage.cpp runtime/AgentLoop.cpp runtime/ResearchRuntime.cpp runtime/ExecutionTrace.cpp runtime/EvidenceCapture.cpp
  runtime/WorkerSession.cpp runtime/ChangeReview.cpp runtime/MemoryLearner.cpp project/ResearchPlan.cpp)
set(NEVERD_AGENT_CONTROLLERS app/AgentController.cpp app/AgentModelController.cpp app/AgentInputController.cpp app/AgentEvidenceController.cpp app/AgentContextController.cpp
  app/AgentSideChatController.cpp app/AgentGoalController.cpp app/SideChat.cpp app/AgentReviewController.cpp app/AgentMemoryController.cpp app/AgentWorkbenchController.cpp)
# The CLI owns the runtime. Direct unit tests reuse the private build objects.
add_library(neverd-agent-objects OBJECT ${NEVERD_AGENT_CONTROLLERS} ${NEVERD_AGENT_SOURCES})
target_compile_features(neverd-agent-objects PUBLIC cxx_std_20)
target_include_directories(neverd-agent-objects PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
# Languages.def: the workbench's interface languages are the Agent's too.
target_include_directories(neverd-agent-objects PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../neverd-gui/app")
target_link_libraries(neverd-agent-objects PUBLIC Qt6::Core Qt6::Concurrent Qt6::Network neverd_tools neverd_tool_json)
if(WIN32)
  target_link_libraries(neverd-agent-objects PUBLIC crypt32)
elseif(APPLE)
  target_sources(neverd-agent-objects PRIVATE platform/KeyringMac.cpp)
  target_link_libraries(neverd-agent-objects PUBLIC "-framework Security" "-framework CoreFoundation")
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(AGENT_SECRET REQUIRED IMPORTED_TARGET libsecret-1)
  target_sources(neverd-agent-objects PRIVATE platform/KeyringUnix.cpp)
  target_link_libraries(neverd-agent-objects PUBLIC PkgConfig::AGENT_SECRET)
endif()
if(MSVC)
  target_compile_options(neverd-agent-objects PRIVATE /utf-8 /W4)
endif()
qt_add_executable(neverd-agent-cli AgentCli.cpp bridge/AgentWorkbench.cpp
  bridge/AgentWorkspace.cpp)
target_link_libraries(neverd-agent-cli PRIVATE neverd-agent-objects)
set_target_properties(neverd-agent-cli PROPERTIES WIN32_EXECUTABLE FALSE)
if(APPLE)
  set_property(TARGET neverd-agent-cli PROPERTY INSTALL_RPATH
    "@loader_path/../neverd-agent.app/Contents/Frameworks")
endif()
if(MSVC)
  target_compile_options(neverd-agent-cli PRIVATE /utf-8 /W4)
endif()
install(TARGETS neverd-agent-cli RUNTIME DESTINATION bin)
qt_add_executable(neverd-agent WIN32 MACOSX_BUNDLE
  main.cpp app/MarkdownDocument.cpp platform/AgentAppearance.cpp
  platform/AgentWindowFrame.cpp platform/AppPaths.cpp platform/Translation.cpp
  platform/WorkbenchSettings.cpp client/AgentProcess.cpp client/WorkspaceClient.cpp)
target_compile_features(neverd-agent PRIVATE cxx_std_20)
target_include_directories(neverd-agent PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(neverd-agent PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../neverd-gui/app")
target_link_libraries(neverd-agent PRIVATE Qt6::Quick Qt6::QuickControls2 Qt6::Svg)
add_dependencies(neverd-agent neverd-agent-cli)
if(MSVC)
  target_compile_options(neverd-agent PRIVATE /utf-8 /W4)
endif()

# Share the workbench's QWindow-only OS integration, without linking its widgets,
# analysis services or application. Frame rendering belongs to this QML shell.
# The theme choice "system" reads the desktop's contrast as the workbench does.
if(WIN32)
  target_sources(neverd-agent PRIVATE ../neverd-gui/app/platform/WindowFrameWindows.cpp
    ../neverd-gui/app/platform/DesktopContrastWindows.cpp)
  target_link_libraries(neverd-agent PRIVATE dwmapi)
elseif(APPLE)
  enable_language(OBJCXX)
  target_sources(neverd-agent PRIVATE ../neverd-gui/app/platform/WindowFrameMac.mm
    ../neverd-gui/app/platform/DesktopContrastMac.mm)
  target_link_libraries(neverd-agent PRIVATE "-framework AppKit")
else()
  if(NOT TARGET Qt6::GuiPrivate)
    find_package(Qt6 6.8 REQUIRED COMPONENTS GuiPrivate)
  endif()
  target_sources(neverd-agent PRIVATE ../neverd-gui/app/platform/WindowFrameUnix.cpp
    ../neverd-gui/app/platform/DesktopContrastUnix.cpp)
  target_link_libraries(neverd-agent PRIVATE Qt6::GuiPrivate)
endif()

set_source_files_properties(ui/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
qt_add_qml_module(neverd-agent
  URI NeverD.Agent
  VERSION 1.0
  QML_FILES ui/Main.qml ui/Theme.qml ui/ActionButton.qml ui/Icon.qml ui/Surface.qml
    ui/Field.qml ui/ProjectSidebar.qml ui/Conversation.qml ui/ChatComposer.qml ui/SidePanel.qml
    ui/WindowControl.qml ui/ChoiceField.qml ui/ModelSettings.qml ui/MarkdownView.qml ui/ContextMeter.qml ui/Backdrop.qml
    ui/ChatTranscript.qml ui/SideChatView.qml ui/GoalStrip.qml ui/GoalEditor.qml ui/InputQueue.qml ui/ObjectApprovalCard.qml ui/ExecutionView.qml ui/EvidenceView.qml ui/ContextPicker.qml ui/ContextChips.qml ui/ReviewView.qml ui/ResearchView.qml ui/MemoryView.qml ui/CheckField.qml
    ui/ThemedMenu.qml ui/ThemedMenuItem.qml ui/ThemedSeparator.qml ui/ThemedScrollBar.qml ui/ThemedToolTip.qml
    ui/ModalDim.qml)

# Interface translations: the workbench's languages (i18n/*.ts), compiled into
# the application and its CLI, which writes progress and errors.
file(GLOB NEVERD_AGENT_TRANSLATIONS CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/i18n/*.ts")
if(NEVERD_AGENT_TRANSLATIONS)
  qt_add_translations(neverd-agent TS_FILES ${NEVERD_AGENT_TRANSLATIONS}
    SOURCE_TARGETS neverd-agent neverd-agent-objects neverd-agent-cli
    LUPDATE_TARGET neverd-agent_lupdate LRELEASE_TARGET neverd-agent_lrelease
    QM_FILES_OUTPUT_VARIABLE NEVERD_AGENT_QM_FILES)
  qt_add_resources(neverd-agent "agent_translations" PREFIX /i18n
    BASE "${CMAKE_CURRENT_BINARY_DIR}" FILES ${NEVERD_AGENT_QM_FILES})
  qt_add_resources(neverd-agent-cli "agent_cli_translations" PREFIX /i18n
    BASE "${CMAKE_CURRENT_BINARY_DIR}" FILES ${NEVERD_AGENT_QM_FILES})
  add_dependencies(neverd-agent-cli neverd-agent_lrelease)
endif()

# Brand artwork is shared; action glyphs belong to ui/Icon.qml.
set(_agent_logo "${CMAKE_CURRENT_SOURCE_DIR}/../../docs/assets/neverd-logo-dark.svg")
set_source_files_properties("${_agent_logo}" PROPERTIES QT_RESOURCE_ALIAS "brand.svg")
qt_add_resources(neverd-agent agent_icons PREFIX "/agent/icons" FILES "${_agent_logo}")

set(_agent_font_dir "${CMAKE_CURRENT_SOURCE_DIR}/../neverd-gui/resources/fonts")
qt_add_resources(neverd-agent agent_fonts PREFIX "/agent/fonts" BASE "${_agent_font_dir}"
  FILES "${_agent_font_dir}/NotoSans.ttf" "${_agent_font_dir}/NotoSansCJK.ttc"
        "${_agent_font_dir}/JetBrainsMonoNL-Regular.ttf")
install(FILES "${_agent_font_dir}/NotoSans-OFL.txt" "${_agent_font_dir}/NotoSansCJK-OFL.txt"
  "${_agent_font_dir}/JetBrainsMonoNL-OFL.txt" DESTINATION share/neverd-agent/licenses/fonts)

set_target_properties(neverd-agent PROPERTIES
  MACOSX_BUNDLE_BUNDLE_NAME "NeverD Agent"
  MACOSX_BUNDLE_GUI_IDENTIFIER "org.neversight.neverd-agent")
install(TARGETS neverd-agent BUNDLE DESTINATION . RUNTIME DESTINATION bin)
set(_agent_deploy_options)
if(APPLE)
  list(APPEND _agent_deploy_options DEPLOY_TOOL_OPTIONS
    "-executable=\${QT_DEPLOY_PREFIX}/bin/$<TARGET_FILE_NAME:neverd-agent-cli>")
endif()
qt_generate_deploy_qml_app_script(TARGET neverd-agent
  OUTPUT_SCRIPT _agent_deploy_script NO_UNSUPPORTED_PLATFORM_ERROR
  ${_agent_deploy_options})
install(SCRIPT ${_agent_deploy_script})

if(BUILD_TESTING)
  find_package(Qt6 6.8 REQUIRED COMPONENTS Test)
  qt_add_executable(NeverDAgentFixtureMcp tests/FixtureMcp.cpp)
  target_link_libraries(NeverDAgentFixtureMcp PRIVATE Qt6::Core)
  qt_add_executable(NeverDAgentChangeReviewTests tests/ChangeReviewTests.cpp)
  target_link_libraries(NeverDAgentChangeReviewTests PRIVATE neverd-agent-objects Qt6::Test)
  add_test(NAME NeverDAgent.ChangeReview COMMAND NeverDAgentChangeReviewTests)
  set_tests_properties(NeverDAgent.ChangeReview PROPERTIES LABELS "NeverDAgent" TIMEOUT 180)
  qt_add_executable(NeverDAgentMarkdownTests tests/MarkdownTests.cpp app/MarkdownDocument.cpp)
  target_compile_features(NeverDAgentMarkdownTests PRIVATE cxx_std_20)
  target_include_directories(NeverDAgentMarkdownTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(NeverDAgentMarkdownTests PRIVATE Qt6::Quick Qt6::Gui Qt6::Test)
  if(MSVC)
    target_compile_options(NeverDAgentMarkdownTests PRIVATE /utf-8)
  endif()
  add_test(NAME NeverDAgent.Markdown COMMAND NeverDAgentMarkdownTests)
  set_tests_properties(NeverDAgent.Markdown PROPERTIES
    LABELS "NeverDAgent" ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
  qt_add_executable(NeverDAgentTests tests/ProjectStoreTests.cpp)
  target_compile_features(NeverDAgentTests PRIVATE cxx_std_20)
  target_include_directories(NeverDAgentTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(NeverDAgentTests PRIVATE neverd-agent-objects Qt6::Test)
  if(WIN32)
    target_link_libraries(NeverDAgentTests PRIVATE crypt32)
  endif()
  if(MSVC)
    target_compile_options(NeverDAgentTests PRIVATE /utf-8)
  endif()
  add_test(NAME NeverDAgent.Workspace COMMAND NeverDAgentTests)
  set_tests_properties(NeverDAgent.Workspace PROPERTIES LABELS "NeverDAgent")
  foreach(_target NeverDAgentModelTests NeverDAgentInteractionTests NeverDAgentMemoryTests NeverDAgentLiveRunner)
    if(_target STREQUAL "NeverDAgentModelTests")
      set(_source tests/ModelTests.cpp)
    elseif(_target STREQUAL "NeverDAgentInteractionTests")
      set(_source tests/InteractionTests.cpp)
    elseif(_target STREQUAL "NeverDAgentMemoryTests")
      set(_source tests/MemoryTests.cpp)
    else()
      set(_source tests/LiveRunner.cpp)
    endif()
    qt_add_executable(${_target} ${_source})
    target_compile_features(${_target} PRIVATE cxx_std_20)
    target_include_directories(${_target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(${_target} PRIVATE neverd-agent-objects Qt6::Test)
    if(WIN32)
      target_link_libraries(${_target} PRIVATE crypt32)
    endif()
    if(MSVC)
      target_compile_options(${_target} PRIVATE /utf-8)
    endif()
  endforeach()
  add_test(NAME NeverDAgent.Models COMMAND NeverDAgentModelTests)
  add_test(NAME NeverDAgent.Interactions COMMAND NeverDAgentInteractionTests)
  add_test(NAME NeverDAgent.Memories COMMAND NeverDAgentMemoryTests)
  set_tests_properties(NeverDAgent.Memories PROPERTIES LABELS "NeverDAgent" TIMEOUT 60)
  target_sources(NeverDAgentInteractionTests PRIVATE
    client/AgentProcess.cpp client/WorkspaceClient.cpp)
  target_link_libraries(NeverDAgentInteractionTests PRIVATE Qt6::Gui)
  add_dependencies(NeverDAgentInteractionTests NeverDAgentFixtureMcp neverd-agent-cli)
  set_tests_properties(NeverDAgent.Interactions PROPERTIES LABELS "NeverDAgent" TIMEOUT 60)
  set_tests_properties(NeverDAgent.Models PROPERTIES LABELS "NeverDAgent" TIMEOUT 60)
  if(WIN32)
    qt_add_executable(NeverDAgentFrameTests tests/WindowFrameTests.cpp
      platform/AgentWindowFrame.cpp ../neverd-gui/app/platform/WindowFrameWindows.cpp)
    target_compile_features(NeverDAgentFrameTests PRIVATE cxx_std_20)
    target_include_directories(NeverDAgentFrameTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
      "${CMAKE_CURRENT_SOURCE_DIR}/../neverd-gui/app")
    target_link_libraries(NeverDAgentFrameTests PRIVATE Qt6::Gui Qt6::Test dwmapi)
    if(MSVC)
      target_compile_options(NeverDAgentFrameTests PRIVATE /utf-8)
    endif()
    add_test(NAME NeverDAgent.WindowFrame COMMAND NeverDAgentFrameTests)
    set_tests_properties(NeverDAgent.WindowFrame PROPERTIES LABELS "NeverDAgent")
  endif()
endif()
