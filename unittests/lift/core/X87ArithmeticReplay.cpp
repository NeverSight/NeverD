//===- X87ArithmeticReplay.cpp - Binary x87 differential protocol --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Standalone test driver. Build with NEVERD_X87_ORIGINAL_REFERENCE on x86-64
/// for original instructions, otherwise link the portable numerical owner.
/// Protocol v1: 24-byte requests (operation, zero, little-endian CW, A80, B80),
/// 12-byte responses (result80, little-endian SW). Never executes input code.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_X87_ORIGINAL_REFERENCE
#include "neverd/ir/low/X87Arithmetic.h"
#endif

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

#ifdef NEVERD_X87_ORIGINAL_REFERENCE
#if !defined(__x86_64__) || !(defined(__clang__) || defined(__GNUC__))
#error "Original x87 reference requires x86-64 GCC or Clang"
#endif
#include <cpuid.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif
#endif

namespace {
void describe() {
#ifdef NEVERD_X87_ORIGINAL_REFERENCE
  bool Translated = false;
#ifdef __APPLE__
  int Value = 0;
  size_t Size = sizeof(Value);
  Translated =
      sysctlbyname("sysctl.proc_translated", &Value, &Size, nullptr, 0) == 0 &&
      Value == 1;
#endif
  unsigned A = 0, B = 0, C = 0, D = 0;
  __get_cpuid(1, &A, &B, &C, &D);
  std::printf("{\"protocol\":1,\"backend\":\"%s\",\"cpuid_signature\":%u,",
              Translated ? "rosetta2-x86-64-original-instructions"
                         : "x86-64-original-instructions",
              A);
#else
  std::printf("{\"protocol\":1,\"backend\":\"neverd-portable-binary80\",");
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
  std::printf("\"process_arch\":\"arm64\",");
#elif defined(__x86_64__) || defined(_M_X64)
  std::printf("\"process_arch\":\"x86-64\",");
#else
  std::printf("\"process_arch\":\"other\",");
#endif
#ifdef __clang__
  std::printf("\"compiler\":\"clang\",\"compiler_major\":%u}\n",
              unsigned(__clang_major__));
#else
  std::printf("\"compiler\":\"gcc\",\"compiler_major\":%u}\n",
              unsigned(__GNUC__));
#endif
}

bool evaluate(const std::array<uint8_t, 24> &Input,
              std::array<uint8_t, 12> &Output) {
  const auto Operation = Input[0];
  const uint16_t Control = uint16_t(Input[2]) | (uint16_t(Input[3]) << 8);
  if (Operation > 3 || Input[1] || (Control & 0xe080) ||
      (Control & 0x300) == 0x100 || (Control & 0x3f) != 0x3f)
    return false;
  std::array<uint8_t, 10> A{}, B{}, Result{};
  for (unsigned I = 0; I != 10; ++I) {
    A[I] = Input[4 + I];
    B[I] = Input[14 + I];
  }
  uint16_t Status = 0;
#ifdef NEVERD_X87_ORIGINAL_REFERENCE
  alignas(16) std::array<uint8_t, 512> Host{};
#define NEVERD_OBSERVE(Instruction)                                            \
  __asm__ volatile(                                                            \
      "fxsave64 %0\n\tfninit\n\tfldcw %3\n\tfldt %5\n\tfldt "                  \
      "%4\n\t" Instruction                                                     \
      " %%st(1), %%st(0)\n\tfnstsw %2\n\tfstpt %1\n\tfxrstor64 %0"             \
      : "=m"(Host), "=m"(Result), "=m"(Status)                                 \
      : "m"(Control), "m"(A), "m"(B)                                           \
      : "memory", "st", "st(1)", "st(2)", "st(3)", "st(4)", "st(5)", "st(6)",  \
        "st(7)")
  switch (Operation) {
  case 0:
    NEVERD_OBSERVE("fadd");
    break;
  case 1:
    NEVERD_OBSERVE("fsub");
    break;
  case 2:
    NEVERD_OBSERVE("fmul");
    break;
  case 3:
    NEVERD_OBSERVE("fdiv");
    break;
  }
#undef NEVERD_OBSERVE
#else
  const auto Value = neverd::evaluateFiniteX87Arithmetic(
      static_cast<neverd::X87ArithmeticOperation>(Operation), A, B, Control);
  if (!Value)
    return false;
  Result = Value->Value;
  Status = Value->Exceptions | (Value->RoundedUp ? 0x200 : 0);
#endif
  for (unsigned I = 0; I != 10; ++I)
    Output[I] = Result[I];
  Output[10] = Status;
  Output[11] = Status >> 8;
  return true;
}
} // namespace

int main(int Argc, char **Argv) {
  if (Argc == 2 && std::strcmp(Argv[1], "--describe") == 0) {
    describe();
    return 0;
  }
  if (Argc != 3)
    return 2;
  FILE *Input = std::fopen(Argv[1], "rb");
  if (!Input)
    return 2;
  FILE *Output = std::fopen(Argv[2], "wb");
  if (!Output) {
    std::fclose(Input);
    return 2;
  }
  int Exit = 0;
  std::array<uint8_t, 24> Request{};
  while (const auto Bytes =
             std::fread(Request.data(), 1, Request.size(), Input)) {
    std::array<uint8_t, 12> Response{};
    if (Bytes != Request.size() || !evaluate(Request, Response)) {
      Exit = 3;
      break;
    }
    if (std::fwrite(Response.data(), 1, Response.size(), Output) !=
        Response.size()) {
      Exit = 2;
      break;
    }
  }
  const bool ReadError = std::ferror(Input);
  const int CloseInput = std::fclose(Input), CloseOutput = std::fclose(Output);
  if (ReadError || CloseInput || CloseOutput)
    Exit = 2;
  return Exit;
}
