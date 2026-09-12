add_subdirectory(intrinsics)
add_subdirectory(low)
add_subdirectory(med)
add_subdirectory(high)

add_neverd_component_library(NeverDIR
  NdTypes.cpp
  SourceABI.cpp
  TargetRegInfo.cpp
  TargetRegInfoAArch64.cpp
  TargetRegInfoARM.cpp
  TargetRegInfoX86.cpp

  LINK_COMPONENTS
  IRHigh
  IRMed
  IRLow
  IRIntrinsics
)

# HighIRPrint calls NdType::str(), which is implemented by the aggregate
# NeverDIR archive.  Record that static-library cycle explicitly so CMake
# repeats the archives for one-pass linkers such as GNU ld.
target_link_libraries(NeverDIRHigh PUBLIC NeverDIR)

# IRHigh's selector-stub query can first extract Loader's call-hint object on
# the second scan. That object then needs SourceABI from the preceding IR
# archive, so one-pass archive linkers need a third scan of this component.
set_property(TARGET NeverDIR PROPERTY LINK_INTERFACE_MULTIPLICITY 3)
