add_neverd_component_library(NeverDPipeline
  Pipeline.cpp
  PipelineDump.cpp
  PipelineFuncDetect.cpp
  PipelineOptimize.cpp
  PipelineReturnModeling.cpp

  LINK_COMPONENTS
  IR
  Decode
  Lift
  BackendLLVM
  DebugInfo
)
