//===- Arm64eOrderedModel.c - Compare signed-offset LRCPC3 quadwords -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercise each byte alignment in Normal memory and inspect the hardware
/// exception state before resuming past an expected alignment fault.
//===----------------------------------------------------------------------===//

#include "Arm64eOrderedModel.def"

#include "Arm64eReturnModel.def"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint64_t Quadword __attribute__((vector_size(ORDERED_QUAD_BYTES)));

typedef struct {
  uint64_t Armed, ESR, FAR, Word;
} FaultRecord;
_Static_assert(offsetof(FaultRecord, Armed) == ORDERED_ARMED_SLOT &&
                   offsetof(FaultRecord, ESR) == ORDERED_ESR_SLOT &&
                   offsetof(FaultRecord, FAR) == ORDERED_FAR_SLOT &&
                   offsetof(FaultRecord, Word) == ORDERED_WORD_SLOT,
               ORDERED_LAYOUT_ERROR);

#define DECLARE(Index, Offset, LD, ST)                                         \
  extern uint64_t ORDERED_SYMBOL(LD)(uintptr_t, uint64_t);                     \
  extern uint64_t ORDERED_SYMBOL(ST)(uintptr_t, uint64_t, uint64_t, uint64_t);
ORDERED_CASES(DECLARE)
#undef DECLARE

static uint64_t accessQuadword(unsigned Case, bool Original, bool Load,
                               uintptr_t Base, unsigned Lane) {
  Quadword Value = {ORDERED_STORE_LOW, ORDERED_STORE_HIGH};
  switch (Case) {
#define ACCESS(Index, Offset, LD, ST)                                          \
  case Index:                                                                  \
    if (Original) {                                                            \
      if (Load) {                                                              \
        __asm__ volatile(ORDERED_LOAD_ASM                                      \
                         : "+w"(Value)                                         \
                         : "r"(Base), "i"(Offset)                              \
                         : "memory");                                          \
        return Value[Lane];                                                    \
      }                                                                        \
      __asm__ volatile(ORDERED_STORE_ASM ::"w"(Value), "r"(Base), "i"(Offset)  \
                       : "memory");                                            \
      return 0;                                                                \
    }                                                                          \
    if (Load)                                                                  \
      return ORDERED_SYMBOL(LD)(Base, Lane);                                   \
    return ORDERED_SYMBOL(ST)(Base, 0, Value[0], Value[1]);
    ORDERED_CASES(ACCESS)
#undef ACCESS
  }
  __builtin_unreachable();
}

static bool checkAccess(unsigned Case, int Offset, uint32_t Word,
                        unsigned Misalignment, bool Original, bool Load,
                        bool Relaxed, unsigned EL) {
  _Alignas(ORDERED_QUAD_BYTES)
      uint8_t Bytes[ORDERED_QUAD_BYTES * ORDERED_GUARD_QUADS];
  for (unsigned I = 0; I != sizeof(Bytes); ++I)
    Bytes[I] = (uint8_t)(I * ORDERED_DATA_STEP + ORDERED_DATA_SEED);
  uint8_t *Address = Bytes + ORDERED_QUAD_BYTES + Misalignment;
  const uintptr_t Base = (uintptr_t)Address - Offset;
  const bool ExpectFault = Misalignment && !Relaxed;
  volatile FaultRecord *Fault = (volatile FaultRecord *)RETURN_MODEL_EL0_DATA;
  // The lifted load exposes one lane per call. Validate both independently.
  for (unsigned Lane = 0; Lane != (Load ? ORDERED_LANES : 1); ++Lane) {
    Fault->Armed = ExpectFault;
    const uint64_t Result = accessQuadword(Case, Original, Load, Base, Lane);
    if (ExpectFault) {
      const uint64_t ESR = Fault->ESR;
      if (Fault->Armed || Fault->FAR != (uintptr_t)Address ||
          (ESR >> ORDERED_EC_SHIFT) !=
              (EL ? ORDERED_ABORT_EL1 : ORDERED_ABORT_EL0) ||
          (ESR & (ORDERED_DFSC_MASK | ORDERED_FNV)) !=
              ORDERED_ALIGNMENT_FAULT ||
          !!(ESR & ORDERED_WRITE) == Load ||
          ((Fault->Word ^ Word) & ~ORDERED_REGISTER_MASK))
        return false;
    } else if (Load) {
      uint64_t Expected;
      __builtin_memcpy_inline(&Expected, Address + Lane * sizeof(Expected),
                              sizeof(Expected));
      if (Result != Expected)
        return false;
    }
  }
  for (unsigned I = 0; I != sizeof(Bytes); ++I) {
    uint8_t Expected = (uint8_t)(I * ORDERED_DATA_STEP + ORDERED_DATA_SEED);
    const ptrdiff_t Pos = Bytes + I - Address;
    if (!Load && !ExpectFault && Pos >= 0 && Pos < ORDERED_QUAD_BYTES) {
      const uint64_t Half = Pos < (ptrdiff_t)sizeof(uint64_t)
                                ? ORDERED_STORE_LOW
                                : ORDERED_STORE_HIGH;
      Expected = (uint8_t)(Half >> ((Pos % sizeof(Half)) * __CHAR_BIT__));
    }
    if (Bytes[I] != Expected)
      return false;
  }
  return true;
}

int compareOrdered(unsigned Mode, unsigned EL) {
  static const struct {
    int Offset;
    uint32_t Load, Store;
  } Cases[] = {
#define ITEM(Index, Offset, LD, ST)                                            \
  {Offset, ORDERED_WORD(LD), ORDERED_WORD(ST)},
      ORDERED_CASES(ITEM)
#undef ITEM
  };
  _Static_assert(sizeof(Cases) / sizeof(*Cases) == ORDERED_CASE_COUNT,
                 ORDERED_CASE_ERROR);
  const bool Relaxed = Mode == ORDERED_MODE_NAA;
  int Check = 0;
  for (unsigned Case = 0; Case != ORDERED_CASE_COUNT; ++Case)
    for (unsigned Align = 0; Align != ORDERED_QUAD_BYTES; ++Align)
      for (unsigned Original = 0; Original != ORDERED_SOURCE_COUNT; ++Original)
        for (unsigned Load = 0; Load != ORDERED_DIRECTION_COUNT; ++Load) {
          ++Check;
          if (!checkAccess(Case, Cases[Case].Offset,
                           Load ? Cases[Case].Load : Cases[Case].Store, Align,
                           Original, Load, Relaxed, EL))
            return Check;
        }
  return 0;
}
