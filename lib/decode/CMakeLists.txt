add_neverd_component_library(NeverDDecode
  Decoder.cpp

  LINK_COMPONENTS
  IR
  Lift
)

# Decoder.cpp contains the native AArch64 operand decoder in its hot path.  Keep
# it explicitly optimized in non-Debug configurations, but let Debug retain the
# toolchain's unoptimized flags for reliable stepping, local inspection, and
# MSVC runtime checks.  Use RelWithDebInfo for profiling with debug symbols.
if(MSVC)
  target_compile_options(NeverDDecode PRIVATE
    "$<$<NOT:$<CONFIG:Debug>>:/O2>")
else()
  target_compile_options(NeverDDecode PRIVATE
    "$<$<NOT:$<CONFIG:Debug>>:-O3;-fomit-frame-pointer>")
endif()
