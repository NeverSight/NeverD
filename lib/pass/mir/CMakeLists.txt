add_neverd_component_library(NeverDPassMIR
  MIRPassRunner.cpp
  MIRPassRunnerRewrite.cpp
  NOPPass.cpp

  LINK_COMPONENTS
  IR
)
