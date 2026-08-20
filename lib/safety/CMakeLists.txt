add_neverd_component_library(NeverDSafety
  SafetyTypes.cpp
  SinkCatalog.cpp
  SinkScanner.cpp
  ArgSlicer.cpp
  ObjectModel.cpp
  HuntEngine.cpp
  AllocLifetime.cpp
  Safety.cpp

  LINK_COMPONENTS
  Symbolic
  Solver
  IR
  Loader
)
