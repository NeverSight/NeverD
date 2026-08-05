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
