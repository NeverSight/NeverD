//===- Arm64eOrderedModel.c - Compare signed-offset ordered accesses ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercise every address residue in Normal memory and inspect the hardware
/// exception state before resuming past an expected alignment fault.
//===----------------------------------------------------------------------===//

#include "Arm64eOrderedModel.def"

#include "Arm64eOrderedAccess.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint64_t Quadword __attribute__((vector_size(ORDERED_QUAD_BYTES)));

typedef struct {
  uint32_t Word;
  int Offset;
  unsigned Width, ResultBytes;
  bool Vector, Signed;
} OrderedCase;

static const OrderedCase Cases[] = {
#define ORDERED_MEMORY_CASE(Word, Scratch, Vector, Access, Result, Signed,     \
                            Offset)                                            \
  {Word, Offset, Access, Result, Vector, Signed},
#include "AArch64OrderedMemoryFixtures.def"
};
_Static_assert(sizeof(Cases) / sizeof(*Cases) == ORDERED_CASE_COUNT,
               ORDERED_CASE_ERROR);

extern uint64_t generatedOrdered(uint32_t Word, uintptr_t Base, uint64_t Lane,
                                 uint64_t Low, uint64_t High);

// Execute the independently assembled fixture word as supplied. Explicit
// register operands match the encoded Rn and Rt and inform the host compiler
// about both the inputs and the effects of the original instruction.
static uint64_t originalOrdered(uint32_t Word, uintptr_t Base, unsigned Lane,
                                OrderedValue Input) {
  register uintptr_t X1 __asm__(ORDERED_BASE_REGISTER) = Base;
  switch (Word) {
#define ORDERED_MEMORY_CASE(Bits, Scratch, Vector, Access, Result, Signed,     \
                            Offset)                                            \
  case Bits: {                                                                 \
    _Static_assert(((Bits) & ORDERED_REGISTER_MASK) ==                         \
                       ORDERED_REGISTER_FIELDS,                                \
                   ORDERED_REGISTER_ERROR);                                    \
    if (Vector) {                                                              \
      register Quadword V0 __asm__(ORDERED_VECTOR_REGISTER) =                  \
          (Quadword){Input.Low, Input.High};                                   \
      __asm__ volatile(ORDERED_INSTRUCTION                                     \
                       : "+w"(V0)                                              \
                       : "r"(X1), [encoding] "i"(Bits)                         \
                       : "memory");                                            \
      return V0[Lane];                                                         \
    }                                                                          \
    register uint64_t X0 __asm__(ORDERED_DATA_REGISTER) = Input.Low;           \
    __asm__ volatile(ORDERED_INSTRUCTION                                       \
                     : "+r"(X0)                                                \
                     : "r"(X1), [encoding] "i"(Bits)                           \
                     : "memory");                                              \
    return X0;                                                                 \
  }
#include "AArch64OrderedMemoryFixtures.def"
  }
  __builtin_unreachable();
}

uint64_t accessOrdered(uint32_t Word, uintptr_t Base, unsigned Lane,
                       OrderedValue Input, bool Original) {
  if (Original)
    return originalOrdered(Word, Base, Lane, Input);
  return generatedOrdered(Word, Base, Lane, Input.Low, Input.High);
}

static uint64_t expectedLane(const OrderedCase *Case, const uint8_t *Address,
                             unsigned Lane) {
  uint64_t Value = 0;
  unsigned Start = Lane * sizeof(Value);
  for (unsigned I = 0; I < sizeof(Value) && Start + I < Case->Width; ++I)
    Value |= (uint64_t)Address[Start + I] << (I * __CHAR_BIT__);
  if (Case->Signed) {
    const uint64_t Sign = UINT64_C(1) << (Case->Width * __CHAR_BIT__ - 1);
    Value = (Value ^ Sign) - Sign;
  }
  return Case->ResultBytes == sizeof(uint32_t) ? (uint32_t)Value : Value;
}

static bool checkAccess(const OrderedCase *Case, unsigned Residue,
                        OrderedValue Input, unsigned Seed, bool Original,
                        unsigned Mode, unsigned EL) {
  _Alignas(ORDERED_QUAD_BYTES)
      uint8_t Bytes[ORDERED_QUAD_BYTES * ORDERED_GUARD_QUADS];
  for (unsigned I = 0; I != sizeof(Bytes); ++I)
    Bytes[I] = (uint8_t)(I * ORDERED_DATA_STEP + Seed);
  uint8_t *Address = Bytes + ORDERED_QUAD_BYTES + Residue;
  const uintptr_t Base = (uintptr_t)Address - Case->Offset;
  const bool Load = Case->ResultBytes != 0;
  const bool Unaligned = Residue % Case->Width != 0;
  const bool Crosses = Residue + Case->Width > ORDERED_QUAD_BYTES;
  const bool ExpectFault = (Unaligned && (Mode & ORDERED_MODE_A)) ||
                           (Crosses && !(Mode & ORDERED_MODE_NAA));
  const unsigned Lanes = Case->Vector && Load ? ORDERED_LANES : 1;
  for (unsigned Lane = 0; Lane != Lanes; ++Lane) {
    OrderedAbort Abort;
    uint64_t Result;
    if (observeOrdered(Case->Word, Base, Lane, Input, Original, ExpectFault,
                       &Result, &Abort) != ExpectFault)
      return false;
    if (ExpectFault) {
      const uint64_t ESR = Abort.ESR;
      if (Abort.FAR != (uintptr_t)Address ||
          (ESR >> ORDERED_EC_SHIFT) !=
              (EL ? ORDERED_ABORT_EL1 : ORDERED_ABORT_EL0) ||
          (ESR & (ORDERED_DFSC_MASK | ORDERED_FNV)) !=
              ORDERED_ALIGNMENT_FAULT ||
          !!(ESR & ORDERED_WRITE) == Load ||
          ((Abort.Word ^ Case->Word) & ~ORDERED_REGISTER_MASK))
        return false;
    } else if (Load && Result != expectedLane(Case, Address, Lane)) {
      return false;
    }
  }
  for (unsigned I = 0; I != sizeof(Bytes); ++I) {
    uint8_t Expected = (uint8_t)(I * ORDERED_DATA_STEP + Seed);
    const ptrdiff_t Pos = Bytes + I - Address;
    if (!Load && !ExpectFault && Pos >= 0 && (size_t)Pos < Case->Width) {
      const uint64_t Half =
          Pos < (ptrdiff_t)sizeof(uint64_t) ? Input.Low : Input.High;
      Expected = (uint8_t)(Half >> ((Pos % sizeof(Half)) * __CHAR_BIT__));
    }
    if (Bytes[I] != Expected)
      return false;
  }
  return true;
}

int compareOrdered(unsigned Mode, unsigned EL, bool WithSIMD) {
  static const OrderedValue Inputs[] = ORDERED_REGISTER_INPUTS;
  static const uint8_t Seeds[] = ORDERED_DATA_SEEDS;
  int Check = 0;
  for (unsigned I = 0; I != ORDERED_CASE_COUNT; ++I) {
    if (Cases[I].Vector && !WithSIMD)
      continue;
    for (unsigned Residue = 0; Residue != ORDERED_QUAD_BYTES; ++Residue)
      for (size_t R = 0; R != sizeof(Inputs) / sizeof(*Inputs); ++R)
        for (size_t M = 0; M != sizeof(Seeds) / sizeof(*Seeds); ++M)
          for (unsigned Original = 0; Original != ORDERED_SOURCE_COUNT;
               ++Original) {
            ++Check;
            if (!checkAccess(&Cases[I], Residue, Inputs[R], Seeds[M], Original,
                             Mode, EL))
              return Check;
          }
  }
  return 0;
}
