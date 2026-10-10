# Identification and image rebuilding are always available. Recovery observes
# a guest process, so it exists only where CPU execution is built.
#
# core/     orchestration and the format registry
# arch/     instruction forms justified by execution evidence
# format/   one directory per executable container
# dynamic/  guest process observation, per instruction set and guest system
set(_unpack_sources
  arch/X64Imports.cpp
  core/Identify.cpp
  core/Image.cpp
  core/Registry.cpp
  core/Unpack.cpp
  core/UnpackJSON.cpp
  core/UnpackNames.cpp
  format/pe/PEDelayImports.cpp
  format/pe/PEEntry.cpp
  format/pe/PEFormat.cpp
  format/pe/PEImage.cpp
  format/pe/PERebuild.cpp
  format/pe/PERuntime.cpp
  format/pe/PETLS.cpp)
set(_unpack_components Support)
if(NEVERD_ENABLE_CPU_EMULATION OR NEVERD_ENABLE_DRIVER_EMULATION)
  list(APPEND _unpack_sources
    dynamic/Platform.cpp
    dynamic/ProcessImports.cpp
    dynamic/ProcessTransfer.cpp
    dynamic/RuntimeState.cpp
    os/windows/RuntimeState.cpp)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/os/windows/NativeRuntime.c")
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/os/windows/NativeRuntime.c" _runtime_source)
  configure_file(os/windows/RuntimeSource.h.in RuntimeSource.h @ONLY)
  list(APPEND _unpack_components EmulationProcess)
  if(NEVERD_ENABLE_DRIVER_EMULATION)
    list(APPEND _unpack_components Emulation)
  endif()
endif()

add_neverd_component_library(NeverDUnpack
  ${_unpack_sources}
  LINK_COMPONENTS ${_unpack_components}
  LLVM_COMPONENTS Object Support)

if(NEVERD_ENABLE_CPU_EMULATION OR NEVERD_ENABLE_DRIVER_EMULATION)
  target_compile_definitions(NeverDUnpack PRIVATE NEVERD_UNPACK_EXECUTION=1)
  target_include_directories(NeverDUnpack PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
endif()
if(WIN32)
  target_compile_definitions(NeverDUnpack PRIVATE NOMINMAX)
endif()
if(NEVERD_ENABLE_DRIVER_EMULATION)
  target_compile_definitions(NeverDUnpack PRIVATE NEVERD_UNPACK_DRIVER_EXECUTION=1)
endif()
