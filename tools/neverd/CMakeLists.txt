add_neverd_tool(neverd
  neverd.cpp
  NeverDCLIOptions.cpp
  NeverDTranslateOutput.cpp
  cmd/NeverDCmdInfo.cpp
  cmd/NeverDCmdTables.cpp
  cmd/NeverDCmdDisasm.cpp
  cmd/NeverDCmdMarkup.cpp
  cmd/NeverDCmdSearch.cpp
  cmd/NeverDCmdExport.cpp
  cmd/NeverDCmdPipeline.cpp
  cmd/NeverDCmdSimplify.cpp
  cmd/NeverDCmdOptimizeIR.cpp
  cmd/NeverDCmdTranslate.cpp
  cmd/NeverDCmdSafety.cpp
  cmd/NeverDCmdConcolic.cpp
  cmd/NeverDCmdMobile.cpp

  LINK_LIBS
  neverd_shared
)

# The CLI uses internal support helpers that are not part of libneverd's C ABI.
# ELF shared-library links only retain archive objects needed by the library
# itself, and Windows does not auto-export them.  Link the implementation into
# the CLI on both platforms.  macOS instead resolves support helpers through
# the dylib to preserve its single LLVM command-line registry.
if(NOT APPLE)
  target_link_libraries(neverd PRIVATE NeverDSupport)
endif()

# Refresh helpers even if only Python sources changed. Keep the runtime beside
# the executable for relocatable build artifacts and multi-config generators.
add_custom_target(neverd-mobile-runtime
  COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:neverd>/mobile
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/mobile/__init__.py
          ${CMAKE_CURRENT_SOURCE_DIR}/mobile/__main__.py
          ${CMAKE_CURRENT_SOURCE_DIR}/mobile/common.py
          ${CMAKE_CURRENT_SOURCE_DIR}/mobile/driver.py
          ${CMAKE_CURRENT_SOURCE_DIR}/mobile/android.py
          ${CMAKE_CURRENT_SOURCE_DIR}/mobile/dalvik_model.py
          ${CMAKE_CURRENT_SOURCE_DIR}/mobile/dalvik_dex.py
          ${CMAKE_CURRENT_SOURCE_DIR}/mobile/dalvik_smali.py
          ${CMAKE_CURRENT_SOURCE_DIR}/mobile/dalvik_java.py
          ${CMAKE_CURRENT_SOURCE_DIR}/mobile/ios.py
          ${CMAKE_CURRENT_SOURCE_DIR}/mobile/macho.py
          ${CMAKE_CURRENT_SOURCE_DIR}/mobile/objc_source.py
          ${CMAKE_CURRENT_SOURCE_DIR}/mobile/swift_signatures.py
          ${CMAKE_CURRENT_SOURCE_DIR}/mobile/swift_source.py
          ${CMAKE_CURRENT_SOURCE_DIR}/mobile/windows_job.py
          $<TARGET_FILE_DIR:neverd>/mobile
  COMMENT "Copying NeverD mobile runtime"
  VERBATIM
)
add_dependencies(neverd neverd-mobile-runtime)
