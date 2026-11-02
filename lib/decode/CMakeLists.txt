add_neverd_component_library(NeverDDecode
  Decoder.cpp
  NativeOperands.cpp
  InstructionRelocations.cpp

  AArch64/AArch64Decoder.cpp
  AArch64/AArch64InstructionRelocations.cpp
  AArch64/AArch64NativeOperands.cpp
  ARM/ARMDecoder.cpp
  ARM/ARMInstructionRelocations.cpp
  ARM/ARMNativeOperands.cpp
  X86/X86Decoder.cpp
  X86/X86MpxDecoder.cpp
  X86/X86InstructionRelocations.cpp
  X86/X86NativeOperands.cpp

  LINK_COMPONENTS
  IR
  Lift
)

# AArch64/AArch64Decoder.cpp contains the native operand decode hot path. Keep
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
