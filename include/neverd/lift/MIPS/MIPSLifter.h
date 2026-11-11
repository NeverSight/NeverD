//===- MIPSLifter.h - Classic MIPS integer lifting -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Profile-selected MIPS semantics, independent of Capstone alias spelling.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIFT_MIPS_MIPSLIFTER_H
#define NEVERD_LIFT_MIPS_MIPSLIFTER_H

#include "neverd/Common.h"
#include "neverd/lift/LiftCommon.h"

#include "capstone/capstone.h"
#include "llvm/ADT/ArrayRef.h"

#include <cstdint>
#include <vector>

namespace neverd {

class MIPSLifter {
public:
  explicit MIPSLifter(Arch Target,
                      InstructionMode Mode = InstructionMode::Default,
                      uint8_t Revision = 2)
      : Target(Target), Mode(Mode), ISARevision(Revision) {}

  void lift(const cs_insn &Instruction, std::vector<LowOp> &Ops,
            std::optional<bool> ForbiddenFollower = std::nullopt) const;
  static std::optional<bool> r6ForbiddenFollower(llvm::ArrayRef<uint8_t> Code,
                                                 bool Little);
  LowDelaySlot liftUnit(const cs_insn &Head, const cs_insn &Slot,
                        std::vector<LowOp> &Ops) const;
  uint32_t word(const cs_insn &Instruction) const;
  // Native instruction identities follow the public Capstone MIPS range.
#define NEVERD_MIPS_PHYSICAL_FORM(Name, Offset, Text)                          \
  static constexpr unsigned Name##ID = MIPS_INS_ENDING + Offset;
#include "neverd/lift/MIPS/MIPSPhysicalForms.def"
  bool isDSPIndexedDoubleword(uint32_t Word) const;
  std::optional<uint32_t> microASEWord(uint32_t PhysicalWord) const;
  /// Resolve a normalized compact branch using the physical occurrence length.
  uint64_t r6BranchTarget(uint32_t Word, uint64_t PC, unsigned Size) const;
  static bool hasDelaySlot(uint32_t Word);
  /// Zero: no slot; one: either physical width; two/four: required width.
  unsigned delaySlotBytes(const cs_insn &Instruction) const;
  LowInstructionTargetMode controlTargetMode(const cs_insn &Instruction) const;
  bool isReturn(const cs_insn &Instruction) const;
  bool isAlignmentNop(const cs_insn &Instruction) const;
  bool isFunctionTerminator(const cs_insn &Instruction) const;
  va_t directCallTarget(const cs_insn &Instruction) const;
  void setDSPEnabled(bool Enabled, bool Rev2 = false) {
    HasDSP = Enabled || Rev2;
    HasDSPR2 = Rev2;
  }
  void setMSAEnabled(bool Enabled) { HasMSA = Enabled; }
  void setMIPS16Enabled(bool Enabled) { HasMIPS16 = Enabled; }
  void setMIPS16E2Enabled(bool Enabled) { HasMIPS16E2 = Enabled; }
  void setMTEnabled(bool Enabled) { HasMT = Enabled; }

private:
  struct State : LiftStateBase {
    uint16_t Width;
    State(va_t Address, std::vector<LowOp> &Ops, uint16_t Width,
          uint16_t Size = 4)
        : LiftStateBase(Address, Size, Ops), Width(Width) {}

    NdVar read(unsigned Register, uint16_t Bytes = 0);
    NdVar select(NdVar Condition, NdVar Yes, NdVar No);
    NdVar accumulator(unsigned Bank);
    void writeAccumulator(unsigned Bank, NdVar Value);
    void discardHILO();
    NdVar vectorElement(unsigned Reg, unsigned Index, uint16_t Bytes);
    void writeVector(unsigned Reg, llvm::MutableArrayRef<NdVar> Elements);
    void requireWord(unsigned Register);
    NdVar resize(NdVar Value, uint16_t Bytes, bool SignExtend = false);
    void write(unsigned Register, NdVar Value, bool SignExtend = true);
    NdVar binary(NdOp Opcode, NdVar A, NdVar B, uint16_t Bytes = 0);
  };

  bool liftInteger(State &S, uint32_t Word) const;
  bool liftSystem(State &S, uint32_t Word) const;
  bool liftBitfield(State &S, uint32_t Word) const;
  bool liftMemory(State &S, uint32_t Word,
                  std::optional<NdVar> BaseOverride = std::nullopt) const;
  bool liftPCRelative(State &S, uint32_t Word, va_t BasePC) const;
  void liftMIPS16Frame(State &S, uint32_t Word) const;
  bool liftDSP(State &S, uint32_t Word) const;
  bool liftDSPAdditional(State &S, uint32_t Word) const;
  bool liftMSA(State &S, uint32_t Word) const;
  bool liftMSAAdditional(State &S, uint32_t Word) const;
  bool liftMSAPermutation(State &S, uint32_t Word) const;
  struct Encoding {
    uint32_t Word;
    unsigned SlotBytes = 0;
    bool Unconditional = false;
    bool PCRelative = false;
    bool Frame = false;
    bool R6 = false;
  };
  Encoding release6Encoding(uint32_t Word) const;
  static bool isR6Compact(uint32_t Word);
  static bool isR6ConditionalCompact(uint32_t Word);
  bool liftR6Compact(State &S, uint32_t Word, bool ForbiddenFollower) const;
  bool liftRelease6(State &S, uint32_t Word) const;
  bool liftNonControl(State &S, Encoding Code, va_t BasePC) const;
  std::optional<Encoding> encoding(const cs_insn &Instruction) const;
  std::optional<Encoding> microEncoding(const cs_insn &Instruction) const;
  std::optional<Encoding> mips16JumpEncoding(const cs_insn &Instruction) const;
  std::optional<Encoding> mips16E2Encoding(uint16_t First, uint16_t Half) const;
  std::optional<Encoding> mips16Encoding(const cs_insn &Instruction) const;
  LowDelaySlot liftControl(const cs_insn &Head, const cs_insn *Slot,
                           Encoding Code, std::vector<LowOp> &Ops) const;
  Arch Target;
  InstructionMode Mode;
  uint8_t ISARevision;
  bool HasDSP = false;
  bool HasDSPR2 = false;
  bool HasMSA = false;
  bool HasMIPS16 = false;
  bool HasMIPS16E2 = false;
  bool HasMT = false;
};

} // namespace neverd

#endif // NEVERD_LIFT_MIPS_MIPSLIFTER_H
