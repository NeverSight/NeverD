# Windows driver execution and its kernel environment share the existing
# component target; each implementation directory owns its source inventory.
add_neverd_component_library(NeverDEmulation
  LINK_COMPONENTS Loader DebugInfo EmulationCPU EmulationABI EmulationRuntime EmulationImage EmulationWindowsProcess EmulationWindowsException EmulationWorkload)
target_sources(NeverDEmulation PRIVATE WindowsUnicode.cpp)

add_subdirectory(driver)
add_subdirectory(kernel)

if(WIN32)
  target_compile_definitions(NeverDEmulation PRIVATE NOMINMAX)
endif()
