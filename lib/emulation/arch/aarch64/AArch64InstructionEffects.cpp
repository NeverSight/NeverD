//===- AArch64InstructionEffects.cpp - Checked ARM64 ISA effects ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "AArch64InstructionEffects.h"

#include "../../core/ExecutionDiagnostics.h"

#include "neverd/ir/intrinsics/AArch64/AArch64StructureMemory.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/MathExtras.h"

#include <climits>

namespace neverd::emulation {
namespace {
static_assert((aarch64::SCTLR & aarch64::PAuthEnableMask) == 0,
              "checked PAuth hints require authentication disabled");
static_assert((aarch64::TCR & 0x3f) == 16 &&
                  ((aarch64::TCR >> 16) & 0x3f) == 16 &&
                  aarch64::UserMax == 0x0000ffffffffffffULL,
              "checked XPACLRI requires 48-bit translation ranges");
bool disabledPAuthHint(uint32_t Word) {
  switch (Word) {
#define NEVERD_AARCH64_PAUTH_HINT(Name, Encoding) case Encoding:
#include "AArch64PAuthHints.def"
#undef NEVERD_AARCH64_PAUTH_HINT
    return true;
  default:
    return false;
  }
}
bool unguardedBTIHint(uint32_t Word) {
  switch (Word) {
#define NEVERD_AARCH64_BTI_HINT(Name, Encoding) case Encoding:
#include "AArch64BTIHints.def"
#undef NEVERD_AARCH64_BTI_HINT
    return true;
  default:
    return false;
  }
}
namespace encoding {
#define NEVERD_AARCH64_ENCODING(Name, Mask, Value)                             \
  bool is##Name(uint32_t Word) { return (Word & Mask) == Value; }
#define NEVERD_AARCH64_FIELD(Name, Value) constexpr unsigned Name = Value;
#include "CheckedAArch64Instructions.def"
#undef NEVERD_AARCH64_ENCODING
#undef NEVERD_AARCH64_FIELD
} // namespace encoding
bool scalarRegister(unsigned Register) {
  switch (Register) {
#define NEVERD_AARCH64_OPERAND(Name, Bits)                                     \
  case AARCH64_REG_##Name:                                                     \
    return true;
#include "AArch64OperandRegisters.def"
#undef NEVERD_AARCH64_OPERAND
  default:
    return false;
  }
}
unsigned vectorRegisterWidth(unsigned Register) {
#define NEVERD_AARCH64_VECTOR_VIEW(Name, Bits)                                 \
  if (Register >= AARCH64_REG_##Name##0 && Register <= AARCH64_REG_##Name##31) \
    return Bits;
#include "AArch64OperandRegisters.def"
#undef NEVERD_AARCH64_VECTOR_VIEW
  return 0;
}
bool baselineFeatures(const cs_insn &I) {
  for (unsigned Index = 0; Index < I.detail->groups_count; ++Index) {
    const auto Group = I.detail->groups[Index];
    if (Group >= AARCH64_FEATURE_HASV8_0A &&
        Group != AARCH64_FEATURE_HASV8_0A &&
        Group != AARCH64_FEATURE_HASFPARMV8 &&
        Group != AARCH64_FEATURE_HASNEON &&
        // This is a disjunction, not an SME requirement. Baseline NEON
        // supplies fixed-width lane moves carrying this Capstone group.
        Group != AARCH64_FEATURE_HASNEONORSME)
      return false;
  }
  return true;
}
llvm::Expected<std::vector<AArch64MemoryAccess>>
structureMemoryEffects(uint32_t Word, const AArch64MachineState &State,
                       uint64_t Base, unsigned First) {
  const auto I = aarch64StructureMemoryInstruction(Word);
  if (!I || I->totalAccessBytes() - 1 > UINT64_MAX - Base)
    return llvm::make_error<UnsupportedExecutionError>();
  std::vector<AArch64MemoryAccess> Accesses;
  Accesses.reserve(I->totalAccessBytes() / I->ElementBytes);
  for (unsigned N = 0; N != I->totalAccessBytes() / I->ElementBytes; ++N) {
    RegisterValue Value{};
    if (!I->isLoad()) {
      const unsigned Register =
          (First + I->memoryVector(N)) & kAddressMemoryRegisterMask;
      const unsigned Offset = I->memoryVectorByte(N);
      Value[0] = State.Vectors[Register][Offset / aarch64::WordBytes] >>
                 ((Offset % aarch64::WordBytes) * CHAR_BIT);
    }
    Accesses.push_back({Base + N * I->ElementBytes, Value, I->ElementBytes,
                        I->isLoad() ? Read : Write});
  }
  return Accesses;
}
} // namespace
llvm::Expected<std::vector<AArch64MemoryAccess>>
getAArch64InstructionEffects(const cs_insn &I,
                             const AArch64MachineState &State) {
  using namespace encoding;
  if (I.size != aarch64::InstructionBytes || !I.detail)
    return llvm::make_error<UnsupportedExecutionError>();
  const uint32_t Word = llvm::support::endian::read32le(I.bytes);
  // Exact HINT-space words are compatible with the fixed disabled-key
  // machine, even when the decoder names their optional PAuth aliases.
  // They still execute through Machine::step and consume an ordinary attempt.
  if (disabledPAuthHint(Word))
    return std::vector<AArch64MemoryAccess>();
  // XPACLRI (HINT #7) strips PAC bits even with authentication disabled.
  // Low 48-bit addresses are fixed points in this translation regime, so
  // PAuth stripping and a baseline transport's legacy HINT agree. Execute
  // the original word; signed/tagged pointers require a separate contract.
  if (Word == 0xd50320ff) {
    if (State.reg(AArch64Register::X30) > aarch64::UserMax)
      return llvm::make_error<UnsupportedExecutionError>();
    return std::vector<AArch64MemoryAccess>();
  }
  // AArch64PageTables never sets GP. Execute the original landing-pad word,
  // including on a transport that implements FEAT_BTI; do not replace bytes
  // or infer guarded-page enforcement from the decoder's optional feature.
  if (unguardedBTIHint(Word))
    return std::vector<AArch64MemoryAccess>();
  enum InstructionKind {
    Integer,
    Memory,
    StructureMemory,
    System,
    CacheMaintenance,
    Barrier,
    Floating,
    Vector,
    ScalarConversion
  } Kind;
  switch (I.id) {
#define NEVERD_AARCH64_INSTRUCTION(Name, Type)                                 \
  case AARCH64_INS_##Name:                                                     \
    Kind = Type;                                                               \
    break;
#include "CheckedAArch64Instructions.def"
#undef NEVERD_AARCH64_INSTRUCTION
  default:
    return llvm::make_error<UnsupportedExecutionError>();
  }
  if (Kind == System &&
      !((I.id == AARCH64_INS_MRS && isReadThreadPointer(Word)) ||
        (I.id == AARCH64_INS_MRS && isReadCacheType(Word)) ||
        (I.id == AARCH64_INS_MSR && isWriteThreadPointer(Word)) ||
        (I.id == AARCH64_INS_MRS &&
         (isReadNZCV(Word) || isReadFPControl(Word) || isReadFPStatus(Word))) ||
        (I.id == AARCH64_INS_MSR &&
         (isWriteNZCV(Word) || isWriteFPControl(Word) ||
          isWriteFPStatus(Word)))))
    return llvm::make_error<UnsupportedExecutionError>();
  const auto Source = Word & RegisterMask;
  if (Kind == CacheMaintenance) {
    if (!isCleanDataToUnification(Word) &&
        !isInvalidateInstructionToUnification(Word))
      return llvm::make_error<UnsupportedExecutionError>();
    const uint64_t Address =
        Source == aarch64::GPRCount ? 0 : State.Registers[Source];
    if (!aarch64::canonicalRange(Address, 1))
      return llvm::make_error<UnsupportedExecutionError>();
    return std::vector<AArch64MemoryAccess>{{Address, {}, 1, Read, true}};
  }
  if (Kind == Barrier) {
    // Named baseline DSB domains/access types only. The remaining options
    // include optional speculation barriers and are not baseline DSB forms.
    if ((I.id == AARCH64_INS_DSB && isDataSynchronization(Word) &&
         ((Word >> BarrierOptionShift) & BarrierAccessMask)) ||
        (I.id == AARCH64_INS_ISB && isInstructionSynchronization(Word)))
      return std::vector<AArch64MemoryAccess>();
    return llvm::make_error<UnsupportedExecutionError>();
  }
  const bool UsesFloatingState = Kind == Floating || Kind == ScalarConversion;
  if (UsesFloatingState &&
      (State.reg(AArch64Register::FPCR) & ~aarch64::AllowedFPCR))
    return llvm::make_error<UnsupportedExecutionError>();
  // MSR NZCV selects only source bits31:28. Its other input bits are ignored
  // by the original instruction, unlike this bounded FP control contract.
  if (Kind == System && I.id == AARCH64_INS_MSR &&
      Source != aarch64::GPRCount &&
      ((isWriteFPControl(Word) &&
        (State.Registers[Source] & ~aarch64::AllowedFPCR)) ||
       (isWriteFPStatus(Word) &&
        (State.Registers[Source] & ~aarch64::AllowedFPSR))))
    return llvm::make_error<UnsupportedExecutionError>();
  if (I.id == AARCH64_INS_HINT && !isNOP(Word))
    return llvm::make_error<UnsupportedExecutionError>();
  const auto &A = I.detail->aarch64;
  if (Kind == ScalarConversion) {
    const bool Resize = I.id == AARCH64_INS_FCVT;
    const bool FromInteger =
        I.id == AARCH64_INS_SCVTF || I.id == AARCH64_INS_UCVTF;
    const unsigned FloatIndex = FromInteger ? 0 : 1;
    // GPR conversions and equal-width scalar SIMD integer-to-float forms use
    // binary32/binary64. Fixed-point immediates, packed vectors and optional
    // half precision remain outside this contract despite their shared IDs.
    if (A.op_count != 2 || A.operands[0].type != AARCH64_OP_REG ||
        A.operands[1].type != AARCH64_OP_REG ||
        A.operands[FloatIndex].vas != AARCH64LAYOUT_INVALID)
      return llvm::make_error<UnsupportedExecutionError>();
    const unsigned Width = vectorRegisterWidth(A.operands[FloatIndex].reg);
    if (Width != 32 && Width != 64)
      return llvm::make_error<UnsupportedExecutionError>();
    const auto &Other = A.operands[1 - FloatIndex];
    if (Resize) {
      const unsigned OtherWidth = vectorRegisterWidth(Other.reg);
      if ((OtherWidth != 32 && OtherWidth != 64) || OtherWidth == Width ||
          Other.vas != AARCH64LAYOUT_INVALID)
        return llvm::make_error<UnsupportedExecutionError>();
    } else if (!scalarRegister(Other.reg) &&
               !(FromInteger && vectorRegisterWidth(Other.reg) == Width &&
                 Other.vas == AARCH64LAYOUT_INVALID)) {
      return llvm::make_error<UnsupportedExecutionError>();
    }
  }
  bool HasVector = UsesFloatingState || Kind == Vector;
  for (unsigned N = 0; N < A.op_count; ++N) {
    const auto &O = A.operands[N];
    if (O.type == AARCH64_OP_REG && !scalarRegister(O.reg)) {
      const unsigned Width = vectorRegisterWidth(O.reg);
      if (!Width)
        return llvm::make_error<UnsupportedExecutionError>();
      HasVector = true;
      if (UsesFloatingState &&
          (Width < FP32Bits ||
           (O.vas != AARCH64LAYOUT_INVALID &&
            (unsigned(O.vas) & LayoutElementMask) < FP32Bits)))
        return llvm::make_error<UnsupportedExecutionError>();
    }
    // FCMLE's encoded zero is a typed system immediate in Capstone, not
    // an ordinary FP operand. Admit only this instruction's exact third zero.
    const bool ExactFloatingZero =
        I.id == AARCH64_INS_FCMLE && A.op_count == 3 && N == 2 &&
        O.type == AARCH64_OP_SYSIMM &&
        O.sysop.sub_type == AARCH64_OP_EXACTFPIMM &&
        O.sysop.imm.exactfpimm == AARCH64_EXACTFPIMM_ZERO;
    if ((Kind == Integer || Kind == Vector || UsesFloatingState) &&
        O.type != AARCH64_OP_REG && O.type != AARCH64_OP_IMM &&
        !(Kind == Floating && O.type == AARCH64_OP_FP) && !ExactFloatingZero)
      return llvm::make_error<UnsupportedExecutionError>();
  }
  if (HasVector && !baselineFeatures(I))
    return llvm::make_error<UnsupportedExecutionError>();
  std::vector<AArch64MemoryAccess> Accesses;
  auto GPR = [&](unsigned R) {
    return R == aarch64::GPRCount ? uint64_t(0) : State.Registers[R];
  };
  const unsigned Rt = Word & RegisterMask;
  const unsigned Rn = (Word >> BaseShift) & RegisterMask;
  const uint64_t Base =
      Rn == aarch64::GPRCount ? State.reg(AArch64Register::SP) : GPR(Rn);
  if (Kind == StructureMemory)
    return structureMemoryEffects(Word, State, Base, Rt);
  if (Kind == Memory) {
    const bool Vector = Word & VectorMemoryBit;
    const auto Operand = [&](unsigned Register) {
      return Vector ? State.Vectors[Register] : RegisterValue{GPR(Register), 0};
    };
    bool Load = Word & LoadBit;
    unsigned Size = 1u << (Word >> SizeShift);
    uint64_t Address = Base;
    bool Writeback = false;
    if (isAcquireReleaseMemory(Word)) {
      // Admit only naturally aligned ordinary RAM. Original instructions still
      // execute through the processor transport, under the shared physical
      // execution lease and RAM transaction; no exclusive monitor is modeled.
      if (Address % Size)
        return llvm::make_error<UnsupportedExecutionError>();
      Accesses.push_back({Address, Operand(Rt), Size, Load ? Read : Write});
    } else if (isUnsignedMemory(Word) || isImmediateMemory(Word) ||
               isRegisterMemory(Word)) {
      unsigned Opcode = (Word >> OpcodeShift) & OpcodeMask;
      Load = Vector ? bool(Opcode & VectorLoadBit) : Opcode != 0;
      if (Vector && (Opcode & VectorFullWidthBit)) {
        if (Size != 1)
          return llvm::make_error<UnsupportedExecutionError>();
        Size = VectorBytes;
      }
      // The size=8/opc=2 encoding is prefetch, not a scalar load.
      if (!Vector && Size == aarch64::WordBytes && Opcode > 1)
        return llvm::make_error<UnsupportedExecutionError>();
      if (isUnsignedMemory(Word))
        Address +=
            uint64_t((Word >> UnsignedOffsetShift) & UnsignedOffsetMask) * Size;
      else if (isImmediateMemory(Word)) {
        const unsigned Mode = (Word >> ModeShift) & OpcodeMask;
        Writeback = Mode == 1 || Mode == 3;
        if (Mode != 1)
          Address += llvm::SignExtend64<ImmediateBits>(Word >> ImmediateShift);
      } else {
        const unsigned Rm = (Word >> IndexShift) & RegisterMask;
        uint64_t Offset = GPR(Rm);
        switch ((Word >> ExtendShift) & ExtendMask) {
        case UXTW:
          Offset = uint32_t(Offset);
          break;
        case UXTX:
          break;
        case SXTW:
          Offset = uint64_t(int64_t(int32_t(Offset)));
          break;
        case SXTX:
          break;
        default:
          return llvm::make_error<UnsupportedExecutionError>();
        }
        if (Word & ScaleBit)
          Offset *= Size;
        Address += Offset;
      }
      if (!Vector && Writeback && Rn != aarch64::GPRCount && Rn == Rt)
        return llvm::make_error<UnsupportedExecutionError>();
      Accesses.push_back({Address, Operand(Rt), Size, Load ? Read : Write});
    } else if (isPairMemory(Word)) {
      const unsigned Rt2 = (Word >> SecondShift) & RegisterMask;
      Size = (Word & PairWidthBit) ? aarch64::WordBytes
                                   : aarch64::InstructionBytes;
      if (Vector) {
        const unsigned Opcode = Word >> SizeShift;
        if (Opcode == OpcodeMask)
          return llvm::make_error<UnsupportedExecutionError>();
        Size = aarch64::InstructionBytes << Opcode;
      }
      const unsigned Mode = (Word >> PairModeShift) & OpcodeMask;
      Writeback = Mode == 1 || Mode == 3;
      if (Mode != 1)
        Address += uint64_t(llvm::SignExtend64<PairOffsetBits>(
                       Word >> PairOffsetShift)) *
                   Size;
      if ((Load && Rt == Rt2) ||
          (!Vector && Writeback && Rn != aarch64::GPRCount &&
           (Rn == Rt || Rn == Rt2)))
        return llvm::make_error<UnsupportedExecutionError>();
      if (Size > UINT64_MAX - Address)
        return llvm::make_error<UnsupportedExecutionError>();
      Accesses.push_back({Address, Operand(Rt), Size, Load ? Read : Write});
      Accesses.push_back(
          {Address + Size, Operand(Rt2), Size, Load ? Read : Write});
    } else if (isLiteralMemory(Word)) {
      const unsigned Opcode = Word >> SizeShift;
      if (Opcode == OpcodeMask)
        return llvm::make_error<UnsupportedExecutionError>();
      Size = Opcode == 1 ? aarch64::WordBytes : aarch64::InstructionBytes;
      if (Vector)
        Size = aarch64::InstructionBytes << Opcode;
      Address = I.address + uint64_t(llvm::SignExtend64<LiteralOffsetBits>(
                                Word >> LiteralOffsetShift)) *
                                aarch64::InstructionBytes;
      Accesses.push_back({Address, {}, Size, Read});
    } else
      return llvm::make_error<UnsupportedExecutionError>();
  }
  for (const auto &M : Accesses)
    if (M.Size - 1 > UINT64_MAX - M.Address)
      return llvm::make_error<UnsupportedExecutionError>();
  return Accesses;
}
} // namespace neverd::emulation
