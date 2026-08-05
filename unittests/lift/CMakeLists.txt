add_neverd_unittest(NeverDLiftTests
  AArch64_ArithTests.cpp
  AArch64_AtomicTests.cpp
  AArch64_BitShiftTests.cpp
  AArch64_CarryTests.cpp
  AArch64_CondTests.cpp
  AArch64_CSelTests.cpp
  AArch64_ExtendTests.cpp
  AArch64_FPTests.cpp
  AArch64_IntrinsicsTests.cpp
  AArch64_LogicTests.cpp
  AArch64_MemTests.cpp
  AArch64_MulDivTests.cpp
  AArch64_PipelineE2ETests.cpp
  AArch64_FastClassifyParityTests.cpp
  AArch64_NativeDecodeParityTests.cpp
  ARM32_ArithTests.cpp
  ARM32_CarryTests.cpp
  ARM32_ControlTests.cpp
  ARM32_ExtendTests.cpp
  ARM32_IntrinsicsTests.cpp
  ARM32_LogicTests.cpp
  ARM32_MemTests.cpp
  ARM32_MulDivTests.cpp
  ARM32_PipelineE2ETests.cpp
  ARM32_ShiftTests.cpp
  X86_32_ArithTests.cpp
  X86_32_BitOpsTests.cpp
  X86_32_CarryTests.cpp
  X86_32_CmovTests.cpp
  X86_32_CmpxchgTests.cpp
  X86_32_ControlTests.cpp
  X86_32_DShiftTests.cpp
  X86_32_FlagsTests.cpp
  X86_32_FPTests.cpp
  X86_32_LodsMiscTests.cpp
  X86_32_LoopTests.cpp
  X86_32_MiscTests.cpp
  X86_32_MulDivTests.cpp
  X86_32_PipelineE2ETests.cpp
  X86_32_RotateTests.cpp
  X86_32_SetccTests.cpp
  X86_32_SignExtTests.cpp
  X86_32_StackTests.cpp
  X86_32_StringTests.cpp
  X86_32_X87FPUTests.cpp
  X86_64_AdxMiscTests.cpp
  X86_64_ArithTests.cpp
  X86_64_BitOpsTests.cpp
  X86_64_BMITests.cpp
  X86_64_CarryOpsTests.cpp
  X86_64_CmpxchgTests.cpp
  X86_64_ControlFlowTests.cpp
  X86_64_ControlSemanticsTests.cpp
  X86_64_DoubleShiftTests.cpp
  X86_64_FlagOpsTests.cpp
  X86_64_FlagsMiscTests.cpp
  X86_64_FmaMiscTests.cpp
  X86_64_FPConvertTests.cpp
  X86_64_IntrinsicsTests.cpp
  X86_64_LodsMiscTests.cpp
  X86_64_LoopBitTests.cpp
  X86_64_MemAtomicTests.cpp
  X86_64_MiscTests.cpp
  X86_64_MulDivTests.cpp
  X86_64_PipelineE2ETests.cpp
  X86_64_RclRcrTests.cpp
  X86_64_RotateShiftTests.cpp
  X86_64_SemanticE2ETests.cpp
  X86_64_SetccCmovTests.cpp
  X86_64_SignConvTests.cpp
  X86_64_SignExtTests.cpp
  X86_64_SSEAVXTests.cpp
  X86_64_SSEFPTests.cpp
  X86_64_SSEMiscTests.cpp
  X86_64_StackMovTests.cpp
  X86_64_StringOpsTests.cpp
  X86_64_X87FPUTests.cpp
  X86_64_SwitchTests.cpp
  X86_64_RoundTripTests.cpp
  AArch64_SwitchTests.cpp
  AArch64_RoundTripTests.cpp
  ARM32_SwitchTests.cpp
  ARM32_RoundTripTests.cpp
  InstructionModeTests.cpp
  MachOI386RelocationTests.cpp
  MedCallingConvTests.cpp
  COFFARMFormatTests.cpp
  FunctionDiscoveryThumbTests.cpp
  NdOpEmulatorTests.cpp
  CircleRangeTests.cpp
  JumpTableEnhancedTests.cpp
  PatchFormatTests.cpp
  TextSectionDetectionTests.cpp
)

target_compile_definitions(NeverDLiftTests PRIVATE
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
  TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
  TEST_OBJ_DIR="${CMAKE_CURRENT_BINARY_DIR}"
)

target_include_directories(NeverDLiftTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

target_link_libraries(NeverDLiftTests PRIVATE
  NeverDBackendC
  NeverDBackendLLVM
  NeverDCodeGen
  NeverDDecode
  NeverDIRLow
  NeverDLibC
  NeverDLoader
  NeverDLift
  NeverDSupport
)

if(TARGET neverd)
  add_dependencies(NeverDLiftTests neverd)
endif()

# --- Cross-compile test .c sources into .o objects at build time ---
find_program(CLANG_EXEC clang)

set(X86_64_TEST_SOURCES
  x86_64/test_arith.c
  x86_64/test_muldiv.c
  x86_64/test_control_flow.c
  x86_64/test_stack_mov.c
  x86_64/test_sse_fp.c
  x86_64/test_carry_ops.c
  x86_64/test_bitops.c
  x86_64/test_setcc_cmov.c
  x86_64/test_cmpxchg_xadd.c
  x86_64/test_signext.c
  x86_64/test_string_ops.c
  x86_64/test_rotate_shift.c
  x86_64/test_misc.c
  x86_64/test_semantic_e2e.c
  x86_64/test_control_semantics.c
  x86_64/test_bmi.c
  x86_64/test_adx_misc.c
  x86_64/test_sse_avx.c
  x86_64/test_flag_ops.c
  x86_64/test_mem_atomic.c
  x86_64/test_double_shift.c
  x86_64/test_flags_misc.c
  x86_64/test_fp_convert.c
  x86_64/test_rcl_rcr.c
  x86_64/test_sign_conv.c
  x86_64/test_loop.c
  x86_64/test_fma_misc.c
  x86_64/test_x87_fpu.c
  x86_64/test_lods_misc.c
  x86_64/test_sse_misc.c
  x86_64/test_intrinsics_system.c
  x86_64/test_switch.c
  x86_64/test_roundtrip.c
  x86_64/test_jumptable_enhanced.c
  x86_64/test_twolevel_switch.c
  x86_64/test_jumptable_masked_abs.c
  x86_64/test_jumptable_worldclass.c
  x86_64/test_jumptable_cgoto.c
  x86_64/test_global_volatile.c
)

set(X86_32_TEST_SOURCES
  x86_32/test_arith32.c
  x86_32/test_control32.c
  x86_32/test_bitops32.c
  x86_32/test_signext32.c
  x86_32/test_muldiv32.c
  x86_32/test_stack32.c
  x86_32/test_flags32.c
  x86_32/test_misc32.c
  x86_32/test_fp32.c
  x86_32/test_cmov32.c
  x86_32/test_rotate32.c
  x86_32/test_carry32.c
  x86_32/test_setcc32.c
  x86_32/test_string32.c
  x86_32/test_cmpxchg32.c
  x86_32/test_loop32.c
  x86_32/test_dshift32.c
  x86_32/test_x87_fpu32.c
  x86_32/test_lods_misc32.c
)

set(AARCH64_TEST_SOURCES
  aarch64/test_param_phi_a64.s
  aarch64/test_arith_a64.c
  aarch64/test_cond_a64.c
  aarch64/test_mem_a64.c
  aarch64/test_bitshift_a64.c
  aarch64/test_fp_a64.c
  aarch64/test_logic_a64.c
  aarch64/test_muldiv_a64.c
  aarch64/test_csel_a64.c
  aarch64/test_carry_a64.c
  aarch64/test_extend_a64.c
  aarch64/test_atomic_a64.c
  aarch64/test_intrinsics_a64.c
  aarch64/test_switch_a64.c
  aarch64/test_roundtrip_a64.c
  aarch64/test_jumptable_enhanced_a64.c
  aarch64/test_jumptable_worldclass_a64.c
)

set(ARM32_TEST_SOURCES
  arm32/test_arith_arm.c
  arm32/test_mem_arm.c
  arm32/test_control_arm.c
  arm32/test_logic_arm.c
  arm32/test_muldiv_arm.c
  arm32/test_carry_arm.c
  arm32/test_extend_arm.c
  arm32/test_shift_arm.c
  arm32/test_intrinsics_arm.c
  arm32/test_switch_arm.c
  arm32/test_roundtrip_arm.c
  arm32/test_jumptable_enhanced_arm.c
  arm32/test_jumptable_worldclass_arm.c
)

set(TEST_OBJECTS "")

set(macho_i386_src
    "${CMAKE_CURRENT_SOURCE_DIR}/format/test_macho_i386.c")
set(macho_i386_obj
    "${CMAKE_CURRENT_BINARY_DIR}/test_macho_i386.o")
set(macho_i386_nopic_obj
    "${CMAKE_CURRENT_BINARY_DIR}/test_macho_i386_nopic.o")
add_custom_command(
  OUTPUT ${macho_i386_obj}
  COMMAND ${CLANG_EXEC} -target i386-apple-macos10.6 -O0 -ffreestanding
          -fno-stack-protector -c ${macho_i386_src} -o ${macho_i386_obj}
  DEPENDS ${macho_i386_src}
  COMMENT "Compiling test_macho_i386.c -> .o (PIC i386 Mach-O)")
add_custom_command(
  OUTPUT ${macho_i386_nopic_obj}
  COMMAND ${CLANG_EXEC} -target i386-apple-macos10.6 -O0 -ffreestanding
          -fno-stack-protector -fno-pic -c ${macho_i386_src}
          -o ${macho_i386_nopic_obj}
  DEPENDS ${macho_i386_src}
  COMMENT "Compiling test_macho_i386.c -> .o (no-PIC i386 Mach-O)")
list(APPEND TEST_OBJECTS ${macho_i386_obj} ${macho_i386_nopic_obj})

foreach(src ${X86_64_TEST_SOURCES})
  get_filename_component(base ${src} NAME_WE)
  set(obj "${CMAKE_CURRENT_BINARY_DIR}/${base}.o")
  set(extra_flags)
  if(src MATCHES "test_jumptable_cgoto")
    # -O0 keeps the computed-goto label table decoupled from the indirect branch
    # by a spill/reload relay (the shared multi-site dispatch the const-base
    # absolute resolver recovers); -O1 would inline the load into the branch.
    set(extra_flags -O0)
  elseif(src MATCHES "test_switch|test_jumptable")
    set(extra_flags -O1)
  elseif(src MATCHES "test_bmi")
    set(extra_flags -mbmi -mbmi2 -mlzcnt -mpopcnt -mcrc32)
  elseif(src MATCHES "test_adx")
    set(extra_flags -madx -mbmi -mlzcnt -mpopcnt -mmovbe)
  elseif(src MATCHES "test_sse_avx")
    set(extra_flags -msse4.2 -mavx)
  elseif(src MATCHES "test_flags_misc")
    set(extra_flags -mbmi -mlzcnt -mpopcnt)
  endif()
  add_custom_command(
    OUTPUT ${obj}
    COMMAND ${CLANG_EXEC} -target x86_64-linux-gnu -nostdlib -static
            -O0 -fno-stack-protector -fno-pie ${extra_flags} -c
            ${CMAKE_CURRENT_SOURCE_DIR}/${src} -o ${obj}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${src}
    COMMENT "Compiling ${src} -> ${base}.o (x86_64 ELF)"
  )
  list(APPEND TEST_OBJECTS ${obj})
endforeach()

foreach(src ${X86_32_TEST_SOURCES})
  get_filename_component(base ${src} NAME_WE)
  set(obj "${CMAKE_CURRENT_BINARY_DIR}/${base}.o")
  set(x86_32_extra_flags)
  if(src MATCHES "test_fp32")
    set(x86_32_extra_flags -msse2)
  endif()
  add_custom_command(
    OUTPUT ${obj}
    COMMAND ${CLANG_EXEC} -target i386-linux-gnu -m32 -nostdlib -static
            -O0 -fno-stack-protector -fno-pie ${x86_32_extra_flags} -c
            ${CMAKE_CURRENT_SOURCE_DIR}/${src} -o ${obj}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${src}
    COMMENT "Compiling ${src} -> ${base}.o (i386 ELF)"
  )
  list(APPEND TEST_OBJECTS ${obj})
endforeach()

foreach(src ${AARCH64_TEST_SOURCES})
  get_filename_component(base ${src} NAME_WE)
  set(obj "${CMAKE_CURRENT_BINARY_DIR}/${base}.o")
  set(a64_extra_flags)
  if(src MATCHES "test_switch|test_jumptable")
    set(a64_extra_flags -O1)
  endif()
  add_custom_command(
    OUTPUT ${obj}
    COMMAND ${CLANG_EXEC} -target aarch64-linux-gnu -nostdlib -static
            -O0 -fno-stack-protector ${a64_extra_flags} -c
            ${CMAKE_CURRENT_SOURCE_DIR}/${src} -o ${obj}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${src}
    COMMENT "Compiling ${src} -> ${base}.o (AArch64 ELF)"
  )
  list(APPEND TEST_OBJECTS ${obj})
endforeach()

foreach(src ${ARM32_TEST_SOURCES})
  get_filename_component(base ${src} NAME_WE)
  set(obj "${CMAKE_CURRENT_BINARY_DIR}/${base}.o")
  set(arm_extra_flags)
  if(src MATCHES "test_switch|test_jumptable")
    set(arm_extra_flags -O1)
  endif()
  add_custom_command(
    OUTPUT ${obj}
    COMMAND ${CLANG_EXEC} -target arm-linux-gnueabi -march=armv7-a -nostdlib -static
            -O0 -fno-stack-protector -marm ${arm_extra_flags} -c
            ${CMAKE_CURRENT_SOURCE_DIR}/${src} -o ${obj}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${src}
    COMMENT "Compiling ${src} -> ${base}.o (ARM32 ELF)"
  )
  list(APPEND TEST_OBJECTS ${obj})
endforeach()

# --- Compile patch-specific test sources (no inline asm) ---
set(PATCH_TEST_SOURCES_X64 x86_64/test_patch_switch.c x86_64/test_patch_xcall.c)
set(PATCH_TEST_SOURCES_A64 aarch64/test_patch_switch_a64.c aarch64/test_patch_xcall_a64.c)
set(PATCH_TEST_SOURCES_ARM arm32/test_patch_switch_arm.c arm32/test_patch_xcall_arm.c)

foreach(src ${PATCH_TEST_SOURCES_X64})
  get_filename_component(base ${src} NAME_WE)
  set(obj "${CMAKE_CURRENT_BINARY_DIR}/${base}.o")
  add_custom_command(
    OUTPUT ${obj}
    COMMAND ${CLANG_EXEC} -target x86_64-linux-gnu -nostdlib -static
            -O1 -fno-stack-protector -fno-pie -c
            ${CMAKE_CURRENT_SOURCE_DIR}/${src} -o ${obj}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${src}
    COMMENT "Compiling ${src} -> ${base}.o (x86_64 ELF)")
  list(APPEND TEST_OBJECTS ${obj})
endforeach()
foreach(src ${PATCH_TEST_SOURCES_A64})
  get_filename_component(base ${src} NAME_WE)
  set(obj "${CMAKE_CURRENT_BINARY_DIR}/${base}.o")
  add_custom_command(
    OUTPUT ${obj}
    COMMAND ${CLANG_EXEC} -target aarch64-linux-gnu -nostdlib -static
            -O1 -fno-stack-protector -c
            ${CMAKE_CURRENT_SOURCE_DIR}/${src} -o ${obj}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${src}
    COMMENT "Compiling ${src} -> ${base}.o (AArch64 ELF)")
  list(APPEND TEST_OBJECTS ${obj})
endforeach()
foreach(src ${PATCH_TEST_SOURCES_ARM})
  get_filename_component(base ${src} NAME_WE)
  set(obj "${CMAKE_CURRENT_BINARY_DIR}/${base}.o")
  add_custom_command(
    OUTPUT ${obj}
    COMMAND ${CLANG_EXEC} -target arm-linux-gnueabi -march=armv7-a -nostdlib -static
            -O1 -fno-stack-protector -marm -c
            ${CMAKE_CURRENT_SOURCE_DIR}/${src} -o ${obj}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${src}
    COMMENT "Compiling ${src} -> ${base}.o (ARM32 ELF)")
  list(APPEND TEST_OBJECTS ${obj})
endforeach()

# --- Compile COFF/PE test binary ---
find_program(LLD_LINK_EXEC lld-link)
if(LLD_LINK_EXEC)
  set(coff_src "${CMAKE_CURRENT_SOURCE_DIR}/x86_64/test_patch_coff.c")
  set(coff_obj "${CMAKE_CURRENT_BINARY_DIR}/test_patch_coff.obj")
  set(coff_exe "${CMAKE_CURRENT_BINARY_DIR}/test_patch_coff.exe")
  add_custom_command(
    OUTPUT ${coff_obj}
    COMMAND ${CLANG_EXEC} -target x86_64-pc-windows-msvc -O1 -c ${coff_src} -o ${coff_obj}
    DEPENDS ${coff_src}
    COMMENT "Compiling test_patch_coff.c -> .obj (x86_64 COFF)")
  add_custom_command(
    OUTPUT ${coff_exe}
    COMMAND ${LLD_LINK_EXEC} /entry:mainCRTStartup /subsystem:console /nodefaultlib ${coff_obj} /out:${coff_exe}
    DEPENDS ${coff_obj}
    COMMENT "Linking test_patch_coff.obj -> .exe (x86_64 PE)")
  list(APPEND TEST_OBJECTS ${coff_obj} ${coff_exe})

  set(coff_arm_src "${CMAKE_CURRENT_SOURCE_DIR}/format/test_pe_arm.c")

  set(coff_a64_obj "${CMAKE_CURRENT_BINARY_DIR}/test_patch_coff_a64.obj")
  set(coff_a64_exe "${CMAKE_CURRENT_BINARY_DIR}/test_patch_coff_a64.exe")
  add_custom_command(
    OUTPUT ${coff_a64_obj}
    COMMAND ${CLANG_EXEC} -target aarch64-pc-windows-msvc -O0
            -ffreestanding -fno-stack-protector -funwind-tables -c
            ${coff_arm_src} -o ${coff_a64_obj}
    DEPENDS ${coff_arm_src}
    COMMENT "Compiling test_pe_arm.c -> .obj (AArch64 COFF)")
  add_custom_command(
    OUTPUT ${coff_a64_exe}
    COMMAND ${LLD_LINK_EXEC} /entry:mainCRTStartup /subsystem:console
            /nodefaultlib /export:pe_leaf /export:pe_stacky
            ${coff_a64_obj} /out:${coff_a64_exe}
    DEPENDS ${coff_a64_obj}
    COMMENT "Linking test_patch_coff_a64.obj -> .exe (AArch64 PE)")

  set(coff_arm_obj "${CMAKE_CURRENT_BINARY_DIR}/test_patch_coff_arm.obj")
  set(coff_arm_tiny_src
      "${CMAKE_CURRENT_SOURCE_DIR}/format/test_pe_tiny_thumb.s")
  set(coff_arm_tiny_obj
      "${CMAKE_CURRENT_BINARY_DIR}/test_patch_coff_arm_tiny.obj")
  set(coff_arm_exe "${CMAKE_CURRENT_BINARY_DIR}/test_patch_coff_arm.exe")
  add_custom_command(
    OUTPUT ${coff_arm_obj}
    COMMAND ${CLANG_EXEC} -target thumbv7-pc-windows-msvc -O0
            -ffreestanding -fno-stack-protector -funwind-tables -c
            ${coff_arm_src} -o ${coff_arm_obj}
    DEPENDS ${coff_arm_src}
    COMMENT "Compiling test_pe_arm.c -> .obj (ARM32 COFF)")
  add_custom_command(
    OUTPUT ${coff_arm_tiny_obj}
    COMMAND ${CLANG_EXEC} -target thumbv7-pc-windows-msvc -c
            ${coff_arm_tiny_src} -o ${coff_arm_tiny_obj}
    DEPENDS ${coff_arm_tiny_src}
    COMMENT "Compiling test_pe_tiny_thumb.s -> .obj (ARM32 COFF)")
  add_custom_command(
    OUTPUT ${coff_arm_exe}
    COMMAND ${LLD_LINK_EXEC} /entry:mainCRTStartup /subsystem:console
            /nodefaultlib /export:pe_leaf /export:pe_stacky /export:pe_tiny
            /export:pe_after_tiny ${coff_arm_obj} ${coff_arm_tiny_obj}
            /out:${coff_arm_exe}
    DEPENDS ${coff_arm_obj} ${coff_arm_tiny_obj}
    COMMENT "Linking ARM32 COFF objects -> test_patch_coff_arm.exe")

  list(APPEND TEST_OBJECTS
    ${coff_a64_obj} ${coff_a64_exe}
    ${coff_arm_obj} ${coff_arm_tiny_obj} ${coff_arm_exe})
endif()

# --- Link into ELF executables for patch testing ---
find_program(LLD_EXEC ld.lld)
set(ELF_EXECUTABLES "")
if(LLD_EXEC)
  # x86_64 ELF executables
  foreach(base_name test_switch test_jumptable_enhanced test_patch_switch test_patch_xcall test_roundtrip)
    set(obj_file "${CMAKE_CURRENT_BINARY_DIR}/${base_name}.o")
    set(elf_file "${CMAKE_CURRENT_BINARY_DIR}/${base_name}_elf")
    add_custom_command(
      OUTPUT ${elf_file}
      COMMAND ${LLD_EXEC} -o ${elf_file} --image-base=0x400000 -e _start ${obj_file}
      DEPENDS ${obj_file}
      COMMENT "Linking ${base_name} -> x86_64 ELF executable")
    list(APPEND ELF_EXECUTABLES ${elf_file})
  endforeach()

  # AArch64 ELF executables
  foreach(a64_base test_patch_switch_a64 test_patch_xcall_a64 test_switch_a64)
    set(a64_obj "${CMAKE_CURRENT_BINARY_DIR}/${a64_base}.o")
    set(a64_elf "${CMAKE_CURRENT_BINARY_DIR}/${a64_base}_elf")
    add_custom_command(
      OUTPUT ${a64_elf}
      COMMAND ${LLD_EXEC} -m aarch64linux -o ${a64_elf} --image-base=0x400000 -e _start ${a64_obj}
      DEPENDS ${a64_obj}
      COMMENT "Linking ${a64_base} -> AArch64 ELF executable")
    list(APPEND ELF_EXECUTABLES ${a64_elf})
  endforeach()

  # ARM32 ELF executables
  foreach(arm_base test_patch_switch_arm test_patch_xcall_arm test_switch_arm)
    set(arm_obj "${CMAKE_CURRENT_BINARY_DIR}/${arm_base}.o")
    set(arm_elf "${CMAKE_CURRENT_BINARY_DIR}/${arm_base}_elf")
    add_custom_command(
      OUTPUT ${arm_elf}
      COMMAND ${LLD_EXEC} -m armelf -o ${arm_elf} --image-base=0x400000 -e _start ${arm_obj}
      DEPENDS ${arm_obj}
      COMMENT "Linking ${arm_base} -> ARM32 ELF executable")
    list(APPEND ELF_EXECUTABLES ${arm_elf})
  endforeach()
endif()

add_custom_target(lift-test-objects DEPENDS ${TEST_OBJECTS} ${ELF_EXECUTABLES})
add_dependencies(NeverDLiftTests lift-test-objects)
