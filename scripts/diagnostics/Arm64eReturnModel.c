//===- Arm64eReturnModel.c - Run arm64e Mach-O comparison programs ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "Arm64eReturnModel.def"
#ifdef RETURN_MODEL_ORDERED
#include "Arm64eOrderedAccess.h"
#include "Arm64eOrderedModel.def"
#endif

#include <stddef.h>
#include <stdint.h>

extern int main(void);
extern int runAtEL0(int Enabled, uintptr_t StackTop);
#if defined(RETURN_MODEL_NORMAL_MEMORY)
#ifdef RETURN_MODEL_ORDERED
typedef struct {
  uint64_t Armed;
  OrderedAbort Abort;
} OrderedMailbox;

_Static_assert(offsetof(OrderedMailbox, Armed) == ORDERED_ARMED_SLOT &&
                   offsetof(OrderedMailbox, Abort.ESR) == ORDERED_ESR_SLOT &&
                   offsetof(OrderedMailbox, Abort.FAR) == ORDERED_FAR_SLOT &&
                   offsetof(OrderedMailbox, Abort.Word) == ORDERED_WORD_SLOT,
               ORDERED_LAYOUT_ERROR);

bool observeOrdered(uint32_t Word, uintptr_t Base, unsigned Lane,
                    OrderedValue Input, bool Original, bool MayFault,
                    uint64_t *Result, OrderedAbort *Abort) {
  volatile OrderedMailbox *Mailbox =
      (volatile OrderedMailbox *)RETURN_MODEL_EL0_DATA;
  Mailbox->Armed = MayFault;
  *Result = accessOrdered(Word, Base, Lane, Input, Original);
  if (!MayFault || Mailbox->Armed)
    return false;
  Abort->ESR = Mailbox->Abort.ESR;
  Abort->FAR = Mailbox->Abort.FAR;
  Abort->Word = Mailbox->Abort.Word;
  return true;
}
#else
extern int compareCursorModes(int Enabled, uintptr_t StackTop);
#endif

// Clang can lower aggregate copies and initializers to these C runtime calls
// in the O0 guest. The flat Mach-O image has no dynamic loader.
void *memcpy(void *Dest, const void *Source, size_t Size) {
  unsigned char *Out = Dest;
  const unsigned char *In = Source;
  for (size_t I = 0; I < Size; ++I)
    Out[I] = In[I];
  return Dest;
}

void *memset(void *Dest, int Byte, size_t Size) {
  unsigned char *Out = Dest;
  for (size_t I = 0; I < Size; ++I)
    Out[I] = (unsigned char)Byte;
  return Dest;
}

#elif defined(RETURN_MODEL_PREFIX)
extern int comparePrefixModes(int Enabled);
#endif

static void writeText(const char *Message) {
  register uintptr_t Operation __asm__(RETURN_MODEL_OPERATION_REGISTER) =
      RETURN_MODEL_WRITE0;
  register const void *Parameter __asm__(RETURN_MODEL_PARAMETER_REGISTER) =
      Message;
  __asm__ volatile(RETURN_MODEL_TRAP
                   : "+r"(Operation)
                   : "r"(Parameter)
                   : "memory");
}

static void finish(const char *Message, unsigned Status) {
  writeText(Message);
  const uintptr_t Exit[] = {RETURN_MODEL_APP_EXIT, Status};
  register uintptr_t Operation __asm__(RETURN_MODEL_OPERATION_REGISTER) =
      RETURN_MODEL_EXIT;
  register const void *Parameter __asm__(RETURN_MODEL_PARAMETER_REGISTER) =
      Exit;
  __asm__ volatile(RETURN_MODEL_TRAP
                   : "+r"(Operation)
                   : "r"(Parameter)
                   : "memory");
  __builtin_unreachable();
}

static void writeValue(const char *Name, uint64_t Value) {
  char Hex[sizeof(Value) * 2 + 1];
  Hex[sizeof(Hex) - 1] = 0;
  for (unsigned I = sizeof(Hex) - 1; I; --I) {
    Hex[I - 1] = RETURN_MODEL_HEX[Value & RETURN_MODEL_NIBBLE_MASK];
    Value >>= RETURN_MODEL_NIBBLE_BITS;
  }
  writeText(Name);
  writeText(Hex);
}

static void checkOracle(int Status, unsigned Mode, const char *Error) {
  if (!Status)
    return;
  uint64_t TCR, SCTLR;
  __asm__ volatile(RETURN_MODEL_READ(TCR_EL1) : "=r"(TCR));
  __asm__ volatile(RETURN_MODEL_READ(SCTLR_EL1) : "=r"(SCTLR));
  writeValue(RETURN_MODEL_STATUS, Status);
  writeValue(RETURN_MODEL_MODE, Mode);
  writeValue(RETURN_MODEL_TCR, TCR);
  writeValue(RETURN_MODEL_SCTLR, SCTLR);
  writeText(RETURN_MODEL_NEWLINE);
  finish(Error, 1);
}

void unexpectedReturnException(void) {
  uint64_t ESR, ELR, FAR;
  __asm__ volatile(RETURN_MODEL_READ(ESR_EL1) : "=r"(ESR));
  __asm__ volatile(RETURN_MODEL_READ(ELR_EL1) : "=r"(ELR));
  __asm__ volatile(RETURN_MODEL_READ(FAR_EL1) : "=r"(FAR));
  writeValue(RETURN_MODEL_ESR, ESR);
  writeValue(RETURN_MODEL_ELR, ELR);
  writeValue(RETURN_MODEL_FAR, FAR);
  writeText(RETURN_MODEL_NEWLINE);
  finish(RETURN_MODEL_EXCEPTION, 1);
}

int runReturnOracle(int Enabled, uintptr_t StackTop) {
#ifdef RETURN_MODEL_ORDERED
  (void)StackTop;
  return compareOrdered(Enabled, ORDERED_EL0, true);
#elif defined(RETURN_MODEL_CURSOR)
  return compareCursorModes(Enabled, StackTop);
#elif defined(RETURN_MODEL_PREFIX)
  (void)StackTop;
  return comparePrefixModes(Enabled);
#else
  (void)Enabled;
  (void)StackTop;
  return main();
#endif
}

void runReturnModel(void) {
#ifdef RETURN_MODEL_ORDERED
  uint64_t ISAR1, MMFR2, Control;
  __asm__ volatile(RETURN_MODEL_READ(ID_AA64ISAR1_EL1) : "=r"(ISAR1));
  if (((ISAR1 >> ORDERED_LRCPC_SHIFT) & RETURN_MODEL_NIBBLE_MASK) <
      ORDERED_LRCPC_LEVEL)
    finish(ORDERED_FEATURE_ERROR, 1);
  __asm__ volatile(RETURN_MODEL_READ(ID_AA64MMFR2_EL1) : "=r"(MMFR2));
  if (((MMFR2 >> ORDERED_LSE2_SHIFT) & RETURN_MODEL_NIBBLE_MASK) !=
      ORDERED_LSE2_LEVEL)
    finish(ORDERED_LSE2_ERROR, 1);
  for (unsigned Mode = 0; Mode != ORDERED_MODE_COUNT; ++Mode) {
    __asm__ volatile(RETURN_MODEL_READ(SCTLR_EL1) : "=r"(Control));
    // The abort handler reads shared Normal memory at both exception levels.
    Control |= ORDERED_SPAN;
    const uint64_t Bits = ((Mode & ORDERED_MODE_NAA) ? ORDERED_NAA : 0) |
                          ((Mode & ORDERED_MODE_A) ? ORDERED_ALIGNMENT : 0);
    Control = (Control & ~(ORDERED_ALIGNMENT | ORDERED_NAA)) | Bits;
    __asm__ volatile(RETURN_MODEL_WRITE(SCTLR_EL1)::"r"(Control) : "memory");
    __asm__ volatile(RETURN_MODEL_READ(SCTLR_EL1) : "=r"(Control));
    if ((Control & (ORDERED_SPAN | ORDERED_ALIGNMENT | ORDERED_NAA)) !=
        (ORDERED_SPAN | Bits))
      finish(ORDERED_CONTROL_ERROR, 1);
    checkOracle(compareOrdered(Mode, ORDERED_EL1, true), Mode,
                RETURN_MODEL_EL1_ERROR);
    checkOracle(runAtEL0(Mode, RETURN_MODEL_STACK_EL0), Mode,
                RETURN_MODEL_EL0_ERROR);
    if (Mode & ORDERED_MODE_A)
      writeText((Mode & ORDERED_MODE_NAA) ? ORDERED_11_PASS : ORDERED_10_PASS);
    else
      writeText((Mode & ORDERED_MODE_NAA) ? ORDERED_01_PASS : ORDERED_00_PASS);
  }
  finish(ORDERED_PASS, 0);
#else
  uint64_t ISAR1, ISAR2, ISAR3, Control;
  __asm__ volatile(RETURN_MODEL_READ(ID_AA64ISAR1_EL1) : "=r"(ISAR1));
  __asm__ volatile(RETURN_MODEL_READ(RETURN_MODEL_ISAR2) : "=r"(ISAR2));
  __asm__ volatile(RETURN_MODEL_READ(RETURN_MODEL_ISAR3) : "=r"(ISAR3));
  const unsigned APA =
      (ISAR1 >> RETURN_MODEL_APA_SHIFT) & RETURN_MODEL_NIBBLE_MASK;
  const unsigned API =
      (ISAR1 >> RETURN_MODEL_API_SHIFT) & RETURN_MODEL_NIBBLE_MASK;
  const unsigned APA3 =
      (ISAR2 >> RETURN_MODEL_APA3_SHIFT) & RETURN_MODEL_NIBBLE_MASK;
  const unsigned PACM =
      (ISAR3 >> RETURN_MODEL_PACM_SHIFT) & RETURN_MODEL_NIBBLE_MASK;
  if ((APA < RETURN_MODEL_PAUTH_LR && API < RETURN_MODEL_PAUTH_LR &&
       APA3 < RETURN_MODEL_PAUTH_LR) ||
      PACM != RETURN_MODEL_PACM_FULL)
    finish(RETURN_MODEL_FEATURE_ERROR, 1);
  __asm__ volatile(RETURN_MODEL_READ(TCR_EL1) : "=r"(Control));
  if (Control != RETURN_MODEL_TCR_48BIT)
    finish(RETURN_MODEL_CONTROL_ERROR, 1);
  __asm__ volatile(RETURN_MODEL_READ(SCTLR_EL1) : "=r"(Control));
  if ((Control & RETURN_MODEL_ENIA_IB) != RETURN_MODEL_ENIA_IB)
    finish(RETURN_MODEL_CONTROL_ERROR, 1);
  for (unsigned Mode = 0; Mode <= RETURN_MODEL_ENPACM_MASK; ++Mode) {
    uint64_t Value;
    __asm__ volatile(RETURN_MODEL_READ(RETURN_MODEL_SCTLR2) : "=r"(Value));
    const uint64_t Mask = RETURN_MODEL_ENPACM_MASK << RETURN_MODEL_ENPACM_SHIFT;
    Value = (Value & ~Mask) | (Mode << RETURN_MODEL_ENPACM_SHIFT);
    __asm__ volatile(RETURN_MODEL_WRITE(RETURN_MODEL_SCTLR2)::"r"(Value)
                     : "memory");
    __asm__ volatile(RETURN_MODEL_READ(RETURN_MODEL_SCTLR2) : "=r"(Value));
    if (((Value & Mask) >> RETURN_MODEL_ENPACM_SHIFT) != Mode)
      finish(RETURN_MODEL_CONTROL_ERROR, 1);
    checkOracle(runReturnOracle(Mode & 1, RETURN_MODEL_STACK_EL1), Mode,
                RETURN_MODEL_EL1_ERROR);
    checkOracle(runAtEL0(Mode >> 1, RETURN_MODEL_STACK_EL0), Mode,
                RETURN_MODEL_EL0_ERROR);
    const char Setting[] = {RETURN_MODEL_ZERO + (Mode & 1),
                            RETURN_MODEL_ZERO + (Mode >> 1), '\n', 0};
    writeText(RETURN_MODEL_OBSERVATION);
    writeText(Setting);
  }
  finish(RETURN_MODEL_PASS, 0);
#endif
}
