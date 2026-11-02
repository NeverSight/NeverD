# Qt stays shared and ships beside the desktop product. macOS uses the
# existing bundle/signing helper; these rules complete Windows/Linux installs.
option(NEVERD_BUNDLE_DESKTOP_RUNTIME "Install the desktop's private runtime dependencies" ON)
if(NOT NEVERD_BUNDLE_DESKTOP_RUNTIME OR APPLE)
  return()
endif()
get_target_property(_neverd_engine_imported neverd_shared IMPORTED)
if(_neverd_engine_imported)
  install(IMPORTED_RUNTIME_ARTIFACTS neverd_shared
    RUNTIME DESTINATION bin LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
else()
  install(TARGETS neverd_shared RUNTIME DESTINATION bin LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
endif()
if(TARGET neverd)
  install(TARGETS neverd RUNTIME DESTINATION bin)
else()
  set(NEVERD_DESKTOP_CLI "" CACHE FILEPATH "Matching CLI executable to include in the desktop package")
  if(NEVERD_DESKTOP_CLI)
    if(NOT EXISTS "${NEVERD_DESKTOP_CLI}")
      message(FATAL_ERROR "NEVERD_DESKTOP_CLI does not exist")
    endif()
    install(PROGRAMS "${NEVERD_DESKTOP_CLI}" DESTINATION bin)
  endif()
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set_property(TARGET neverd-gui PROPERTY INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
endif()
get_filename_component(_neverd_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
install(FILES "${_neverd_root}/LICENSE" "${_neverd_root}/NOTICE"
  "${_neverd_root}/ATTRIBUTION.md" "${_neverd_root}/THIRD_PARTY_NOTICES.md"
  DESTINATION share/neverd)
install(DIRECTORY "$<TARGET_FILE_DIR:neverd_shared>/licenses/"
  DESTINATION share/neverd/licenses OPTIONAL)
install(DIRECTORY "${neverd_docking_SOURCE_DIR}/LICENSES/"
  DESTINATION share/neverd/licenses/KDDockWidgets)
install(FILES "${neverd_docking_SOURCE_DIR}/LICENSE.txt"
  "${neverd_docking_SOURCE_DIR}/3RDPARTY.md"
  DESTINATION share/neverd/licenses/KDDockWidgets)
set(_qt_notices "${_neverd_root}/tools/neverd-gui/licenses/Qt-${Qt6_VERSION}")
if(NOT IS_DIRECTORY "${_qt_notices}")
  set(_qt_notices "")
endif()
set(NEVERD_DESKTOP_QT_LICENSES "${_qt_notices}" CACHE PATH
  "License and attribution catalog matching the selected Qt kit")
set(NEVERD_DESKTOP_EXTRA_LICENSES "" CACHE PATH
  "Additional runtime dependency notices to include in the desktop package")
foreach(_kind QT EXTRA)
  if(NEVERD_DESKTOP_${_kind}_LICENSES)
    if(NOT IS_DIRECTORY "${NEVERD_DESKTOP_${_kind}_LICENSES}")
      message(FATAL_ERROR "NEVERD_DESKTOP_${_kind}_LICENSES must be a directory")
    endif()
    install(DIRECTORY "${NEVERD_DESKTOP_${_kind}_LICENSES}/"
      DESTINATION "share/neverd/licenses/${_kind}")
  endif()
endforeach()
# These libraries can be opened by name instead of appearing in ELF imports:
# FreeType loads HarfBuzz for auto-hinting; Qt's TLS backend loads OpenSSL.
set(_runtime_libraries)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_library(_neverd_desktop_harfbuzz NAMES harfbuzz libharfbuzz.so.0)
  find_library(_neverd_desktop_ssl NAMES ssl libssl.so.3)
  find_library(_neverd_desktop_crypto NAMES crypto libcrypto.so.3)
  foreach(_name harfbuzz ssl crypto)
    if(_neverd_desktop_${_name})
      list(APPEND _runtime_libraries "${_neverd_desktop_${_name}}")
    endif()
  endforeach()
endif()
set(NEVERD_DESKTOP_RUNTIME_LIBRARIES "${_runtime_libraries}" CACHE STRING
  "Additional libraries opened at runtime, including their dependencies")
set(_source_files "$<TARGET_FILE:neverd-gui>;$<TARGET_FILE:neverd-worker>;$<TARGET_FILE:neverd-mcp>;$<TARGET_FILE:neverd_shared>")
if(TARGET neverd)
  list(APPEND _source_files "$<TARGET_FILE:neverd>")
elseif(NEVERD_DESKTOP_CLI)
  list(APPEND _source_files "${NEVERD_DESKTOP_CLI}")
endif()
qt6_generate_deploy_script(TARGET neverd-gui OUTPUT_SCRIPT _neverd_runtime_script
  CONTENT "
set(NEVERD_DEPLOY_SYSTEM \"${CMAKE_SYSTEM_NAME}\")
set(NEVERD_DEPLOY_CONFIG \"$<CONFIG>\")
set(NEVERD_DEPLOY_EXECUTABLE_SUFFIX \"${CMAKE_EXECUTABLE_SUFFIX}\")
set(NEVERD_DEPLOY_SEARCH_DIRS \"$<TARGET_FILE_DIR:Qt6::Core>\" \"$<TARGET_FILE_DIR:neverd_shared>\")
set(NEVERD_DEPLOY_RUNTIME_LIBRARIES \"${NEVERD_DESKTOP_RUNTIME_LIBRARIES}\")
set(NEVERD_DEPLOY_SOURCE_FILES \"${_source_files}\")
include(\"${CMAKE_CURRENT_LIST_DIR}/NeverDDesktopRuntime.cmake\")
")
install(SCRIPT "${_neverd_runtime_script}")
