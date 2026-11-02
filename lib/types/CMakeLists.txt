# The type libraries of system headers (HeaderLibraries.def), embedded so
# that reading one reads no file at run time, each in a unit of its own
# (HeaderLibrarySource.cpp.in): the largest compiles in about 5 s and 0.6 GB.
set(NEVERD_HEADER_LIBRARIES
  WindowsKernelX64 WindowsUserX64 WindowsKernelX86 WindowsUserX86
  WindowsUserARM64 WindowsKernelARM64 LinuxX64 LinuxX86 LinuxARM64 LinuxARM
  AndroidX64 AndroidX86
  AndroidARM64 AndroidARM MuslX64 MuslX86 MuslARM64 MuslARM MacOSX64
  MacOSARM64 IOSARM64 FreeBSDX64 FreeBSDX86 FreeBSDARM64 FreeBSDARM UefiX64
  UefiX86 UefiARM64)
set(NEVERD_HEADER_LIBRARY_SOURCES)
set(NEVERD_WINDOWS_TYPES_DIRECTORY "" CACHE PATH
    "Local Microsoft SDK/WDK type libraries from generate_microsoft_windows_types.py")
if(NEVERD_WINDOWS_TYPES_DIRECTORY)
  set(WindowsManifest "${NEVERD_WINDOWS_TYPES_DIRECTORY}/manifest.json")
  file(READ "${WindowsManifest}" WindowsInputs)
  string(JSON WindowsProvider GET "${WindowsInputs}" provider)
  string(JSON WindowsArchitecture GET "${WindowsInputs}" architecture)
  string(JSON WindowsSchema GET "${WindowsInputs}" schema)
  string(JSON WindowsCount LENGTH "${WindowsInputs}" libraries)
  if(WindowsArchitecture STREQUAL "all")
    set(WindowsExpectedCount 6)
    set(WindowsSuffix "(X64|X86|ARM64)")
  elseif(WindowsArchitecture MATCHES "^(x64|x86|arm64)$")
    set(WindowsExpectedCount 2)
    string(TOUPPER "${WindowsArchitecture}" WindowsSuffix)
  else()
    message(FATAL_ERROR "Unsupported Microsoft type library architecture")
  endif()
  if((NOT WindowsProvider STREQUAL "Microsoft SDK/WDK" AND
      NOT WindowsProvider STREQUAL "Microsoft SDK/WDK with Wine compatibility declarations") OR
     NOT WindowsSchema EQUAL 1 OR NOT WindowsCount EQUAL WindowsExpectedCount)
    message(FATAL_ERROR "Invalid Microsoft type library manifest")
  endif()
  set(WindowsNames)
  string(JSON WindowsInputs SET "${WindowsInputs}" supplements "{}")
  math(EXPR WindowsLastIndex "${WindowsCount} - 1")
  foreach(Index RANGE 0 ${WindowsLastIndex})
    string(JSON Name GET "${WindowsInputs}" libraries ${Index} name)
    string(JSON Expected GET "${WindowsInputs}" libraries ${Index} sha256)
    if(NOT Name MATCHES "^Windows(Kernel|User)${WindowsSuffix}$" OR Name IN_LIST WindowsNames)
      message(FATAL_ERROR "Invalid or duplicate Microsoft type library: ${Name}")
    endif()
    list(APPEND WindowsNames "${Name}")
    set(Source "${NEVERD_WINDOWS_TYPES_DIRECTORY}/${Name}.h")
    file(SHA256 "${Source}" Actual)
    if(NOT Actual STREQUAL Expected)
      message(FATAL_ERROR "Changed Microsoft type library: ${Name}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${Source}")
    set(Compatibility "${CMAKE_CURRENT_SOURCE_DIR}/headers/${Name}.h")
    file(SHA256 "${Compatibility}" CompatibilityHash)
    string(JSON WindowsInputs SET "${WindowsInputs}" supplements "${Name}"
           "\"${CompatibilityHash}\"")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${Compatibility}")
  endforeach()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${WindowsManifest}")
  file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/neverd-windows-types-inputs.json"
       CONTENT "${WindowsInputs}\n" @ONLY)
else()
  file(REMOVE "${CMAKE_BINARY_DIR}/neverd-windows-types-inputs.json")
endif()
foreach(NEVERD_HEADER_LIBRARY IN LISTS NEVERD_HEADER_LIBRARIES)
  set(Source "${CMAKE_CURRENT_SOURCE_DIR}/headers/${NEVERD_HEADER_LIBRARY}.h")
  set(NEVERD_HEADER_SUPPLEMENT "")
  set(Supplement)
  if(NEVERD_WINDOWS_TYPES_DIRECTORY AND NEVERD_HEADER_LIBRARY IN_LIST WindowsNames)
    set(Supplement "${CMAKE_CURRENT_BINARY_DIR}/headers/${NEVERD_HEADER_LIBRARY}Supplement.inc")
    add_custom_command(
      OUTPUT "${Supplement}"
      COMMAND "${CMAKE_COMMAND}" "-DINPUT=${Source}" "-DOUTPUT=${Supplement}"
              -P "${PROJECT_SOURCE_DIR}/cmake/NeverDEmbedBytes.cmake"
      DEPENDS "${Source}" "${PROJECT_SOURCE_DIR}/cmake/NeverDEmbedBytes.cmake"
      COMMENT "Embedding the ${NEVERD_HEADER_LIBRARY} compatibility supplement"
      VERBATIM)
    set(NEVERD_HEADER_SUPPLEMENT "#include \"${NEVERD_HEADER_LIBRARY}Supplement.inc\"")
    set(Source "${NEVERD_WINDOWS_TYPES_DIRECTORY}/${NEVERD_HEADER_LIBRARY}.h")
  endif()
  set(Embedded
      "${CMAKE_CURRENT_BINARY_DIR}/headers/${NEVERD_HEADER_LIBRARY}.inc")
  set(Unit
      "${CMAKE_CURRENT_BINARY_DIR}/headers/${NEVERD_HEADER_LIBRARY}Source.cpp")
  add_custom_command(
    OUTPUT "${Embedded}"
    COMMAND "${CMAKE_COMMAND}" "-DINPUT=${Source}" "-DOUTPUT=${Embedded}"
            -P "${PROJECT_SOURCE_DIR}/cmake/NeverDEmbedBytes.cmake"
    DEPENDS "${Source}" "${PROJECT_SOURCE_DIR}/cmake/NeverDEmbedBytes.cmake"
    COMMENT "Embedding the ${NEVERD_HEADER_LIBRARY} type library"
    VERBATIM)
  configure_file(HeaderLibrarySource.cpp.in "${Unit}" @ONLY)
  set_source_files_properties("${Unit}" PROPERTIES OBJECT_DEPENDS
                              "${Embedded};${Supplement}")
  list(APPEND NEVERD_HEADER_LIBRARY_SOURCES "${Unit}" "${Embedded}" ${Supplement})
endforeach()

add_neverd_component_library(NeverDTypes
  CTypeParser.cpp
  CTypeRender.cpp
  DWARFTypeImport.cpp
  GoTypeParser.cpp
  GoTypeRender.cpp
  HeaderLibrary.cpp
  JavaScriptTypeRender.cpp
  LocalTypes.cpp
  LuaTypeRender.cpp
  PDBTypeImport.cpp
  PythonTypeRender.cpp
  RustTypeParser.cpp
  RustTypeRender.cpp
  TypeFit.cpp
  TypeImport.cpp
  TypeJSON.cpp
  TypeLayout.cpp
  TypeRender.cpp
  ViewPlan.cpp
  ${NEVERD_HEADER_LIBRARY_SOURCES}
)

target_include_directories(NeverDTypes PRIVATE
                           "${CMAKE_CURRENT_BINARY_DIR}/headers")
