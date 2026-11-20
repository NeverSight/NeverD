//===- MedMIPSRecordFrame.cpp - Link identity through record-copy loops ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "MedMIPSRecordFrame.h"

#include "../../../../safety/StackSlotFlow.h"

#include "neverd/ir/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/med/MIPS/MIPSIntrinsicShapes.h"
#include "neverd/lift/MIPS/MIPSRegs.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/MathExtras.h"

#include <map>
#include <set>
#include <tuple>

namespace neverd::mips {
namespace {
enum class Origin { Unknown, Number, NonFrame, Stack, Link };
struct Value {
  Origin From = Origin::Unknown;
  uint64_t Bits = 0;
  // A partial FPR write or stack word can contain both proven data bytes
  // and an unknown suffix. Bit zero describes the least significant byte.
  uint64_t NonFrameBytes = 0;
  static uint64_t lanes(unsigned Width) {
    return Width >= 64 ? UINT64_MAX : (uint64_t(1) << Width) - 1;
  }
  uint64_t knownBytes(unsigned Width) const {
    return external() ? lanes(Width) : NonFrameBytes & lanes(Width);
  }
  static Value fromBytes(uint64_t Known, unsigned Width) {
    if (Width && Width <= 64 && (Known & lanes(Width)) == lanes(Width))
      return {Origin::NonFrame};
    return {Origin::Unknown, 0, Known & lanes(Width)};
  }
  bool external() const {
    return From == Origin::Number || From == Origin::NonFrame ||
           From == Origin::Link;
  }
  bool operator==(const Value &) const = default;
};

using Key = std::tuple<MedVar::VarKind, int, int, uint64_t, uint16_t>;
Key key(const MedVar &V) { return {V.Kind, V.Id, V.SSAVer, V.RegOff, V.Size}; }
struct Byte {
  Value Source;
  unsigned Lane;
  unsigned Width;
};

// A private stack address and an incoming link retain identity only while all
// their bytes survive. Bytes read through a caller-owned input can contribute
// data, but cannot designate this newly allocated private frame.
class FrameWalk {
  const MedFunc &F;
  Arch Target;
  const TargetRegInfo &TRI;
  RecordFrameCallInfo CallInfo;
  size_t &Budget;
  std::map<Key, Value> Values;
  std::map<int64_t, Byte> Memory;
  int64_t CurrentSP = 0;
  bool Called = false;

  Value read(const MedVar &V) const {
    if (V.isConst())
      return V.Size <= 8 ? Value{Origin::Number, V.ConstVal & mask(V.Size)}
                         : Value{Origin::NonFrame};
    const auto It = Values.find(key(V));
    return It == Values.end() ? Value{} : It->second;
  }
  static uint64_t mask(unsigned Bytes) {
    return Bytes >= 8 ? UINT64_MAX : (uint64_t(1) << (Bytes * 8)) - 1;
  }
  bool privateBytes(int64_t At, unsigned Width) const {
    return Width && At >= CurrentSP && At <= -int64_t(Width);
  }
  Value load(Value Address, unsigned Width) const {
    if (Address.external())
      return {Origin::NonFrame};
    const int64_t At = int64_t(Address.Bits);
    if (Address.From != Origin::Stack || !privateBytes(At, Width))
      return {};
    Value Whole;
    bool Intact = true, Numeric = Width <= 8;
    uint64_t Known = 0;
    uint64_t Bits = 0;
    for (unsigned I = 0; I < Width; ++I) {
      const auto It = Memory.find(At + I);
      if (It == Memory.end()) {
        Intact = Numeric = false;
        continue;
      }
      const auto &B = It->second;
      const unsigned Lane = archLittleEndian(Target) ? I : Width - 1 - I;
      if (!I)
        Whole = B.Source;
      Intact &= B.Source == Whole && B.Width == Width && B.Lane == Lane;
      if (Lane < 64 && B.Lane < 64 &&
          ((B.Source.knownBytes(B.Width) >> B.Lane) & 1))
        Known |= uint64_t(1) << Lane;
      Numeric &= B.Source.From == Origin::Number && B.Lane < 8;
      if (Numeric)
        Bits |= ((B.Source.Bits >> (B.Lane * 8)) & 255) << (Lane * 8);
    }
    if (Intact)
      return Whole;
    if (Numeric)
      return {Origin::Number, Bits};
    return Value::fromBytes(Known, Width);
  }
  bool store(Value Address, Value Data, unsigned Width) {
    if (Address.external())
      return Data.external();
    const int64_t At = int64_t(Address.Bits);
    if (Address.From != Origin::Stack || !privateBytes(At, Width) ||
        Width > Budget)
      return false;
    Budget -= Width;
    for (unsigned I = 0; I < Width; ++I)
      Memory[At + I] = {Data, archLittleEndian(Target) ? I : Width - 1 - I,
                        Width};
    return true;
  }
  Value compute(const MedOp &O) const {
    if (!O.NumInputs)
      return {};
    const auto A = read(O.Inputs[0]);
    const unsigned W = O.Output.Size;
    if (O.Opcode == NdOp::COPY && O.NumInputs == 1)
      return A;
    if ((O.Opcode == NdOp::INT_SEXT || O.Opcode == NdOp::INT_ZEXT) &&
        O.NumInputs == 1) {
      unsigned From = O.Inputs[0].Size;
      if (From == W)
        return A;
      if (A.From == Origin::Number && From && From <= 8 && W <= 8) {
        const auto N = A.Bits & mask(From);
        return {Origin::Number, (O.Opcode == NdOp::INT_SEXT
                                     ? uint64_t(llvm::SignExtend64(N, From * 8))
                                     : N) &
                                    mask(W)};
      }
      if (A.From == Origin::Stack && isMIPSN32(Target) && From == 4 && W == 8 &&
          O.Opcode == NdOp::INT_SEXT)
        return A;
      uint64_t Known = A.knownBytes(From);
      if (W > From && (O.Opcode == NdOp::INT_ZEXT ||
                       (From && From <= 64 && ((Known >> (From - 1)) & 1))))
        Known |= Value::lanes(W) & ~Value::lanes(From);
      return Value::fromBytes(Known, W);
    }
    if (O.NumInputs != 2)
      return {};
    auto B = read(O.Inputs[1]);
    if (O.Opcode == NdOp::SUBBYTES && B.From == Origin::Number &&
        B.Bits <= O.Inputs[0].Size && W <= O.Inputs[0].Size - B.Bits) {
      if (!B.Bits && (W == O.Inputs[0].Size ||
                      (A.From == Origin::Stack && W >= TRI.PointerSize)))
        return A;
      if (A.From == Origin::Number && B.Bits < 8)
        return {Origin::Number, (A.Bits >> (8 * B.Bits)) & mask(W)};
      return B.Bits < 64
                 ? Value::fromBytes(A.knownBytes(O.Inputs[0].Size) >> B.Bits, W)
                 : Value{};
    }
    if (O.Opcode == NdOp::CONCAT && W == O.Inputs[0].Size + O.Inputs[1].Size &&
        W <= 64 && O.Inputs[1].Size < 64) {
      const auto Low = O.Inputs[1].Size;
      const auto High = O.Inputs[0].Size;
      if (W <= 8 && Low && Low < 8 && A.From == Origin::Number &&
          B.From == Origin::Number)
        return {Origin::Number,
                ((A.Bits << (8 * Low)) | (B.Bits & mask(Low))) & mask(W)};
      return Value::fromBytes((A.knownBytes(High) << Low) | B.knownBytes(Low),
                              W);
    }
    if (W > 8 || !W)
      return {};
    if (O.Opcode == NdOp::INT_ADD || O.Opcode == NdOp::INT_SUB) {
      const bool Sub = O.Opcode == NdOp::INT_SUB;
      const Value Base = !Sub && A.From == Origin::Number ? B : A;
      const Value Delta = !Sub && A.From == Origin::Number ? A : B;
      if (Base.From == Origin::Stack && Delta.From == Origin::Number &&
          (W == 4 || W == 8)) {
        int64_t D = llvm::SignExtend64(Delta.Bits, W * 8), At;
        if ((Sub ? llvm::SubOverflow(int64_t(Base.Bits), D, At)
                 : llvm::AddOverflow(int64_t(Base.Bits), D, At)) ||
            At < -limits::ir::kSourceFrameAnalysisMaxFrame || At > 0)
          return {};
        return {Origin::Stack, uint64_t(At)};
      }
      if (A.From == Origin::Number && B.From == Origin::Number)
        return {Origin::Number,
                (Sub ? A.Bits - B.Bits : A.Bits + B.Bits) & mask(W)};
    } else if (O.Opcode == NdOp::INT_NOTEQUAL || O.Opcode == NdOp::INT_EQUAL) {
      if (A.From == B.From &&
          (A.From == Origin::Number || A.From == Origin::Stack))
        return {Origin::Number,
                uint64_t((A.Bits == B.Bits) == (O.Opcode == NdOp::INT_EQUAL))};
    } else if (O.Opcode == NdOp::INT_OR || O.Opcode == NdOp::INT_LEFT) {
      if (A.From == Origin::Number && B.From == Origin::Number) {
        uint64_t N = O.Opcode == NdOp::INT_OR ? A.Bits | B.Bits
                     : B.Bits < 64            ? A.Bits << B.Bits
                                              : 0;
        return {Origin::Number, N & mask(W)};
      }
    } else {
      return {};
    }
    return A.external() && B.external() ? Value{Origin::NonFrame} : Value{};
  }

  bool call(const MedOp &O) {
    if (!O.SourceCallHint || !O.SourceCallHint->Signature.MIPSRecords ||
        O.IsTailCall || !O.NumInputs || CurrentSP >= 0)
      return false;
    const auto Info = CallInfo(O);
    if (!Info || Info->PrivateStart < CurrentSP || Info->PrivateStart > 0)
      return false;
    for (unsigned I = 0; I < O.NumInputs; ++I)
      if (!read(O.Inputs[I]).external())
        return false;
    const auto End = Memory.lower_bound(Info->PrivateStart);
    for (auto It = Memory.lower_bound(CurrentSP); It != End; ++It)
      if (!It->second.Source.external())
        return false;
    Memory.erase(Memory.begin(), End);
    // SSA before call-clobber recovery can still mention the old volatile
    // register. Preserve only the incoming stack and ABI callee-saved bank.
    for (auto &[Id, V] : Values)
      if (std::get<0>(Id) == MedVar::Reg &&
          std::get<3>(Id) != TRI.StackPointer &&
          !TRI.isCalleeSaveReg(std::get<3>(Id)))
        V = {};
    if (O.Output.Size) {
      const bool Numeric = Info->NumericResultBytes == O.Output.Size;
      Values[key(O.Output)] = Numeric ? Value{Origin::NonFrame} : Value{};
    }
    Called = true;
    return true;
  }

public:
  FrameWalk(const MedFunc &F, Arch Target, RecordFrameCallInfo CallInfo,
            size_t &Budget)
      : F(F), Target(Target), TRI(getTargetRegInfo(Target)), CallInfo(CallInfo),
        Budget(Budget) {}
  bool run(const MedOp &Return) {
    if (F.Blocks.empty())
      return false;
    std::map<int, const MedBlock *> Blocks;
    const MedBlock *Block = nullptr;
    for (const auto &B : F.Blocks) {
      if (!Blocks.emplace(B.Id, &B).second || !B.ExceptionalPreds.empty() ||
          !B.ExceptionalSuccs.empty())
        return false;
      if (B.StartAddr == F.Entry) {
        if (Block)
          return false;
        Block = &B;
      }
    }
    if (!Block || !Block->Preds.empty())
      return false;
    safety::detail::ExactPhiGraph Graph(F);
    for (size_t I = 0; I < F.Blocks.size(); ++I) {
      const auto &B = F.Blocks[I];
      if (!Graph.hasCompleteNormalPredecessors(B))
        return false;
      for (const auto &Phi : B.Phis)
        if (!Graph.hasCompleteIncoming(I, Phi))
          return false;
    }
    int Pred = -1;
    std::set<int> Seen;
    while (Block && Budget) {
      --Budget;
      Seen.insert(Block->Id);
      llvm::SmallVector<Value, 8> Incoming;
      for (const auto &Phi : Block->Phis) {
        if (!Budget || Phi.ExceptionalEntry)
          return false;
        --Budget;
        auto It = llvm::find_if(Phi.Args,
                                [&](const auto &A) { return A.first == Pred; });
        if (It == Phi.Args.end())
          return false;
        Incoming.push_back(read(It->second));
      }
      for (unsigned I = 0; I < Incoming.size(); ++I)
        Values[key(Block->Phis[I].Output)] = Incoming[I];
      const MedBlock *Next = nullptr;
      for (const auto &O : Block->Ops) {
        if (!Budget || O.NumInputs > O.Inputs.size() ||
            !O.IntrinsicOutputs.empty() ||
            O.MemoryOrdering != NdMemoryOrdering::None ||
            O.MemoryAddressSpace != NdMemoryAddressSpace::Default)
          return false;
        --Budget;
        if (O.Opcode == NdOp::RETURN)
          return &O == &Return && &O == &Block->Ops.back() && O.NumInputs &&
                 read(O.Inputs[O.NumInputs - 1]).From == Origin::Link &&
                 CurrentSP == 0 && Block->Succs.empty() &&
                 Seen.size() == Blocks.size();
        if (O.Opcode == NdOp::COND_BR) {
          if (&O != &Block->Ops.back() || Block->Succs.size() != 2 ||
              O.NumInputs != 2 || !O.Inputs[0].isConst())
            return false;
          const auto Test = read(O.Inputs[1]);
          if (Test.From != Origin::Number)
            return false;
          for (int S : Block->Succs) {
            auto It = Blocks.find(S);
            if (It == Blocks.end())
              return false;
            if ((It->second->StartAddr == O.Inputs[0].ConstVal) ==
                bool(Test.Bits)) {
              if (Next)
                return false;
              Next = It->second;
            }
          }
          if (!Next)
            return false;
          continue;
        }
        if (O.Opcode == NdOp::STORE && O.NumInputs == 2) {
          if (!store(read(O.Inputs[0]), read(O.Inputs[1]), O.Inputs[1].Size))
            return false;
          continue;
        }
        if (O.Opcode == NdOp::CALL || O.Opcode == NdOp::INDIR_CALL) {
          if (!call(O))
            return false;
          continue;
        }
        if (O.Opcode == NdOp::INTRINSIC && O.NumInputs &&
            O.Inputs[0].isConst() && O.Output.Kind == MedVar::Temp &&
            O.Inputs[0].ConstVal == uint64_t(Intrinsic::MIPSUnpredictable) &&
            mipsScalarMedShapeIsValid(O, Target)) {
          Values[key(O.Output)] = {};
          continue;
        }
        if (O.Opcode == NdOp::INTRINSIC && O.NumInputs &&
            O.Inputs[0].isConst() && !O.Output.Size) {
          const auto Id = static_cast<Intrinsic>(O.Inputs[0].ConstVal);
          if ((Id == Intrinsic::RequireAligned && O.NumInputs == 3 &&
               O.Inputs[2].isConst() &&
               llvm::isPowerOf2_64(O.Inputs[2].ConstVal)) ||
              (Id == Intrinsic::MIPSRequireWord &&
               mipsScalarMedShapeIsValid(O, Target)))
            continue;
        }
        if (O.Opcode == NdOp::NOP && !O.Output.Size && !O.NumInputs)
          continue;
        switch (O.Opcode) {
        case NdOp::COPY:
        case NdOp::LOAD:
        case NdOp::INT_SEXT:
        case NdOp::INT_ZEXT:
        case NdOp::SUBBYTES:
        case NdOp::CONCAT:
        case NdOp::INT_ADD:
        case NdOp::INT_SUB:
        case NdOp::INT_OR:
        case NdOp::INT_LEFT:
        case NdOp::INT_EQUAL:
        case NdOp::INT_NOTEQUAL:
          if (!O.Output.Size)
            return false;
          break;
        default:
          return false;
        }
        Value V;
        if (O.Opcode == NdOp::COPY && O.NumInputs == 1 &&
            O.Output == O.Inputs[0] && Block->StartAddr == F.Entry && !Called &&
            safety::detail::isAuthenticatedEntryRegisterLiveIn(F, O.Output)) {
          if (O.Output.RegOff == TRI.StackPointer)
            V = {Origin::Stack};
          else if (O.Output.RegOff == TRI.LinkRegister &&
                   O.Output.Size == TRI.FullRegWidth)
            V = {Origin::Link};
          else if (safety::detail::isAuthenticatedNonFrameEntryRegister(
                       F, O.Output, TRI.StackPointer, TRI.FramePointer))
            V = {Origin::NonFrame};
        } else if (O.Opcode == NdOp::LOAD && O.NumInputs == 1)
          V = load(read(O.Inputs[0]), O.Output.Size);
        else
          V = compute(O);
        if (O.Output.Kind == MedVar::Reg &&
            O.Output.RegOff == TRI.StackPointer) {
          if (V.From != Origin::Stack || O.Output.Size != TRI.FullRegWidth)
            return false;
          int64_t NewSP = int64_t(V.Bits);
          // Bytes below a raised stack pointer are no longer caller-private.
          if (NewSP > CurrentSP)
            Memory.erase(Memory.begin(), Memory.lower_bound(NewSP));
          CurrentSP = NewSP;
        }
        Values[key(O.Output)] = V;
      }
      Pred = Block->Id;
      if (!Next) {
        if (Block->Succs.size() != 1)
          return false;
        const auto It = Blocks.find(Block->Succs.front());
        if (It == Blocks.end())
          return false;
        Next = It->second;
      }
      if (!llvm::is_contained(Next->Preds, Pred))
        return false;
      Block = Next;
    }
    return false;
  }
};
} // namespace

bool preservesRecordFrameLink(const MedFunc &F, const MedOp &Return,
                              Arch Target, RecordFrameCallInfo CallInfo,
                              size_t &Budget) {
  return FrameWalk(F, Target, CallInfo, Budget).run(Return);
}
} // namespace neverd::mips
