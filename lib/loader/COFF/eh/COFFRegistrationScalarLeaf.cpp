//===- COFFRegistrationScalarLeaf.cpp - PE32 scalar helpers ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

/// \file
/// Authenticate scalar helper paths and retain their code and storage extents.
//===----------------------------------------------------------------------===//

#include "COFFRegistrationScalarLeaf.h"

#include "neverd/decode/Decoder.h"
#include "neverd/loader/Limits.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/support/BinaryEncoding.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <set>
#include <tuple>
#include <vector>

namespace neverd::coff_loader {
namespace {

unsigned fullRegisterBit(x86_reg Reg) {
  switch (Reg) {
  case X86_REG_EAX:
    return 1;
  case X86_REG_ECX:
    return 2;
  case X86_REG_EDX:
    return 4;
  default:
    return 0;
  }
}

unsigned scalarRegisterBit(x86_reg Reg) {
  switch (Reg) {
  case X86_REG_AL:
  case X86_REG_AH:
  case X86_REG_AX:
    return 1;
  case X86_REG_CL:
  case X86_REG_CH:
  case X86_REG_CX:
    return 2;
  case X86_REG_DL:
  case X86_REG_DH:
  case X86_REG_DX:
    return 4;
  default:
    return fullRegisterBit(Reg);
  }
}

struct ScalarState {
  va_t Address;
  unsigned Registers = 0;
  bool Scratch = false;
  bool Flags = false;
};

class ScalarLeafProof {
  const BinaryImage &Image;
  va_t Entry;
  Decoder Dec;
  std::map<va_t, va_t> Instructions;
  std::set<va_t> ImageReads;
  std::map<va_t, va_t> AbsoluteOperands;

  // Distinct paths may reach one instruction with different initialized
  // registers. Check each state; neither a favorable path nor a join supplies
  // a definition to another path. All edges advance and work stays bounded.
  std::set<std::tuple<va_t, unsigned, bool, bool>> Seen;
  std::vector<ScalarState> Work;

  bool recordInstruction(va_t Address, unsigned Size) {
    if (!Size ||
        Size > ::neverd::limits::loader::
                   kCOFFRegistrationScalarLeafMaxHelperBytes ||
        Address < Entry ||
        Address - Entry > ::neverd::limits::loader::
                                  kCOFFRegistrationScalarLeafMaxHelperBytes -
                              Size)
      return false;
    auto Next = Instructions.lower_bound(Address);
    if (Next != Instructions.end() && Next->first == Address)
      return Next->second == Address + Size;
    if ((Next != Instructions.end() && Next->first < Address + Size) ||
        (Next != Instructions.begin() && std::prev(Next)->second > Address))
      return false;
    Instructions.emplace_hint(Next, Address, Address + Size);
    return true;
  }

  bool readScalar(const cs_x86_op &Op, const ScalarState &State,
                  bool &Absolute) {
    if (Op.type == X86_OP_IMM)
      return true;
    if (Op.type == X86_OP_REG) {
      const auto Bit = scalarRegisterBit(Op.reg);
      return Bit && (State.Registers & Bit);
    }
    if (Op.type != X86_OP_MEM || Op.size != 4 ||
        Op.mem.segment != X86_REG_INVALID || Op.mem.index != X86_REG_INVALID)
      return false;
    if (Op.mem.base == X86_REG_EBP)
      return Op.mem.disp == 8 || (Op.mem.disp == -4 && State.Scratch);
    if (Op.mem.base != X86_REG_INVALID || Op.mem.disp < 0 ||
        uint64_t(Op.mem.disp) > UINT32_MAX - 3 ||
        !(isFileBackedWritableImageRange(Image, va_t(Op.mem.disp), 4) ||
          readImmutableImageBytes(Image, va_t(Op.mem.disp), 4)))
      return false;
    // Only storage ownership matters: the retained instruction still reads
    // the current runtime value, including the CRT's mutable ISA level.
    Absolute = true;
    ImageReads.insert(va_t(Op.mem.disp));
    return true;
  }

  bool forward(ScalarState State, va_t Target) {
    if (Target <= State.Address ||
        Target - Entry >=
            ::neverd::limits::loader::kCOFFRegistrationScalarLeafMaxHelperBytes)
      return false;
    State.Address = Target;
    Work.push_back(State);
    return true;
  }

public:
  ScalarLeafProof(const BinaryImage &Image, va_t Entry)
      : Image(Image), Entry(Entry) {}

  X86RegistrationScalarLeaf dependencies() const {
    X86RegistrationScalarLeaf Result;
    for (const auto &[Begin, End] : Instructions) {
      if (!Result.CodeRanges.empty() && Result.CodeRanges.back().End == Begin)
        Result.CodeRanges.back().End = End;
      else
        Result.CodeRanges.push_back({Begin, End});
    }
    for (const auto Address : ImageReads)
      Result.ImageReads.push_back({Address, Address + 4});
    Result.AbsoluteOperands = AbsoluteOperands;
    return Result;
  }

  bool run() {
    // EBP saves the caller frame, while the ECX push reserves one private
    // slot. Its incoming value is deliberately not an initialized scalar.
    const auto Prefix = readImmutableCodeBytes(Image, Entry, 4);
    if (!Prefix || *Prefix != std::vector<uint8_t>({0x55, 0x8b, 0xec, 0x51}) ||
        !Dec.init(Image) || !recordInstruction(Entry, 4))
      return false;
    Work.push_back({Entry + 4});
    unsigned Budget =
        ::neverd::limits::loader::kCOFFRegistrationScalarLeafMaxProofStates;
    unsigned Returns = 0;
    while (!Work.empty()) {
      auto State = Work.back();
      Work.pop_back();
      if (!Budget--)
        return false;
      if (!Seen.emplace(State.Address, State.Registers, State.Scratch,
                        State.Flags)
               .second)
        continue;
      const auto Return = readImmutableCodeBytes(Image, State.Address, 4);
      if (Return && *Return == std::vector<uint8_t>({0x8b, 0xe5, 0x5d, 0xc3})) {
        if (!(State.Registers & 1) || !recordInstruction(State.Address, 4))
          return false;
        ++Returns;
        continue;
      }
      unsigned Available =
          std::min<va_t>(15, Entry +
                                 ::neverd::limits::loader::
                                     kCOFFRegistrationScalarLeafMaxHelperBytes -
                                 State.Address);
      const uint8_t *Bytes = nullptr;
      while (Available && !(Bytes = Image.readVA(State.Address, Available)))
        --Available;
      DecodedInsn Insn{};
      if (!Available || !Dec.decodeOne(Bytes, Available, State.Address, Insn) ||
          !Insn.Raw || !Insn.Raw->detail ||
          !recordInstruction(State.Address, Insn.Size))
        return false;
      const auto &X = Insn.Raw->detail->x86;
      if (std::any_of(std::begin(X.prefix), std::end(X.prefix),
                      [](uint8_t Prefix) { return Prefix != 0; }))
        return false;
      bool Absolute = false;
      bool Conditional = false;
      bool Jump = false;
      const auto Next = State.Address + Insn.Size;
      const auto &Dst = X.operands[0];
      const auto &Src = X.operands[1];
      const auto Destination = Dst.type == X86_OP_REG && Dst.size == 4
                                   ? fullRegisterBit(Dst.reg)
                                   : 0;
      switch (Insn.Id) {
      case X86_INS_MOV:
        if (X.op_count != 2 || !Destination || Src.size != 4 ||
            !readScalar(Src, State, Absolute))
          return false;
        State.Registers |= Destination;
        break;
      case X86_INS_XOR:
        if (X.op_count != 2 || !Destination ||
            !(State.Registers & Destination) || Src.type != X86_OP_IMM)
          return false;
        State.Flags = true;
        break;
      case X86_INS_CMP:
      case X86_INS_TEST:
        if (X.op_count != 2 || !readScalar(Dst, State, Absolute) ||
            !readScalar(Src, State, Absolute))
          return false;
        State.Flags = true;
        break;
      case X86_INS_CMOVE:
        if (X.op_count != 2 || !Destination || !State.Flags ||
            !(State.Registers & Destination) || Src.size != 4 ||
            !readScalar(Src, State, Absolute))
          return false;
        break;
      case X86_INS_STMXCSR:
        if (X.op_count != 1 || Dst.type != X86_OP_MEM || Dst.size != 4 ||
            Dst.mem.segment != X86_REG_INVALID || Dst.mem.base != X86_REG_EBP ||
            Dst.mem.index != X86_REG_INVALID || Dst.mem.disp != -4)
          return false;
        State.Scratch = true;
        break;
      case X86_INS_JE:
      case X86_INS_JNE:
      case X86_INS_JL:
        if (!State.Flags)
          return false;
        Conditional = true;
        [[fallthrough]];
      case X86_INS_JMP:
        if (X.op_count != 1 || Dst.type != X86_OP_IMM || Dst.imm <= 0 ||
            uint64_t(Dst.imm) > UINT32_MAX || !forward(State, va_t(Dst.imm)))
          return false;
        Jump = true;
        break;
      default:
        return false;
      }
      std::vector<va_t> Operands;
      if (Absolute) {
        if (X.encoding.disp_size != 4 ||
            unsigned(X.encoding.disp_offset) + 4 > Insn.Size)
          return false;
        Operands.push_back(State.Address + X.encoding.disp_offset);
        AbsoluteOperands.emplace(
            Operands.back(), readLE<uint32_t>(Bytes + X.encoding.disp_offset));
      }
      if (!readImmutablePE32CodeBytes(Image, State.Address, Insn.Size,
                                      Operands))
        return false;
      if ((!Jump || Conditional) && !forward(State, Next))
        return false;
    }
    return Returns != 0;
  }
};

} // namespace

std::optional<X86RegistrationScalarLeaf>
getCheckedX86RegistrationScalarLeaf(const BinaryImage &Image, va_t Entry) {
  if (Image.Arch != Arch::X86 || Image.Bits != Bitness::Bits32 ||
      Image.Format != BinaryFormat::COFF || !Entry ||
      Entry > UINT32_MAX - ::neverd::limits::loader::
                               kCOFFRegistrationScalarLeafMaxHelperBytes)
    return std::nullopt;
  ScalarLeafProof Proof(Image, Entry);
  if (!Proof.run())
    return std::nullopt;
  return Proof.dependencies();
}

} // namespace neverd::coff_loader
