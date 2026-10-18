# The type libraries of system headers (HeaderLibraries.def), embedded so
# that reading one reads no file at run time, each in a unit of its own
# (HeaderLibrarySource.cpp.in): the largest compiles in about 5 s and 0.6 GB.
set(NEVERD_HEADER_LIBRARIES
  WindowsKernelX64 WindowsUserX64 WindowsKernelX86 WindowsUserX86
  WindowsUserARM64 LinuxX64 LinuxX86 LinuxARM64 LinuxARM)
set(NEVERD_HEADER_LIBRARY_SOURCES)
foreach(NEVERD_HEADER_LIBRARY IN LISTS NEVERD_HEADER_LIBRARIES)
  set(Source "${CMAKE_CURRENT_SOURCE_DIR}/headers/${NEVERD_HEADER_LIBRARY}.h")
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
                              "${Embedded}")
  list(APPEND NEVERD_HEADER_LIBRARY_SOURCES "${Unit}" "${Embedded}")
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
  TypeJSON.cpp
  TypeLayout.cpp
  TypeRender.cpp
  ViewPlan.cpp
  ${NEVERD_HEADER_LIBRARY_SOURCES}
)

target_include_directories(NeverDTypes PRIVATE
                           "${CMAKE_CURRENT_BINARY_DIR}/headers")
