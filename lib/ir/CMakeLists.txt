add_subdirectory(intrinsics)
add_subdirectory(low)
add_subdirectory(med)
add_subdirectory(high)

add_neverd_component_library(NeverDIR
  NdTypes.cpp
  TargetRegInfo.cpp

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
