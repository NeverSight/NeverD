add_neverd_component_library(NeverDDecode
  Decoder.cpp

  LINK_COMPONENTS
  IR
  Lift
)

# Decoder.cpp contains the native AArch64 operand decoder in its hot path.
# Keeping this target at -O0 in a Debug tree makes the benchmark measure the
# build mode rather than the decoder and leaves normal debug runs needlessly
# bottlenecked on instruction dispatch.  As with the Capstone DFA target, keep
# the decode engine optimized in every configuration; debug information is
# still emitted by Debug/RelWithDebInfo builds.
if(MSVC)
  target_compile_options(NeverDDecode PRIVATE /O2)
else()
  target_compile_options(NeverDDecode PRIVATE -O3 -fomit-frame-pointer)
endif()
