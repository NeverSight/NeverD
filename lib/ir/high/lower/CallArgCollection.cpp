//===- CallArgCollection.cpp - Call argument collection
//--------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Collects function call arguments by scanning backward from the call site
/// for register writes and stack stores that match the target ABI.  Shared
/// register/live-in recovery lives here; ISA stack-argument quirks live in
/// CallArgCollectionX86.cpp, CallArgCollectionARM.cpp, and
/// CallArgCollectionAArch64.cpp.
///
//===----------------------------------------------------------------------===//

#include "CallArgCollectionDetail.h"
#include "HighEntryStackOffsets.h"
#include "VariadicStackArguments.h"

#include "neverd/Common.h"
#include "neverd/Limits.h"
#include "neverd/ir/Limits.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/low/ImportCallee.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/ir/med/MedCallConvention.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/FunctionNames.h"
#include "neverd/loader/SymbolDecoration.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>

namespace {

bool isAddressAddOp(const neverd::MedOp &Op) {
  if (Op.Opcode != neverd::NdOp::INT_ADD || Op.NumInputs < 2)
    return false;
  return Op.Inputs[0].isConst() != Op.Inputs[1].isConst();
}

const neverd::MedOp *uniqueSsaDef(const neverd::MedFunc *Func,
                                  const neverd::MedVar &V) {
  if (!Func || V.Id < 0)
    return nullptr;
  const neverd::MedOp *Def = nullptr;
  for (const auto &Blk : Func->Blocks)
    for (const auto &Op : Blk.Ops)
      if (Op.Output.Id == V.Id && Op.Output.SSAVer == V.SSAVer) {
        if (Def)
          return nullptr;
        Def = &Op;
      }
  return Def;
}

bool isLeaLikeCallSetup(const neverd::MedFunc *Func, const neverd::MedOp &Op) {
  if (isAddressAddOp(Op))
    return true;
  if (Op.Opcode != neverd::NdOp::COPY || Op.NumInputs < 1)
    return false;
  const neverd::MedOp *Src = uniqueSsaDef(Func, Op.Inputs[0]);
  return Src && isAddressAddOp(*Src);
}

} // namespace

namespace neverd {

namespace call_args_detail {

extern const CallArgPolicy Win64CallArgPolicy;
extern const CallArgPolicy I386CallArgPolicy;

const CallArgPolicy *callArgPolicy(Arch A, BinaryFormat F) {
  static constexpr const CallArgPolicy *Policies[] = {&Win64CallArgPolicy,
                                                      &I386CallArgPolicy};
  // An entry for the image's own format wins over one for every format.
  for (BinaryFormat Wanted : {F, BinaryFormat::Unknown})
    for (const CallArgPolicy *P : Policies)
      if (P->TheArch == A && P->Format == Wanted)
        return P;
  return nullptr;
}

void collectSpilledStackArgs(const CallArgScan &Scan,
                             std::vector<ExprPtr> &Found) {
  const TargetRegInfo &TRI = *Scan.TRI;
  const int64_t SlotBytes = static_cast<int64_t>(TRI.PointerSize);
  const BinaryFormat Format =
      Scan.Image ? Scan.Image->abiFormat() : BinaryFormat::Unknown;
  const bool Reserved =
      Scan.Convention && Scan.Convention->ReservedOutgoingArea;
  const auto Layout = TRI.integerArgumentLayout(Format);

  auto preserved = [&](const MedVar &V) {
    return V.Kind == MedVar::Reg &&
           isCallPreservedReg(TRI, Format, V.RegOff, V.Size);
  };

  // Written slots of a reserved outgoing area (call-time offset, address) so
  // a slot left unwritten between two written ones can still be read
  // afterwards.
  std::vector<std::pair<int64_t, MedVar>> StoredSlots;
  auto considerStore = [&](const std::vector<MedOp> &Ops, int J) {
    const MedOp &Prev = Ops[static_cast<size_t>(J)];
    if (Prev.Opcode != NdOp::STORE || Prev.NumInputs < 2 ||
        Prev.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        (Scan.IsPcRootedStore && Scan.IsPcRootedStore(Prev)))
      return;
    MedVar Stored = Prev.Inputs[1];
    // Incoming callee-save spills (SSA 0) are prologue saves. A computed
    // length parked in ESI/RDI is a real outgoing stack argument
    // (for example, a value stored at [rsp+0x20]).
    if (preserved(Stored) && Stored.SSAVer == 0)
      return;
    if (!Scan.ResolveWindow && Stored.Kind == MedVar::Reg) {
      for (int K = J - 1; K >= 0; --K) {
        const MedOp &Def = Ops[static_cast<size_t>(K)];
        if (endsCallSetup(Def) && !preserved(Stored))
          break;
        if (Def.Output.Kind != MedVar::Reg || Def.Output.Size == 0 ||
            Def.Output.RegOff != Stored.RegOff)
          continue;
        if (Def.Opcode == NdOp::COPY && Def.NumInputs >= 1)
          Stored = Def.Inputs[0];
        else
          Stored = Def.Output;
        break;
      }
    }

    // The incoming value of a scratch register that carries no argument is
    // undefined: storing it, as an alignment `push rax` does, passes nothing.
    if (Scan.Convention && Scan.Convention->UndefinedIncomingScratchRegisters &&
        Stored.Kind == MedVar::Reg && Stored.SSAVer == 0 &&
        Stored.RegOff != Scan.SpRegOff && !preserved(Stored) &&
        gprFamilyOf(Scan.TheArch, Stored.RegOff) &&
        llvm::none_of(Layout.Registers, [&](uint64_t Reg) {
          return gprFamilyOf(Scan.TheArch, Reg) ==
                 gprFamilyOf(Scan.TheArch, Stored.RegOff);
        }))
      return;
    // Nor does an indirect call take a prologue's save of a register's
    // incoming value (`push {r4, r6, r7, lr}` above the outgoing area).
    if (Scan.Indirect && isIncomingRegisterSave(TRI, Stored))
      return;

    const MedVar &AddrVar = Prev.Inputs[0];
    // A store below the stack pointer the call is made with was popped
    // again before it, as the get-PC push is: no argument.
    if (Scan.CallStackOffsetOf)
      if (const std::optional<int64_t> Above = Scan.CallStackOffsetOf(AddrVar);
          Above && *Above < 0)
        return;
    int64_t StackOff = -1;

    if (AddrVar.Kind == MedVar::Reg && AddrVar.RegOff == Scan.SpRegOff)
      StackOff = 0;

    if (StackOff < 0 && !AddrVar.isConst()) {
      for (int K = J - 1; K >= 0; --K) {
        const MedOp &DefOp = Ops[static_cast<size_t>(K)];
        if (DefOp.Output.Id != AddrVar.Id ||
            DefOp.Output.SSAVer != AddrVar.SSAVer)
          continue;
        if (DefOp.Opcode == NdOp::INT_ADD && DefOp.NumInputs >= 2) {
          bool HasSP = false;
          int64_t ConstOff = -1;
          for (uint8_t KI = 0; KI < DefOp.NumInputs; ++KI) {
            if (DefOp.Inputs[KI].Kind == MedVar::Reg &&
                DefOp.Inputs[KI].RegOff == Scan.SpRegOff)
              HasSP = true;
            if (DefOp.Inputs[KI].isConst())
              ConstOff = static_cast<int64_t>(DefOp.Inputs[KI].ConstVal);
          }
          if (HasSP && ConstOff >= 0)
            StackOff = ConstOff;
        }
        break;
      }
    }

    // An `r11 = rsp` frame addresses the outgoing area from the entry stack.
    if (StackOff < 0 && Reserved && Scan.EntryOffsetOf)
      if (std::optional<int64_t> Entry = Scan.EntryOffsetOf(AddrVar))
        StackOff = *Entry + Scan.FrameSize;

    // A tail jump leaves on the entry stack: a stored slot is the callee's
    // argument at its entry offset, whatever stack pointer the store used.
    if (Scan.TailJump) {
      std::optional<int64_t> Entry =
          Scan.EntryOffsetOf ? Scan.EntryOffsetOf(AddrVar) : std::nullopt;
      const int64_t ReturnAddress =
          Layout.EntryStackBase - Layout.CallStackBase;
      if (!Entry || *Entry < ReturnAddress)
        return;
      StackOff = *Entry - ReturnAddress;
    }

    if (StackOff < 0 || SlotBytes == 0)
      return;
    int ArgPos = -1;
    if (Reserved) {
      if (StackOff < Layout.CallStackBase)
        return;
      const int64_t SlotOff = StackOff - Layout.CallStackBase;
      if (SlotOff % SlotBytes != 0)
        return;
      ArgPos = static_cast<int>(Layout.Registers.size()) +
               static_cast<int>(SlotOff / SlotBytes);
      // A local can reuse the outgoing area between calls. A callee's
      // stack-argument summary still requires its latest stored value,
      // even when this function read or took the slot's address earlier.
      // Without that summary, a read-back slot remains a local. A tail
      // jump may pass its own incoming arguments after reading them.
      if (ArgPos >= Scan.CalleeStackArgs && !Scan.TailJump &&
          Scan.LoadedEntrySlots &&
          Scan.LoadedEntrySlots->count(StackOff - Scan.FrameSize))
        return;
    } else {
      if (StackOff >= Scan.MaxArgs * SlotBytes || StackOff % SlotBytes != 0)
        return;
      ArgPos = Scan.FirstStackSlot + static_cast<int>(StackOff / SlotBytes);
    }
    if (ArgPos < 0 || ArgPos >= Scan.MaxArgs || Found[ArgPos])
      return;
    if (Reserved)
      StoredSlots.push_back({StackOff, AddrVar});
    if (Scan.ResolveWindow) {
      if (ExprPtr E = Scan.ResolveWindow(Prev.Inputs[1], Ops, J - 1))
        if (E->Kind != ExprKind::Undef) {
          Found[ArgPos] = std::move(E);
          return;
        }
    }
    for (int Peel = 0;
         Peel < ::neverd::limits::ir::kCallArgCollectionConsiderStorePeelLimit;
         ++Peel) {
      ExprPtr E = Scan.ToExpr(Stored);
      if (E && E->Kind != ExprKind::Undef) {
        Found[ArgPos] = std::move(E);
        return;
      }
      if (Stored.Kind != MedVar::Reg)
        return;
      bool Progress = false;
      for (int K = J - 1; K >= 0; --K) {
        const MedOp &Def = Ops[static_cast<size_t>(K)];
        if (endsCallSetup(Def) && !preserved(Stored))
          break;
        const bool SameSsa =
            Def.Output.Id == Stored.Id && Def.Output.SSAVer == Stored.SSAVer;
        const bool SameReg = Def.Output.Kind == MedVar::Reg &&
                             Def.Output.Size > 0 &&
                             Def.Output.RegOff == Stored.RegOff;
        if (!SameSsa && !SameReg)
          continue;
        if (Def.NumInputs >= 1 && Def.Inputs[0].Kind == MedVar::Reg)
          Stored = Def.Inputs[0];
        else if (Def.Opcode == NdOp::COPY && Def.NumInputs >= 1)
          Stored = Def.Inputs[0];
        else
          Stored = Def.Output;
        Progress = true;
        break;
      }
      if (!Progress)
        return;
    }
  };

  auto scanWindow = [&](const std::vector<MedOp> &Ops, int Before,
                        int WindowFloor) {
    for (int J = Before; J >= WindowFloor; --J) {
      const MedOp &Prev = Ops[static_cast<size_t>(J)];
      if (endsCallSetup(Prev))
        break;
      // HighIR has no MedCallInfo on the ordinary decompile path. Match the
      // MedABI scan's call boundary, but do not attach stores from an older
      // reserved outgoing area to this call. An SP-to-SP copy may only cross
      // the scan boundary when it copies the currently reaching SP value;
      // restoring an older SP version changes the outgoing area again.
      if (Reserved && Prev.Output.Kind == MedVar::Reg &&
          Prev.Output.Size != 0 && Prev.Output.RegOff == Scan.SpRegOff) {
        const bool SpCopy = Prev.Opcode == NdOp::COPY && Prev.NumInputs >= 1 &&
                            Prev.Inputs[0].Kind == MedVar::Reg &&
                            Prev.Inputs[0].RegOff == Scan.SpRegOff &&
                            Prev.Inputs[0].Size == Prev.Output.Size;
        bool SameSpBase = SpCopy && isNoopRegisterCopy(Prev);
        if (SpCopy && !SameSpBase)
          for (int K = J - 1; K >= WindowFloor; --K) {
            const MedOp &Def = Ops[static_cast<size_t>(K)];
            if (endsCallSetup(Def))
              break;
            if (Def.Output.Kind != MedVar::Reg || Def.Output.Size == 0 ||
                Def.Output.RegOff != Scan.SpRegOff)
              continue;
            SameSpBase = Def.Output.Id == Prev.Inputs[0].Id &&
                         Def.Output.SSAVer == Prev.Inputs[0].SSAVer &&
                         Def.Output.Size == Prev.Inputs[0].Size;
            break;
          }
        if (!SameSpBase)
          break;
      }
      considerStore(Ops, J);
    }
  };

  const int StoreScanStart =
      Reserved
          ? 0
          : std::max(0, static_cast<int>(Scan.CallIdx) - Scan.StoreScanWindow);
  scanWindow(*Scan.Ops, static_cast<int>(Scan.CallIdx) - 1, StoreScanStart);
  for (const auto &W : Scan.ExtraWindows)
    if (W.Ops)
      scanWindow(*W.Ops, W.Before, 0);

  // A reserved slot the caller did not write between two it did is still an
  // argument: the callee reads whatever the slot holds.  Read it through the
  // address of a written neighbour rather than dropping later arguments.
  if (!Reserved || StoredSlots.empty())
    return;
  const int FirstStackArg = static_cast<int>(Layout.Registers.size());
  int Last = -1;
  for (int K = FirstStackArg; K < Scan.MaxArgs; ++K)
    if (Found[K])
      Last = K;
  const auto &[BaseOff, BaseAddr] = StoredSlots.front();
  for (int K = FirstStackArg; K < Last; ++K) {
    if (Found[K])
      continue;
    const int64_t Off =
        Layout.CallStackBase + int64_t(K - FirstStackArg) * SlotBytes;
    ExprPtr Address = HighExpr::makeBinop(
        NdOp::INT_ADD, Scan.ToExpr(BaseAddr),
        HighExpr::makeConst(static_cast<uint64_t>(Off - BaseOff),
                            TRI.PointerSize));
    Found[K] = HighExpr::makeLoad(std::move(Address),
                                  NdType::makeInt(TRI.PointerSize));
  }
}

void collectFullBankStackArgs(const CallArgScan &Scan,
                              std::vector<ExprPtr> &Found) {
  const BinaryFormat Format =
      Scan.Image ? Scan.Image->abiFormat() : BinaryFormat::Unknown;
  if (Scan.FirstStackSlot !=
      static_cast<int>(Scan.TRI->integerParamRegs(Format).size())) {
    collectSpilledStackArgs(Scan, Found);
    return;
  }
  CallArgScan FullBank = Scan;
  FullBank.StoreScanWindow = limits::kFullBankCallArgStoreScanWindow;
  collectSpilledStackArgs(FullBank, Found);
}

} // namespace call_args_detail

unsigned MedToHighConverter::importFixedArgCount(size_t CallIdx,
                                                 const std::vector<MedOp> &Ops,
                                                 va_t ResolvedSlot) const {
  if (!Image || CallIdx >= Ops.size())
    return 0;
  const MedOp &Call = Ops[CallIdx];
  if ((Call.Opcode != NdOp::CALL && Call.Opcode != NdOp::INDIR_CALL) ||
      Call.SourceCallHint || Call.NumInputs < 1)
    return 0;
  const va_t Key =
      Call.Inputs[0].isConst() ? Call.Inputs[0].ConstVal : ResolvedSlot;
  // One whose prototype the user declares reads what its summary counts.
  if (Key && PrototypeArgumentCallees && PrototypeArgumentCallees->count(Key))
    return static_cast<unsigned>(std::max<int>(0, Call.CalleeRegisterArgs));
  const std::string Import = Key ? importCalleeName(*Image, Key) : "";
  if (Import.empty())
    return 0;
  // A Mach-O bind names the symbol; a PE import entry is the C name.
  const std::string Name =
      cNameOfStatedName(Import, Image->Format, Image->Arch).str();
  if (const libc::LibCPrototype *Prototype =
          libc::libcPrototype(Name, Image->abiFormat()))
    return llvm::any_of(
               llvm::ArrayRef(Prototype->Params.data(), Prototype->ParamCount),
               libc::isFloatingType)
               ? 0
               : Prototype->ParamCount;
  // The name rules fit the C library's own routines, not another library's
  // `g_printf(fmt, ...)`.
  if (!libc::isKnownFunction(Name))
    return 0;
  if (const unsigned Fixed =
          libc::varArgFixedCount(Name, libc::libcVariantOf(*Image)))
    return Fixed;
  const auto Arity =
      importNamesAreCNames(Image->Format)
          ? libc::libcArity(Name, libc::libcVariantOf(*Image))
          : libc::libcArityForSymbol(Name, libc::libcVariantOf(*Image));
  return Arity && Arity->FpArgs == 0 && Arity->IntArgs > 0
             ? static_cast<unsigned>(Arity->IntArgs)
             : 0;
}

std::vector<ExprPtr>
MedToHighConverter::collectCallArgs(const MedBlock &CurBlock, size_t CallIdx,
                                    va_t ResolvedSlot) {
  const auto &Ops = CurBlock.Ops;
  if (auto Registration = collectRegistrationCallArgs(CurBlock, CallIdx))
    return std::move(*Registration);
  // The integer argument register at RegOff as this call reads it, where a
  // call that may change the register ends a path back from it.
  auto ReachingAtCall = [&](uint64_t RegOff, MedVar &Out,
                            bool ParameterAtEntry = false) {
    return CurMed &&
           detail::reachingRegisterAtCall(
               *CurMed, CurBlock, CallIdx, RegOff, getTargetRegInfo(TargetArch),
               Image ? Image->abiFormat() : BinaryFormat::Unknown, Out,
               ParameterAtEntry);
  };
  // A value this function sets: a constant, a parameter, or a definition
  // other than a register's incoming value.  A join is set when every value
  // reaching it is: a loop's running value is, but not one whose entry edge
  // brings the register's incoming value.
  auto JoinOf = [&](const MedVar &Arg) -> const PhiNode * {
    if (!CurMed)
      return nullptr;
    for (const MedBlock &Block : CurMed->Blocks)
      for (const PhiNode &Phi : Block.Phis)
        if (varKey(Phi.Output) == varKey(Arg))
          return &Phi;
    return nullptr;
  };
  auto ParameterRegister = [&](const MedVar &Arg) {
    return Arg.Kind == MedVar::Reg && CurMed &&
           llvm::any_of(CurMed->Params, [&](const MedVar &P) {
             return P.RegOff == Arg.RegOff;
           });
  };
  VarKeySet JoinsSeen;
  std::function<bool(const MedVar &)> SetInFunction =
      [&](const MedVar &Arg) -> bool {
    if (Arg.isConst() || Arg.Kind == MedVar::Param)
      return true;
    if (PhiOutputVars.count(varKey(Arg))) {
      if (!JoinsSeen.insert(varKey(Arg)).second)
        return true;
      const PhiNode *Join = JoinOf(Arg);
      return !Join || llvm::all_of(Join->Args, [&](const auto &Incoming) {
        // A register's version zero reaching its own join is the value it
        // holds at entry; any other version is the loop around again.
        if (varKey(Incoming.second) == varKey(Arg))
          return Arg.Kind != MedVar::Reg || Arg.SSAVer != 0 ||
                 ParameterRegister(Arg);
        return SetInFunction(Incoming.second);
      });
    }
    if (const MedOp *Def = uniqueMedDefinition(Arg);
        Def && !(Def->Opcode == NdOp::COPY && Def->NumInputs >= 1 &&
                 Def->Inputs[0] == Def->Output))
      return true;
    return ParameterRegister(Arg);
  };

  if (CallIdx < Ops.size() && Ops[CallIdx].IntegerCallSignature) {
    const auto &Call = Ops[CallIdx];
    if (!hasBoundIntegerCallArguments(Call))
      throw std::runtime_error("An integer header call lost its operands");
    std::vector<ExprPtr> Arguments;
    for (unsigned I = 1; I < Call.NumInputs; ++I) {
      ExprPtr Value = medvarToExpr(Call.Inputs[I]);
      const auto Bytes = Call.IntegerCallSignature->Parameters[I - 1].Bytes;
      if (!Value || !Value->Type || Value->Type->Size < Bytes)
        throw std::runtime_error("An integer header argument is incomplete");
      if (Value->Type->Size > Bytes) {
        Value = HighExpr::makeBinop(NdOp::SUBBYTES, Value,
                                    HighExpr::makeConst(0, 4));
        Value->Type = NdType::makeInt(Bytes, false);
      }
      Arguments.push_back(std::move(Value));
    }
    return Arguments;
  }

  if (CallIdx < Ops.size() && Ops[CallIdx].ScalarCallSignature) {
    const auto &Call = Ops[CallIdx];
    const auto &Signature = *Call.ScalarCallSignature;
    if (!boundFloatingCallArguments(Call, TargetArch,
                                    Image ? Image->abiFormat()
                                          : BinaryFormat::Unknown))
      throw std::runtime_error(
          "A mixed header call lost its complete operands");
    std::vector<ExprPtr> Arguments;
    for (unsigned I = 1; I < Call.NumInputs; ++I) {
      const auto &Parameter = Signature.Scalars[I - 1];
      ExprPtr Value = medvarToExpr(Call.Inputs[I]);
      if (!Value || !Value->Type || Value->Type->Size < Parameter.Bytes)
        throw std::runtime_error("A mixed header argument is incomplete");
      if (Value->Type->Size > Parameter.Bytes) {
        Value = HighExpr::makeBinop(NdOp::SUBBYTES, Value,
                                    HighExpr::makeConst(0, 4));
        Value->Type = NdType::makeInt(Parameter.Bytes, false);
      }
      if (Parameter.Floating != (Value->Type->Kind == NdTypeKind::Float))
        Value = HighExpr::makeBitCast(
            Value, Parameter.Floating
                       ? NdType::makeFloat(Parameter.Bytes)
                       : NdType::makeInt(Parameter.Bytes, false));
      Arguments.push_back(std::move(Value));
    }
    return Arguments;
  }

  // A call whose inputs are every argument it passes (a formatted call)
  // passes exactly those: a floating one as its bits' value, an integer one
  // that carries an address as that address.
  if (CallIdx < Ops.size() && Ops[CallIdx].ExactArguments) {
    const MedOp &Call = Ops[CallIdx];
    std::vector<ExprPtr> Exact;
    for (unsigned I = 1; I < Call.NumInputs; ++I) {
      ExprPtr Value = medvarToExpr(Call.Inputs[I]);
      if (Value && Value->Type && ((Call.ExactFloatInputs >> (I - 1)) & 1)) {
        Value =
            HighExpr::makeBitCast(Value, NdType::makeFloat(Value->Type->Size));
      } else if (Value && Value->Type && Value->Type->Kind == NdTypeKind::Int &&
                 ((Call.ExactPointerInputs >> (I - 1)) & 1)) {
        auto Address = std::make_shared<HighExpr>();
        Address->Kind = ExprKind::Cast;
        Address->Type = Address->CastTo = NdType::makePtr(NdType::makeVoid());
        Address->Operands.push_back(std::move(Value));
        Value = std::move(Address);
      }
      Exact.push_back(std::move(Value));
    }
    return Exact;
  }
  std::vector<ExprPtr> Hinted;
  if (CallIdx < Ops.size() && Ops[CallIdx].SourceCallHint) {
    const auto &Call = Ops[CallIdx];
    const auto &Signature = Call.SourceCallHint->Signature;
    const auto Bindings = sourceABIParameters(Signature);
    const size_t Count = Signature.HasExplicitABI ? Bindings.size()
                                                  : Signature.Parameters.size();
    const bool HasComponents =
        std::any_of(Signature.Parameters.begin(), Signature.Parameters.end(),
                    [](const auto &P) { return !P.Components.empty(); });
    if ((!Signature.HasExplicitABI && HasComponents) ||
        Count > static_cast<size_t>(limits::kMaxBoundSourceCallArgs) ||
        Call.NumInputs != Count + 1)
      return {};
    {
      Hinted.reserve(Count);
      size_t Index = 0;
      for (const auto &P : Signature.Parameters) {
        if (P.Components.empty()) {
          Hinted.push_back(medvarToExpr(Call.Inputs[++Index]));
        } else {
          std::vector<ExprPtr> Leaves;
          for (const auto &Member : sourceABIValueMembers(P.Type))
            Leaves.push_back(
                sourceScalarValue(Call.Inputs[++Index], Member.Type));
          if (P.Type->Kind == NdTypeKind::Int) {
            auto Value = HighExpr::makeBinop(NdOp::CONCAT, Leaves.back(),
                                             Leaves.front());
            Value->Type = NdType::makeInt(P.Type->Size, false);
            Hinted.push_back(std::move(Value));
          } else {
            Hinted.push_back(HighExpr::makeRecord(P.Type, std::move(Leaves)));
          }
        }
      }
    }
    // A validated call-site declaration fixes every operand and its width.
    // A register scan can see the whole register or unrelated setup past
    // that declared list; neither may replace these bound operands.
    // A zero-argument binding is also complete, without a heuristic scan.
    if (Hinted.empty() || Signature.HasExplicitABI ||
        Call.SourceCallHint->CallKind == SourceCallTypeHint::Kind::Analyst)
      return Hinted;
  }

  const int MaxArgs = limits::kMaxCallArgs;
  std::vector<ExprPtr> Found(MaxArgs);
  // The value the block's own write leaves in each argument register.
  std::vector<std::optional<MedVar>> FoundSource(MaxArgs);

  const auto &TRI = getTargetRegInfo(TargetArch);
  const BinaryFormat Format =
      Image ? Image->abiFormat() : BinaryFormat::Unknown;
  const auto ParamRegs = TRI.integerParamRegs(Format);
  const CallArgumentConvention *Convention =
      callArgumentConvention(TargetArch, Format);
  const call_args_detail::CallArgPolicy *Policy =
      call_args_detail::callArgPolicy(TargetArch, Format);
  const auto IntLayout = TRI.integerArgumentLayout(Format);
  auto integerSlot = [&](uint64_t RegOff) -> int {
    // With positional slots `regToArgIdx` maps XMM0 onto slot 0, same as
    // RCX. A `movups` leftover is not integer `this` for cstr/GetLength.
    if (Convention && Convention->PositionalArgumentSlots)
      return IntLayout.registerIndex(RegOff);
    return regToArgIdx(RegOff);
  };
  const uint64_t SpRegOff = TRI.StackPointer;

  auto IsCalleeSave = [&TRI](const MedVar &V) -> bool {
    return V.Kind == MedVar::Reg && TRI.isCalleeSaveReg(V.RegOff);
  };

  bool ReachedBlockStart = true;
  for (int J = static_cast<int>(CallIdx) - 1; J >= 0; --J) {
    const MedOp &Prev = Ops[J];
    if (endsCallArgumentSetup(Prev, /*RegistersOnly=*/true)) {
      ReachedBlockStart = false;
      break;
    }
    if (call_args_detail::isNoopRegisterCopy(Prev))
      continue;
    if (Prev.Output.Kind == MedVar::Reg && Prev.Output.Size > 0) {
      const int ArgIdx = integerSlot(Prev.Output.RegOff);
      if (ArgIdx >= 0 && ArgIdx < MaxArgs && !Found[ArgIdx]) {
        // The low register the lifter syncs after a whole register's write
        // is no narrower argument: after `lea rdi, [rip + s]` the call reads
        // all of RDI, as MedABIPass does.
        const MedVar Whole =
            Prev.Opcode == NdOp::SUBBYTES
                ? argRegSourceValueInBlock(CurBlock, J, TRI, IntLayout)
                : Prev.Output;
        if (Prev.Opcode == NdOp::COPY && Prev.NumInputs >= 1) {
          Found[ArgIdx] = medvarToExpr(Prev.Inputs[0]);
          FoundSource[ArgIdx] = Prev.Inputs[0];
        } else if (Whole.Size > Prev.Output.Size) {
          Found[ArgIdx] = medvarToExpr(Whole);
          FoundSource[ArgIdx] = Whole;
        } else {
          Found[ArgIdx] = medOpToExpr(Prev);
          FoundSource[ArgIdx] = Prev.Output;
        }
      }
    }
  }
  // A write in this block follows its entry PHIs and reaches the call.
  // Recover an entry PHI only for slots without an in-block write below.

  std::vector<call_args_detail::CallArgScan::OpWindow> ExtraWindows;
  auto blockById = [&](int Id) -> const MedBlock * {
    if (!CurMed)
      return nullptr;
    for (const auto &Blk : CurMed->Blocks)
      if (Blk.Id == Id)
        return &Blk;
    return nullptr;
  };
  // `COPY R9 = RDI.61` after `COPY RDI.59 = rax` leaves a dangling SSA.
  // Walk the trailing window for the last write of that register.
  auto exprFromWindowValue = [&](MedVar V, const std::vector<MedOp> &Ops,
                                 int Before) -> ExprPtr {
    const BinaryFormat Format =
        Image ? Image->abiFormat() : BinaryFormat::Unknown;
    auto preserved = [&](const MedVar &Reg) {
      return Reg.Kind == MedVar::Reg && call_args_detail::isCallPreservedReg(
                                            TRI, Format, Reg.RegOff, Reg.Size);
    };
    for (int Peel = 0;
         Peel <
         ::neverd::limits::ir::kCallArgCollectionExprFromWindowValuePeelLimit;
         ++Peel) {
      ExprPtr E = medvarToExpr(V);
      if (E && E->Kind != ExprKind::Undef)
        return E;
      if (V.Kind != MedVar::Reg && V.Kind != MedVar::Temp)
        return E;
      bool Progress = false;
      for (int K = Before; K >= 0; --K) {
        const MedOp &Def = Ops[static_cast<size_t>(K)];
        const bool SameSsa = Def.Output.Id == V.Id &&
                             Def.Output.SSAVer == V.SSAVer &&
                             Def.Output.Size > 0;
        const bool SameReg =
            V.Kind == MedVar::Reg && Def.Output.Kind == MedVar::Reg &&
            Def.Output.Size > 0 && Def.Output.RegOff == V.RegOff;
        if (SameSsa || SameReg) {
          if (Def.Opcode == NdOp::CALL || Def.Opcode == NdOp::INDIR_CALL ||
              Def.Opcode == NdOp::INTRINSIC)
            return medOpToExpr(Def);
          if (Def.Opcode == NdOp::COPY && Def.NumInputs >= 1) {
            ExprPtr Src = medvarToExpr(Def.Inputs[0]);
            if (Src && Src->Kind != ExprKind::Undef)
              return Src;
            V = Def.Inputs[0];
            Before = K - 1;
            Progress = true;
            break;
          }
          if ((Def.Opcode == NdOp::SUBBYTES || Def.Opcode == NdOp::INT_ZEXT ||
               Def.Opcode == NdOp::INT_SEXT || Def.Opcode == NdOp::CAST) &&
              Def.NumInputs >= 1) {
            V = Def.Inputs[0];
            Before = K - 1;
            Progress = true;
            break;
          }
          ExprPtr D = medOpToExpr(Def);
          if (D && D->Kind != ExprKind::Undef)
            return D;
        }
        if (call_args_detail::endsCallSetup(Def) && !preserved(V))
          break;
      }
      if (!Progress)
        return E;
    }
    return medvarToExpr(V);
  };
  auto predSetupExpr = [&](const MedVar &LiveIn) -> ExprPtr {
    const MedBlock *Pred = nullptr;
    const MedOp *Def = nullptr;
    int DefIdx = -1;
    int Hits = 0;
    if (CurMed) {
      for (const auto &Blk : CurMed->Blocks) {
        for (int I = 0; I < static_cast<int>(Blk.Ops.size()); ++I) {
          const MedOp &Op = Blk.Ops[static_cast<size_t>(I)];
          if (Op.Output.Id != LiveIn.Id || Op.Output.SSAVer != LiveIn.SSAVer)
            continue;
          ++Hits;
          Pred = &Blk;
          Def = &Op;
          DefIdx = I;
        }
      }
    }
    if (Hits != 1) {
      Pred = nullptr;
      Def = nullptr;
      DefIdx = -1;
      for (int PredId : CurBlock.Preds) {
        Pred = blockById(PredId);
        if (!Pred)
          continue;
        for (int I = 0; I < static_cast<int>(Pred->Ops.size()); ++I) {
          const MedOp &Op = Pred->Ops[static_cast<size_t>(I)];
          if (Op.Output.Id == LiveIn.Id && Op.Output.SSAVer == LiveIn.SSAVer) {
            Def = &Op;
            DefIdx = I;
            break;
          }
        }
        if (Def)
          break;
      }
    }
    if (!Def)
      return medvarToExpr(LiveIn);
    ExprPtr E = medOpToExpr(*Def);
    if (E && E->Kind != ExprKind::Undef)
      return E;
    if (Def->Opcode == NdOp::COPY && Def->NumInputs >= 1 && Pred)
      E = exprFromWindowValue(Def->Inputs[0], Pred->Ops, DefIdx - 1);
    if (E && E->Kind != ExprKind::Undef)
      return E;
    if (Pred)
      return exprFromWindowValue(LiveIn, Pred->Ops, DefIdx - 1);
    return E;
  };
  // A join that brings the register's incoming value on some path carries
  // nothing the function passes.
  auto EntryJoin = [&](const MedVar &LiveIn) {
    JoinsSeen.clear();
    return PhiOutputVars.count(varKey(LiveIn)) && !SetInFunction(LiveIn);
  };
  // Nor does what earlier calls left in the register on every path
  // (ValuesLeftByCalls), though the register carries a parameter on entry.
  auto LeftByCall = [&](const MedVar &LiveIn) {
    if (!CurMed)
      return false;
    if (LeftByCallsFor != CurMed) {
      LeftByCalls.emplace(*CurMed);
      LeftByCallsFor = CurMed;
    }
    return LeftByCalls->contains(LiveIn);
  };
  auto tryPredSetupArg = [&](int K) -> bool {
    MedVar LiveIn;
    if (!ReachingAtCall(ParamRegs[K], LiveIn))
      return false;
    if (!isCallArgSetupDef(CurBlock, LiveIn, ParamRegs[K]) || EntryJoin(LiveIn))
      return false;
    ExprPtr E = predSetupExpr(LiveIn);
    if (!E || E->Kind == ExprKind::Undef)
      return false;
    Found[K] = std::move(E);
    return true;
  };
  // Join `mov edx; jmp call` keeps a dominating `lea r8` in the shared
  // fork. Leftover r9 from an earlier Find is not that fork write.
  auto tryDominatingForkSetup = [&](int K) -> bool {
    if (Found[K] || CurBlock.Preds.size() < 2)
      return false;
    std::optional<int> ForkId;
    for (int PredId : CurBlock.Preds) {
      const MedBlock *Pred = blockById(PredId);
      if (!Pred || Pred->Preds.size() != 1)
        return false;
      if (!ForkId)
        ForkId = Pred->Preds[0];
      else if (*ForkId != Pred->Preds[0])
        return false;
    }
    const MedBlock *Fork = ForkId ? blockById(*ForkId) : nullptr;
    if (!Fork)
      return false;
    MedVar LiveIn;
    if (!ReachingAtCall(ParamRegs[K], LiveIn))
      return false;
    for (const auto &Op : Fork->Ops) {
      if (Op.Output.Id != LiveIn.Id || Op.Output.SSAVer != LiveIn.SSAVer)
        continue;
      if (Op.Opcode == NdOp::COPY && Op.NumInputs >= 1 &&
          Op.Inputs[0].Kind == MedVar::Reg &&
          Op.Inputs[0].RegOff == ParamRegs[K] &&
          Op.Inputs[0].Id == Op.Output.Id &&
          Op.Inputs[0].SSAVer == Op.Output.SSAVer)
        return false;
      if (Op.Output.Kind != MedVar::Reg || Op.Output.RegOff != ParamRegs[K] ||
          Op.Output.Size == 0)
        return false;
      Found[K] = medOpToExpr(Op);
      return true;
    }
    return false;
  };
  auto takePrecedingSetup = [&](int K) {
    return tryPredSetupArg(K) || tryDominatingForkSetup(K);
  };
  auto reachingAtEntry = [&](uint64_t RegOff, MedVar &LiveIn) {
    return ReachingAtCall(RegOff, LiveIn);
  };
  auto toExpr = [this](const MedVar &V) { return medvarToExpr(V); };
  auto opToExpr = [this](const MedOp &Op) { return medOpToExpr(Op); };
  auto argIdx = [this](uint64_t RegOff) { return regToArgIdx(RegOff); };
  auto paramIdx = [this](const MedVar &V) { return abiParamIndex(V); };
  call_args_detail::CallArgContext Ctx{Found,
                                       MaxArgs,
                                       ParamRegs,
                                       CurBlock,
                                       CurMed,
                                       toExpr,
                                       opToExpr,
                                       integerSlot,
                                       argIdx,
                                       paramIdx,
                                       reachingAtEntry,
                                       exprFromWindowValue,
                                       takePrecedingSetup};

  // A block boundary (an EH state change, a split `cmp/jnz`) can isolate the
  // CALL from setup at the end of its single predecessor; the convention
  // says whether to read it there.
  if (ReachedBlockStart && CallIdx == 0 && CurBlock.Preds.size() == 1 &&
      Policy && Policy->ReadsPredecessorWindow) {
    for (int PredId : CurBlock.Preds) {
      const MedBlock *Pred = blockById(PredId);
      if (!Pred || Pred->Ops.empty())
        continue;
      ExtraWindows.push_back(
          {&Pred->Ops, static_cast<int>(Pred->Ops.size()) - 1});
      if (Policy->TakePredecessorRegisters)
        Policy->TakePredecessorRegisters(Ctx, *Pred);
    }
  }

  if (ReachedBlockStart) {
    int MaxRegArg = -1;
    for (int K = 0; K < MaxArgs; ++K)
      if (Found[K] && Found[K]->Kind != ExprKind::Undef)
        MaxRegArg = K;
    // Fill holes below the highest written slot.  Do not extend arity with
    // live-in r9 when the call only wrote rcx/rdx/r8 (GSHandlerCheckCommon).
    int FillLast = MaxRegArg;
    if (Policy && Policy->RecoverCallOnlySetup && MaxRegArg < 0)
      Policy->RecoverCallOnlySetup(Ctx, FillLast);
    // An indirect call whose own block sets no argument register takes the
    // consecutive setup writes of the block before it (`mov rdi, [rdi]; test
    // rdi, rdi; je; mov rax, [rdi]; call [rax+58h]`) when its calling
    // convention says so, never padded to this function's own parameters.  A
    // direct callee has its own summary.
    if (Convention && Convention->IndirectCallsTakePrecedingSetup &&
        MaxRegArg < 0 && CallIdx < Ops.size() &&
        Ops[CallIdx].Opcode == NdOp::INDIR_CALL &&
        Ops[CallIdx].CalleeRegisterArgs < 0 && !Ops[CallIdx].SourceCallHint) {
      for (int K = 0; K < static_cast<int>(ParamRegs.size()) && K < MaxArgs;
           ++K) {
        if ((!tryPredSetupArg(K) && !tryDominatingForkSetup(K)) || !Found[K] ||
            Found[K]->Kind == ExprKind::Undef) {
          Found[K] = nullptr;
          break;
        }
        FillLast = K;
      }
    }
    if (Policy && Policy->ExtendWrittenArgs && MaxRegArg >= 0)
      Policy->ExtendWrittenArgs(Ctx, MaxRegArg, FillLast);
    // A C library import reads its fixed parameters however the call is
    // reached: one set before this block, on each path into it, is the
    // value reaching the call (`fprintf(stderr, fmt)` after a join; a
    // `jmp rax` alone in its block in register_tm_clones).  Where an import
    // takes every argument on the stack (i386), none is in a register.
    const unsigned Fixed =
        Convention && Convention->RegparmOnlyForInternalCalls
            ? 0
            : importFixedArgCount(CallIdx, Ops, ResolvedSlot);
    if (Fixed)
      FillLast =
          std::max(FillLast, std::min(static_cast<int>(Fixed),
                                      static_cast<int>(ParamRegs.size())) -
                                 1);
    for (int K = 0; K <= FillLast && K < static_cast<int>(ParamRegs.size());
         ++K) {
      if (Found[K])
        continue;
      // An import's fixed argument may be a parameter this function only
      // passes on, which its entry holds no copy of (`open(path, 0)` with
      // RDI untouched).
      MedVar LiveIn;
      if (!ReachingAtCall(ParamRegs[K], LiveIn,
                          /*ParameterAtEntry=*/K < static_cast<int>(Fixed)))
        continue;
      // The arguments end before such a join, and past the fixed ones an
      // import reads, before what earlier calls left.
      if (EntryJoin(LiveIn) ||
          (K >= static_cast<int>(Fixed) && LeftByCall(LiveIn))) {
        std::fill(Found.begin() + K, Found.end(), nullptr);
        break;
      }
      Found[K] = medvarToExpr(LiveIn);
    }
  }

  if (Policy && Policy->ResolvePassThroughParams && CurMed)
    Policy->ResolvePassThroughParams(Ctx);

  // A floating argument is the low bytes of its vector register the callee
  // reads; the register's other lanes, perhaps never defined, are no part
  // of it.
  auto CallInput = [&](const MedOp &Call, unsigned Input) {
    ExprPtr Value = medvarToExpr(Call.Inputs[Input]);
    const uint16_t Width = Call.vectorArgumentWidth(Input);
    if (Width && Value->Type && Width < Value->Type->Size) {
      Value =
          HighExpr::makeBinop(NdOp::SUBBYTES, Value, HighExpr::makeConst(0, 4));
      Value->Type = NdType::makeInt(Width, false);
    }
    return Value;
  };
  // A summarized callee published exactly the register arguments it reads
  // as the CALL's inputs (LowToMed); SSA already renamed them to the values
  // reaching the call, including a caller's pass-through argument.
  if (CallIdx < Ops.size() && Ops[CallIdx].CalleeRegisterArgs >= 0 &&
      !Ops[CallIdx].SourceCallHint) {
    const MedOp &Call = Ops[CallIdx];
    const int RegisterSlots = static_cast<int>(ParamRegs.size());
    for (int I = 0; I < RegisterSlots && I < MaxArgs; ++I)
      Found[I] = I < Call.CalleeRegisterArgs && 1 + I < Call.NumInputs
                     ? CallInput(Call, static_cast<unsigned>(1 + I))
                     : nullptr;
    // So did its floating arguments, which follow the register arguments
    // below: a vector register the scan above saw written is none, nor the
    // stack argument its slot index would otherwise read as.
    if (Call.CalleeVectorArgs >= 0)
      for (uint64_t Vector : TRI.FPParamRegs)
        if (const int K = integerSlot(Vector);
            K >= RegisterSlots && K < MaxArgs)
          Found[K] = nullptr;
  }

  // `mov r8d, [p+field]; call` is often `LOAD t; ZEXT r8, t`. The zext is the
  // last param-reg write, so the call would keep a dangling SSA of t if the
  // load assign is later DCE'd. The argument is the loaded value.
  auto sameSsaDef = [](const MedVar &Out, const MedVar &Wanted) {
    if (Out.Id != Wanted.Id || Out.SSAVer != Wanted.SSAVer || Out.Size == 0 ||
        Out.Kind != Wanted.Kind)
      return false;
    if (Out.Kind == MedVar::Reg)
      return Out.RegOff == Wanted.RegOff;
    return true;
  };
  auto attachProducingLoad = [&](ExprPtr E, const std::vector<MedOp> &Win,
                                 int Before) -> ExprPtr {
    if (!E)
      return E;
    const HighExpr *Cur = E.get();
    bool PeeledView = false;
    for (int Peel = 0;
         Peel < ::neverd::limits::ir::
                    kCallArgCollectionAttachProducingLoadPeelLimit &&
         Cur;
         ++Peel) {
      if (Cur->Kind == ExprKind::Load)
        return E;
      const bool View =
          (Cur->Kind == ExprKind::UnaryOp &&
           (Cur->Op == NdOp::INT_ZEXT || Cur->Op == NdOp::INT_SEXT ||
            Cur->Op == NdOp::CAST)) ||
          Cur->Kind == ExprKind::Cast || Cur->Kind == ExprKind::BitCast;
      if (!View || Cur->Operands.empty() || !Cur->Operands[0])
        break;
      PeeledView = true;
      Cur = Cur->Operands[0].get();
    }
    // A bare live-in register is not a widened load. Matching Id/SSA across
    // Kind would steal a later integer LOAD onto a dominating `lea rcx`.
    if (!PeeledView || !Cur || Cur->Kind != ExprKind::Var || Cur->Var.Id < 0)
      return E;
    MedVar Wanted = Cur->Var;
    for (int K = Before; K >= 0; --K) {
      const MedOp &Def = Win[static_cast<size_t>(K)];
      if (!sameSsaDef(Def.Output, Wanted))
        continue;
      if (Def.Opcode == NdOp::LOAD)
        return medOpToExpr(Def);
      if ((Def.Opcode == NdOp::COPY || Def.Opcode == NdOp::INT_ZEXT ||
           Def.Opcode == NdOp::INT_SEXT || Def.Opcode == NdOp::CAST) &&
          Def.NumInputs >= 1) {
        Wanted = Def.Inputs[0];
        continue;
      }
      break;
    }
    return E;
  };
  auto uniqueLoadDef = [&](const MedVar &V) -> const MedOp * {
    if (!CurMed || V.Id < 0)
      return nullptr;
    const MedOp *UniqueLoad = nullptr;
    for (const auto &Blk : CurMed->Blocks)
      for (const auto &Op : Blk.Ops)
        if (Op.Opcode == NdOp::LOAD && sameSsaDef(Op.Output, V)) {
          if (UniqueLoad)
            return nullptr;
          UniqueLoad = &Op;
        }
    return UniqueLoad;
  };
  std::function<ExprPtr(ExprPtr, int)> inlineUniqueLoadVars =
      [&](ExprPtr E, int Depth) -> ExprPtr {
    if (!E ||
        Depth >
            ::neverd::limits::ir::
                kCallArgCollectionMedToHighConverterCollectCallArgsInlineUniqueLoadVarsDepthLimit)
      return E;
    if (E->Kind == ExprKind::Var) {
      if (const MedOp *Load = uniqueLoadDef(E->Var))
        return inlineUniqueLoadVars(medOpToExpr(*Load), Depth + 1);
      return E;
    }
    if (E->Kind != ExprKind::BinOp && E->Kind != ExprKind::UnaryOp &&
        E->Kind != ExprKind::Load && E->Kind != ExprKind::Cast &&
        E->Kind != ExprKind::BitCast)
      return E;
    auto Copy = std::make_shared<HighExpr>(*E);
    for (auto &Op : Copy->Operands)
      Op = inlineUniqueLoadVars(Op, Depth + 1);
    return Copy;
  };
  auto attachProducingLoadFromWindows = [&](ExprPtr E) -> ExprPtr {
    ExprPtr Attached =
        attachProducingLoad(E, Ops, static_cast<int>(CallIdx) - 1);
    if (Attached && Attached->Kind == ExprKind::Load)
      return Attached;
    for (const auto &Window : ExtraWindows) {
      if (!Window.Ops)
        continue;
      Attached = attachProducingLoad(E, *Window.Ops, Window.Before);
      if (Attached && Attached->Kind == ExprKind::Load)
        return Attached;
    }
    if (!CurMed || !E)
      return E;
    const HighExpr *Cur = E.get();
    bool PeeledView = false;
    for (int Peel = 0;
         Peel < ::neverd::limits::ir::
                    kCallArgCollectionAttachProducingLoadFromWindowsPeelLimit &&
         Cur;
         ++Peel) {
      if (Cur->Kind == ExprKind::Load)
        return E;
      const bool View =
          (Cur->Kind == ExprKind::UnaryOp &&
           (Cur->Op == NdOp::INT_ZEXT || Cur->Op == NdOp::INT_SEXT ||
            Cur->Op == NdOp::CAST)) ||
          Cur->Kind == ExprKind::Cast || Cur->Kind == ExprKind::BitCast;
      if (!View || Cur->Operands.empty() || !Cur->Operands[0])
        break;
      PeeledView = true;
      Cur = Cur->Operands[0].get();
    }
    if (!PeeledView || !Cur || Cur->Kind != ExprKind::Var)
      return E;
    if (const MedOp *UniqueLoad = uniqueLoadDef(Cur->Var))
      return medOpToExpr(*UniqueLoad);
    return E;
  };
  for (int K = 0; K < MaxArgs; ++K) {
    if (!Found[K])
      continue;
    Found[K] = attachProducingLoadFromWindows(Found[K]);
    if (Found[K] && Found[K]->Kind == ExprKind::Load)
      Found[K] = inlineUniqueLoadVars(Found[K], 0);
  }

  // A call through a pointer passes no register argument where only directly
  // called internal functions take them and an unknown callee takes every
  // argument on the stack (an ELF or Mach-O i386 image; a Windows one may
  // call __thiscall or __fastcall code), as MedIR's recovery has it
  // (MedABIPass).  Its stack arguments then start at the first position.
  if (Convention && Convention->RegparmOnlyForInternalCalls && Image &&
      CallIdx < Ops.size() && Ops[CallIdx].Opcode == NdOp::INDIR_CALL &&
      !Ops[CallIdx].SourceCallHint &&
      !(Convention->ImportsMayTakeRegisterArguments &&
        Convention->ImportsMayTakeRegisterArguments(*Image)))
    for (size_t K = 0; K < ParamRegs.size() && K < static_cast<size_t>(MaxArgs);
         ++K)
      Found[K] = nullptr;

  // A variadic callee that saves the vector argument registers takes every
  // integer and vector argument register, as its parameters say: a caller
  // passes the ones it sets, and the vector registers' low doubles carry its
  // floating arguments.  Its summary lists none of them.
  const VariadicVectorCallee *VariadicCallee = nullptr;
  if (VariadicVectorCallees && CallIdx < Ops.size() &&
      Ops[CallIdx].Opcode == NdOp::CALL && Ops[CallIdx].NumInputs >= 1 &&
      Ops[CallIdx].Inputs[0].isConst())
    if (const auto Callee =
            VariadicVectorCallees->find(Ops[CallIdx].Inputs[0].ConstVal);
        Callee != VariadicVectorCallees->end())
      VariadicCallee = &Callee->second;

  // Stack arguments follow the register arguments found, though a variadic
  // callee's follow every register: a store to the outgoing area before a
  // call that sets fewer is the caller's own (clang's `movsd [rsp], xmm1`
  // keeping a double across the call), not an argument in a register's place.
  int FirstStackSlot = 0;
  if (VariadicCallee)
    FirstStackSlot = static_cast<int>(ParamRegs.size());
  else
    for (int K = 0; K < MaxArgs; ++K) {
      if (Found[K] && Found[K]->Kind != ExprKind::Undef)
        FirstStackSlot = K + 1;
      else
        break;
    }

  call_args_detail::CallArgScan Scan;
  Scan.Ops = &Ops;
  Scan.CallIdx = CallIdx;
  Scan.SpRegOff = SpRegOff;
  Scan.TRI = &TRI;
  Scan.Image = Image;
  Scan.TheArch = TargetArch;
  Scan.Convention = Convention;
  Scan.MaxArgs = MaxArgs;
  if (CallIdx < Ops.size() && !Ops[CallIdx].SourceCallHint) {
    const MedOp &Call = Ops[CallIdx];
    Scan.CalleeRegisterArgs = Call.CalleeRegisterArgs;
    Scan.CalleeStackArgs = Call.CalleeStackArgs;
    Scan.HeaderVariadic = Call.CalleeVariadicFrom >= 0;
    Scan.CalleeInImage =
        Call.Opcode == NdOp::CALL && Call.NumInputs >= 1 &&
        Call.Inputs[0].isConst() &&
        (!Image || importCalleeName(*Image, Call.Inputs[0].ConstVal).empty());
  }
  Scan.FirstStackSlot = FirstStackSlot;
  // A tail jump that returns a register pair or struct still leaves on the
  // entry stack: the forwarder remodel unpacks its result into the field
  // registers between the call and the RETURN, at the call's address.
  size_t AfterCall = CallIdx + 1;
  while (AfterCall < Ops.size() && Ops[AfterCall].Opcode == NdOp::SUBBYTES &&
         Ops[AfterCall].Addr == Ops[CallIdx].Addr &&
         Ops[AfterCall].NumInputs >= 1 &&
         Ops[AfterCall].Inputs[0] == Ops[CallIdx].Output)
    ++AfterCall;
  Scan.TailJump = AfterCall < Ops.size() &&
                  Ops[AfterCall].Opcode == NdOp::RETURN &&
                  Ops[AfterCall].Addr == Ops[CallIdx].Addr;
  Scan.Indirect =
      CallIdx < Ops.size() && Ops[CallIdx].Opcode == NdOp::INDIR_CALL;
  Scan.StoreScanWindow = limits::kCallArgStoreScanWindow;
  Scan.ExtraWindows = ExtraWindows;
  auto ToExpr = [this](const MedVar &V) { return medvarToExpr(V); };
  Scan.ToExpr = ToExpr;
  Scan.IsCalleeSave = IsCalleeSave;
  auto ReachingRegArg = [&](int Index) -> ExprPtr {
    if (Index < 0 || Index >= static_cast<int>(ParamRegs.size()))
      return nullptr;
    // What earlier calls left there passes no value where the callee is not
    // known to read the register: the slot still takes its place, as zero.
    // A register it reads keeps its unknown value.
    MedVar LiveIn;
    if (ReachingAtCall(ParamRegs[Index], LiveIn)) {
      const bool Read =
          CallIdx < Ops.size() && Ops[CallIdx].CalleeRegisterArgs > Index;
      return !Read && LeftByCall(LiveIn) ? nullptr : medvarToExpr(LiveIn);
    }
    // A register this function never writes, at its only call, still holds
    // its incoming value: a recovered parameter is passed straight through.
    if (auto Param = untouchedParamRegister(ParamRegs[Index]))
      return medvarToExpr(*Param);
    return nullptr;
  };
  Scan.ReachingRegArg = ReachingRegArg;
  auto IsOwnParameter = [&](int Index) {
    return CurMed && Index >= 0 && Index < static_cast<int>(ParamRegs.size()) &&
           llvm::any_of(
               CurMed->Params,
               [&](const MedVar &P) { return P.RegOff == ParamRegs[Index]; });
  };
  Scan.IsOwnParameter = IsOwnParameter;
  auto OwnStackParam = [&](int Index) -> ExprPtr {
    if (CurMed)
      for (const MedVar &P : CurMed->Params)
        if (P.Kind == MedVar::Param && P.Id == Index && P.RegOff == kNoParamReg)
          return medvarToExpr(P);
    return nullptr;
  };
  Scan.OwnStackParam = OwnStackParam;
  // The single definition of \p V in this function, or null.
  auto UniqueDef = [&](const MedVar &V) { return uniqueMedDefinition(V); };
  auto EntryOffsetOf = [&](const MedVar &V) -> std::optional<int64_t> {
    if (!CurMed)
      return std::nullopt;
    indexMedDefinitions();
    return EntryStackOffsets->offset(V);
  };
  Scan.EntryOffsetOf = EntryOffsetOf;
  // An outgoing stack argument is placed by the stack pointer the call is
  // made with (CallArgCollectionX86.cpp): the last one its block defines, or
  // else the one reaching the block.
  MedVar CallStack;
  bool CallStackKnown = false;
  for (size_t J = CallIdx; J-- > 0;)
    if (const MedOp &Op = Ops[J]; Op.Output.Kind == MedVar::Reg &&
                                  Op.Output.RegOff == SpRegOff &&
                                  Op.Output.Size) {
      CallStack = Op.Output;
      CallStackKnown = true;
      break;
    }
  if (!CallStackKnown)
    CallStackKnown = reachingRegAtBlockEntry(CurBlock, SpRegOff, CallStack);
  if (CallStackKnown)
    Scan.CallStackEntryOffset = EntryOffsetOf(CallStack);
  // A frame realigned by `and esp, -16` relates the two only through its
  // alignment, as call-ABI recovery's call-SP map does (MedABIPass.cpp).
  auto CallStackOffsetOf =
      [&](const MedVar &Address) -> std::optional<int64_t> {
    if (!CurMed || !CallStackKnown)
      return std::nullopt;
    indexMedDefinitions();
    return EntryStackOffsets->callOffsetFrom(Address, CallStack);
  };
  Scan.CallStackOffsetOf = CallStackOffsetOf;
  // A stack slot below the stack pointer an address is made from, another
  // pointer (read from memory, or a register this function received), or a
  // fixed address holds no outgoing argument.  An address this cannot
  // follow may.
  auto IsNoArgumentStore = [&](const MedVar &Address) {
    MedVar Cur = Address;
    int64_t Addend = 0;
    for (int Depth = 0; Depth <= limits::kCallArgStoreAddressDepth; ++Depth) {
      if (Cur.isConst())
        return true;
      if (Cur.Kind == MedVar::Reg && Cur.RegOff == SpRegOff)
        return Addend < 0;
      const MedOp *Def = UniqueDef(Cur);
      if (!Def)
        return Cur.Kind == MedVar::Reg && Cur.SSAVer == 0;
      if (Def->Opcode == NdOp::LOAD)
        return true;
      if ((Def->Opcode == NdOp::COPY || Def->Opcode == NdOp::INT_ZEXT) &&
          Def->NumInputs >= 1) {
        const MedVar &In = Def->Inputs[0];
        // The identity copy that names a register's entry value: a pointer
        // this function received.
        if (In.Kind == Cur.Kind && In.RegOff == Cur.RegOff && In.Id == Cur.Id &&
            In.SSAVer == Cur.SSAVer)
          return Cur.Kind == MedVar::Reg;
        Cur = In;
        continue;
      }
      if ((Def->Opcode != NdOp::INT_ADD && Def->Opcode != NdOp::INT_SUB) ||
          Def->NumInputs != 2 || !Def->Inputs[1].isConst())
        return false;
      const int64_t C = static_cast<int64_t>(Def->Inputs[1].ConstVal);
      Addend += Def->Opcode == NdOp::INT_ADD ? C : -C;
      Cur = Def->Inputs[0];
    }
    return false;
  };
  Scan.IsNoArgumentStore = IsNoArgumentStore;
  auto IsPcRootedStore = [&](const MedOp &Store) {
    return I386PcRootedStores.count({Store.Addr, Store.OriginSeq}) != 0;
  };
  Scan.IsPcRootedStore = IsPcRootedStore;
  Scan.FrameSize = CurMed ? CurMed->FrameSize : 0;
  if (CurMed && LoadedEntrySlotsFor != CurMed) {
    LoadedEntrySlots.clear();
    auto AddSlot = [&](const MedVar &V) {
      if (auto Off = EntryOffsetOf(V))
        LoadedEntrySlots.insert(*Off);
    };
    for (const auto &Blk : CurMed->Blocks)
      for (const auto &Op : Blk.Ops) {
        if (Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
          continue;
        if (Op.Opcode == NdOp::LOAD && Op.NumInputs >= 1)
          AddSlot(Op.Inputs[0]);
        // A slot whose address escapes (kept in a register, stored, or
        // passed to a callee) is a local the callee or a later load reads
        // through that pointer, not an outgoing argument.
        // The stack and frame pointers themselves only locate the frame.
        if ((Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::COPY) &&
            Op.Output.Kind == MedVar::Reg && Op.Output.RegOff != SpRegOff &&
            Op.Output.RegOff != TRI.FramePointer)
          AddSlot(Op.Output);
        if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 2)
          AddSlot(Op.Inputs[1]);
      }
    LoadedEntrySlotsFor = CurMed;
  }
  Scan.LoadedEntrySlots = &LoadedEntrySlots;
  Scan.ResolveWindow = exprFromWindowValue;

  std::vector<ExprPtr> Args;
  switch (TargetArch) {
  case Arch::X86:
  case Arch::X64:
    call_args_detail::collectCallArgsX86(Scan, Found, Args);
    break;
  case Arch::ARM:
    call_args_detail::collectCallArgsARM(Scan, Found, Args);
    break;
  case Arch::AArch64:
    call_args_detail::collectCallArgsAArch64(Scan, Found, Args);
    break;
  default:
    call_args_detail::collectSpilledStackArgs(Scan, Found);
    break;
  }

  if (Args.empty()) {
    for (int K = 0; K < MaxArgs; ++K) {
      if (!Found[K])
        break;
      Args.push_back(Found[K]);
    }
  }

  // A callee summary already published exactly the register arguments the
  // callee reads, SSA-renamed to their reaching values; guessing a
  // pass-through parameter for an unread slot would print the function's
  // incoming RCX where the caller loaded 0xC9.
  const bool SummarizedCallee = CallIdx < Ops.size() &&
                                Ops[CallIdx].CalleeRegisterArgs >= 0 &&
                                !Ops[CallIdx].SourceCallHint;
  // A recursive call passes this function's own parameters: its register
  // arguments are this definition's register parameters.
  const bool SelfCall = CallIdx < Ops.size() && CurMed &&
                        Ops[CallIdx].NumInputs >= 1 &&
                        Ops[CallIdx].Inputs[0].isConst() &&
                        Ops[CallIdx].Inputs[0].ConstVal == CurMed->Entry;
  // The signature this function is printed with: a native source type hint
  // when it has one, else its recovered parameters.
  const size_t OwnParamCount =
      CurMed
          ? (CurMed->SourceTypeHint ? CurMed->SourceTypeHint->Parameters.size()
                                    : CurMed->Params.size())
          : 0;
  // A recursive call passes exactly the parameters of the signature it
  // calls.  A missing register argument is this function's own parameter
  // when one was recovered for that position, else a value the call site
  // does not determine.
  const bool PassesOwnSignature =
      SelfCall && Policy && Policy->RecursiveCallsPassOwnSignature;
  auto MatchOwnSignature = [&](std::vector<ExprPtr> Collected) {
    if (!PassesOwnSignature)
      return Collected;
    if (Collected.size() > OwnParamCount)
      Collected.resize(OwnParamCount);
    for (size_t I = Collected.size(); I < OwnParamCount; ++I) {
      if (I < CurMed->Params.size() &&
          CurMed->Params[I].RegOff != kNoParamReg &&
          CurMed->Params[I].Id >= 0) {
        MedVar Param = CurMed->Params[I];
        Param.Kind = MedVar::Param;
        Param.Id = static_cast<int>(I);
        Collected.push_back(HighExpr::makeVar(Param, TypeRef{}));
      } else {
        Collected.push_back(HighExpr::makeUndef(8));
      }
    }
    return Collected;
  };
  if (Policy && Policy->FillUnwrittenParams && CurMed && !SummarizedCallee)
    Policy->FillUnwrittenParams(Ctx, Hinted.size(), SelfCall, OwnParamCount);

  auto BoundKnownCalleeArity = [&](std::vector<ExprPtr> Collected) {
    if (CallIdx >= Ops.size())
      return Collected;
    if (PassesOwnSignature)
      return MatchOwnSignature(std::move(Collected));
    const MedOp &Call = Ops[CallIdx];
    // A call to an import whose prototype the user declares, through its
    // stub or its slot, passes what call-ABI recovery gave it.
    if (PrototypeArgumentCallees) {
      if (Call.NumInputs >= 1 && Call.Inputs[0].isConst() &&
          PrototypeArgumentCallees->count(Call.Inputs[0].ConstVal))
        return Collected;
      if (CurMed)
        if (const MedCallInfo *Info =
                CurMed->findCall(CurBlock.Id, static_cast<int>(CallIdx));
            Info && PrototypeArgumentCallees->count(Info->TargetAddr))
          return Collected;
    }
    std::string Name;
    if (Call.SourceCallHint && !Call.SourceCallHint->TargetName.empty())
      Name = Call.SourceCallHint->TargetName;
    else if (Call.NumInputs >= 1 && Call.Inputs[0].isConst())
      Name = calleeDisplayName(Call.Inputs[0].ConstVal);
    if (Name.empty())
      return Collected;
    size_t N = 0;
    if (const std::optional<size_t> Count =
            Convention && Convention->PrototypeArgCount
                ? Convention->PrototypeArgCount(Name)
                : std::nullopt) {
      // A platform prototype fixes the stack arguments too: outgoing-area
      // stores for a later call are not arguments of this one.
      N = *Count;
    } else {
      const auto Arity = libc::libcArityForSymbol(
          Name, Image ? libc::libcVariantOf(*Image) : libc::LibCVariant::GNU);
      if (!Arity)
        return Collected;
      N = static_cast<size_t>(std::max(0, Arity->IntArgs) +
                              std::max(0, Arity->FpArgs));
    }
    if (Collected.size() > N)
      Collected.resize(N);
    return Collected;
  };

  // Where call-ABI recovery ended an indirect call's register arguments at
  // the register that holds its target, no scan goes past that position:
  // the target's earlier writes, which dead-code elimination left, are stale
  // there (`add r0, pc; ldr r0, [r0]; blx r0`).
  size_t TargetLimit = static_cast<size_t>(MaxArgs);
  if (CurMed)
    if (const MedCallInfo *Info =
            CurMed->findCall(CurBlock.Id, static_cast<int>(CallIdx));
        Info && Info->TargetRegisterArgIdx >= 0)
      TargetLimit = static_cast<size_t>(Info->TargetRegisterArgIdx);
  if (Hinted.empty() && CurMed) {
    if (const MedCallInfo *CI =
            CurMed->findCall(CurBlock.Id, static_cast<int>(CallIdx))) {
      // Where call-setup recovery gives a call its arguments, it gives a
      // direct call all of them, none included.  Nothing bounds an indirect
      // call's by its callee, so it takes those this function sets, up to
      // the first it does not: a register's entry value where no parameter
      // arrives, or what an earlier call left there, is no argument.  A
      // value that reaches the call through a join is one (a loop's running
      // value left in x0), though the call's block writes no register.  The
      // scan below adds what the block writes past them.  Where only direct
      // calls take register arguments (regparm), the scan alone gives an
      // indirect call its stack arguments.
      const bool SetupConvention =
          Convention && Convention->ArgumentsFromCallSetup;
      // A direct call's final recovered list also owns Win64 stack slots.
      // Appending this block's adjacent locals can revive parameters removed
      // by the shared callee bound, or turn a required unknown into a stale
      // value from the emitter's independent setup scan.
      const bool CompleteSetup =
          !CI->IsIndirect &&
          (SetupConvention || (Convention && Convention->StackArgumentSummary));
      // A loader-bound import retains its fixed contract through a captured
      // runtime pointer: an independent setup scan must not append adjacent
      // frame locals to its recovered list. Like any indirect call's, a value
      // recovery left unknown within that list still comes from the scan.
      // Where only internal calls take register arguments, the scan alone
      // gives an indirect call its stack arguments, an import's included.
      const bool KnownImport =
          Image && Convention && !Convention->RegparmOnlyForInternalCalls &&
          !importCalleeName(*Image, CI->TargetAddr).empty();
      size_t Recovered = CI->Args.size();
      if (SetupConvention && CI->IsIndirect && !KnownImport) {
        // The scan gives such a call its stack arguments, though not the
        // incoming ones a tail jump passes on untouched, which recovery
        // promotes to stack parameters.
        if (Convention->RegparmOnlyForInternalCalls) {
          Recovered = 0;
          while (Recovered < CI->Args.size() &&
                 CI->Args[Recovered].Kind == MedVar::Param &&
                 CI->Args[Recovered].RegOff == kNoParamReg)
            ++Recovered;
        }
        for (size_t K = 0; K < Recovered; ++K) {
          JoinsSeen.clear();
          if (!SetInFunction(CI->Args[K])) {
            Recovered = K;
            break;
          }
        }
      }
      if (CompleteSetup ||
          (SetupConvention
               ? Recovered || TargetLimit < static_cast<size_t>(MaxArgs)
               : !CI->Args.empty())) {
        std::vector<ExprPtr> FromABI;
        FromABI.reserve(Recovered);
        for (size_t K = 0; K < Recovered; ++K) {
          ExprPtr Arg = medvarToExpr(CI->Args[K]);
          // The callee reads only the low lane's scalar.
          const uint16_t Read =
              K < CI->ArgReadBytes.size() ? CI->ArgReadBytes[K] : 0;
          if (Read && Arg && Arg->Type && Read < Arg->Type->Size) {
            Arg = HighExpr::makeBinop(NdOp::SUBBYTES, Arg,
                                      HighExpr::makeConst(0, 4));
            Arg->Type = NdType::makeInt(Read, false);
          }
          const bool FloatingLane =
              CI->IntegerRegisterArgCount >= 0 &&
              K >= static_cast<size_t>(CI->IntegerRegisterArgCount) &&
              K - CI->IntegerRegisterArgCount <
                  static_cast<size_t>(
                      std::max(0, CI->FloatingRegisterArgCount));
          if (Read && FloatingLane && Ops[CallIdx].CalleeFloatReturnBytes &&
              Arg && Arg->Type && Arg->Type->Kind == NdTypeKind::Int &&
              Arg->Type->Size == Read)
            Arg =
                HighExpr::makeBitCast(std::move(Arg), NdType::makeFloat(Read));
          if (Ops[CallIdx].X87ArgumentInput && K + 1 == CI->Args.size() &&
              Arg && Arg->Type && Arg->Type->Size == 10 &&
              Arg->Type->Kind != NdTypeKind::Float)
            Arg = HighExpr::makeBitCast(std::move(Arg), NdType::makeFloat(10));
          FromABI.push_back(std::move(Arg));
        }
        // The register that holds an indirect call's target is no argument
        // (`mov r2, r0; mov r0, r1; blx r2`).
        const MedVar &Target = Ops[CallIdx].Inputs[0];
        auto HoldsTarget = [&](size_t I) {
          const std::optional<MedVar> &Source = FoundSource[I];
          return CI->IsIndirect && !Target.isConst() && Source &&
                 Source->Kind == Target.Kind && Source->Id == Target.Id &&
                 Source->SSAVer == Target.SSAVer;
        };
        size_t End = FromABI.size();
        while (!CompleteSetup && !KnownImport && End < TargetLimit &&
               Found[End] && Found[End]->Kind != ExprKind::Undef &&
               !HoldsTarget(End))
          ++End;
        for (size_t I = FromABI.size(); I < End; ++I)
          FromABI.push_back(Found[I]);
        // The scan numbers registers in the convention's order, which does
        // not index arguments passed in the callee's own.
        for (size_t I = 0; I < FromABI.size(); ++I)
          if (!CompleteSetup &&
              (!FromABI[I] || FromABI[I]->Kind == ExprKind::Undef) &&
              !CI->ArgumentsInCalleeRegisterOrder && Found[I] &&
              Found[I]->Kind != ExprKind::Undef)
            FromABI[I] = Found[I];
        return BoundKnownCalleeArity(std::move(FromABI));
      }
    }
  }

  if (!Hinted.empty()) {
    // Source ABI operands are authoritative, though a convention may still
    // prefer a scanned value for some of them (MergeScannedArgs); AArch64
    // selector stubs must not replace `_cmd` with the const-0 overwrite
    // placeholder.
    if (Policy && Policy->MergeScannedArgs)
      Policy->MergeScannedArgs(Ctx, Hinted,
                               Ops[CallIdx].SourceCallHint->Signature);
    return MatchOwnSignature(std::move(Hinted));
  }
  Args.clear();
  // A summarized callee fixes this positional register-argument prefix.
  // Preserve its unknown slots, including trailing ones. Only arguments
  // beyond that required prefix may end at the first unrecovered value.
  const int ReadSlots = SummarizedCallee ? Ops[CallIdx].CalleeRegisterArgs : 0;
  // With floating arguments, which follow the register arguments, the
  // summary fixes the whole register prefix: a slot it does not read
  // carries none.
  const bool ExactRegisters =
      SummarizedCallee && Ops[CallIdx].CalleeVectorArgs > 0;
  size_t End = 0;
  for (int K = 0; K < MaxArgs; ++K) {
    if (ExactRegisters && K >= ReadSlots &&
        K < static_cast<int>(ParamRegs.size()))
      continue;
    if (K >= ReadSlots && (!Found[K] || Found[K]->Kind == ExprKind::Undef ||
                           static_cast<size_t>(K) >= TargetLimit))
      break;
    End = static_cast<size_t>(K) + 1;
  }
  for (size_t K = 0; K < End; ++K) {
    if (ExactRegisters && K >= static_cast<size_t>(ReadSlots) &&
        K < ParamRegs.size())
      continue;
    Args.push_back(Found[K] ? Found[K] : HighExpr::makeUndef(8));
  }
  if (VariadicCallee) {
    const size_t Integers = ParamRegs.size();
    // Then the slots it stores, read as the pass that sized the callee's
    // overflow parameters read them (VariadicStackArguments.h).  A shorter
    // call leaves the rest unset, and va_arg does not read them: no value,
    // which C passes as zero, and leaves off a call of `T f(named, ...)`.
    std::vector<ExprPtr> Stack;
    if (CurMed) {
      indexMedDefinitions();
      for (const detail::StackSlotValue &Slot :
           detail::storedStackArguments(*CurMed, CurBlock, CallIdx, TargetArch,
                                        Format, *EntryStackOffsets)) {
        ExprPtr Value = medvarToExpr(Slot.Value);
        if (Slot.Value.Size > TRI.PointerSize) {
          Value = HighExpr::makeBinop(NdOp::SUBBYTES, Value,
                                      HighExpr::makeConst(Slot.Byte, 4));
          Value->Type = NdType::makeInt(TRI.PointerSize, false);
        }
        Stack.push_back(std::move(Value));
      }
    }
    const bool StoresStackArguments = !Stack.empty();
    const size_t Overflow =
        static_cast<size_t>(std::max(0, VariadicCallee->OverflowCount));
    Stack.resize(std::min(Stack.size(), Overflow));
    while (Stack.size() < Overflow)
      Stack.push_back(HighExpr::makeUndef(TRI.PointerSize));
    // The integer registers the call sets up to the first it leaves unset
    // are arguments, and a named parameter the callee reads is one, though
    // this function only passes its own on.  A register set past that hole
    // is the setup of a later call, which GCC hoists across this one when
    // the callee leaves it alone (-fipa-ra).  Where the call stores a stack
    // argument, every integer register is one, as a scalar goes on the stack
    // only once they are all taken: each may be set on the paths into the
    // call (`lea r8, s; je; mov r8, [x]`), or before an earlier call that
    // leaves it alone.  Where one of them holds nothing the function sets,
    // the store is the caller's own.
    const auto Reaching = [&](size_t K) -> ExprPtr {
      MedVar LiveIn;
      if (!ReachingAtCall(ParamRegs[K], LiveIn, /*ParameterAtEntry=*/true) ||
          LeftByCall(LiveIn))
        return nullptr;
      JoinsSeen.clear();
      return SetInFunction(LiveIn) ? medvarToExpr(LiveIn) : nullptr;
    };
    const int Named = VariadicCallee->NamedIntegerRegisters;
    std::vector<ExprPtr> Registers(Integers);
    for (size_t K = 0; K < Integers; ++K) {
      if (Found[K] && Found[K]->Kind != ExprKind::Undef)
        Registers[K] = Found[K];
      else if (static_cast<int>(K) < Named)
        Registers[K] = Reaching(K);
    }
    size_t Passed = 0;
    while (Passed < Integers && Registers[Passed])
      ++Passed;
    if (StoresStackArguments && Passed < Integers) {
      std::vector<ExprPtr> Every = Registers;
      bool Complete = true;
      for (size_t K = Passed; K < Integers && Complete; ++K)
        if (!Every[K])
          Complete = static_cast<bool>(Every[K] = Reaching(K));
      if (Complete) {
        Registers = std::move(Every);
        Passed = Integers;
      }
    }
    Args.clear();
    for (size_t K = 0; K < Integers; ++K)
      Args.push_back(K < Passed ? std::move(Registers[K])
                                : HighExpr::makeUndef(TRI.PointerSize));
    // A vector register past the count the call loads (AL) holds what
    // earlier code left there, no argument.
    const int VectorCount = Ops[CallIdx].VariadicVectorCount;
    const auto VectorRegs = TRI.floatingParamRegs(Format);
    for (size_t K = 0; K < VectorRegs.size(); ++K) {
      const uint64_t Vector = VectorRegs[K];
      if (VectorCount >= 0 && K >= static_cast<size_t>(VectorCount)) {
        Args.push_back(HighExpr::makeUndef(sizeof(double)));
        continue;
      }
      std::optional<MedVar> Reaching;
      bool AfterCall = false;
      for (size_t J = CallIdx; J-- > 0 && !Reaching;) {
        const MedOp &Prev = Ops[J];
        if (Prev.Opcode == NdOp::CALL || Prev.Opcode == NdOp::INDIR_CALL) {
          AfterCall = true;
          break;
        }
        if (Prev.Output.Kind == MedVar::Reg && Prev.Output.Size > 0 &&
            Prev.Output.RegOff == Vector)
          Reaching = Prev.Output;
      }
      // A parameter this function only passes on, which its entry holds no
      // copy of, enters as that parameter (detectSummaryVectorParams).
      MedVar LiveIn;
      if (!Reaching && CurMed &&
          detail::reachingRegisterAtBlockEntry(
              *CurMed, CurBlock, Vector, getTargetRegInfo(TargetArch), LiveIn,
              /*ParameterAtEntry=*/!AfterCall))
        Reaching = LiveIn;
      ExprPtr Value = Reaching ? medvarToExpr(*Reaching)
                               : HighExpr::makeUndef(sizeof(double));
      if (Value && Value->Type && Value->Type->Size > sizeof(double)) {
        Value = HighExpr::makeBinop(NdOp::SUBBYTES, Value,
                                    HighExpr::makeConst(0, 4));
        Value->Type = NdType::makeInt(sizeof(double), false);
      }
      Args.push_back(std::move(Value));
    }
    Args.insert(Args.end(), Stack.begin(), Stack.end());
    return Args;
  }
  // A summarized callee's floating arguments follow its register arguments,
  // before any on the stack, as its parameters do.
  if (ExactRegisters) {
    const MedOp &Call = Ops[CallIdx];
    const size_t First = 1 + static_cast<size_t>(Call.CalleeRegisterArgs);
    const size_t At =
        std::min(Args.size(), static_cast<size_t>(std::max(0, ReadSlots)));
    std::vector<ExprPtr> Vectors;
    for (int K = 0; K < Call.CalleeVectorArgs && First + K < Call.NumInputs;
         ++K)
      Vectors.push_back(CallInput(Call, static_cast<unsigned>(First + K)));
    Args.insert(Args.begin() + static_cast<std::ptrdiff_t>(At), Vectors.begin(),
                Vectors.end());
  }
  return BoundKnownCalleeArity(std::move(Args));
}

std::optional<MedVar>
MedToHighConverter::untouchedParamRegister(uint64_t RegOff) const {
  if (!CurMed)
    return std::nullopt;
  // Any write to the register, or to one of its byte lanes, and any other
  // call that may clobber it, leaves its value at the call undetermined here.
  size_t Calls = 0;
  auto Writes = [&](const MedVar &V) {
    return V.Kind == MedVar::Reg && V.RegOff >= RegOff && V.RegOff < RegOff + 8;
  };
  for (const auto &Blk : CurMed->Blocks) {
    for (const auto &Phi : Blk.Phis)
      if (Writes(Phi.Output))
        return std::nullopt;
    for (const auto &Op : Blk.Ops) {
      if (Writes(Op.Output))
        return std::nullopt;
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL)
        ++Calls;
    }
  }
  if (Calls != 1)
    return std::nullopt;
  for (size_t I = 0; I < CurMed->Params.size(); ++I)
    if (CurMed->Params[I].RegOff == RegOff) {
      MedVar Param = CurMed->Params[I];
      Param.Kind = MedVar::Param;
      Param.Id = static_cast<int>(I);
      return Param;
    }
  return std::nullopt;
}

bool MedToHighConverter::reachingRegAtBlockEntry(const MedBlock &B,
                                                 uint64_t RegOff,
                                                 MedVar &Out) const {
  return CurMed && detail::reachingRegisterAtBlockEntry(
                       *CurMed, B, RegOff, getTargetRegInfo(TargetArch), Out);
}

bool MedToHighConverter::isCallArgSetupDef(const MedBlock &CallBlk,
                                           const MedVar &LiveIn,
                                           uint64_t RegOff) const {
  if (!CurMed)
    return false;
  for (int PredId : CallBlk.Preds) {
    const MedBlock *Pred = nullptr;
    for (const auto &Blk : CurMed->Blocks)
      if (Blk.Id == PredId) {
        Pred = &Blk;
        break;
      }
    if (!Pred)
      continue;
    for (const auto &Phi : Pred->Phis)
      if (Phi.Output.Id == LiveIn.Id && Phi.Output.SSAVer == LiveIn.SSAVer)
        return Phi.Output.Kind == MedVar::Reg && Phi.Output.RegOff == RegOff &&
               Phi.Output.Size > 0;
    for (const auto &Op : Pred->Ops) {
      if (Op.Output.Id != LiveIn.Id || Op.Output.SSAVer != LiveIn.SSAVer)
        continue;
      if (Op.Opcode == NdOp::COPY && Op.NumInputs >= 1 &&
          Op.Inputs[0].Kind == MedVar::Reg && Op.Inputs[0].RegOff == RegOff &&
          Op.Inputs[0].Id == Op.Output.Id &&
          Op.Inputs[0].SSAVer == Op.Output.SSAVer)
        return false;
      return Op.Output.Kind == MedVar::Reg && Op.Output.RegOff == RegOff &&
             Op.Output.Size > 0;
    }
  }
  // `lea rcx` before `ja` / `test; jz` sits in a dominating ancestor, not
  // the immediate pred. Leftover r9 from an earlier Find is not a lea.
  const MedOp *Unique = uniqueSsaDef(CurMed, LiveIn);
  if (!Unique || !isLeaLikeCallSetup(CurMed, *Unique))
    return false;
  if (Unique->Opcode == NdOp::COPY && Unique->NumInputs >= 1 &&
      Unique->Inputs[0].Kind == MedVar::Reg &&
      Unique->Inputs[0].RegOff == RegOff &&
      Unique->Inputs[0].Id == Unique->Output.Id &&
      Unique->Inputs[0].SSAVer == Unique->Output.SSAVer)
    return false;
  return Unique->Output.Kind == MedVar::Reg &&
         Unique->Output.RegOff == RegOff && Unique->Output.Size > 0;
}

int MedToHighConverter::regToArgIdx(uint64_t RegOff) const {
  return getTargetRegInfo(TargetArch)
      .regToArgIdx(RegOff, Image ? Image->abiFormat() : BinaryFormat::Unknown);
}

std::string MedToHighConverter::calleeDisplayName(va_t Target) const {
  if (ResolvedCalleeNames) {
    auto It = ResolvedCalleeNames->find(Target);
    if (It != ResolvedCalleeNames->end())
      return It->second;
  }
  auto Synthetic = [Target] {
    return (kAutoFuncPrefix + llvm::utohexstr(Target)).str();
  };
  auto Usable = [](llvm::StringRef Name) {
    return !Name.empty() && !Name.starts_with(kAutoFuncPrefix);
  };
  if (FuncNames) {
    auto It = FuncNames->find(Target);
    if (It != FuncNames->end() && Usable(It->second))
      return It->second;
  }
  if (Image) {
    if (const Import *Imp = Image->findImportAt(Target);
        Imp && Usable(Imp->Name))
      return Imp->Name;
    const std::string FromImage = Image->getFunctionNameAt(Target);
    if (Usable(FromImage))
      return FromImage;
  }
  return Synthetic();
}

void MedToHighConverter::resolveCalleeNames(
    const std::set<va_t> &Targets, std::map<va_t, std::string> &Names) const {
  if (!Image) {
    for (va_t Target : Targets)
      Names[Target] = calleeDisplayName(Target);
    return;
  }

  // BinaryImage keeps publicly editable metadata vectors, so build these
  // exact-address indexes for this immutable pipeline stage rather than
  // caching them on the image, and only for the targets: an image can hold
  // a hundred thousand symbols.  They choose what calleeDisplayName does: the
  // *first* import at an address, including an empty name, as findImportAt
  // does, then the name getFunctionNameAt gives the function.  The first
  // symbol at a MinGW function's start is often its object's `.text` section
  // symbol, which that name skips.
  std::map<va_t, const Import *> ImportsByIAT;
  for (const Import &Imp : Image->Imports)
    if (Targets.count(Imp.IATAddr))
      ImportsByIAT.try_emplace(Imp.IATAddr, &Imp);
  std::map<va_t, std::string> ScannedNames;
  if (!ImageFunctionNames)
    ScannedNames = functionNamesAt(*Image, Targets);
  const std::map<va_t, std::string> &ImageNames =
      ImageFunctionNames ? *ImageFunctionNames : ScannedNames;

  auto Usable = [](llvm::StringRef Name) {
    return !Name.empty() && !Name.starts_with(kAutoFuncPrefix);
  };
  for (va_t Target : Targets) {
    if (ResolvedCalleeNames) {
      auto It = ResolvedCalleeNames->find(Target);
      if (It != ResolvedCalleeNames->end()) {
        Names[Target] = It->second;
        continue;
      }
    }
    if (FuncNames) {
      auto It = FuncNames->find(Target);
      if (It != FuncNames->end() && Usable(It->second)) {
        Names[Target] = It->second;
        continue;
      }
    }
    const Import *Imp = nullptr;
    if (auto It = ImportsByIAT.find(Target); It != ImportsByIAT.end())
      Imp = It->second;
    else
      Imp = Image->findImportStubAt(Target);
    if (Imp && Usable(Imp->Name)) {
      Names[Target] = Imp->Name;
      continue;
    }
    // An address no export or symbol names has none of its own.
    const auto FromImage = ImageNames.find(Target);
    Names[Target] = FromImage != ImageNames.end() && Usable(FromImage->second)
                        ? FromImage->second
                        : (kAutoFuncPrefix + llvm::utohexstr(Target)).str();
  }
}

int MedToHighConverter::abiParamIndex(const MedVar &V) const {
  if (!CurMed)
    return V.Kind == MedVar::Param ? V.Id : -1;

  auto SlotByReg = [&](uint64_t RegOff) -> int {
    if (RegOff == kNoParamReg)
      return -1;
    for (size_t I = 0; I < CurMed->Params.size(); ++I)
      if (CurMed->Params[I].RegOff == RegOff)
        return static_cast<int>(I);
    const int Idx = regToArgIdx(RegOff);
    if (Idx >= 0 && static_cast<size_t>(Idx) < CurMed->Params.size())
      return Idx;
    return -1;
  };

  if (V.Kind == MedVar::Param) {
    // Register and stack parameters number their ids apart: ARM32's r0 can
    // carry id 8, the same as the ninth argument's stack slot.  An id of the
    // same kind decides first.
    int IdMatch = -1, AnyIdMatch = -1;
    for (size_t I = 0; I < CurMed->Params.size(); ++I) {
      const MedVar &P = CurMed->Params[I];
      if (P.Id < 0 || P.Id != V.Id)
        continue;
      if (V.RegOff != kNoParamReg && P.RegOff == V.RegOff)
        return static_cast<int>(I);
      if (AnyIdMatch < 0)
        AnyIdMatch = static_cast<int>(I);
      if (IdMatch < 0 && (V.RegOff == kNoParamReg) == (P.RegOff == kNoParamReg))
        IdMatch = static_cast<int>(I);
    }
    if (IdMatch >= 0)
      return IdMatch;
    if (AnyIdMatch >= 0)
      return AnyIdMatch;
    const int ByReg = SlotByReg(V.RegOff);
    if (ByReg >= 0)
      return ByReg;
    if (V.Id >= 0 && static_cast<size_t>(V.Id) < CurMed->Params.size())
      return V.Id;
    return -1;
  }

  if (V.Kind == MedVar::Reg)
    return SlotByReg(V.RegOff);
  return -1;
}

} // namespace neverd
