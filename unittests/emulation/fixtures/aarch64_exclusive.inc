//===- aarch64_exclusive.inc - Original ARM64 exclusive loop probes -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
// Shared by the original Windows process fixture and the native ARM64 oracle.
// The instruction words are independently assembled test data, not production
// decoder encodings. All constrained-unpredictable register overlaps are
// absent.
typedef unsigned long long ExclusiveWord;
typedef struct __attribute__((aligned(16))) {
  ExclusiveWord Low, High;
} ExclusivePair;
static const ExclusiveWord ExclusiveInitial = 0xfedcba9876543210ULL;
static const ExclusiveWord ExclusiveUpdated = 0x0123456789abcdefULL;
enum {
  ExclusiveAttempts = 4096,
  ExclusiveByteBits = 8,
  ExclusiveWordBits = 64
};

#define NEVERD_EXCLUSIVE_CASE(Name, Load, Store, Width, Count)                 \
  static int checkExclusive##Name(void) {                                      \
    for (ExclusiveWord Clear = 0; Clear != 2; ++Clear) {                       \
      unsigned Attempt;                                                        \
      for (Attempt = 0; Attempt != ExclusiveAttempts; ++Attempt) {             \
        volatile ExclusivePair Memory = {ExclusiveInitial, ExclusiveUpdated};  \
        ExclusiveWord Low, High, Status;                                       \
        __asm__ volatile(                                                      \
            "mov x1, %[address]\n\t"                                           \
            "mov x3, xzr\n\t"                                                  \
            ".inst " #Load "\n\t"                                              \
            "mov %[low], x0\n\t"                                               \
            "mov %[high], x3\n\t"                                              \
            "mov x0, %[next_low]\n\t"                                          \
            "mov x3, %[next_high]\n\t"                                         \
            "cbz %[clear], 1f\n\t"                                             \
            "clrex\n\t"                                                        \
            "1:\n\t"                                                           \
            ".inst " #Store "\n\t"                                             \
            "mov %[status], x2\n\t"                                            \
            : [low] "=&r"(Low), [high] "=&r"(High), [status] "=&r"(Status)     \
            : [address] "r"(&Memory), [clear] "r"(Clear),                      \
              [next_low] "r"(ExclusiveUpdated),                                \
              [next_high] "r"(ExclusiveInitial)                                \
            : "x0", "x1", "x2", "x3", "memory");                               \
        const ExclusiveWord Mask =                                             \
            ~0ULL >> (ExclusiveWordBits - Width * ExclusiveByteBits);          \
        const ExclusiveWord ExpectedHigh =                                     \
            Count == 1                                                         \
                ? 0                                                            \
                : (Width == sizeof(ExclusiveWord) ? ExclusiveUpdated           \
                                                  : ExclusiveInitial >> 32) &  \
                      Mask;                                                    \
        if (Low != (ExclusiveInitial & Mask) || High != ExpectedHigh ||        \
            Status > 1 || (Clear && !Status))                                  \
          return 0;                                                            \
        const ExclusivePair Original = {ExclusiveInitial, ExclusiveUpdated};   \
        const unsigned char *Before = (const unsigned char *)&Original;        \
        const volatile unsigned char *After =                                  \
            (const volatile unsigned char *)&Memory;                           \
        for (unsigned Byte = 0; Byte != sizeof(Memory); ++Byte) {              \
          unsigned char Expected = Before[Byte];                               \
          if (!Status && Byte < Width * Count)                                 \
            Expected = (unsigned char)((Byte < Width ? ExclusiveUpdated        \
                                                     : ExclusiveInitial) >>    \
                                       ((Byte % Width) * ExclusiveByteBits));  \
          if (After[Byte] != Expected)                                         \
            return 0;                                                          \
        }                                                                      \
        if (Clear || !Status)                                                  \
          break;                                                               \
      }                                                                        \
      if (Attempt == ExclusiveAttempts)                                        \
        return 0;                                                              \
    }                                                                          \
    return 1;                                                                  \
  }
#include "../aarch64/AArch64ExclusiveCases.def"
#undef NEVERD_EXCLUSIVE_CASE

static int checkExclusiveAccesses(void) {
#define NEVERD_EXCLUSIVE_CASE(Name, Load, Store, Width, Count)                 \
  if (!checkExclusive##Name())                                                 \
    return 0;
#include "../aarch64/AArch64ExclusiveCases.def"
#undef NEVERD_EXCLUSIVE_CASE
  return 1;
}
