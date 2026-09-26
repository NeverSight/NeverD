add_neverd_component_library(NeverDLibC
  LibCCallTraits.cpp
  LibCNames.cpp

  # No-return target lookup uses BinaryImage import-thunk decoding.
  LINK_COMPONENTS
  Loader
)
