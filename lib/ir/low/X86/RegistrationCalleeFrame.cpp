//===- RegistrationCalleeFrame.cpp - PE32 private callee frames -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

/// \file
/// Check private callee frames and invocation-specific scalar effects.
//===----------------------------------------------------------------------===//

#include "RegistrationABIPrivate.h"
#include "RegistrationFrameBranch.h"
#include "RegistrationFrameEffects.h"
#include "RegistrationOriginalImage.h"
#include "RegistrationPrivateFrame.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/LowNoReturn.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/support/BinaryEncoding.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace neverd::registration_abi {
namespace {
size_t stateWork(const PrivateFrameState &State) {
  return State.Frame.cellCount() + State.Frame.OtherRegisterBytes.size() +
         State.Borrow.cellCount() + State.Borrow.OtherRegisterBytes.size() +
         State.BorrowSpills.cellCount() +
         State.BorrowSpills.OtherRegisterBytes.size() +
         State.CopySource.cellCount() +
         State.CopySource.OtherRegisterBytes.size() +
         State.CopySourceSpills.cellCount() +
         State.CopySourceSpills.OtherRegisterBytes.size() +
         State.ObjectInitialized.size() + State.Initialized.size() +
         State.Frame.Registers.size() + State.Borrow.Registers.size() +
         State.BorrowSpills.Registers.size() +
         State.CopySource.Registers.size() +
         State.CopySourceSpills.Registers.size();
}

/// A bound invocation may use immutable scalar bytes to prove its memory
/// footprint. The loader still owns mappings and conflicting fixups. No raw
/// byte pattern acquires an address identity here.
std::optional<registration_state::FrameValue>
immutableScalar(const BinaryImage &Image, va_t Address, uint16_t Width,
                size_t &Work) {
  const auto Read =
      readImmutableRegistrationScalar(Image, Address, Width, Work);
  if (Read && Read->Value)
    return registration_state::FrameValue::constant(*Read->Value);
  return std::nullopt;
}
} // namespace

bool PrivateFrameState::merge(const PrivateFrameState &Other) {
  bool Changed = Frame.merge(Other.Frame);
  Changed |= Borrow.merge(Other.Borrow);
  Changed |= BorrowSpills.merge(Other.BorrowSpills);
  Changed |= CopySource.merge(Other.CopySource);
  Changed |= CopySourceSpills.merge(Other.CopySourceSpills);
  for (auto It = ObjectInitialized.begin(); It != ObjectInitialized.end();)
    if (!Other.ObjectInitialized.count(*It)) {
      It = ObjectInitialized.erase(It);
      Changed = true;
    } else
      ++It;
  for (auto It = Initialized.begin(); It != Initialized.end();)
    if (!Other.Initialized.count(*It)) {
      It = Initialized.erase(It);
      Changed = true;
    } else
      ++It;
  return Changed;
}

bool hasPrivateCallerFrame(const LowFunc &Function, const BinaryImage &Image,
                           size_t &Work, ImageFrameEffects &Effects,
                           const PrivateFrameOptions &Options) {
  const auto [BorrowECX, ThrowProof, IndependentScalarReturn,
              DiscardReturnValue, CopyObjectSize, Initializer, Initializers,
              ObjectCallbacks, CallbackStackPopBytes, ScalarArguments,
              ScalarRegisters, ReturnValue, ExcludeCalls, OriginalImage] =
      Options;
  if (ReturnValue)
    ReturnValue->reset();
  if ((ReturnValue || ScalarRegisters || ExcludeCalls) && !ScalarArguments)
    return false;
  if (OriginalImage && (!ScalarArguments || !ExcludeCalls || !ReturnValue))
    return false;
  if (ScalarArguments &&
      (BorrowECX || ThrowProof || DiscardReturnValue || CopyObjectSize ||
       Initializer || Initializers || ObjectCallbacks || CallbackStackPopBytes))
    return false;
  if (CopyObjectSize && (!BorrowECX || !DiscardReturnValue || ThrowProof))
    return false;
  if (Initializer && (!BorrowECX || DiscardReturnValue || CopyObjectSize ||
                      ThrowProof || Initializer->StackPopBytes > UINT16_MAX ||
                      Initializer->StackPopBytes % 4))
    return false;
  if (Initializers && !ThrowProof && !Initializer)
    return false;
  if ((ObjectCallbacks &&
       (!BorrowECX || ThrowProof || CopyObjectSize || Initializer ||
        Initializers || CallbackStackPopBytes > UINT16_MAX ||
        CallbackStackPopBytes % 4)) ||
      (!ObjectCallbacks && CallbackStackPopBytes))
    return false;
  const uint32_t ScalarArgumentBytes =
      Initializer ? Initializer->StackPopBytes : CallbackStackPopBytes;
  const bool InitializesObject = CopyObjectSize || Initializer;
  if (IndependentScalarReturn)
    *IndependentScalarReturn = true;
  using namespace registration_state;
  auto Charge = [&](size_t Amount) { return chargeCalleeWork(Work, Amount); };
  Decoder Dec;
  if (!Dec.init(Image))
    return false;
  for (const auto &Block : Function.Blocks)
    for (const auto &Insn : Block.InstructionBoundaries)
      if (!Charge(1) || !hasImmutableCalleeInstruction(Image, Insn, Dec))
        return false;
  std::map<int, const LowBlock *> Blocks;
  std::map<va_t, int> Entries;
  PrivateFrameState Initial;
  for (unsigned Index = 0; Index != Initial.Frame.Registers.size(); ++Index)
    Initial.Frame.Registers[Index] = {{},   {},    false,
                                      true, false, uint8_t(Index + 1)};
  Initial.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride] =
      FrameValue::frame(4);
  FrameValue CallerPC;
  CallerPC.MayBeFrame = CallerPC.ReturnPC = true;
  Initial.Frame.Cells[4] = CallerPC;
  if (ScalarRegisters)
    for (const auto &[Index, Value] : *ScalarRegisters) {
      if (!Charge(1) || !isX86VolatileFrameRegister(Index) || Value.MayBeFrame)
        return false;
      Initial.Frame.Registers[Index] = Value;
    }
  if (ScalarArguments)
    for (const auto &[Offset, Value] : *ScalarArguments) {
      if (Offset < 8 ||
          Offset >
              ::neverd::limits::ir::
                  kRegistrationConditionalCallHasConditionalCallFrameOffsetLimit ||
          (Offset & 3) || Value.MayBeFrame)
        return false;
      Initial.Frame.store(Offset, 4, Value);
      for (int Byte = 0; Byte != 4; ++Byte)
        Initial.Initialized.insert(Offset + Byte);
    }
  if (BorrowECX) {
    Initial.Frame.Registers[x86reg::RCX / x86reg::GeneralRegStride] = {};
    Initial.Borrow.Registers[x86reg::RCX / x86reg::GeneralRegStride] =
        FrameValue::frame(0);
  }
  if (CopyObjectSize)
    Initial.CopySourceSpills.Cells[8] = FrameValue::frame(0);
  for (const auto &Block : Function.Blocks) {
    if (!Charge(1) || !Blocks.emplace(Block.Id, &Block).second ||
        !Entries.emplace(Block.StartAddr, Block.Id).second)
      return false;
    for (const auto &Op : Block.Ops)
      for (unsigned Index = 0; Index != Op.NumInputs; ++Index) {
        const auto &Input = Op.Inputs[Index];
        if (!Charge(1 + Input.Size))
          return false;
        if (Input.isReg() && Input.Offset >= 8 * x86reg::GeneralRegStride)
          for (uint64_t Byte = 0; Byte != Input.Size; ++Byte)
            Initial.Frame.OtherRegisterBytes[Input.Offset + Byte] = {
                {}, {}, false, true};
      }
  }
  const auto Entry = llvm::find_if(Function.Blocks, [&](const LowBlock &Block) {
    return Block.StartAddr == Function.Entry;
  });
  if (Entry == Function.Blocks.end())
    return false;
  std::map<int, PrivateFrameState> Incoming;
  Incoming.emplace(Entry->Id, Initial);
  // Ordinary contracts join incoming states. Exact bound invocations retain
  // each reached state, so a finite table loop does not lose its induction
  // value at the first backedge. Repeated states close a fixed point; the
  // shared work limit bounds new states and comparisons without guessing an
  // iteration count from the amount of mapped storage.
  std::map<int, std::vector<PrivateFrameState>> BoundIncoming;
  if (ScalarArguments)
    BoundIncoming[Entry->Id].push_back(Initial);
  std::map<std::pair<va_t, uint16_t>, std::optional<FrameValue>> ImageValues;
  std::optional<FrameValue> Returned;
  bool CompleteReturns = true;
  std::deque<std::pair<int, size_t>> Pending{{Entry->Id, 0}};
  while (!Pending.empty()) {
    const auto [Id, StateIndex] = Pending.front();
    Pending.pop_front();
    if (!Blocks.count(Id))
      return false;
    const auto &Before =
        ScalarArguments ? BoundIncoming.at(Id).at(StateIndex) : Incoming.at(Id);
    if (!Charge(1 + stateWork(Before)))
      return false;
    PrivateFrameState State = Before;
    FrameTransfer Transfer(State.Frame, 0);
    FrameTransfer BorrowTransfer(State.Borrow, 0);
    FrameTransfer CopySourceTransfer(State.CopySource, 0);
    bool Threw = false;
    for (const auto &Op : Blocks.at(Id)->Ops) {
      size_t RegisterBytes = Op.Output.Size;
      for (unsigned Index = 0; Index != Op.NumInputs; ++Index)
        RegisterBytes += Op.Inputs[Index].Size;
      if (!Charge(1 + RegisterBytes) ||
          (Op.Opcode == NdOp::INTRINSIC &&
           !registration_state::isFramePreservingIntrinsic(Op)) ||
          Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
        return false;
      Transfer.beginInstruction(Op.Addr);
      BorrowTransfer.beginInstruction(Op.Addr);
      CopySourceTransfer.beginInstruction(Op.Addr);
      auto BorrowValue = BorrowTransfer.evaluate(Op, false);
      auto CopySourceValue = CopySourceTransfer.evaluate(Op, false);
      std::optional<FrameValue> ImageValue;
      const auto Memory = lowMemoryOperands(Op);
      if (Memory.Address) {
        if (!Memory.Complete ||
            !Charge(State.Frame.Cells.size() + State.Borrow.Cells.size() +
                    State.BorrowSpills.Cells.size() +
                    State.CopySource.Cells.size() +
                    State.CopySourceSpills.Cells.size() + Memory.AccessSize))
          return false;
        const auto Address = Transfer.read(*Memory.Address);
        const auto ImageVA = Address.preferredAddress();
        const auto Object = BorrowTransfer.read(*Memory.Address);
        const auto CopySource = CopySourceTransfer.read(*Memory.Address);
        if ((Object.MayBeFrame && !Object.Offset) ||
            (CopySource.MayBeFrame && !CopySource.Offset) ||
            (Object.MayBeFrame && CopySource.MayBeFrame))
          return false;
        if (BorrowECX && Object.Offset) {
          const int64_t End = int64_t(*Object.Offset) + Memory.AccessSize;
          if (Address.Offset || ImageVA || Address.MayBeFrame ||
              *Object.Offset < 0 || End > INT32_MAX ||
              End > limits::kMaxSourceCallBorrowedBytes ||
              (CopyObjectSize && uint64_t(End) > CopyObjectSize))
            return false;
          if (InitializesObject && Op.Opcode != NdOp::STORE)
            for (int64_t Byte = *Object.Offset; Byte != End; ++Byte)
              if (!State.ObjectInitialized.count(int32_t(Byte)))
                return false;
          if (Op.Opcode != NdOp::STORE)
            Effects.ECXReads.emplace(*Object.Offset, int32_t(End));
          if (Memory.StoredValue) {
            const auto Stored = Transfer.read(*Memory.StoredValue);
            const auto Borrowed = BorrowTransfer.read(*Memory.StoredValue);
            if (Op.Opcode != NdOp::STORE || Stored.MayBeFrame ||
                Borrowed.MayBeFrame ||
                CopySourceTransfer.read(*Memory.StoredValue).MayBeFrame)
              return false;
            Effects.ECXWrites.emplace(*Object.Offset, int32_t(End));
            State.Borrow.store(*Object.Offset, Memory.AccessSize, Borrowed);
            if (InitializesObject)
              for (int64_t Byte = *Object.Offset; Byte != End; ++Byte)
                State.ObjectInitialized.insert(int32_t(Byte));
          }
        } else if (CopyObjectSize && CopySource.Offset) {
          const int64_t End = int64_t(*CopySource.Offset) + Memory.AccessSize;
          if (Address.Offset || ImageVA || Address.MayBeFrame ||
              *CopySource.Offset < 0 || uint64_t(End) > CopyObjectSize ||
              Op.Opcode != NdOp::LOAD || Memory.StoredValue)
            return false;
          Effects.CopySourceReads.emplace(*CopySource.Offset, int32_t(End));
        } else {
          // An unknown address can point into the caller frame. Stack arguments
          // also require a separate extent/initialization proof, not a guessed
          // parameter list from an arbitrary positive EBP displacement.
          const bool CopyArgument = CopyObjectSize && Address.Offset == 8 &&
                                    Memory.AccessSize == 4 &&
                                    Op.Opcode == NdOp::LOAD;
          const bool ScalarArgument =
              (Initializer || ObjectCallbacks) && Address.Offset &&
              *Address.Offset >= 8 &&
              uint64_t(*Address.Offset) + Memory.AccessSize <=
                  uint64_t(8) + ScalarArgumentBytes &&
              Op.Opcode == NdOp::LOAD && Memory.AccessSize <= 4 &&
              (*Address.Offset - 8) % 4 + Memory.AccessSize <= 4;
          if ((!Address.Offset && !ImageVA) ||
              (Address.MayBeFrame && !Address.Offset) ||
              (Address.Offset && !CopyArgument && !ScalarArgument &&
               int64_t(*Address.Offset) + Memory.AccessSize > 8 &&
               (!ScalarArguments || Memory.AccessSize != 4 ||
                !ScalarArguments->count(*Address.Offset) ||
                Op.Opcode != NdOp::LOAD)))
            return false;
          if (ImageVA && *ImageVA != 0) {
            const auto *Owner = Image.getSegmentFor(*ImageVA);
            if (!Owner || *ImageVA < Owner->VA ||
                uint64_t(*ImageVA) - Owner->VA > Owner->Size ||
                Memory.AccessSize >
                    Owner->Size - (uint64_t(*ImageVA) - Owner->VA))
              return false;
          }
          if (ImageVA && Op.Opcode != NdOp::STORE)
            Effects.Reads.emplace(*ImageVA, va_t(*ImageVA) + Memory.AccessSize);
          if (ScalarArguments && ImageVA && !Address.MayBeFrame &&
              Op.MemoryOrdering == NdMemoryOrdering::None &&
              Op.Opcode == NdOp::LOAD && Memory.AccessSize == Op.Output.Size) {
            const auto Key = std::make_pair(va_t(*ImageVA), Memory.AccessSize);
            if (!Charge(1 + ImageValues.size()))
              return false;
            const auto [It, New] = ImageValues.try_emplace(Key);
            if (New) {
              if (OriginalImage &&
                  !OriginalImage->permitsRead(Key.first, Key.second, Work))
                return false;
              It->second =
                  OriginalImage
                      ? OriginalImage->readScalar(Key.first, Key.second, Work)
                      : immutableScalar(Image, Key.first, Key.second, Work);
            }
            if (Work >= limits::kMaxRegistrationEHStateWork)
              return false;
            ImageValue = It->second;
          }
          if (ImageVA && Memory.StoredValue)
            Effects.Writes.emplace(*ImageVA,
                                   va_t(*ImageVA) + Memory.AccessSize);
          if (Address.Offset) {
            const int64_t End = int64_t(*Address.Offset) + Memory.AccessSize;
            if (End > INT32_MAX)
              return false;
            const bool Reads = Op.Opcode != NdOp::STORE;
            // The real caller PC intentionally identifies the regenerated call
            // site. Other bytes need a store by this invocation on every path.
            const bool CallerPC =
                *Address.Offset == 4 && Memory.AccessSize == 4;
            const auto SP =
                State.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride]
                    .Offset;
            if (!CallerPC && (!SP || *Address.Offset < *SP))
              return false;
            if (Reads && !CallerPC && !CopyArgument && !ScalarArgument)
              for (int64_t Byte = *Address.Offset; Byte < End; ++Byte)
                if (!State.Initialized.count(int32_t(Byte)))
                  return false;
            if (Memory.StoredValue) {
              if (End > 4 || Op.Opcode != NdOp::STORE)
                return false;
              const auto Value = Transfer.read(*Memory.StoredValue);
              State.Frame.store(*Address.Offset, Memory.AccessSize, Value);
              if (BorrowECX)
                State.BorrowSpills.store(
                    *Address.Offset, Memory.AccessSize,
                    BorrowTransfer.read(*Memory.StoredValue));
              if (CopyObjectSize)
                State.CopySourceSpills.store(
                    *Address.Offset, Memory.AccessSize,
                    CopySourceTransfer.read(*Memory.StoredValue));
              for (int64_t Byte = *Address.Offset; Byte < End; ++Byte)
                State.Initialized.insert(int32_t(Byte));
            }
            if (Op.Opcode == NdOp::LOAD) {
              BorrowValue =
                  State.BorrowSpills.load(Address.Offset, Memory.AccessSize);
              CopySourceValue = State.CopySourceSpills.load(Address.Offset,
                                                            Memory.AccessSize);
            }
          } else if (Memory.StoredValue &&
                     Transfer.read(*Memory.StoredValue).MayBeFrame) {
            if (!Transfer.read(*Memory.StoredValue).ReturnPC)
              return false;
            Effects.CallerPCWrites.emplace(*ImageVA,
                                           va_t(*Address.preferredAddress()) +
                                               Memory.AccessSize);
          }
          if (Memory.StoredValue && !Address.Offset &&
              (BorrowTransfer.read(*Memory.StoredValue).MayBeFrame ||
               CopySourceTransfer.read(*Memory.StoredValue).MayBeFrame))
            return false;
        }
      }
      std::optional<int32_t> AfterCallSP;
      std::optional<FrameValue> AfterCallResult;
      std::optional<FrameValue> AfterCallObject;
      bool AfterCallScalar = false;
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
        if (ExcludeCalls)
          return false;
        if (BorrowECX && !Initializer && !ObjectCallbacks)
          return false;
        const RegistrationInitializerCalleeABI *Init = nullptr;
        if (Initializers)
          if (const auto It = Initializers->find({Op.Addr, Op.Seq});
              It != Initializers->end())
            Init = &It->second;
        if (Init) {
          if (Op.Opcode != NdOp::CALL || Op.NumInputs != 1 ||
              !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 4 ||
              Op.Inputs[0].Offset != Init->Object.Target)
            return false;
          const auto Call =
              Initializer
                  ? applyBorrowedObjectCall(State, Init->Object,
                                            Init->Initialized, Work, Effects)
                  : applyPrivateInitializerCall(State, *Init, Work, Effects,
                                                *ThrowProof);
          if (!Call)
            return false;
          AfterCallSP = Call->StackOffset;
          if (Initializer)
            AfterCallObject = Call->Object;
          else
            AfterCallResult = Call->Object;
        } else if (ObjectCallbacks) {
          const auto It = ObjectCallbacks->find({Op.Addr, Op.Seq});
          if (It == ObjectCallbacks->end() || Op.Opcode != NdOp::CALL ||
              Op.NumInputs != 1 || !Op.Inputs[0].isConst() ||
              Op.Inputs[0].Size != 4 ||
              Op.Inputs[0].Offset != It->second.Target)
            return false;
          const auto Call =
              applyBorrowedObjectCall(State, It->second, {}, Work, Effects);
          if (!Call)
            return false;
          AfterCallSP = Call->StackOffset;
          // A checked scalar result grants no object identity or initialized
          // bytes. Every other volatile result remains unknown.
          AfterCallScalar = It->second.HasIndependentScalarReturn;
          if (AfterCallScalar && !hasX86ScalarCallOutput(Op))
            return false;
        } else if (ThrowProof) {
          const auto SP =
              State.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride]
                  .Offset;
          if (!SP || *SP > INT32_MAX - 8 || Op.Opcode != NdOp::CALL ||
              Op.NumInputs != 1 || !Op.Inputs[0].isConst() ||
              Op.Inputs[0].Size != 4 ||
              Op.Inputs[0].Offset != ThrowProof->ImportVA ||
              Op.Addr != ThrowProof->ThrowCallVA ||
              Op.Seq != ThrowProof->ThrowOpSeq)
            return false;
          const auto Object = State.Frame.load(*SP, 4);
          const auto Table = State.Frame.load(*SP + 4, 4);
          if (Object.Constant == 0 && Table.Constant == 0 &&
              !Object.MayBeFrame && !Table.MayBeFrame) {
            ThrowProof->IsRethrow = true;
            Threw = true;
            break;
          }
          if (!Object.Offset || Object.ReturnPC || Object.FrameOnlyFromCall ||
              !Table.preferredAddress() || Table.MayBeFrame)
            return false;
          auto ObjectABI = getCheckedX86RegistrationThrowObjectABI(
              Image, *Table.preferredAddress(), &Work);
          if (!ObjectABI ||
              !Charge(ObjectABI->Info.ObjectSize + State.Frame.Cells.size()))
            return false;
          const auto &Info = ObjectABI->Info;
          const int64_t End = int64_t(*Object.Offset) + Info.ObjectSize;
          if (*Object.Offset < *SP + 8 || End > 4)
            return false;
          for (int64_t Byte = *Object.Offset; Byte != End; ++Byte)
            if (!State.Initialized.count(int32_t(Byte)))
              return false;
          for (const auto &[Cell, Value] : State.Frame.Cells)
            if (Value.MayBeFrame && int64_t(Cell) < End &&
                int64_t(*Object.Offset) < int64_t(Cell) + 4)
              return false;
          for (const auto &Range : Info.ReadOnlyRanges)
            Effects.Reads.emplace(Range.Begin, Range.End);
          Effects.Reads.emplace(Info.TypeDescriptorRange.Begin,
                                Info.TypeDescriptorRange.End);
          for (const auto *Callback : ObjectABI->callbacks()) {
            if (!Callback)
              continue;
            if (!Charge(Callback->ImageReads.size() +
                        Callback->ImageWrites.size() +
                        Callback->CodeRanges.size()))
              return false;
            for (const auto &Range : Callback->ImageReads)
              Effects.Reads.emplace(Range.Begin, Range.End);
            for (const auto &Range : Callback->ImageWrites)
              Effects.Writes.emplace(Range.Begin, Range.End);
            ThrowProof->CodeRanges.insert(ThrowProof->CodeRanges.end(),
                                          Callback->CodeRanges.begin(),
                                          Callback->CodeRanges.end());
          }
          ThrowProof->ObjectOffset = *Object.Offset;
          ThrowProof->ThrowInfo = std::move(ObjectABI->Info);
          Threw = true;
          break;
        }
        if (!AfterCallSP) {
          if (Initializer)
            return false;
          if (!Charge(State.Frame.Cells.size() + State.Initialized.size()))
            return false;
          for (unsigned Register = 0; Register != State.Frame.Registers.size();
               ++Register)
            if (Register != x86reg::RBP / x86reg::GeneralRegStride &&
                Register != x86reg::RSP / x86reg::GeneralRegStride &&
                State.Frame.Registers[Register].MayBeFrame &&
                !State.Frame.Registers[Register].ReturnPC &&
                State.Frame.Registers[Register].SavedRegister != Register + 1)
              return false;
          for (const auto &[Offset, Value] : State.Frame.Cells)
            if (Offset < 0 && Value.MayBeFrame)
              return false;
          const auto SP =
              State.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride]
                  .Offset;
          if (!SP)
            return false;
          if (ScalarArguments) {
            // A closed RaiseException invocation reads no argument array.
            // Its callbacks remain obligations of the enclosing SEH solver.
            if (!hasClosedRegistrationRaise(Image, Op, State.Frame))
              return false;
          }
          unsigned Pop = 0;
          if (Op.NumInputs == 1 && Op.Inputs[0].isConst())
            if (auto Known = checkedRegistrationImportStackPop(
                    Image, Op.Inputs[0].Offset))
              Pop = *Known;
          if (int64_t(*SP) + Pop > INT32_MAX)
            return false;
          AfterCallSP = int32_t(int64_t(*SP) + Pop);
          // A callee owns everything below call SP and may overwrite its
          // outgoing argument area. A later read needs a fresh local store.
          for (auto It = State.Initialized.begin();
               It != State.Initialized.end();)
            if (*It < *AfterCallSP)
              It = State.Initialized.erase(It);
            else
              ++It;
          for (auto It = State.Frame.Cells.begin();
               It != State.Frame.Cells.end();)
            if (It->first < *AfterCallSP)
              It = State.Frame.Cells.erase(It);
            else
              ++It;
          for (auto It = State.BorrowSpills.Cells.begin();
               It != State.BorrowSpills.Cells.end();)
            if (It->first < *AfterCallSP)
              It = State.BorrowSpills.Cells.erase(It);
            else
              ++It;
        }
      }
      if (Op.Opcode == NdOp::COND_BR || Op.Opcode == NdOp::INDIR_BR ||
          (Op.Opcode == NdOp::RETURN && !DiscardReturnValue && !Initializer) ||
          Op.Opcode == NdOp::INTRINSIC)
        for (unsigned Index = 0; Index != Op.NumInputs; ++Index) {
          const auto Value = Transfer.read(Op.Inputs[Index]);
          if (BorrowTransfer.read(Op.Inputs[Index]).MayBeFrame)
            return false;
          if (CopySourceTransfer.read(Op.Inputs[Index]).MayBeFrame)
            return false;
          if (Value.MayBeFrame &&
              !(Op.Opcode == NdOp::RETURN &&
                (Value.FrameOnlyFromCall || Value.ReturnPC ||
                 Value.SavedRegister ==
                     x86reg::RAX / x86reg::GeneralRegStride + 1)))
            return false;
        }
      if (Op.Opcode == NdOp::RETURN) {
        if (ReturnValue) {
          if (!Charge(1))
            return false;
          if (Op.NumInputs != 1 || Op.Inputs[0].Size != 4)
            CompleteReturns = false;
          else {
            const auto Value = Transfer.read(Op.Inputs[0]);
            Returned = Returned ? join(*Returned, Value) : Value;
          }
        }
        if (Initializer) {
          if (Op.NumInputs != 1 || Op.Inputs[0].Size != 4 ||
              Transfer.read(Op.Inputs[0]).MayBeFrame ||
              BorrowTransfer.read(Op.Inputs[0]).Offset != 0 ||
              !Charge(State.ObjectInitialized.size() +
                      (Initializer->Initialized
                           ? Initializer->Initialized->size()
                           : 0)))
            return false;
          if (!Initializer->Initialized)
            Initializer->Initialized = State.ObjectInitialized;
          else
            std::erase_if(*Initializer->Initialized, [&](int32_t Byte) {
              return !State.ObjectInitialized.count(Byte);
            });
        }
        if (IndependentScalarReturn)
          *IndependentScalarReturn &=
              Op.NumInputs == 1 && Op.Inputs[0].Size == 4 &&
              !Transfer.read(Op.Inputs[0]).MayBeFrame &&
              !BorrowTransfer.read(Op.Inputs[0]).MayBeFrame &&
              !CopySourceTransfer.read(Op.Inputs[0]).MayBeFrame;
        if (CopyObjectSize && State.ObjectInitialized.size() != CopyObjectSize)
          return false;
        if (State.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride]
                .Offset != 4)
          return false;
        for (uint64_t Register :
             {x86reg::RBX, x86reg::RBP, x86reg::RSI, x86reg::RDI}) {
          const auto Index = Register / x86reg::GeneralRegStride;
          if (State.Frame.Registers[Index] != Initial.Frame.Registers[Index])
            return false;
          if (State.Borrow.Registers[Index] != Initial.Borrow.Registers[Index])
            return false;
          if (State.CopySource.Registers[Index] !=
              Initial.CopySource.Registers[Index])
            return false;
        }
      }
      if (OriginalImage && Op.Opcode != NdOp::LOAD)
        ImageValue = OriginalImage->evaluate(Op, Transfer, Work);
      if (OriginalImage && Work >= limits::kMaxRegistrationEHStateWork)
        return false;
      Transfer.write(Op, ImageValue.value_or(Transfer.evaluate(Op, false)));
      if (BorrowECX)
        BorrowTransfer.write(Op, BorrowValue);
      if (CopyObjectSize)
        CopySourceTransfer.write(Op, CopySourceValue);
      if (AfterCallSP)
        State.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride] =
            FrameValue::frame(*AfterCallSP);
      if (AfterCallSP && ObjectCallbacks)
        State.Borrow.Registers[x86reg::RSP / x86reg::GeneralRegStride] = {};
      if (AfterCallScalar) {
        LowOp Result;
        Result.Opcode = NdOp::COPY;
        Result.Output = Op.Output;
        for (auto *Domain : {&State.Frame, &State.Borrow, &State.CopySource})
          Domain->Registers[x86reg::RAX / x86reg::GeneralRegStride] = {};
        Transfer.write(Result, {});
        BorrowTransfer.write(Result, {});
        CopySourceTransfer.write(Result, {});
      }
      if (AfterCallResult) {
        State.Frame.Registers[x86reg::RAX / x86reg::GeneralRegStride] =
            *AfterCallResult;
        LowOp Result;
        Result.Opcode = NdOp::COPY;
        Result.Output = Op.Output;
        Transfer.write(Result, *AfterCallResult);
      }
      if (AfterCallObject) {
        // The callee's returned pointer belongs to our borrowed object domain,
        // never to its private frame. Its stack cleanup has no object alias.
        LowOp Result;
        Result.Opcode = NdOp::COPY;
        Result.Output = Op.Output;
        Transfer.write(Result, {});
        BorrowTransfer.write(Result, *AfterCallObject);
        State.Frame.Registers[x86reg::RAX / x86reg::GeneralRegStride] = {};
        State.Borrow.Registers[x86reg::RAX / x86reg::GeneralRegStride] =
            *AfterCallObject;
        State.Borrow.Registers[x86reg::RSP / x86reg::GeneralRegStride] = {};
      }
    }
    if (Threw)
      continue;
    std::optional<va_t> BoundSuccessor;
    const auto &Block = *Blocks.at(Id);
    if (ScalarArguments) {
      if (!Charge(1 + Entries.size()))
        return false;
      BoundSuccessor = knownFrameBranchTarget(
          Block, Transfer, [&](va_t Address) -> std::optional<int> {
            const auto Found = Entries.find(Address);
            return Found == Entries.end() ? std::nullopt
                                          : std::optional<int>{Found->second};
          });
    }
    for (int Successor : Blocks.at(Id)->Succs) {
      if (!Blocks.count(Successor))
        return false;
      if (BoundSuccessor && *BoundSuccessor != Blocks.at(Successor)->StartAddr)
        continue;
      if (ScalarArguments) {
        if (!Charge(1 + stateWork(State)))
          return false;
        auto &Reached = BoundIncoming[Successor];
        bool Found = false;
        for (const auto &Existing : Reached) {
          if (!Charge(1 + stateWork(State) + stateWork(Existing)))
            return false;
          if (Existing == State) {
            Found = true;
            break;
          }
        }
        if (!Found) {
          Pending.emplace_back(Successor, Reached.size());
          Reached.push_back(State);
        }
        continue;
      }
      const auto Existing = Incoming.find(Successor);
      if (!Charge(
              1 + stateWork(State) +
              (Existing == Incoming.end() ? 0 : stateWork(Existing->second))))
        return false;
      auto [It, New] = Incoming.emplace(Successor, State);
      if (New || It->second.merge(State))
        Pending.emplace_back(Successor, 0);
    }
  }
  if (ReturnValue && CompleteReturns)
    *ReturnValue = Returned;
  return true;
}

} // namespace neverd::registration_abi
