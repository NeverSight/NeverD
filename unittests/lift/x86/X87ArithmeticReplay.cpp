//===- X87ArithmeticReplay.cpp - Binary x87 differential protocol --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Standalone portable numerical driver. X87OriginalReplay.c supplies the
/// independent original-instruction reference without a C++ dependency.
/// Protocol v1: 24-byte requests (operation, zero, little-endian CW, A80, B80),
/// 12-byte responses (result80, little-endian SW). Never executes input code.
//===----------------------------------------------------------------------===//

#ifdef NEVERD_X87_ORIGINAL_REFERENCE
#error "Build X87OriginalReplay.c with the C compiler for original instructions"
#endif
#include "neverd/ir/low/X86/X87Arithmetic.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {
void describe() {
  std::printf("{\"protocol\":1,\"backend\":\"neverd-portable-binary80\",");
#if defined(__aarch64__) || defined(_M_ARM64)
  std::printf("\"process_arch\":\"arm64\",");
#elif defined(__x86_64__) || defined(_M_X64)
  std::printf("\"process_arch\":\"x86-64\",");
#elif defined(__i386__) || defined(_M_IX86)
  std::printf("\"process_arch\":\"i386\",");
#else
  std::printf("\"process_arch\":\"other\",");
#endif
#ifdef __clang__
  std::printf("\"compiler\":\"clang\",\"compiler_major\":%u}\n",
              unsigned(__clang_major__));
#elif defined(__GNUC__)
  std::printf("\"compiler\":\"gcc\",\"compiler_major\":%u}\n",
              unsigned(__GNUC__));
#elif defined(_MSC_VER)
  std::printf("\"compiler\":\"msvc\",\"compiler_version\":%u}\n",
              unsigned(_MSC_VER));
#else
  std::printf("\"compiler\":\"unknown\"}\n");
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
  const auto Value = neverd::evaluateFiniteX87Arithmetic(
      static_cast<neverd::X87ArithmeticOperation>(Operation), A, B, Control);
  if (!Value)
    return false;
  Result = Value->Value;
  Status = Value->Exceptions | (Value->RoundedUp ? 0x200 : 0);
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
