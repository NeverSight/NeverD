//===- X87NativeContext.cpp - Native exact x87 context evaluator --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// An instruction table instantiates bytes; X87Context.h alone admits them.
//===----------------------------------------------------------------------===//
#include "neverd/ir/low/X86/X87NativeContext.h"

#include "neverd/ir/X86/X87BCD.h"
#include "neverd/ir/X86/X87Context.h"
#include "neverd/ir/X86/X87Control.h"

#include "llvm/ADT/StringMap.h"
#include "llvm/TargetParser/Host.h"

#include <cstring>
#include <utility>

#if (defined(__x86_64__) || defined(_M_X64)) &&                                \
    (defined(__clang__) || defined(__GNUC__))
#include <cpuid.h>
#endif

namespace neverd {
#if (defined(__x86_64__) || defined(_M_X64)) &&                                \
    (defined(__clang__) || defined(__GNUC__))
namespace {
#define X87_NATIVE_CLOBBERS                                                    \
  "memory", "rax", "r11", "cc", "st", "st(1)", "st(2)", "st(3)", "st(4)",      \
      "st(5)", "st(6)", "st(7)", "mm0", "mm1", "mm2", "mm3", "mm4", "mm5",     \
      "mm6", "mm7", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6",    \
      "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14",     \
      "xmm15"
#define X87_NATIVE_END "fxsave64 (%[state])\n\tfxrstor64 (%[host])"
#define X87_NATIVE_FLAGS                                                       \
  "testw $2048, (%[flags])\n\tjz 1f\n\t"                                       \
  "movl $2147483647, %%r11d\n\taddl $1, %%r11d\n\tjmp 2f\n\t"                  \
  "1: xorl %%r11d, %%r11d\n\t2: movq %[flags], %%rax\n\tmovb (%%rax), "        \
  "%%ah\n\tsahf\n\t"
#define X87_NATIVE_ARGS                                                        \
  [state] "r"(State), [host] "r"(Host), [memory] "r"(Address),                 \
      [flags] "r"(&Flags), [out] "r"(&Result), [escape] "i"(Escape),           \
      [modrm] "i"(ModRM), [suffix] "i"(x87ContextThreeDNowOpcode(Selector))

template <uint32_t Selector>
void execute(uint8_t *State, uint8_t *Host, void *Memory, uint64_t Flags,
             uint64_t &Result) {
  constexpr unsigned Escape = Selector & 0xff;
  constexpr unsigned ModRM =
      ((Selector >> 8) & 0xff) | ((Selector & X87ContextMemory) ? 2 : 0);
  constexpr bool ReadsFlags = !(Selector & X87ContextMemory) &&
                              (Escape == 0xda || Escape == 0xdb) &&
                              ModRM >= 0xc0 && ModRM <= 0xdf;
  constexpr bool WritesFlags = !(Selector & X87ContextMemory) &&
                               (Escape == 0xdb || Escape == 0xdf) &&
                               ModRM >= 0xe8 && ModRM <= 0xf7;
  register void *Address __asm__("r10") = Memory;
  if constexpr (Selector & X87ContextThreeDNow) {
    if constexpr (Selector & X87ContextMemory)
      __asm__ volatile(
          "fxrstor64 (%[state])\n\t.byte "
          "65,15,15,%c[modrm],%c[suffix]\n\t" X87_NATIVE_END ::X87_NATIVE_ARGS
          : X87_NATIVE_CLOBBERS);
    else
      __asm__ volatile(
          "fxrstor64 (%[state])\n\t.byte "
          "15,15,%c[modrm],%c[suffix]\n\t" X87_NATIVE_END ::X87_NATIVE_ARGS
          : X87_NATIVE_CLOBBERS);
  } else if constexpr (!(Selector & X87ContextMemory) && Escape == 0xdf &&
                       ModRM == 0xe0) {
    __asm__ volatile(
        "fxrstor64 (%[state])\n\tfnstsw (%[out])\n\t" X87_NATIVE_END ::
            X87_NATIVE_ARGS
        : X87_NATIVE_CLOBBERS);
  } else if constexpr (WritesFlags) {
    __asm__ volatile("fxrstor64 (%[state])\n\t" X87_NATIVE_FLAGS
                     ".byte %c[escape],%c[modrm]\n\t"
                     "lahf\n\tmovzbl %%ah, %%eax\n\tmovb %%al,(%[out])\n\tseto "
                     "1(%[out])\n\t" X87_NATIVE_END ::X87_NATIVE_ARGS
                     : X87_NATIVE_CLOBBERS);
  } else if constexpr (ReadsFlags) {
    __asm__ volatile(
        "fxrstor64 (%[state])\n\t" X87_NATIVE_FLAGS
        ".byte %c[escape],%c[modrm]\n\t" X87_NATIVE_END ::X87_NATIVE_ARGS
        : X87_NATIVE_CLOBBERS);
  } else if constexpr (Selector & X87ContextMemory) {
    if constexpr (Selector & X87ContextOperand16)
      __asm__ volatile(
          "fxrstor64 (%[state])\n\t.byte "
          "102,65,%c[escape],%c[modrm]\n\t" X87_NATIVE_END ::X87_NATIVE_ARGS
          : X87_NATIVE_CLOBBERS);
    else
      __asm__ volatile(
          "fxrstor64 (%[state])\n\t.byte "
          "65,%c[escape],%c[modrm]\n\t" X87_NATIVE_END ::X87_NATIVE_ARGS
          : X87_NATIVE_CLOBBERS);
  } else {
    __asm__ volatile("fxrstor64 (%[state])\n\t.byte "
                     "%c[escape],%c[modrm]\n\t" X87_NATIVE_END ::X87_NATIVE_ARGS
                     : X87_NATIVE_CLOBBERS);
  }
}
using Execute = void (*)(uint8_t *, uint8_t *, void *, uint64_t, uint64_t &);
template <size_t... I> constexpr auto registers(std::index_sequence<I...>) {
  return std::array<Execute, sizeof...(I)>{
      &execute<(0xd8 + I / 64) | ((0xc0 + I % 64) << 8)>...};
}
template <bool Short, size_t... I>
constexpr auto memory(std::index_sequence<I...>) {
  return std::array<Execute, sizeof...(I)>{
      &execute<(0xd8 + I / 8) | ((I % 8) << 11) | X87ContextMemory |
               (Short ? X87ContextOperand16 : 0)>...};
}
template <unsigned Opcode, bool Memory, size_t... I>
constexpr auto threeDNow(std::index_sequence<I...>) {
  return std::array<Execute, sizeof...(I)>{
      &execute<X87ContextThreeDNow | 0x0f |
               ((Memory ? I << 3 : 0xc0 + I) << 8) |
               (Opcode << X87ContextThreeDNowOpcodeShift) |
               (Memory ? X87ContextMemory : 0)>...};
}
Execute threeDNowExecutor(uint32_t Selector) {
  const unsigned ModRM = (Selector >> 8) & 0xff;
  switch (x87ContextThreeDNowOpcode(Selector)) {
#define NEVERD_3DNOW(Name, Opcode, Reads, Enhanced)                            \
  case Opcode: {                                                               \
    static constexpr auto Registers =                                          \
        threeDNow<Opcode, false>(std::make_index_sequence<64>{});              \
    static constexpr auto Memory =                                             \
        threeDNow<Opcode, true>(std::make_index_sequence<8>{});                \
    return (Selector & X87ContextMemory) ? Memory[ModRM >> 3]                  \
                                         : Registers[ModRM - 0xc0];            \
  }
#include "neverd/ir/X86/X86ThreeDNow.def"
#undef NEVERD_3DNOW
  default:
    return nullptr;
  }
}
constexpr auto RegisterOps = registers(std::make_index_sequence<512>{});
constexpr auto MemoryOps = memory<false>(std::make_index_sequence<64>{});
constexpr auto ShortMemoryOps = memory<true>(std::make_index_sequence<64>{});
#undef X87_NATIVE_ARGS
#undef X87_NATIVE_FLAGS
#undef X87_NATIVE_END
#undef X87_NATIVE_CLOBBERS
} // namespace
#endif

bool executeNativeX87Context(uint32_t Selector, std::array<uint8_t, 512> &State,
                             void *Memory, uint64_t Flags, uint64_t &Result) {
  const auto Spec = x87ContextInstruction(Selector);
  if (!Spec)
    return false;
  if (Spec->MemoryBytes && !Memory)
    return false;
#if (defined(__x86_64__) || defined(_M_X64)) &&                                \
    (defined(__clang__) || defined(__GNUC__))
  if (Spec->InvalidatesPayloads || Spec->ThreeDNow) {
    // LLVM no longer reports the legacy 3DNow feature through Host.h.
    unsigned EAX, EBX, ECX, EDX;
    if (!__get_cpuid(0x80000001, &EAX, &EBX, &ECX, &EDX) || !(EDX & (1U << 31)))
      return false;
    if (Spec->ThreeDNow &&
        x86ThreeDNowInstruction(x87ContextThreeDNowOpcode(Selector))
            ->Enhanced &&
        !(EDX & (1U << 30)))
      return false;
  }
  if ((Selector & X87ContextMemory) && ((Selector >> 8) & 0xff) == 8 &&
      ((Selector & 0xff) == 0xdb || (Selector & 0xff) == 0xdd ||
       (Selector & 0xff) == 0xdf)) {
    static const bool HasSSE3 = llvm::sys::getHostCPUFeatures().lookup("sse3");
    if (!HasSSE3)
      return false;
  }
  const uint16_t Control = State[0] | (unsigned(State[1]) << 8);
  const uint16_t Status = State[2] | (unsigned(State[3]) << 8);
  if (Spec->Waiting && x87HasPendingException(Control, Status))
    return false;
  if ((Selector & X87ContextMemory) && (Selector & 0xffff) == 0x20df &&
      !x87PackedBCDIsValid(
          {static_cast<const uint8_t *>(Memory), Spec->MemoryBytes}))
    return false;
  if ((Selector & 0xff) == 0x9b)
    return true;
  alignas(16) uint8_t Host[512], Active[512];
  __asm__ volatile("fxsave64 (%0)" ::"r"(Host) : "memory");
  std::memcpy(Active, State.data(), 512);
  std::memcpy(Active + 24, Host + 24, 8);
  std::memcpy(Active + 160, Host + 160, 352);
  Result = 0;
  const unsigned Escape = Selector & 0xff, ModRM = (Selector >> 8) & 0xff;
  if (Spec->ThreeDNow) {
    threeDNowExecutor(Selector)(Active, Host, Memory, Flags, Result);
  } else if (Spec->InvalidatesPayloads) {
    execute<0x0e0f>(Active, Host, Memory, Flags, Result);
  } else if (Selector == 0x770f) {
    __asm__ volatile(
        "fxrstor64 (%0)\n\temms\n\tfxsave64 (%0)\n\tfxrstor64 (%1)" ::"r"(
            Active),
        "r"(Host)
        : "memory", "st", "st(1)", "st(2)", "st(3)", "st(4)", "st(5)", "st(6)",
          "st(7)", "mm0", "mm1", "mm2", "mm3", "mm4", "mm5", "mm6", "mm7",
          "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
          "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
  } else if (Selector & X87ContextMemory) {
    const size_t Index = (Escape - 0xd8) * 8 + (ModRM >> 3);
    (Selector & X87ContextOperand16 ? ShortMemoryOps : MemoryOps)[Index](
        Active, Host, Memory, Flags, Result);
  } else {
    RegisterOps[(Escape - 0xd8) * 64 + ModRM - 0xc0](Active, Host, Memory,
                                                     Flags, Result);
  }
  std::memcpy(State.data(), Active, 24);
  std::memcpy(State.data() + 32, Active + 32, 128);
  return true;
#else
  return false;
#endif
}
} // namespace neverd
