//===- RegistrationDisplayFrame.cpp - x86 SEH in the decompile view -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The decompile view of an x86 SEH frame defers the native registration
/// proofs, so nothing else says which scope each stretch of code runs under or
/// how control moves between the function and its handlers.  The code does:
///
/// - It stores the try level into its frame before it enters and after it
///   leaves each `__try`, and `_local_unwind4(cookie, frame, stop)` or
///   `_local_unwind2(frame, stop)` leaves the level at `stop`, having run the
///   finally handlers on the way.  A runtime entry to a handler or filter runs
///   at the level enclosing the scope.
/// - A `__finally` is one body inside the function, entered by the runtime at
///   its handler and on the normal path by a `call`.  It returns with a `ret`
///   its own frame stays through; a `ret` after the frame pointer goes back
///   leaves the function, as a `return` in a `__finally` compiles to.  A body
///   only that way never returns to its `call`, and what follows the `call` is
///   then not what runs next.
/// - An `__except` handler starts with `mov esp, [ebp + k]` since the runtime
///   enters it on its own stack.  Where the prologue is the slot's only writer,
///   what comes back is a constant distance from EBP, the one register the
///   runtime restores.
///
//===----------------------------------------------------------------------===//

#include "RegistrationDisplayFrame.h"

#include "RegistrationABIPrivate.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/COFF/COFFSEH4Entry.h"
#include "neverd/loader/ExceptionFunction.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace neverd::x86_registration {

namespace {

using Location = std::pair<VnodeSpace, uint64_t>;

Location locationOf(const NdVar &V) { return {V.Space, V.Offset}; }

int64_t signedValue(const NdVar &V) {
  return V.Size >= 8 ? static_cast<int64_t>(V.Offset)
                     : llvm::SignExtend64(V.Offset, V.Size * 8);
}

/// The value of \p Op's output as a distance from a base some of \p Known
/// holds, through what an address computation goes through.
std::optional<int64_t> distanceOf(const LowOp &Op,
                                  const std::map<Location, int64_t> &Known) {
  auto Read = [&](const NdVar &V) -> std::optional<int64_t> {
    const auto It = Known.find(locationOf(V));
    return It == Known.end() ? std::nullopt
                             : std::optional<int64_t>(It->second);
  };
  switch (Op.Opcode) {
  case NdOp::COPY:
  case NdOp::INT_ZEXT:
    return Op.NumInputs == 1 ? Read(Op.Inputs[0]) : std::nullopt;
  case NdOp::SUBBYTES:
    return Op.NumInputs == 2 && Op.Inputs[1].isConst() && !Op.Inputs[1].Offset
               ? Read(Op.Inputs[0])
               : std::nullopt;
  case NdOp::INT_ADD:
  case NdOp::INT_SUB: {
    if (Op.NumInputs != 2 || !Op.Inputs[1].isConst())
      return std::nullopt;
    const auto Base = Read(Op.Inputs[0]);
    if (!Base)
      return std::nullopt;
    const int64_t Step = signedValue(Op.Inputs[1]);
    return Op.Opcode == NdOp::INT_ADD ? *Base + Step : *Base - Step;
  }
  default:
    return std::nullopt;
  }
}

void note(const LowOp &Op, std::map<Location, int64_t> &Known) {
  if (Op.Output.Size == 0 || (!Op.Output.isReg() && !Op.Output.isTemp()))
    return;
  if (const auto Distance = distanceOf(Op, Known))
    Known[locationOf(Op.Output)] = *Distance;
  else
    Known.erase(locationOf(Op.Output));
}

bool isCall(const LowOp &Op) {
  return Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL;
}

void erase(LowOp &Op) {
  const va_t Address = Op.Addr;
  const int Seq = Op.Seq;
  Op = LowOp{};
  Op.Addr = Address;
  Op.Seq = Seq;
}

constexpr int64_t kRuntimeFrameAboveRecord = 16;
/// Where `__SEH_prolog4` saves the stack pointer, from the frame pointer.
constexpr int64_t kSavedStackSlot = -24;

/// What an instruction changes the try level to.  The instruction goes, so what
/// it runs under does not matter: the level changes where it starts.
struct LevelChange {
  int32_t Level = 0;
  uint8_t Width = sizeof(int32_t);

  /// \p Level after the change, whose narrow form keeps the bytes it leaves.
  int32_t applyTo(int32_t Before) const {
    if (Width >= sizeof(int32_t))
      return Level;
    const uint32_t Mask = (uint32_t(1) << (Width * 8)) - 1;
    return static_cast<int32_t>((static_cast<uint32_t>(Before) & ~Mask) |
                                (static_cast<uint32_t>(Level) & Mask));
  }
};

class DisplayFrame {
public:
  DisplayFrame(LowFunc &Func, const BinaryImage *Image)
      : Func(Func), Image(Image), EH(*Func.ExceptionMetadata),
        Chain(*EH.Registration), SP(NdVar::reg(x86reg::RSP, sizeof(uint32_t))),
        FP(NdVar::reg(x86reg::RBP, sizeof(uint32_t))) {
    for (LowBlock &Block : Func.Blocks) {
      ById.emplace(Block.Id, &Block);
      ByStart.emplace(Block.StartAddr, &Block);
    }
  }

  void run() {
    readSharedPrologue();
    if (!readsLikeItsRuntime())
      return;
    // The loader's bound stops at what looks like padding, as a `__debugbreak`
    // in a handler does; the blocks say how far the code runs.
    for (const LowBlock &Block : Func.Blocks)
      Func.ExceptionMetadata->CodeRange.End =
          std::max(Func.ExceptionMetadata->CodeRange.End, Block.EndAddr);
    popImportArguments();
    restoreStackPointers();
    const std::set<int> RanBefore = runByEntries();
    readFinallyBodies();
    dropWhatRanOnlyPastACall(RanBefore);
    readLevelStores();
    readLocalUnwinds();
    moveLevelChangesToCalls();
    reloadFinallyRegisters();
    if (!readLevels())
      return;
    readOwners();
    writeStretches();
    Func.RegistrationStates->Display = std::move(Facts);
  }

private:
  LowFunc &Func;
  const BinaryImage *Image;
  const ExceptionFunction &EH;
  const RegistrationChainInfo &Chain;
  const NdVar SP;
  const NdVar FP;
  std::map<int, LowBlock *> ById;
  std::map<va_t, LowBlock *> ByStart;
  std::map<va_t, LevelChange> Changes;
  std::map<int, std::set<int32_t>> LevelsIn;
  std::map<int, va_t> Owners;
  std::optional<uint64_t> NextTemporary;
  /// Where each block's stack reload is.
  std::map<int, va_t> ClauseEnds;
  /// Where the function reloads the stack pointer it saved, as it does to go on
  /// from a handler.
  std::set<va_t> StackReloads;
  /// Where `__SEH_prolog4` left the stack pointer, from the frame pointer: what
  /// it saves in the frame for a handler to reload.
  std::optional<int64_t> SharedSave;
  /// The blocks that call `__SEH_epilog4`.
  std::set<int> Epilogues;
  /// What the frame's `__finally` scopes run.
  struct FinallyBody {
    va_t Root = 0;
    std::set<int> Blocks;
    bool Back = false;
    /// The blocks with a `call` of this body, on the normal path.
    std::vector<LowBlock *> Calls;
  };
  std::vector<FinallyBody> Bodies;
  RegistrationDisplayFacts Facts;

  /// The blocks \p First runs through ordinary edges, short of \p Stop.
  /// A temporary no operation names yet.
  uint64_t nextTemporary() {
    if (!NextTemporary) {
      NextTemporary = 0;
      for (const LowBlock &Block : Func.Blocks)
        for (const LowOp &Op : Block.Ops) {
          if (Op.Output.isTemp())
            NextTemporary = std::max(*NextTemporary, Op.Output.Offset + 1);
          for (unsigned K = 0; K < Op.NumInputs; ++K)
            if (Op.Inputs[K].isTemp())
              NextTemporary = std::max(*NextTemporary, Op.Inputs[K].Offset + 1);
        }
    }
    return (*NextTemporary)++;
  }

  std::set<int> reachedFrom(int First, const std::set<int> &Stop = {},
                            const std::map<int, va_t> *Ends = nullptr) const {
    std::set<int> Reached;
    std::vector<int> Work{First};
    while (!Work.empty()) {
      const int Id = Work.back();
      Work.pop_back();
      if (Stop.count(Id) || !Reached.insert(Id).second)
        continue;
      if (Ends && Ends->count(Id))
        continue;
      if (const auto It = ById.find(Id); It != ById.end())
        Work.insert(Work.end(), It->second->Succs.begin(),
                    It->second->Succs.end());
    }
    return Reached;
  }

  /// A stdcall import pops what the caller pushed, which the other way to the
  /// code after a `__try`, through its handler, knows from the saved stack
  /// pointer.  Without this the two disagree where the stack pointer is.
  void popImportArguments() {
    if (!Image)
      return;
    for (LowBlock &Block : Func.Blocks)
      for (size_t I = 0; I < Block.Ops.size(); ++I) {
        const LowOp &Op = Block.Ops[I];
        if (Op.Opcode != NdOp::INDIR_CALL || Op.NumInputs != 1 ||
            !Op.Inputs[0].isConst() || Op.Inputs[0].Size != sizeof(uint32_t))
          continue;
        const auto Pop = registration_abi::checkedRegistrationImportStackPop(
            *Image, Op.Inputs[0].Offset);
        if (!Pop || !*Pop)
          continue;
        LowOp Adjust = Op;
        Adjust.Opcode = NdOp::INT_ADD;
        Adjust.NumInputs = 0;
        Adjust.Output = SP;
        Adjust.addInput(SP);
        Adjust.addInput(NdVar::scalar(*Pop, sizeof(uint32_t)));
        Block.Ops.insert(Block.Ops.begin() + static_cast<ptrdiff_t>(I) + 1,
                         Adjust);
        ++I;
      }
  }

  /// At /O1 and /O2 a frame calls `__SEH_prolog4` to build itself and
  /// `__SEH_epilog4` to take itself down.  Neither returns to code the ordinary
  /// call effects describe, so the two calls become what they do to the frame.
  void readSharedPrologue() {
    if (!Image)
      return;
    const auto Prologue = coff_loader::getX86SEH4PrologueShape(*Image, EH);
    if (!Prologue)
      return;
    for (LowBlock &Block : Func.Blocks)
      for (LowOp &Op : Block.Ops) {
        if (Op.Opcode != NdOp::CALL)
          continue;
        if (Op.Addr == Prologue->CallVA) {
          // ESP at the first push is where EBP goes: the helper leaves the
          // saved EBP there, in the slot of the first argument it took.
          bool FirstPush = true;
          for (LowOp &Before : Block.Ops) {
            if (&Before == &Op)
              break;
            if (Before.Opcode != NdOp::STORE)
              continue;
            erase(Before);
            if (FirstPush) {
              Before.Opcode = NdOp::COPY;
              Before.Output = FP;
              Before.addInput(SP);
            }
            FirstPush = false;
          }
          LowOp Stack = Op;
          Stack.Opcode = NdOp::INT_ADD;
          Stack.NumInputs = 0;
          Stack.Output = SP;
          Stack.addInput(FP);
          Stack.addInput(NdVar::scalar(
              static_cast<uint64_t>(int64_t(Prologue->PostCallStackOffset)),
              sizeof(uint32_t)));
          Op = Stack;
          SharedSave = int64_t(Prologue->PostCallStackOffset);
          continue;
        }
        if (Op.NumInputs != 1 || !Op.Inputs[0].isConst() ||
            !coff_loader::getX86SEH4EpilogueShape(*Image, EH, *Prologue,
                                                  Op.Addr))
          continue;
        // The frame comes down and the `ret` after it leaves the function.
        LowOp Stack = Op;
        Stack.Opcode = NdOp::INT_ADD;
        Stack.NumInputs = 0;
        Stack.Output = SP;
        Stack.addInput(FP);
        Stack.addInput(NdVar::scalar(4, sizeof(uint32_t)));
        Op = Stack;
        Epilogues.insert(Block.Id);
      }
  }

  /// A write of the prologue: where it lands, from the entry stack pointer, and
  /// whether it saves a register the function has not touched.
  struct PrologueStore {
    LowOp *Op;
    int64_t Slot;
    bool SavesRegister;
  };

  /// Distances from the stack pointer at entry after the prologue, up to its
  /// first call.  Installing the record ends a block; what runs straight on is
  /// still the prologue.  \p Builds gets the writes before the record goes into
  /// FS:[0], and \p Installed its address.
  std::map<Location, int64_t>
  walkPrologue(std::map<int64_t, int64_t> &Saved,
               std::vector<PrologueStore> &Builds, va_t &Installed,
               std::optional<int64_t> &InstalledRecord,
               std::optional<int64_t> &InstalledStack) {
    std::map<Location, int64_t> Known{{locationOf(SP), 0}};
    const auto First = ByStart.find(Func.Entry);
    std::set<const LowBlock *> Walked;
    std::set<Location> Written;
    Installed = 0;
    for (LowBlock *Block = First == ByStart.end() ? nullptr : First->second;
         Block && Walked.insert(Block).second;) {
      for (LowOp &Op : Block->Ops) {
        // Straight on from a frame without a call runs into its epilogue.
        if (isCall(Op) || (Op.Output == FP && Known.count(locationOf(FP))))
          return Known;
        if (Op.Opcode == NdOp::STORE && Op.NumInputs == 2) {
          if (!Installed &&
              Op.MemoryAddressSpace != NdMemoryAddressSpace::Default) {
            Installed = Op.Addr;
            if (const auto Record = Known.find(locationOf(Op.Inputs[1]));
                Record != Known.end())
              InstalledRecord = Record->second;
            if (const auto Stack = Known.find(locationOf(SP));
                Stack != Known.end())
              InstalledStack = Stack->second;
            continue;
          }
          const auto Address = Known.find(locationOf(Op.Inputs[0]));
          if (Address == Known.end())
            continue;
          const auto Value = Known.find(locationOf(Op.Inputs[1]));
          if (Value != Known.end())
            Saved[Address->second] = Value->second;
          if (!Installed)
            Builds.push_back({&Op, Address->second,
                              Op.Inputs[1].isReg() &&
                                  !Written.count(locationOf(Op.Inputs[1]))});
          continue;
        }
        if (Op.Output.isReg())
          Written.insert(locationOf(Op.Output));
        note(Op, Known);
      }
      const auto Next = Block->Succs.size() == 1
                            ? ById.find(Block->Succs.front())
                            : ById.end();
      Block = Next == ById.end() ? nullptr : Next->second;
    }
    return Known;
  }

  /// The runtime enters a handler with EBP sixteen bytes above the record.
  /// Where that is not the frame's EBP, as clang-cl has it, or nothing says
  /// where the record is, the handlers' code cannot be read as this function's.
  bool readsLikeItsRuntime() {
    if (SharedSave)
      return true;
    std::map<int64_t, int64_t> Saved;
    std::vector<PrologueStore> Builds;
    va_t Installed = 0;
    std::optional<int64_t> Record;
    std::optional<int64_t> Stack;
    const std::map<Location, int64_t> Prologue =
        walkPrologue(Saved, Builds, Installed, Record, Stack);
    const auto Frame = Prologue.find(locationOf(FP));
    return Frame != Prologue.end() &&
           Record == Frame->second - kRuntimeFrameAboveRecord;
  }

  void restoreStackPointers() {
    std::map<int64_t, int64_t> Saved;
    std::vector<PrologueStore> Builds;
    va_t Installed = 0;
    std::optional<int64_t> InstalledRecord;
    std::optional<int64_t> InstalledStack;
    const std::map<Location, int64_t> Prologue =
        walkPrologue(Saved, Builds, Installed, InstalledRecord, InstalledStack);
    const auto Frame = Prologue.find(locationOf(FP));
    if (Frame == Prologue.end())
      return;
    Facts.FrameFromEntry = static_cast<int32_t>(Frame->second);
    if (SharedSave)
      Saved[Frame->second + kSavedStackSlot] = Frame->second + *SharedSave;
    if (const auto Stack = Saved.find(Frame->second + kSavedStackSlot);
        Stack != Saved.end())
      Facts.StackFromEntry = static_cast<int32_t>(Stack->second);
    else if (InstalledStack)
      Facts.StackFromEntry = static_cast<int32_t>(*InstalledStack);

    struct Slot {
      std::vector<LowOp *> Writes;
      std::vector<LowOp *> Reloads;
      unsigned Reads = 0;
    };
    std::map<int64_t, Slot> Slots;
    for (LowBlock &Block : Func.Blocks) {
      // Distances from the frame pointer, within each instruction.
      std::map<Location, int64_t> Local;
      va_t Instruction = InvalidVA;
      LowOp *Load = nullptr;
      int64_t LoadSlot = 0;
      bool InPrologue = false;
      for (LowOp &Op : Block.Ops) {
        if (Op.Addr != Instruction) {
          Instruction = Op.Addr;
          Local = {{locationOf(FP), 0}};
          Load = nullptr;
        }
        if (Op.Opcode == NdOp::STORE && Op.NumInputs == 2) {
          if (const auto It = Local.find(locationOf(Op.Inputs[0]));
              It != Local.end()) {
            Slots[It->second].Writes.push_back(&Op);
            Facts.FrameWrites[static_cast<int32_t>(It->second)].push_back(
                Op.Addr);
          }
          continue;
        }
        if (Op.Opcode == NdOp::LOAD && Op.NumInputs == 1) {
          if (const auto It = Local.find(locationOf(Op.Inputs[0]));
              It != Local.end()) {
            Load = &Op;
            LoadSlot = It->second;
            Facts.FrameReads[static_cast<int32_t>(LoadSlot)].push_back(Op.Addr);
            // The prologue's own reads, such as encoding the table pointer.
            if (Op.Addr < Func.Entry || Op.Addr >= Installed)
              ++Slots[LoadSlot].Reads;
            InPrologue = Op.Addr >= Func.Entry && Op.Addr < Installed;
          }
        } else if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1 && Load &&
                   Op.Output == SP && Op.Inputs[0] == Load->Output) {
          Slot &Reloaded = Slots[LoadSlot];
          Reloaded.Reloads.push_back(Load);
          if (!InPrologue)
            --Reloaded.Reads;
        }
        note(Op, Local);
        // `lea r32, [ebp + k]`: what the register points at can change.
        if (Op.Output.isReg() && Op.Output != FP && Op.Output != SP)
          if (const auto It = Local.find(locationOf(Op.Output));
              It != Local.end())
            Facts.FrameAddressesTaken.insert(static_cast<int32_t>(It->second));
      }
    }
    for (auto &[Offset, Slot] : Slots) {
      const auto Value = Saved.find(Frame->second + Offset);
      // The function saves it again where a `setjmp` could come back, with its
      // pushes popped: the same stack pointer.
      if (Value == Saved.end() ||
          !llvm::all_of(Slot.Writes, [&](const LowOp *Write) {
            return Write->Inputs[1] == SP;
          }))
        continue;
      for (LowOp *Load : Slot.Reloads) {
        StackReloads.insert(Load->Addr);
        LowOp Reload = *Load;
        Reload.Opcode = NdOp::INT_ADD;
        Reload.NumInputs = 0;
        Reload.addInput(FP);
        Reload.addInput(
            NdVar::scalar(static_cast<uint64_t>(Value->second - Frame->second),
                          sizeof(uint32_t)));
        *Load = Reload;
      }
      // Nothing else read the save, nor does the runtime.
      if (!Slot.Reads)
        for (LowOp *Write : Slot.Writes)
          erase(*Write);
    }
    // What only builds the frame goes: the record, the table pointer's
    // encoding, the cookie the runtime alone reads, and the saved registers,
    // whose restores then write what nothing reads.  A slot the function reads
    // back stays, as a /GS cookie does, except the link the epilogue puts back
    // into FS:[0].
    if (!Installed)
      return;
    const int64_t Link = Frame->second - kRuntimeFrameAboveRecord;
    for (const PrologueStore &Store : Builds) {
      const auto Slot = Slots.find(Store.Slot - Frame->second);
      if (Store.SavesRegister || Store.Slot == Link || Slot == Slots.end() ||
          !Slot->second.Reads)
        erase(*Store.Op);
    }
  }

  /// Whether \p Block's `ret` leaves the function: the frame pointer goes back
  /// first.
  bool leavesFunction(const LowBlock &Block) const {
    return Epilogues.count(Block.Id) ||
           llvm::any_of(Block.Ops,
                        [&](const LowOp &Op) { return Op.Output == FP; });
  }

  void readFinallyBodies() {
    std::map<va_t, const RegistrationScopeRecord *> Roots;
    for (const RegistrationScopeRecord &Scope : Chain.Scopes)
      if (Scope.IsFinally)
        Roots.emplace(Scope.HandlerVA, &Scope);
    for (const auto &[RootVA, Scope] : Roots) {
      const auto Root = ByStart.find(RootVA);
      if (Root == ByStart.end())
        continue;
      FinallyBody Body;
      Body.Root = RootVA;
      Body.Blocks = reachedFrom(Root->second->Id);
      for (int Id : Body.Blocks) {
        const LowBlock &Block = *ById.at(Id);
        if (leavesFunction(Block))
          continue;
        for (const LowOp &Op : Block.Ops)
          if (Op.Opcode == NdOp::RETURN) {
            Facts.FinallyReturns.insert(Op.Addr);
            Body.Back = true;
          }
      }
      Bodies.push_back(std::move(Body));
    }
    auto Within = [&](const FinallyBody &Body, va_t Address) {
      return llvm::any_of(Body.Blocks, [&](int Id) {
        const LowBlock &Block = *ById.at(Id);
        return Address >= Block.StartAddr && Address < Block.EndAddr;
      });
    };
    for (LowBlock &Block : Func.Blocks)
      for (size_t I = 0; I < Block.Ops.size(); ++I) {
        const LowOp &Op = Block.Ops[I];
        if (Op.Opcode != NdOp::CALL || Op.NumInputs != 1 ||
            !Op.Inputs[0].isConst())
          continue;
        const auto Called = llvm::find_if(Bodies, [&](const FinallyBody &Body) {
          return Within(Body, Op.Inputs[0].Offset);
        });
        if (Called == Bodies.end())
          continue;
        Facts.NoValueCalls.insert(Op.Addr);
        Called->Calls.push_back(&Block);
        if (Called->Back)
          continue;
        // Nothing runs after the call.  The instructions the compiler left
        // there, such as a jump to where the source would go on, are not code.
        for (size_t J = I + 1; J < Block.Ops.size(); ++J)
          erase(Block.Ops[J]);
        for (int Succ : Block.Succs)
          if (const auto It = ById.find(Succ); It != ById.end())
            std::erase(It->second->Preds, Block.Id);
        Block.Succs.clear();
        break;
      }
  }

  /// The frame word \p Register holds whenever a `ret` in \p Block runs, from
  /// what runs before it in \p Body: the word it last loaded or stored.
  std::optional<int64_t>
  wordInRegister(const FinallyBody &Body, const LowBlock &Block,
                 const NdVar &Reg,
                 std::map<int, std::optional<int64_t>> &Seen) const {
    // A way in that loops says nothing; a join asks twice for one answer.
    if (const auto Known = Seen.find(Block.Id); Known != Seen.end())
      return Known->second;
    Seen[Block.Id] = std::nullopt;
    std::optional<int64_t> Word;
    bool Written = false;
    std::map<Location, int64_t> Local;
    va_t Instruction = InvalidVA;
    // Forward, keeping the last answer: values in this block overwrite it.
    std::map<Location, int64_t> Loaded;
    for (const LowOp &Op : Block.Ops) {
      if (Op.Addr != Instruction) {
        Instruction = Op.Addr;
        Local = {{locationOf(FP), 0}};
      }
      if (Op.Opcode == NdOp::STORE && Op.NumInputs == 2) {
        const auto Slot = Local.find(locationOf(Op.Inputs[0]));
        if (Slot == Local.end())
          continue;
        if (locationOf(Op.Inputs[1]) == locationOf(Reg) &&
            Op.Inputs[1].Size == sizeof(uint32_t)) {
          Word = Slot->second;
          Written = true;
        } else if (Word == Slot->second) {
          Word.reset();
        }
        continue;
      }
      if (Op.Opcode == NdOp::LOAD && Op.NumInputs == 1) {
        const auto Slot = Local.find(locationOf(Op.Inputs[0]));
        if (locationOf(Op.Output) == locationOf(Reg)) {
          Written = true;
          Word = Slot != Local.end() ? std::optional<int64_t>(Slot->second)
                                     : std::nullopt;
        } else if (Slot != Local.end()) {
          Loaded[locationOf(Op.Output)] = Slot->second;
        } else {
          Loaded.erase(locationOf(Op.Output));
        }
      } else if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
                 locationOf(Op.Output) == locationOf(Reg)) {
        const auto From = Loaded.find(locationOf(Op.Inputs[0]));
        Written = true;
        if (From != Loaded.end())
          Word = From->second;
        else
          Word.reset();
        note(Op, Local);
        continue;
      } else if (locationOf(Op.Output) == locationOf(Reg)) {
        Written = true;
        Word.reset();
      }
      if (isCall(Op)) {
        Word.reset();
        Written = true;
      }
      note(Op, Local);
    }
    if (Written)
      return Seen[Block.Id] = Word;
    // The value came in: every way in has to say the same.
    if (Block.Preds.empty())
      return std::nullopt;
    std::optional<int64_t> Agreed;
    for (int Pred : Block.Preds) {
      if (!Body.Blocks.count(Pred))
        return std::nullopt;
      const auto Up = wordInRegister(Body, *ById.at(Pred), Reg, Seen);
      if (!Up || (Agreed && Agreed != Up))
        return std::nullopt;
      Agreed = Up;
    }
    return Seen[Block.Id] = Agreed;
  }

  /// What ordinary flow runs from the function's entry and each runtime entry.
  std::set<int> runByEntries() const {
    std::set<int> Ran;
    auto Run = [&](va_t Entry) {
      if (const auto It = ByStart.find(Entry); It != ByStart.end()) {
        const std::set<int> From = reachedFrom(It->second->Id);
        Ran.insert(From.begin(), From.end());
      }
    };
    Run(Func.Entry);
    for (const RegistrationScopeRecord &Scope : Chain.Scopes) {
      Run(Scope.HandlerVA);
      Run(Scope.FilterVA);
    }
    return Ran;
  }

  /// Code only the path past the `call` of a body that never comes back ran
  /// does not run.  Left in, it would join what does as a path that knows
  /// nothing.
  void dropWhatRanOnlyPastACall(const std::set<int> &RanBefore) {
    const std::set<int> Ran = runByEntries();
    for (const int Id : RanBefore) {
      if (Ran.count(Id))
        continue;
      LowBlock &Block = *ById.at(Id);
      for (LowOp &Op : Block.Ops)
        erase(Op);
      for (int Succ : Block.Succs)
        if (const auto It = ById.find(Succ); It != ById.end())
          std::erase(It->second->Preds, Id);
      Block.Succs.clear();
    }
  }

  /// Whether the code at \p Block reads \p Reg before it writes it, as far as
  /// straight-line flow goes.
  bool readsBeforeWriting(const LowBlock *Block, const NdVar &Reg) const {
    std::set<const LowBlock *> Walked;
    while (Block && Walked.insert(Block).second) {
      for (const LowOp &Op : Block->Ops) {
        // A body going back to its own `call`: that path may read it.  Leaving
        // the function is no read; whether a caller reads the register is the
        // return contract's question.
        if (Op.Opcode == NdOp::RETURN)
          return Facts.FinallyReturns.count(Op.Addr) != 0;
        for (uint8_t I = 0; I < Op.NumInputs; ++I)
          if (locationOf(Op.Inputs[I]) == locationOf(Reg))
            return true;
        if (locationOf(Op.Output) == locationOf(Reg))
          return false;
        if (isCall(Op))
          return false;
      }
      const auto Next = Block->Succs.size() == 1
                            ? ById.find(Block->Succs.front())
                            : ById.end();
      Block = Next == ById.end() ? nullptr : Next->second;
    }
    return false;
  }

  /// What runs between leaving a `__try` and the `call` of its body cannot
  /// raise and runs before the body, as the abnormal flag's clearing does.  C
  /// says that at the end of the `__try`, so the level changes at the call.
  void moveLevelChangesToCalls() {
    for (const FinallyBody &Body : Bodies)
      for (const LowBlock *Site : Body.Calls) {
        const auto Call = llvm::find_if(Site->Ops, [&](const LowOp &Op) {
          return Op.Opcode == NdOp::CALL && Facts.NoValueCalls.count(Op.Addr);
        });
        if (Call == Site->Ops.end())
          continue;
        const va_t CallAt = Call->Addr;
        auto ChangeIn = [&](const LowBlock &Block, va_t End) {
          auto It = Changes.lower_bound(End);
          if (It == Changes.begin())
            return Changes.end();
          --It;
          return It->first >= Block.StartAddr ? It : Changes.end();
        };
        auto Change = ChangeIn(*Site, CallAt);
        if (Change == Changes.end()) {
          // A level store ends its block.
          if (Site->Preds.size() != 1 ||
              !isQuiet(*Site, Site->StartAddr, CallAt))
            continue;
          const LowBlock &Pred = *ById.at(Site->Preds.front());
          Change = ChangeIn(Pred, Pred.EndAddr);
          if (Change == Changes.end() ||
              !isQuiet(Pred, Change->first, Pred.EndAddr))
            continue;
        } else if (!isQuiet(*Site, Change->first, CallAt)) {
          continue;
        }
        const LevelChange Moved = Change->second;
        Changes.erase(Change);
        Changes[CallAt] = Moved;
      }
  }

  /// An optimized normal path goes on using what a `__finally` body leaves in a
  /// register, as `add $3, %ecx` after `call body`, where the body wrote the
  /// register to a frame word as it ended.  C says that by reading the word
  /// back once the clause has run.
  void reloadFinallyRegisters() {
    // A body nested in a body passes the register on: the outer one reads the
    // inner's reload, so inner calls come first, whatever order the table has.
    for (size_t Pass = 0; Pass < Bodies.size(); ++Pass)
      if (!reloadFinallyRegistersOnce())
        break;
  }

  bool reloadFinallyRegistersOnce() {
    bool Changed = false;
    const std::array<uint64_t, 3> Scratch = {x86reg::RAX, x86reg::RCX,
                                             x86reg::RDX};
    for (const FinallyBody &Body : Bodies) {
      if (!Body.Back)
        continue;
      for (LowBlock *Site : Body.Calls) {
        const auto CallAt = llvm::find_if(Site->Ops, [&](const LowOp &Op) {
          return Op.Opcode == NdOp::CALL && Op.NumInputs == 1 &&
                 Op.Inputs[0].isConst() &&
                 llvm::any_of(Body.Blocks, [&](int Id) {
                   const LowBlock &B = *ById.at(Id);
                   return Op.Inputs[0].Offset >= B.StartAddr &&
                          Op.Inputs[0].Offset < B.EndAddr;
                 });
        });
        if (CallAt == Site->Ops.end())
          continue;
        const size_t Index = CallAt - Site->Ops.begin();
        const LowOp Call = *CallAt;
        std::vector<LowOp> Reloads;
        for (const uint64_t Offset : Scratch) {
          const NdVar Reg = NdVar::reg(Offset, sizeof(uint32_t));
          // After the call, in this block or the next.
          bool Read = false;
          bool Decided = false;
          for (size_t I = Index + 1; I < Site->Ops.size() && !Decided; ++I) {
            const LowOp &Op = Site->Ops[I];
            for (uint8_t K = 0; K < Op.NumInputs; ++K)
              if (locationOf(Op.Inputs[K]) == locationOf(Reg))
                Read = Decided = true;
            if (!Decided && locationOf(Op.Output) == locationOf(Reg))
              Decided = true;
          }
          if (!Decided && Site->Succs.size() == 1)
            Read = readsBeforeWriting(ById.at(Site->Succs.front()), Reg);
          if (!Read)
            continue;
          std::optional<int64_t> Word;
          bool Agreed = true;
          for (int Id : Body.Blocks) {
            const LowBlock &Block = *ById.at(Id);
            if (leavesFunction(Block) ||
                llvm::none_of(Block.Ops, [](const LowOp &Op) {
                  return Op.Opcode == NdOp::RETURN;
                }))
              continue;
            std::map<int, std::optional<int64_t>> Seen;
            const auto Here = wordInRegister(Body, Block, Reg, Seen);
            if (!Here || (Word && Word != Here))
              Agreed = false;
            Word = Here;
          }
          if (!Agreed || !Word)
            continue;
          // `t = ebp + word; r = load t`, at the call.
          const NdVar Address = NdVar::tmp(nextTemporary(), sizeof(uint32_t));
          LowOp Add = Call;
          Add.Opcode = NdOp::INT_ADD;
          Add.NumInputs = 0;
          Add.Output = Address;
          Add.addInput(FP);
          Add.addInput(
              NdVar::scalar(static_cast<uint64_t>(*Word), sizeof(uint32_t)));
          LowOp Load = Call;
          Load.Opcode = NdOp::LOAD;
          Load.NumInputs = 0;
          Load.Output = Reg;
          Load.addInput(Address);
          Reloads.push_back(Add);
          Reloads.push_back(Load);
        }
        if (Reloads.empty())
          continue;
        Changed = true;
        Site->Ops.insert(Site->Ops.begin() + static_cast<ptrdiff_t>(Index) + 1,
                         Reloads.begin(), Reloads.end());
        for (LowInstructionBoundary &Boundary : Site->InstructionBoundaries) {
          if (Boundary.FirstOp > Index)
            Boundary.FirstOp += Reloads.size();
          else if (Index - Boundary.FirstOp < Boundary.OpCount)
            Boundary.OpCount += Reloads.size();
        }
      }
    }
    return Changed;
  }

  /// The stores into the try-level slot: those the loader's byte scan named,
  /// and any the scan's bound left out, by what the instructions write.
  void readLevelStores() {
    std::map<va_t, const RegistrationTryLevelStore *> Scanned;
    for (const RegistrationTryLevelStore &Store : Chain.TryLevelStores)
      Scanned.emplace(Store.StoreVA, &Store);
    const int64_t LevelSlot = Chain.TryLevelOffset.value_or(-4);
    for (LowBlock &Block : Func.Blocks) {
      std::map<Location, int64_t> Frame{{locationOf(FP), 0}};
      std::map<Location, int64_t> Constants;
      for (LowOp &Op : Block.Ops) {
        if (Op.Opcode != NdOp::STORE || Op.NumInputs != 2) {
          note(Op, Frame);
          if (Op.Output.Size && (Op.Output.isReg() || Op.Output.isTemp())) {
            // `and dword ptr [ebp - 4], 0`, `or dword ptr [ebp - 4], -1` and
            // `xor reg, reg` before a store of the register.
            std::optional<int64_t> Value;
            if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1) {
              if (Op.Inputs[0].isConst())
                Value = signedValue(Op.Inputs[0]);
              else if (const auto It = Constants.find(locationOf(Op.Inputs[0]));
                       It != Constants.end())
                Value = It->second;
            } else if (Op.NumInputs == 2 && Op.Inputs[1].isConst()) {
              const int64_t Right = signedValue(Op.Inputs[1]);
              if ((Op.Opcode == NdOp::INT_AND && Right == 0) ||
                  (Op.Opcode == NdOp::INT_OR && Right == -1))
                Value = Right;
            } else if (Op.Opcode == NdOp::INT_XOR && Op.NumInputs == 2 &&
                       Op.Inputs[0] == Op.Inputs[1]) {
              Value = 0;
            }
            if (Value)
              Constants[locationOf(Op.Output)] = *Value;
            else
              Constants.erase(locationOf(Op.Output));
          }
          continue;
        }
        const auto It = Scanned.find(Op.Addr);
        if (It != Scanned.end()) {
          Changes[Op.Addr] = {It->second->Level,
                              static_cast<uint8_t>(It->second->Width)};
          erase(Op);
          continue;
        }
        const auto Slot = Frame.find(locationOf(Op.Inputs[0]));
        if (Slot == Frame.end() || Slot->second != LevelSlot ||
            Op.Inputs[1].Size != sizeof(int32_t))
          continue;
        std::optional<int64_t> Value;
        if (Op.Inputs[1].isConst())
          Value = signedValue(Op.Inputs[1]);
        else if (const auto Known = Constants.find(locationOf(Op.Inputs[1]));
                 Known != Constants.end())
          Value = Known->second;
        if (!Value)
          continue;
        Changes[Op.Addr] = {static_cast<int32_t>(*Value), sizeof(int32_t)};
        erase(Op);
      }
    }
  }

  void readLocalUnwinds() {
    if (!Image)
      return;
    for (LowBlock &Block : Func.Blocks) {
      // The stack pointer can come from the frame pointer at a handler's start.
      // Distances from either base compare only within it.
      constexpr int64_t kFrameBase = int64_t(1) << 40;
      std::map<Location, int64_t> Known{{locationOf(SP), 0},
                                        {locationOf(FP), kFrameBase}};
      std::vector<std::pair<LowOp *, int64_t>> Pushes;
      for (LowOp &Op : Block.Ops) {
        if (Op.Opcode == NdOp::STORE && Op.NumInputs == 2) {
          if (const auto It = Known.find(locationOf(Op.Inputs[0]));
              It != Known.end())
            Pushes.emplace_back(&Op, It->second);
          continue;
        }
        if (isCall(Op) && Op.NumInputs == 1 && Op.Inputs[0].isConst() &&
            Op.Inputs[0].Size == sizeof(uint32_t))
          if (const auto Contract = getCheckedX86LocalUnwindContract(
                  *Image, Op.Inputs[0].Offset, Op.Opcode == NdOp::INDIR_CALL))
            readLocalUnwind(Block, Op, *Contract, Known, Pushes);
        if (isCall(Op))
          Pushes.clear();
        note(Op, Known);
      }
    }
  }

  /// The constant \p Push stores, which /O2 can keep in a register: `mov esi,
  /// -2` at the top of a function that pushes esi at each local unwind.
  std::optional<int32_t> constantPushed(const LowBlock &Block,
                                        const LowOp &Push) const {
    NdVar Value = Push.Inputs[1];
    const LowBlock *At = &Block;
    va_t Before = Push.Addr;
    for (size_t Step = 0; Step < ById.size(); ++Step) {
      if (Value.isConst())
        return static_cast<int32_t>(Value.Offset);
      if (!Value.isReg())
        return std::nullopt;
      const LowOp *Written = nullptr;
      for (const LowOp &Op : At->Ops) {
        if (Op.Addr >= Before)
          break;
        if (Op.Output == Value)
          Written = &Op;
      }
      if (Written) {
        // `xor r, r` zeroes; a plain move goes on from what it copies.
        if (Written->Opcode == NdOp::INT_XOR && Written->NumInputs == 2 &&
            Written->Inputs[0] == Written->Inputs[1])
          return 0;
        if (Written->Opcode != NdOp::COPY || Written->NumInputs != 1)
          return std::nullopt;
        Before = Written->Addr;
        Value = Written->Inputs[0];
        continue;
      }
      if (At->Preds.size() != 1)
        return std::nullopt;
      const LowBlock &Pred = *ById.at(At->Preds.front());
      // `cmp eax, 1; jne past; push eax`: the branch says what it holds.
      if (const auto Known = comparedConstant(Pred, *At, Value))
        return Known;
      At = &Pred;
      Before = At->EndAddr;
    }
    return std::nullopt;
  }

  /// What \p Value equals on the edge from \p Pred to \p Next, when the branch
  /// that ends \p Pred compares it with a constant and takes that edge if
  /// equal.
  std::optional<int32_t> comparedConstant(const LowBlock &Pred,
                                          const LowBlock &Next,
                                          const NdVar &Value) const {
    const auto DefOf = [&](const NdVar &V) -> const LowOp * {
      for (auto Op = Pred.Ops.rbegin(); Op != Pred.Ops.rend(); ++Op)
        if (Op->Output == V)
          return &*Op;
      return nullptr;
    };
    const auto Branch =
        llvm::find_if(llvm::reverse(Pred.Ops), [](const LowOp &Op) {
          return Op.Opcode == NdOp::COND_BR;
        });
    if (Branch == llvm::reverse(Pred.Ops).end() || Branch->NumInputs != 2 ||
        !Branch->Inputs[0].isConst())
      return std::nullopt;
    bool Holds = Branch->Inputs[0].Offset == Next.StartAddr;
    NdVar Condition = Branch->Inputs[1];
    for (size_t Step = 0; Step < Pred.Ops.size(); ++Step) {
      const LowOp *Def = DefOf(Condition);
      if (!Def)
        return std::nullopt;
      if (Def->Opcode == NdOp::BOOL_NOT && Def->NumInputs == 1) {
        Holds = !Holds;
        Condition = Def->Inputs[0];
        continue;
      }
      if (Def->Opcode == NdOp::COPY && Def->NumInputs == 1) {
        Condition = Def->Inputs[0];
        continue;
      }
      if ((Def->Opcode != NdOp::INT_EQUAL &&
           Def->Opcode != NdOp::INT_NOTEQUAL) ||
          Def->NumInputs != 2 || !Def->Inputs[1].isConst() ||
          (Def->Opcode == NdOp::INT_EQUAL) != Holds)
        return std::nullopt;
      const auto Compared = static_cast<int32_t>(Def->Inputs[1].Offset);
      if (Def->Inputs[0] == Value)
        return Compared;
      // `cmp` compares by subtracting.
      const LowOp *Difference = DefOf(Def->Inputs[0]);
      if (Difference && Difference->Opcode == NdOp::INT_SUB && !Compared &&
          Difference->NumInputs == 2 && Difference->Inputs[0] == Value &&
          Difference->Inputs[1].isConst())
        return static_cast<int32_t>(Difference->Inputs[1].Offset);
      return std::nullopt;
    }
    return std::nullopt;
  }

  void readLocalUnwind(const LowBlock &Block, const LowOp &Call,
                       const RegistrationLocalUnwindContract &Contract,
                       const std::map<Location, int64_t> &Known,
                       const std::vector<std::pair<LowOp *, int64_t>> &Pushes) {
    // `_local_unwind4(cookie, frame, stop)` or `_local_unwind2(frame, stop)`.
    const size_t Arguments =
        Contract.Protocol == RegistrationLocalUnwindContract::Kind::EH4 ? 3 : 2;
    if (Pushes.size() < Arguments)
      return;
    const auto Run = llvm::ArrayRef(Pushes).take_back(Arguments);
    const auto CallStack = Known.find(locationOf(SP));
    if (CallStack == Known.end())
      return;
    for (size_t I = 0; I < Arguments; ++I)
      if (Run[I].second !=
          CallStack->second + static_cast<int64_t>(4 * (Arguments - 1 - I)))
        return;
    const auto Level = constantPushed(Block, *Run.front().first);
    if (!Level)
      return;
    // The call itself runs at the level it leaves.
    va_t After = Block.EndAddr;
    for (const LowOp &Op : Block.Ops)
      if (Op.Addr > Call.Addr) {
        After = Op.Addr;
        break;
      }
    Changes[After] = {*Level, sizeof(int32_t)};
    Facts.LocalUnwinds.insert(Call.Addr);
    Facts.NoValueCalls.insert(Call.Addr);
    for (const auto &Push : Run)
      erase(*Push.first);
  }

  /// The try levels each block starts at, from entry and from each runtime
  /// entry.
  bool readLevels() {
    const auto Entry = ByStart.find(Func.Entry);
    if (Entry == ByStart.end())
      return false;
    std::vector<int> Work;
    auto Reach = [&](int Block, int32_t Level) {
      if (LevelsIn[Block].insert(Level).second)
        Work.push_back(Block);
    };
    Reach(Entry->second->Id, Chain.SeededTryLevel.value_or(-1));
    for (const RegistrationScopeRecord &Scope : Chain.Scopes)
      for (const va_t Root : {Scope.HandlerVA, Scope.FilterVA})
        if (const auto It = ByStart.find(Root); Root && It != ByStart.end())
          Reach(It->second->Id, Scope.EnclosingLevel);
    size_t Budget = limits::kMaxRegistrationEHStateWork;
    while (!Work.empty()) {
      const int Id = Work.back();
      Work.pop_back();
      const LowBlock &Block = *ById.at(Id);
      const std::set<int32_t> In = LevelsIn[Id];
      if (Budget < In.size())
        return false;
      Budget -= In.size();
      for (int32_t Level : In) {
        for (auto It = Changes.lower_bound(Block.StartAddr);
             It != Changes.end() && It->first < Block.EndAddr; ++It)
          Level = It->second.applyTo(Level);
        for (int Succ : Block.Succs)
          Reach(Succ, Level);
      }
    }
    return true;
  }

  /// For each block no ordinary path runs, the handler or filter only whose
  /// runtime entry does.
  void readOwners() {
    const auto Entry = ByStart.find(Func.Entry);
    // A `__finally` the normal path runs on into, past a call that never
    // returns, is the clause all the same: C runs it there too.
    std::set<int> FinallyRoots;
    for (const RegistrationScopeRecord &Scope : Chain.Scopes)
      if (const auto It = ByStart.find(Scope.HandlerVA);
          Scope.IsFinally && It != ByStart.end())
        FinallyRoots.insert(It->second->Id);
    std::set<int> Main = reachedFrom(Entry->second->Id, FinallyRoots);
    // A `__finally` that reloads the stack pointer has left its clause, as
    // `goto` or `return` out of it does, and what follows is the function's.
    for (const va_t Reload : StackReloads)
      if (const LowBlock *Block = Func.blockFor(Reload))
        if (const auto [It, New] = ClauseEnds.try_emplace(Block->Id, Reload);
            !New)
          It->second = std::min(It->second, Reload);
    for (int Root : FinallyRoots)
      for (int Id : reachedFrom(Root, {}, &ClauseEnds))
        if (ClauseEnds.count(Id))
          for (int Next : ById.at(Id)->Succs)
            Main.merge(reachedFrom(Next, FinallyRoots));
    std::map<int, std::set<va_t>> Runners;
    for (const RegistrationScopeRecord &Scope : Chain.Scopes)
      for (const va_t Root : {Scope.HandlerVA, Scope.FilterVA})
        if (const auto It = ByStart.find(Root); Root && It != ByStart.end())
          for (int Id : reachedFrom(It->second->Id, Main,
                                    Scope.IsFinally ? &ClauseEnds : nullptr))
            Runners[Id].insert(Root);
    for (const auto &[Id, Roots] : Runners)
      if (Roots.size() == 1)
        Owners[Id] = *Roots.begin();
    // A `__try` in a handler's code: what its own handler runs and that handler
    // both reach belongs to the one around it, as a `__finally` goes on after a
    // `__try` it holds.
    std::map<va_t, va_t> Around;
    for (const auto &[At, Change] : Changes) {
      if (Change.Level < 0 ||
          static_cast<size_t>(Change.Level) >= Chain.Scopes.size())
        continue;
      const LowBlock *Block = Func.blockFor(At);
      const auto Owner = Block ? Owners.find(Block->Id) : Owners.end();
      if (Owner == Owners.end())
        continue;
      const RegistrationScopeRecord &Scope = Chain.Scopes[Change.Level];
      for (const va_t Root : {Scope.HandlerVA, Scope.FilterVA})
        if (Root && Root != Owner->second)
          Around.emplace(Root, Owner->second);
    }
    if (Around.empty())
      return;
    for (const auto &[Id, Roots] : Runners) {
      if (Roots.size() < 2)
        continue;
      std::set<va_t> Outermost;
      for (const va_t Root : Roots) {
        bool Inside = false;
        size_t Depth = 0;
        for (auto It = Around.find(Root);
             !Inside && It != Around.end() && Depth < Around.size();
             It = Around.find(It->second), ++Depth)
          Inside = Roots.count(It->second) != 0;
        if (!Inside)
          Outermost.insert(Root);
      }
      if (Outermost.size() == 1)
        Owners[Id] = *Outermost.begin();
    }
  }

  /// Whether what \p Block does from \p Begin up to \p End calls nothing and
  /// touches only the frame.
  bool isQuiet(const LowBlock &Block, va_t Begin, va_t End) const {
    std::map<Location, int64_t> Local;
    va_t Instruction = InvalidVA;
    for (const LowOp &Op : Block.Ops) {
      if (Op.Addr < Begin || Op.Addr >= End)
        continue;
      if (Op.Addr != Instruction) {
        Instruction = Op.Addr;
        Local = {{locationOf(FP), 0}};
      }
      if (isCall(Op) || Op.Opcode == NdOp::RETURN ||
          Op.Opcode == NdOp::INDIR_BR)
        return false;
      if ((Op.Opcode == NdOp::STORE || Op.Opcode == NdOp::LOAD) &&
          (!Op.NumInputs || !Local.count(locationOf(Op.Inputs[0])) ||
           Op.MemoryAddressSpace != NdMemoryAddressSpace::Default))
        return false;
      note(Op, Local);
    }
    return true;
  }

  void writeStretches() {
    for (const LowBlock &Block : Func.Blocks) {
      const auto In = LevelsIn.find(Block.Id);
      if (In == LevelsIn.end() || In->second.empty())
        continue;
      const auto Owner = Owners.find(Block.Id);
      std::set<int32_t> Levels = In->second;
      // Past its stack reload the code of a `__finally` is the function's.
      std::optional<va_t> Left;
      if (const auto End = ClauseEnds.find(Block.Id);
          End != ClauseEnds.end() && Owner != Owners.end() &&
          llvm::any_of(Chain.Scopes, [&](const RegistrationScopeRecord &Scope) {
            return Scope.IsFinally && Scope.HandlerVA == Owner->second;
          }))
        Left = End->second;
      auto EmitOwned = [&](va_t Begin, va_t End, bool Owned) {
        if (Begin < End)
          Facts.Stretches.push_back({{Begin, End},
                                     {Levels.begin(), Levels.end()},
                                     Owned ? Owner->second : va_t(0),
                                     isQuiet(Block, Begin, End)});
      };
      auto Emit = [&](va_t Begin, va_t End) {
        const bool Owned = Owner != Owners.end();
        if (Left && *Left > Begin && *Left < End) {
          EmitOwned(Begin, *Left, Owned);
          EmitOwned(*Left, End, false);
        } else {
          EmitOwned(Begin, End, Owned && !(Left && *Left <= Begin));
        }
      };
      va_t Begin = Block.StartAddr;
      for (auto It = Changes.lower_bound(Block.StartAddr);
           It != Changes.end() && It->first < Block.EndAddr; ++It) {
        Emit(Begin, It->first);
        std::set<int32_t> Next;
        for (int32_t Level : Levels)
          Next.insert(It->second.applyTo(Level));
        Levels = std::move(Next);
        Begin = It->first;
      }
      Emit(Begin, Block.EndAddr);
    }
    llvm::sort(Facts.Stretches, [](const RegistrationDisplayFacts::Stretch &A,
                                   const RegistrationDisplayFacts::Stretch &B) {
      return A.Range.Begin < B.Range.Begin;
    });
  }
};

} // namespace

void prepareRegistrationDisplayFrame(LowFunc &Func, const BinaryImage *Image) {
  const ExceptionFunction &EH = *Func.ExceptionMetadata;
  if (EH.ParseStatus != ExceptionParseStatus::Complete || !EH.Registration ||
      EH.Cxx || !Func.RegistrationStates ||
      (EH.Personality != ExceptionPersonality::ExceptHandler3 &&
       EH.Personality != ExceptionPersonality::ExceptHandler4) ||
      EH.Registration->Scopes.size() > limits::kMaxRegistrationEHRecords)
    return;
  DisplayFrame(Func, Image).run();
}

} // namespace neverd::x86_registration
