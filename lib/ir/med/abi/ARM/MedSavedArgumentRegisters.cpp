//===- MedSavedArgumentRegisters.cpp - ARM registers only saved -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The AAPCS argument registers a function only saves.  A save alone supplies
/// no evidence of an argument: MSVC ARM pushes r3 beside its callee-saved
/// registers to keep the stack 8-byte aligned and pops it back unread
/// (`push {r3, r4, r11, lr}` ... `pop {r3, r4, r11, pc}`), reserves words for
/// its locals with `push {r2, r3, ...}` and writes them before it reads them,
/// and its /Od code homes argument registers it never reads back.  Such a
/// register's incoming value reaches nothing the function does, nor do the
/// stores that save it, which go.  A register a call reads is a forwarded
/// argument, which call recovery adds where the callee takes it.  A filter
/// or a termination handler the runtime runs out of line reads its parent's
/// frame too, through the frame registers the unwinder restores, anywhere in
/// its protected range: a byte one reads that no store has overwritten there
/// is read.  Where one may reach the frame some other way, or the function
/// has C++'s catch and unwind funclets or an unknown routine's, no register
/// is only saved.
///
//===----------------------------------------------------------------------===//

#include "../../pass/MedEntryStackOffsets.h"

#include "neverd/Common.h"
#include "neverd/Limits.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/low/SEHLocalUnwindProtocol.h"
#include "neverd/ir/med/MedCallingConvDetail.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/lift/ARM/ARMRegs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/RuntimeProviders.h"
#include "neverd/loader/ExceptionWindowsEH.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace neverd::med_calling_conv_detail {

namespace {
/// Whether \p Op only moves the bytes of its inputs.
bool carries(const MedOp &Op) {
  switch (Op.Opcode) {
  case NdOp::COPY:
  case NdOp::SUBBYTES:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
  case NdOp::CONCAT:
    return true;
  default:
    return false;
  }
}

bool isCall(const MedOp &Op) {
  return Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL;
}

/// What a handler the runtime runs out of line reads of its parent's frame:
/// the bytes, by entry-SP displacement, and the protected range in which it
/// may run.
struct HandlerRead {
  ExceptionAddressRange Range;
  int64_t Begin = 0;
  int64_t End = 0;
};

/// Whether \p Target is a runtime local unwind routine's import, which
/// takes the frame to run the function's own termination handlers.
bool isLocalUnwind(const BinaryImage &Img, va_t Target) {
  const Import *Imported = Img.findImportStubAt(Target);
  return Imported &&
         coff_loader::isX86ContinuationRuntimeProvider(Imported->Module) &&
         isLowSEHLocalUnwindRoutine(stripLeadingUnderscores(Imported->Name));
}

bool callsLocalUnwind(const BinaryImage &Img, const LowOp &Op) {
  return Op.Opcode == NdOp::CALL && Op.NumInputs && Op.Inputs[0].isConst() &&
         isLocalUnwind(Img, static_cast<va_t>(Op.Inputs[0].Offset));
}

/// What the handler the runtime runs out of line at \p Entry, in the
/// protected range \p Range, reads of the frame of \p Parent, appended to
/// \p Reads.  It reaches that frame through the callee-saved registers the
/// unwinder restores to frame addresses (MedFunc::SEHFuncletRegisters), by
/// loads and stores at those registers plus constants, which nothing writes;
/// a register the unwinder restores to what the function was entered with
/// addresses its caller's memory at nonnegative distances, and one restored
/// to a constant any memory but the frame.  False where the handler may
/// reach the frame some other way: a frame address going anywhere but such
/// a load's or store's address or, in an argument register, to a local
/// unwind, which runs the parent's own termination handlers; or a value the
/// runtime or the parent left in a register (the establisher frame, another
/// argument register, a callee-saved register restored to no known value)
/// used for anything but a save on the handler's own stack, read back from
/// there as the same.  A call takes only the arguments the handler places,
/// which no source can make the establisher frame.
bool handlerFrameReads(const MedFunc &Parent, const BinaryImage &Img,
                       const std::map<va_t, const LowFunc *> *Bodies,
                       va_t Entry, const ExceptionAddressRange &Range,
                       std::vector<HandlerRead> &Reads) {
  const TargetRegInfo &TRI = getTargetRegInfo(Img.Arch);
  const uint64_t Word = TRI.PointerSize;
  const auto Base = [&](uint64_t Offset) { return Offset - Offset % Word; };
  const uint64_t SP = Base(TRI.StackPointer);
  // The callee-saved registers the unwinder restores: to a frame address,
  // or to a value addressing what the frame is not.
  enum class Addresses : uint8_t { Caller, Anything };
  std::map<uint64_t, int64_t> Frame;
  std::map<uint64_t, Addresses> Elsewhere;
  if (const auto Found = Parent.SEHFuncletRegisters.find(Entry);
      Found != Parent.SEHFuncletRegisters.end())
    for (const auto &[Register, Value] : Found->second)
      switch (Value.ValueKind) {
      case SEHFuncletRegisterValue::Kind::EntryStackOffset:
        Frame.emplace(Base(Register), Value.Offset);
        break;
      case SEHFuncletRegisterValue::Kind::EntryRegister:
      case SEHFuncletRegisterValue::Kind::EntryArgument:
        Elsewhere.emplace(Base(Register), Addresses::Caller);
        break;
      case SEHFuncletRegisterValue::Kind::Constant:
        Elsewhere.emplace(Base(Register), Addresses::Anything);
        break;
      }
  // What the handler is entered with beside the exception pointers or the
  // abnormal flag in the first argument register: the other argument
  // registers, the scratch register, and the callee-saved registers no
  // frame address is known for.
  std::set<uint64_t> Left;
  const llvm::ArrayRef<uint64_t> Arguments =
      TRI.integerParamRegs(BinaryFormat::COFF);
  for (size_t I = 1; I < Arguments.size(); ++I)
    Left.insert(Base(Arguments[I]));
  Left.insert(Base(armreg::R12));
  for (uint64_t Register = armreg::R4; Register <= armreg::R11;
       Register += Word)
    if (!Frame.count(Base(Register)) && TRI.isCalleeSaveReg(Register))
      Left.insert(Base(Register));

  // The handler's code, and where each part goes: the blocks of the body
  // the pipeline lifted, whose tables it resolved, or else its instructions,
  // each decoded once.
  struct Unit {
    std::vector<LowOp> Ops;
    std::vector<va_t> Succs;
  };
  std::map<va_t, Unit> Units;
  std::vector<va_t> Work;
  const LowFunc *Body = nullptr;
  if (Bodies)
    if (const auto Found = Bodies->find(Entry); Found != Bodies->end())
      Body = Found->second;
  if (Body && !Body->Blocks.empty() &&
      Body->Blocks.front().StartAddr == Entry) {
    const auto StartOf = [&](int Id) -> std::optional<va_t> {
      if (Id < 0 || static_cast<size_t>(Id) >= Body->Blocks.size())
        return std::nullopt;
      return Body->Blocks[Id].StartAddr;
    };
    for (const LowBlock &Block : Body->Blocks) {
      Unit U;
      U.Ops = Block.Ops;
      for (const LowOp &Op : Block.Ops)
        if (Op.Opcode == NdOp::INDIR_BR &&
            llvm::none_of(Body->JumpTables, [&](const JumpTable &Table) {
              return Table.InsnAddr == Op.Addr && !Table.AnalysisOnly;
            }))
          return false;
      for (const int Succ : Block.Succs) {
        const auto Start = StartOf(Succ);
        if (!Start)
          return false;
        U.Succs.push_back(*Start);
      }
      for (const ExceptionalEdge &Edge : Block.ExceptionalSuccs) {
        const auto Start = StartOf(Edge.BlockId);
        if (!Start)
          return false;
        U.Succs.push_back(*Start);
      }
      Units.emplace(Block.StartAddr, std::move(U));
    }
  } else {
    Decoder Dec;
    if (!Dec.init(Img))
      return false;
    Work.push_back(Entry);
    size_t Budget = limits::kMaxSEHEstablisherFrameWork;
    while (!Work.empty()) {
      const va_t At = Work.back();
      Work.pop_back();
      if (Units.count(At))
        continue;
      if (!Budget--)
        return false;
      const Segment *Seg = Img.getSegmentFor(At);
      if (!Seg || At - Seg->VA >= Seg->Data.size() || !Dec.selectMode(Img, At))
        return false;
      const size_t Offset = static_cast<size_t>(At - Seg->VA);
      DecodedExecutionUnit Decoded;
      const int Size = Dec.decodeUnitForLift(
          Seg->Data.data() + Offset, Seg->Data.size() - Offset, At, Decoded);
      if (Size <= 0 || Decoded.HasDelaySlot)
        return false;
      Unit U;
      try {
        Dec.liftUnitToLow(Decoded, U.Ops);
      } catch (const UnliftedInstruction &) {
        return false;
      }
      bool FallsThrough = true;
      for (const LowOp &Op : U.Ops) {
        if (Op.Opcode == NdOp::RETURN) {
          FallsThrough = false;
        } else if (Op.Opcode == NdOp::INDIR_BR) {
          return false;
        } else if (Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR) {
          if (!Op.NumInputs || !Op.Inputs[0].isConst())
            return false;
          U.Succs.push_back(Op.Inputs[0].Offset);
          if (Op.Opcode == NdOp::BRANCH)
            FallsThrough = false;
        }
      }
      if (FallsThrough)
        U.Succs.push_back(At + static_cast<va_t>(Size));
      for (const va_t Succ : U.Succs)
        Work.push_back(Succ);
      Units.emplace(At, std::move(U));
    }
  }
  if (!Units.count(Entry))
    return false;

  // On some way to each instruction: what may hold a value the handler was
  // entered with, in a register or a word of its own stack, and which
  // register may hold a frame address; on every way, which restored register
  // still holds what it was restored to; and where the handler's own stack
  // pointer stands.
  struct State {
    int64_t Stack = 0;
    std::set<uint64_t> Registers;
    std::set<int64_t> Slots;
    std::set<uint64_t> FrameHeld;
    std::set<uint64_t> Restored;
  };
  std::set<uint64_t> Restored;
  for (const auto &[Register, Kind] : Elsewhere)
    Restored.insert(Register);
  std::map<va_t, State> In;
  In.emplace(Entry, State{0, Left, {}, {}, Restored});
  Work.assign(1, Entry);
  while (!Work.empty()) {
    const va_t At = Work.back();
    Work.pop_back();
    State S = In.at(At);
    // Within the unit: temporaries holding a frame address, an address on
    // the handler's own stack, or one a restored register makes elsewhere,
    // with how far it is from that register.
    std::map<uint64_t, int64_t> FrameTemps, StackTemps;
    std::map<uint64_t, std::pair<Addresses, int64_t>> ElsewhereTemps;
    const auto FrameAt = [&](const NdVar &V) -> std::optional<int64_t> {
      if (V.isTemp()) {
        const auto Found = FrameTemps.find(V.Offset);
        return Found == FrameTemps.end() ? std::nullopt
                                         : std::optional(Found->second);
      }
      if (V.isReg() && V.Size == Word)
        if (const auto Found = Frame.find(Base(V.Offset)); Found != Frame.end())
          return Found->second;
      return std::nullopt;
    };
    const auto StackAt = [&](const NdVar &V) -> std::optional<int64_t> {
      if (V.isTemp()) {
        const auto Found = StackTemps.find(V.Offset);
        return Found == StackTemps.end() ? std::nullopt
                                         : std::optional(Found->second);
      }
      if (V.isReg() && Base(V.Offset) == SP)
        return S.Stack;
      return std::nullopt;
    };
    const auto ElsewhereAt =
        [&](const NdVar &V) -> std::optional<std::pair<Addresses, int64_t>> {
      if (V.isTemp()) {
        const auto Found = ElsewhereTemps.find(V.Offset);
        return Found == ElsewhereTemps.end() ? std::nullopt
                                             : std::optional(Found->second);
      }
      if (V.isReg() && V.Size == Word && S.Restored.count(Base(V.Offset)))
        if (const auto Found = Elsewhere.find(Base(V.Offset));
            Found != Elsewhere.end())
          return std::pair(Found->second, int64_t{0});
      return std::nullopt;
    };
    const auto Step = [](const LowOp &Op) -> std::optional<int64_t> {
      if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1)
        return 0;
      if ((Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB) &&
          Op.NumInputs == 2 && Op.Inputs[1].isConst())
        return (Op.Opcode == NdOp::INT_ADD ? 1 : -1) *
               static_cast<int64_t>(Op.Inputs[1].Offset);
      return std::nullopt;
    };
    for (const LowOp &Op : Units.at(At).Ops) {
      const bool Memory = Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE;
      std::optional<int64_t> Slot;
      if (Memory && Op.NumInputs >= 1)
        Slot = StackAt(Op.Inputs[0]);
      const std::optional<int64_t> By = Step(Op);
      bool LeftValue = false;
      bool HoldsFrame = false;
      // What the op's temporary output holds, from its inputs.
      std::optional<int64_t> NewFrame, NewStack;
      std::optional<std::pair<Addresses, int64_t>> NewElsewhere;
      for (unsigned I = 0; I < Op.NumInputs; ++I) {
        const NdVar &Input = Op.Inputs[I];
        if (Input.isConst())
          continue;
        if (const auto Frame = FrameAt(Input)) {
          if (Memory && I == 0) {
            if (Op.Opcode == NdOp::LOAD)
              Reads.push_back({Range, *Frame, *Frame + Op.Output.Size});
            continue;
          }
          if (By && I == 0) {
            if (Op.Output.isTemp())
              NewFrame = *Frame + *By;
            else if (Op.Output.isReg())
              HoldsFrame = true;
            else
              return false;
            continue;
          }
          return false;
        }
        if (const auto Other = ElsewhereAt(Input)) {
          const bool Reach = Other->first == Addresses::Anything;
          if (Memory && I == 0 && (Reach || Other->second >= 0))
            continue;
          if (By && I == 0 && Op.Output.isTemp()) {
            NewElsewhere = std::pair(Other->first, Other->second + *By);
            continue;
          }
        }
        if (Input.isReg() && S.FrameHeld.count(Base(Input.Offset)))
          return false;
        // A value the handler was entered with goes only to its own stack.
        if (Input.isReg() && S.Registers.count(Base(Input.Offset))) {
          if (Op.Opcode == NdOp::STORE && I == 1 && Slot) {
            LeftValue = true;
            continue;
          }
          return false;
        }
      }
      // What the op makes of the stack pointer.
      if (Op.Output.isTemp() && By && Op.NumInputs >= 1)
        if (const auto Stack = StackAt(Op.Inputs[0]))
          NewStack = *Stack + *By;
      if (Op.Output.isTemp()) {
        const uint64_t Out = Op.Output.Offset;
        FrameTemps.erase(Out);
        StackTemps.erase(Out);
        ElsewhereTemps.erase(Out);
        if (NewFrame)
          FrameTemps[Out] = *NewFrame;
        if (NewStack)
          StackTemps[Out] = *NewStack;
        if (NewElsewhere)
          ElsewhereTemps[Out] = *NewElsewhere;
      }
      if (Op.Opcode == NdOp::STORE && Slot) {
        for (int64_t Byte = *Slot; Byte < *Slot + Op.Inputs[1].Size; ++Byte)
          if (LeftValue)
            S.Slots.insert(Byte);
          else
            S.Slots.erase(Byte);
      }
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
        // A frame address goes in an argument register only to a local
        // unwind.
        if (llvm::any_of(
                Arguments,
                [&](uint64_t R) { return S.FrameHeld.count(Base(R)) != 0; }) &&
            !callsLocalUnwind(Img, Op))
          return false;
        for (const uint64_t Argument : Arguments) {
          S.Registers.erase(Base(Argument));
          S.FrameHeld.erase(Base(Argument));
        }
        S.Registers.erase(Base(armreg::R12));
        S.FrameHeld.erase(Base(armreg::R12));
      }
      if (Op.Opcode == NdOp::RETURN &&
          llvm::any_of(S.FrameHeld,
                       [&](uint64_t R) { return TRI.isCalleeSaveReg(R); }))
        return false;
      if (Op.Output.isReg()) {
        const uint64_t Out = Base(Op.Output.Offset);
        if (Frame.count(Out))
          return false;
        S.Restored.erase(Out);
        if (Out == SP) {
          const auto Stack =
              Op.NumInputs ? StackAt(Op.Inputs[0]) : std::optional<int64_t>();
          if (!Stack || !By)
            return false;
          S.Stack = *Stack + *By;
        } else {
          // A load of a word the handler saved there is that value again.
          bool Reloaded = false;
          if (Op.Opcode == NdOp::LOAD && Slot)
            for (int64_t Byte = *Slot; Byte < *Slot + Op.Output.Size; ++Byte)
              Reloaded |= S.Slots.count(Byte) != 0;
          if (Reloaded)
            S.Registers.insert(Out);
          else
            S.Registers.erase(Out);
          if (HoldsFrame)
            S.FrameHeld.insert(Out);
          else
            S.FrameHeld.erase(Out);
        }
      }
    }
    for (const va_t Succ : Units.at(At).Succs) {
      const auto [It, Inserted] = In.try_emplace(Succ, S);
      if (Inserted) {
        Work.push_back(Succ);
        continue;
      }
      State &Next = It->second;
      if (Next.Stack != S.Stack)
        return false;
      const size_t Before = Next.Registers.size() + Next.Slots.size() +
                            Next.FrameHeld.size() + Next.Restored.size();
      Next.Registers.insert(S.Registers.begin(), S.Registers.end());
      Next.Slots.insert(S.Slots.begin(), S.Slots.end());
      Next.FrameHeld.insert(S.FrameHeld.begin(), S.FrameHeld.end());
      for (auto R = Next.Restored.begin(); R != Next.Restored.end();)
        R = S.Restored.count(*R) ? std::next(R) : Next.Restored.erase(R);
      if (Next.Registers.size() + Next.Slots.size() + Next.FrameHeld.size() +
              Next.Restored.size() !=
          Before)
        Work.push_back(Succ);
    }
  }
  return true;
}
} // namespace

namespace {
/// The argument registers \p Func only saves, with the stores that save
/// them in \p Saves.
std::set<uint64_t> onlySaved(const MedFunc &Func, Arch TargetArch,
                             BinaryFormat Format, llvm::ArrayRef<uint64_t> Regs,
                             const BinaryImage *Image,
                             const std::map<va_t, const LowFunc *> *Bodies,
                             std::vector<const MedOp *> &SavesOut) {
  std::set<uint64_t> Result;
  if (Func.Blocks.empty() || Regs.empty())
    return Result;
  // What the handlers the runtime runs out of line read of the frame.
  std::vector<HandlerRead> HandlerReads;
  // Those handlers, whose reads those are.
  std::set<va_t> OwnHandlers;
  if (const auto &EH = Func.ExceptionMetadata) {
    switch (EH->Personality) {
    case ExceptionPersonality::Unknown:
    case ExceptionPersonality::CxxFrameHandler3:
    case ExceptionPersonality::CxxFrameHandler4:
    case ExceptionPersonality::GSHandlerCheckEH:
    case ExceptionPersonality::GSHandlerCheckEH4:
      return Result;
    default:
      break;
    }
    if (EH->Cxx)
      return Result;
    if (EH->SEH)
      for (const SEHScopeRecord &Scope : EH->SEH->Scopes)
        if (Scope.Kind != SEHScopeKind::CatchAll) {
          if (!Image ||
              !handlerFrameReads(Func, *Image, Bodies, Scope.FilterOrFinallyVA,
                                 Scope.GuardedRange, HandlerReads))
            return Result;
          OwnHandlers.insert(Scope.FilterOrFinallyVA);
        }
  }
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  MedEntryStackOffsets Offsets(Func, TargetArch, Format);
  const auto FrameOffset = [&](const MedVar &V) -> std::optional<int64_t> {
    if (V.isConst())
      return std::nullopt;
    return Offsets.query(V);
  };
  const auto InArgumentRegister = [&](const MedVar &V) {
    return V.Kind == MedVar::Reg && llvm::is_contained(Regs, V.RegOff);
  };

  // Where each value is read.
  struct Read {
    const MedOp *Op = nullptr;
    unsigned Input = 0;
    const PhiNode *Phi = nullptr;
  };
  llvm::DenseMap<ValueKey, llvm::SmallVector<Read, 2>> Reads;
  for (const MedBlock &Block : Func.Blocks) {
    for (const PhiNode &Phi : Block.Phis)
      for (const auto &[Pred, Arg] : Phi.Args)
        if (!Arg.isConst())
          Reads[valueKey(Arg)].push_back({nullptr, 0, &Phi});
    for (const MedOp &Op : Block.Ops)
      for (unsigned I = 0; I < Op.NumInputs; ++I)
        if (!Op.Inputs[I].isConst())
          Reads[valueKey(Op.Inputs[I])].push_back({&Op, I, nullptr});
  }

  // Each register whose incoming value every read moves on or stores in
  // the frame, with the slots it goes to and the stores that save it.
  struct Saved {
    uint64_t Register = 0;
    std::vector<std::pair<int64_t, int64_t>> Slots;
    std::vector<const MedOp *> Saves;
  };
  std::vector<Saved> Stored;
  for (const uint64_t Register : Regs) {
    llvm::SmallVector<MedVar, 4> Work;
    for (const MedOp &Op : Func.Blocks.front().Ops) {
      if (Op.Opcode != NdOp::COPY)
        break;
      if (!isEntryLiveInCopy(Op, Register))
        continue;
      Work.push_back(Op.Output);
      if (isRenamedEntryLiveInCopy(Op, Register))
        Work.push_back(Op.Inputs[0]);
    }
    std::vector<std::pair<int64_t, int64_t>> Slots;
    std::vector<const MedOp *> Saves;
    llvm::DenseSet<ValueKey> Seen;
    bool OnlyStored = !Work.empty();
    while (OnlyStored && !Work.empty()) {
      const MedVar Value = Work.pop_back_val();
      if (!Seen.insert(valueKey(Value)).second)
        continue;
      const auto Found = Reads.find(valueKey(Value));
      if (Found == Reads.end())
        continue;
      for (const Read &R : Found->second) {
        if (R.Phi) {
          Work.push_back(R.Phi->Output);
          continue;
        }
        const MedOp &Op = *R.Op;
        if (carries(Op)) {
          Work.push_back(Op.Output);
          continue;
        }
        if (Op.Opcode == NdOp::STORE && R.Input == 1)
          if (const auto Offset = FrameOffset(Op.Inputs[0])) {
            Slots.emplace_back(*Offset, *Offset + Op.Inputs[1].Size);
            Saves.push_back(&Op);
            continue;
          }
        OnlyStored = false;
        break;
      }
    }
    if (OnlyStored && !Slots.empty())
      Stored.push_back({Register, std::move(Slots), std::move(Saves)});
  }
  if (Stored.empty())
    return Result;

  // A frame address may address the function's own loads and stores and
  // make other frame addresses, and nothing else: one stored, passed in an
  // argument register or turned into anything else lets other code read the
  // frame, as a va_list does.  A join nothing reads takes it nowhere, as the
  // frame pointer's at an epilogue that pops the register, and neither does
  // an argument register only a call of one of the function's own handlers
  // takes, whose reads are HandlerReads, or of the runtime's local unwind,
  // which runs them: a `goto` out of a `__finally` hands them the frame.
  const auto RunsOwnHandler = [&](const MedOp &Op) {
    if (Op.Opcode != NdOp::CALL || !Op.NumInputs || !Op.Inputs[0].isConst())
      return false;
    const va_t Target = static_cast<va_t>(Op.Inputs[0].ConstVal);
    return OwnHandlers.count(Target) ||
           (Image && isLocalUnwind(*Image, Target));
  };
  const auto ToALocalUnwind = [&](const MedBlock &Block, size_t At) {
    const MedOp &Def = Block.Ops[At];
    if (const auto Found = Reads.find(valueKey(Def.Output));
        Found != Reads.end())
      for (const Read &R : Found->second)
        if (R.Phi || R.Input == 0 || !RunsOwnHandler(*R.Op))
          return false;
    for (size_t J = At + 1; J < Block.Ops.size(); ++J) {
      const MedOp &Op = Block.Ops[J];
      if (isCall(Op))
        return RunsOwnHandler(Op);
      if (Op.Output.Kind == MedVar::Reg &&
          Op.Output.RegOff == Def.Output.RegOff)
        return true;
    }
    return false;
  };
  for (const MedBlock &Block : Func.Blocks) {
    for (const PhiNode &Phi : Block.Phis)
      for (const auto &[Pred, Arg] : Phi.Args)
        if (FrameOffset(Arg) &&
            (!FrameOffset(Phi.Output) || InArgumentRegister(Phi.Output)) &&
            Reads.count(valueKey(Phi.Output)))
          return Result;
    for (size_t K = 0; K < Block.Ops.size(); ++K) {
      const MedOp &Op = Block.Ops[K];
      for (unsigned I = 0; I < Op.NumInputs; ++I) {
        if (!FrameOffset(Op.Inputs[I]))
          continue;
        if ((Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE) && I == 0)
          continue;
        if ((Op.Opcode == NdOp::COPY || Op.Opcode == NdOp::INT_ADD ||
             Op.Opcode == NdOp::INT_SUB) &&
            FrameOffset(Op.Output) &&
            (!InArgumentRegister(Op.Output) || ToALocalUnwind(Block, K)))
          continue;
        return Result;
      }
    }
  }

  // Whether something takes a byte of \p Value other than the moves that
  // carry it on, as the copy of a `pop`'s load into its register: a register
  // a return hands back is taken, which no return lists yet.
  const auto Taken = [&](const MedVar &Value) {
    llvm::SmallVector<MedVar, 4> Work{Value};
    llvm::DenseSet<ValueKey> Seen;
    while (!Work.empty()) {
      const MedVar V = Work.pop_back_val();
      if (!Seen.insert(valueKey(V)).second)
        continue;
      if (V.Kind == MedVar::Reg && TRI.isReturnReg(V.RegOff))
        return true;
      const auto Found = Reads.find(valueKey(V));
      if (Found == Reads.end())
        continue;
      for (const Read &R : Found->second) {
        if (R.Phi) {
          Work.push_back(R.Phi->Output);
          continue;
        }
        if (!carries(*R.Op))
          return true;
        Work.push_back(R.Op->Output);
      }
    }
    return false;
  };

  // The frame's loads.  One reads nothing where nothing takes its value and
  // its block returns with no call after it, which could take the register
  // it went to as an argument.
  struct FrameLoad {
    int64_t Begin = 0;
    int64_t End = 0;
    bool Unread = false;
  };
  llvm::DenseMap<const MedOp *, size_t> LoadIndex;
  std::vector<FrameLoad> Loads;
  for (const MedBlock &Block : Func.Blocks)
    for (size_t I = 0; I < Block.Ops.size(); ++I) {
      const MedOp &Op = Block.Ops[I];
      if (Op.Opcode != NdOp::LOAD || Op.NumInputs < 1)
        continue;
      const auto Offset = FrameOffset(Op.Inputs[0]);
      if (!Offset)
        continue;
      const bool Unread =
          !Taken(Op.Output) && Block.Ops.back().Opcode == NdOp::RETURN &&
          std::none_of(Block.Ops.begin() + static_cast<std::ptrdiff_t>(I) + 1,
                       Block.Ops.end(), isCall);
      LoadIndex[&Op] = Loads.size();
      Loads.push_back({*Offset, *Offset + Op.Output.Size, Unread});
    }

  // Whether a return may hand back \p Register as the function was entered
  // with it: some way from the entry to a return writes no byte of it, nor
  // calls, as a destructor that returns `this` leaves r0.  That is the
  // return's value until the return contracts settle it, which no return
  // lists yet.
  llvm::DenseMap<int, size_t> IndexOf;
  for (size_t B = 0; B < Func.Blocks.size(); ++B)
    IndexOf[Func.Blocks[B].Id] = B;
  const auto MayReturnIncoming = [&](uint64_t Register) {
    const auto Writes = [&](const MedBlock &Block) {
      return llvm::any_of(Block.Ops, [&](const MedOp &Op) {
        if (isCall(Op))
          return true;
        const MedVar &Out = Op.Output;
        return Out.Kind == MedVar::Reg && Out.Size &&
               Out.RegOff < Register + TRI.FullRegWidth &&
               Register < Out.RegOff + Out.Size &&
               !isEntryLiveInCopy(Op, Out.RegOff);
      });
    };
    std::vector<char> Untouched(Func.Blocks.size(), 0);
    llvm::SmallVector<size_t, 8> Work{0};
    Untouched[0] = 1;
    while (!Work.empty()) {
      const MedBlock &Block = Func.Blocks[Work.pop_back_val()];
      if (Writes(Block))
        continue;
      if (!Block.Ops.empty() && Block.Ops.back().Opcode == NdOp::RETURN)
        return true;
      for (const int Succ : Block.Succs)
        if (const auto Found = IndexOf.find(Succ);
            Found != IndexOf.end() && !Untouched[Found->second]) {
          Untouched[Found->second] = 1;
          Work.push_back(Found->second);
        }
    }
    return false;
  };

  // Whether every load that takes a byte of [Begin, End) reads bytes other
  // stores overwrote on every way to it from \p Save in the entry block, as
  // a slot MSVC reserves with `push {r2, r3, ...}` is a local the function
  // writes before it reads.  A handler the unwinder enters knows what each
  // block it guards had written when it began, as the exception may strike
  // before any of its stores; a block no edge reaches knows of no write.
  const auto OverwrittenBeforeRead = [&](const MedOp *Save, int64_t Begin,
                                         int64_t End) {
    const int64_t Width = End - Begin;
    if (Width <= 0 || Width > 32)
      return false;
    const uint32_t All =
        Width == 32 ? ~uint32_t{0} : (uint32_t{1} << Width) - 1;
    const auto BytesOf = [&](int64_t Lo, int64_t Hi) -> uint32_t {
      Lo = std::max(Lo, Begin);
      Hi = std::min(Hi, End);
      if (Lo >= Hi)
        return 0;
      const int64_t N = Hi - Lo;
      return (N == 32 ? ~uint32_t{0} : (uint32_t{1} << N) - 1) << (Lo - Begin);
    };
    const MedBlock &Entry = Func.Blocks.front();
    const auto SaveAt =
        std::find_if(Entry.Ops.begin(), Entry.Ops.end(),
                     [&](const MedOp &Op) { return &Op == Save; });
    if (SaveAt == Entry.Ops.end())
      return false;
    const size_t First = static_cast<size_t>(SaveAt - Entry.Ops.begin()) + 1;
    // What each block's ops write of the bytes, and whether a taken load
    // reads one before it is written, from what the block enters with, or a
    // handler run out of line may read one where an op of its range runs.
    const auto Walk = [&](size_t B, uint32_t Mask,
                          bool Check) -> std::optional<uint32_t> {
      const MedBlock &Block = Func.Blocks[B];
      for (size_t I = B == 0 ? First : 0; I < Block.Ops.size(); ++I) {
        const MedOp &Op = Block.Ops[I];
        if (Check)
          for (const HandlerRead &Read : HandlerReads) {
            const uint32_t Bytes = BytesOf(Read.Begin, Read.End);
            if ((Mask & Bytes) != Bytes && Read.Range.contains(Op.Addr))
              return std::nullopt;
          }
        if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 2)
          if (const auto Offset = FrameOffset(Op.Inputs[0]))
            Mask |= BytesOf(*Offset, *Offset + Op.Inputs[1].Size);
        if (Check && Op.Opcode == NdOp::LOAD)
          if (const auto Found = LoadIndex.find(&Op);
              Found != LoadIndex.end()) {
            const FrameLoad &Load = Loads[Found->second];
            const uint32_t Read = BytesOf(Load.Begin, Load.End);
            if (!Load.Unread && (Mask & Read) != Read)
              return std::nullopt;
          }
      }
      return Mask;
    };
    std::vector<uint32_t> In(Func.Blocks.size(), All);
    std::vector<char> Reached(Func.Blocks.size(), 0);
    llvm::SmallVector<size_t, 8> Work;
    Reached[0] = 1;
    In[0] = 0;
    Work.push_back(0);
    for (size_t B = 1; B < Func.Blocks.size(); ++B)
      if (Func.Blocks[B].Preds.empty() &&
          Func.Blocks[B].ExceptionalPreds.empty()) {
        Reached[B] = 1;
        In[B] = 0;
        Work.push_back(B);
      }
    const auto Meet = [&](int Id, uint32_t Mask) {
      const auto Found = IndexOf.find(Id);
      if (Found == IndexOf.end())
        return;
      const size_t S = Found->second;
      const uint32_t Next = Reached[S] ? (In[S] & Mask) : Mask;
      if (Reached[S] && Next == In[S])
        return;
      Reached[S] = 1;
      In[S] = Next;
      Work.push_back(S);
    };
    while (!Work.empty()) {
      const size_t B = Work.pop_back_val();
      const uint32_t Out = *Walk(B, In[B], /*Check=*/false);
      for (const int Succ : Func.Blocks[B].Succs)
        Meet(Succ, Out);
      for (const ExceptionalEdge &Edge : Func.Blocks[B].ExceptionalSuccs)
        Meet(Edge.BlockId, In[B]);
    }
    for (size_t B = 0; B < Func.Blocks.size(); ++B)
      if (Reached[B] && !Walk(B, In[B], /*Check=*/true))
        return false;
    return true;
  };

  // A register is only saved where no load that reads anything takes a byte
  // of its slots, or, saved once in the entry block, none takes a byte it
  // has not seen overwritten.
  for (const Saved &S : Stored) {
    if (S.Register == TRI.IntReturnReg && MayReturnIncoming(S.Register))
      continue;
    // A handler run out of line reads a slot of its own.
    const bool HandlerReadsIt =
        llvm::any_of(S.Slots, [&](const std::pair<int64_t, int64_t> &Slot) {
          return llvm::any_of(HandlerReads, [&](const HandlerRead &Read) {
            return Read.Begin < Slot.second && Slot.first < Read.End;
          });
        });
    const bool Unread = llvm::all_of(Loads, [&](const FrameLoad &Load) {
      return Load.Unread ||
             llvm::none_of(
                 S.Slots, [&](const std::pair<int64_t, int64_t> &Slot) {
                   return Load.Begin < Slot.second && Slot.first < Load.End;
                 });
    });
    if ((Unread && !HandlerReadsIt) ||
        (S.Saves.size() == 1 &&
         OverwrittenBeforeRead(S.Saves.front(), S.Slots.front().first,
                               S.Slots.front().second))) {
      Result.insert(S.Register);
      SavesOut.insert(SavesOut.end(), S.Saves.begin(), S.Saves.end());
    }
  }
  return Result;
}
} // namespace

std::set<uint64_t> dropOnlySavedArgumentRegisters(
    MedFunc &Func, Arch TargetArch, BinaryFormat Format,
    llvm::ArrayRef<uint64_t> Regs, const BinaryImage *Image,
    const std::map<va_t, const LowFunc *> *Bodies) {
  std::vector<const MedOp *> Saves;
  std::set<uint64_t> Registers =
      onlySaved(Func, TargetArch, Format, Regs, Image, Bodies, Saves);
  if (Saves.empty())
    return Registers;
  // What the saves store is no value the function or anything after it
  // sees: neither C route writes it.
  const llvm::DenseSet<const MedOp *> Dropped(Saves.begin(), Saves.end());
  for (MedBlock &Block : Func.Blocks)
    llvm::erase_if(Block.Ops,
                   [&](const MedOp &Op) { return Dropped.contains(&Op); });
  // Nor is the value the register came in with, once nothing reads it.
  llvm::DenseSet<ValueKey> Read;
  for (const MedBlock &Block : Func.Blocks) {
    for (const PhiNode &Phi : Block.Phis)
      for (const auto &[Pred, Arg] : Phi.Args)
        if (!Arg.isConst())
          Read.insert(valueKey(Arg));
    for (const MedOp &Op : Block.Ops)
      for (unsigned I = 0; I < Op.NumInputs; ++I)
        if (!Op.Inputs[I].isConst() &&
            !(Op.Opcode == NdOp::COPY && Op.Inputs[I].Kind == Op.Output.Kind &&
              Op.Inputs[I].Id == Op.Output.Id))
          Read.insert(valueKey(Op.Inputs[I]));
  }
  auto &Entry = Func.Blocks.front().Ops;
  llvm::erase_if(Entry, [&](const MedOp &Op) {
    return Op.Opcode == NdOp::COPY &&
           llvm::any_of(Registers,
                        [&](uint64_t R) { return isEntryLiveInCopy(Op, R); }) &&
           !Read.contains(valueKey(Op.Output)) &&
           !Read.contains(valueKey(Op.Inputs[0]));
  });
  return Registers;
}

} // namespace neverd::med_calling_conv_detail
