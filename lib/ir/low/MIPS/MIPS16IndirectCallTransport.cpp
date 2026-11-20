//===- MIPS16IndirectCallTransport.cpp - o32 interworking calls -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/MIPS/MIPS16IndirectCallTransport.h"

#include "neverd/ArchSupport.h"
#include "neverd/decode/Decoder.h"
#include "neverd/lift/MIPS/MIPSRegs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ExecutableCodeOwnerIndex.h"

#include "llvm/Support/Endian.h"

namespace neverd {
namespace {
struct WordTransfer {
  unsigned GPR, FPR;
  bool High;
  bool operator==(const WordTransfer &) const = default;
};

bool isMove(uint32_t Word, unsigned To, unsigned From) {
  if ((Word & 63) != 33 && (Word & 63) != 37)
    return false;
  const uint32_t Fields = Word & ~63u;
  return Fields == ((From << 21) | (To << 11)) ||
         Fields == ((From << 16) | (To << 11));
}

std::optional<WordTransfer> transfer(uint32_t Word, bool ToFP) {
  const unsigned RS = Word >> 21 & 31;
  if (Word >> 26 != 17 || (Word & 0x7ff) ||
      (RS != (ToFP ? 4u : 0u) && RS != (ToFP ? 7u : 3u)))
    return {};
  return WordTransfer{Word >> 16 & 31, Word >> 11 & 31, RS == 3 || RS == 7};
}

// Match significance, independently of the order of moves to distinct FPRs.
// In FR=1 an MTC1 invalidates the upper word. Its MTHC1 must follow it.
bool matches(llvm::ArrayRef<WordTransfer> Actual, unsigned GPR, unsigned FPR,
             unsigned Bytes, bool Little, bool FP64, bool ToFP) {
  if (Actual.size() != Bytes / 4)
    return false;
  if (!Bytes)
    return true;
  const WordTransfer Low{GPR + (Bytes == 8 && !Little), FPR, false};
  if (Bytes == 4)
    return Actual.front() == Low;
  const WordTransfer High{GPR + Little, FPR + !FP64, FP64};
  return (Actual[0] == Low && Actual[1] == High) ||
         ((!FP64 || !ToFP) && Actual[1] == Low && Actual[0] == High);
}

bool classify(llvm::ArrayRef<WordTransfer> Arguments,
              llvm::ArrayRef<WordTransfer> Results, bool Returning, bool Little,
              bool FP64, MIPS16IndirectCallTransport &Proof) {
  bool Found = false;
  for (unsigned First : {0u, 4u, 8u})
    for (unsigned Second : {0u, 4u, 8u}) {
      if ((!First && Second) || Arguments.size() != (First + Second) / 4)
        continue;
      const unsigned NextGPR = First == 8 || Second == 8 ? 6 : 5;
      if (matches(Arguments.take_front(First / 4), 4, 12, First, Little, FP64,
                  true) &&
          matches(Arguments.drop_front(First / 4), NextGPR, 14, Second, Little,
                  FP64, true)) {
        Proof.FloatingPrefix = {First, Second};
        Found = true;
      }
    }
  if (!Found)
    return false;
  if (!Returning)
    return !Arguments.empty() && Results.empty();
  for (unsigned Bytes : {4u, 8u})
    for (unsigned Elements : {1u, 2u}) {
      if (Results.size() != Bytes / 4 * Elements)
        continue;
      // libgcc can copy the imaginary component before the real one.
      bool OK = true;
      for (unsigned I = 0; I < Elements; ++I) {
        llvm::SmallVector<WordTransfer, 2> Component;
        const unsigned GPR = 2 + I * (Bytes / 4);
        for (const auto &T : Results)
          if (T.GPR >= GPR && T.GPR < GPR + Bytes / 4)
            Component.push_back(T);
        OK &= matches(Component, GPR, I * 2, Bytes, Little, FP64, false);
      }
      if (OK) {
        Proof.ResultBytes = Bytes;
        Proof.ResultElements = Elements;
        return true;
      }
    }
  return false;
}
} // namespace

std::optional<MIPS16IndirectCallTransport>
proveMIPS16IndirectCallTransport(const BinaryImage &Image, va_t Entry,
                                 const ExecutableCodeOwnerIndex *Owners) {
  if (!isMIPS(Image.Arch) || isMIPS64(Image.Arch) || !Image.MIPS ||
      !Image.MIPS->HasMIPS16 || !Image.MIPS->hasScalarFPABI(false) ||
      Image.instructionModeAt(Entry) != InstructionMode::MIPSClassic)
    return {};
  const auto End = Image.getFunctionMetadataEnd(Entry, Owners);
  if (End == InvalidVA || End <= Entry)
    return {};
  const bool Little = archLittleEndian(Image.Arch);
  const auto WordAt = [&](va_t PC) -> std::optional<uint32_t> {
    if (PC < Entry || PC >= End || End - PC < 4 ||
        Image.instructionModeAt(PC) != InstructionMode::MIPSClassic)
      return {};
    const auto *P = Image.readVA(PC, 4);
    if (!P)
      return {};
    return Little ? llvm::support::endian::read32le(P)
                  : llvm::support::endian::read32be(P);
  };
  llvm::SmallVector<WordTransfer, 4> Arguments, Results;
  MIPS16IndirectCallTransport Proof;
  va_t PC = Entry;
  const auto First = WordAt(PC);
  if (!First)
    return {};
  const bool Returning = isMove(*First, 18, 31);
  if (Returning)
    PC += 4;
  while (const auto Word = WordAt(PC)) {
    const auto T = transfer(*Word, true);
    if (!T)
      break;
    if (Arguments.size() == 4)
      return {};
    Arguments.push_back(*T);
    PC += 4;
  }
  const va_t CallPC = PC;
  // jalr ra,v0 or jr v0. The executed slot publishes v0 unchanged in t9.
  const auto Slot = WordAt(PC + 4);
  if (WordAt(PC) != (Returning ? 0x0040f809u : 0x00400008u) || !Slot ||
      !isMove(*Slot, 25, 2))
    return {};
  PC += 8;
  if (Returning) {
    while (const auto Word = WordAt(PC)) {
      const auto T = transfer(*Word, false);
      if (!T)
        break;
      if (Results.size() == 4)
        return {};
      Results.push_back(*T);
      PC += 4;
    }
    if (WordAt(PC) != 0x02400008u) // jr s2 (saved caller link)
      return {};
    const auto Last = WordAt(PC + 4);
    if (!Last)
      return {};
    if (*Last) {
      const auto T = transfer(*Last, false);
      if (!T || Results.size() == 4)
        return {};
      Results.push_back(*T);
    }
    PC += 8;
  }
  if (!classify(Arguments, Results, Returning, Little,
                Image.MIPS->hasScalarFP64ABI(false), Proof))
    return {};
  Decoder D;
  if (!D.init(Image))
    return {};
  for (va_t Cursor = Entry; Cursor < PC;) {
    const auto Available = std::min<va_t>(8, PC - Cursor);
    const auto *Code = Image.readVA(Cursor, Available);
    DecodedExecutionUnit Unit;
    if (!Code || !D.selectMode(Image, Cursor) ||
        !D.decodeUnitForLift(Code, Available, Cursor, Unit))
      return {};
    std::vector<LowOp> Ops;
    try {
      D.liftUnitToLow(Unit, Ops);
    } catch (const UnliftedInstruction &) {
      return {};
    }
    if (Cursor == CallPC) {
      const auto Transfer = Returning ? NdOp::INDIR_CALL : NdOp::INDIR_BR;
      if (Ops.empty() || Ops.back().Opcode != Transfer ||
          Ops.back().NumInputs != 1)
        return {};
      Ops.back().Opcode = NdOp::INDIR_CALL;
      Ops.back().Output = NdVar::reg(mipsreg::V0, 4);
    } else if (Cursor + Unit.spanBytes() == PC) {
      if (Ops.empty() || Ops.back().Opcode != NdOp::INDIR_BR)
        return {};
      Ops.pop_back(); // The private source expansion resumes in the caller.
    }
    Proof.Effects.insert(Proof.Effects.end(), Ops.begin(), Ops.end());
    Cursor += Unit.spanBytes();
  }
  return Proof;
}
} // namespace neverd
