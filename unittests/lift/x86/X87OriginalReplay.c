//===- X87OriginalReplay.c - Native x86 arithmetic reference --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Protocol-v1 original-instruction oracle with no C++ or LLVM dependency.
/// Supports actual i386/x64 processes and explicitly identifies Rosetta.
/// Requests contain operands, never executable instructions from the caller.
//===----------------------------------------------------------------------===//

#if !(defined(__i386__) || defined(__x86_64__)) ||                             \
    !(defined(__clang__) || defined(__GNUC__))
#error "Original x87 reference requires x86 GCC or Clang"
#endif

#include <cpuid.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

static bool available(unsigned *Signature) {
  unsigned B = 0, C = 0, D = 0;
  return __get_cpuid(1, Signature, &B, &C, &D) &&
         (D & ((1u << 24) | 1u)) == ((1u << 24) | 1u); // FPU and FXSAVE
}

static void describe(unsigned Signature) {
#ifdef __i386__
  const char *Backend = "i386-original-instructions", *Architecture = "i386";
#else
  bool Translated = false;
#ifdef __APPLE__
  int Value = 0;
  size_t Size = sizeof(Value);
  Translated =
      sysctlbyname("sysctl.proc_translated", &Value, &Size, NULL, 0) == 0 &&
      Value == 1;
#endif
  const char *Backend = Translated ? "rosetta2-x86-64-original-instructions"
                                   : "x86-64-original-instructions";
  const char *Architecture = "x86-64";
#endif
  printf("{\"protocol\":1,\"backend\":\"%s\",\"cpuid_signature\":%u,"
         "\"process_arch\":\"%s\",",
         Backend, Signature, Architecture);
#ifdef __clang__
  printf("\"compiler\":\"clang\",\"compiler_major\":%u}\n",
         (unsigned)__clang_major__);
#else
  printf("\"compiler\":\"gcc\",\"compiler_major\":%u}\n", (unsigned)__GNUC__);
#endif
}

static bool canonicalFinite(const uint8_t *Value) {
  unsigned Exponent = (Value[8] | ((unsigned)Value[9] << 8)) & 0x7fff;
  bool IntegerBit = (Value[7] & 0x80) != 0;
  return Exponent != 0x7fff && IntegerBit == (Exponent != 0);
}

static bool evaluate(const uint8_t Input[24], uint8_t Output[12]) {
  unsigned Operation = Input[0];
  uint16_t Control = (uint16_t)Input[2] | ((uint16_t)Input[3] << 8);
  if (Operation > 3 || Input[1] || (Control & 0xe080) ||
      (Control & 0x300) == 0x100 || (Control & 0x3f) != 0x3f ||
      !canonicalFinite(Input + 4) || !canonicalFinite(Input + 14))
    return false;
  uint8_t A[10], B[10], Result[10];
  memcpy(A, Input + 4, sizeof(A));
  memcpy(B, Input + 14, sizeof(B));
  uint16_t Status;
  alignas(16) uint8_t Host[512];
#ifdef __i386__
#define NEVERD_SAVE "fxsave"
#define NEVERD_RESTORE "fxrstor"
#else
#define NEVERD_SAVE "fxsave64"
#define NEVERD_RESTORE "fxrstor64"
#endif
#define NEVERD_OBSERVE(Instruction)                                            \
  __asm__ volatile(                                                            \
      NEVERD_SAVE                                                              \
      " %0\n\tfninit\n\tfldcw %3\n\tfldt %5\n\tfldt "                          \
      "%4\n\t" Instruction                                                     \
      " %%st(1), %%st(0)\n\tfnstsw %2\n\tfstpt %1\n\t" NEVERD_RESTORE " %0"    \
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
#undef NEVERD_RESTORE
#undef NEVERD_SAVE
  memcpy(Output, Result, sizeof(Result));
  Output[10] = Status;
  Output[11] = Status >> 8;
  return true;
}

int main(int Argc, char **Argv) {
  unsigned Signature = 0;
  if (!available(&Signature)) {
    fprintf(stderr, "x87/FXSAVE CPU features are unavailable\n");
    return 2;
  }
  if (Argc == 2 && strcmp(Argv[1], "--describe") == 0) {
    describe(Signature);
    return 0;
  }
  if (Argc != 3)
    return 2;
  FILE *Input = fopen(Argv[1], "rb");
  if (!Input)
    return 2;
  FILE *Output = fopen(Argv[2], "wb");
  if (!Output) {
    fclose(Input);
    return 2;
  }
  int Exit = 0;
  uint8_t Request[24], Response[12];
  size_t Bytes;
  while ((Bytes = fread(Request, 1, sizeof(Request), Input))) {
    if (Bytes != sizeof(Request) || !evaluate(Request, Response)) {
      Exit = 3;
      break;
    }
    if (fwrite(Response, 1, sizeof(Response), Output) != sizeof(Response)) {
      Exit = 2;
      break;
    }
  }
  bool ReadError = ferror(Input);
  int CloseInput = fclose(Input), CloseOutput = fclose(Output);
  return ReadError || CloseInput || CloseOutput ? 2 : Exit;
}
