//===- RegistrationCalleeFrame.cpp - PE32 private callee frames -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationABIPrivate.h"
#include "RegistrationFrame.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/LowNoReturn.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/support/BinaryEncoding.h"

#include <deque>
#include <set>

namespace neverd::registration_abi {
namespace {

struct PrivateFrameState {
  registration_state::FrameState Frame;
  registration_state::FrameState Borrow;
  registration_state::FrameState BorrowSpills;
  std::set<int32_t> Initialized;

  bool merge(const PrivateFrameState &Other) {
    bool Changed = Frame.merge(Other.Frame);
    Changed |= Borrow.merge(Other.Borrow);
    Changed |= BorrowSpills.merge(Other.BorrowSpills);
    for (auto It = Initialized.begin(); It != Initialized.end();)
      if (!Other.Initialized.count(*It)) {
        It = Initialized.erase(It);
        Changed = true;
      } else
        ++It;
    return Changed;
  }
};
} // namespace

bool hasPrivateCallerFrame(const LowFunc &Function, const BinaryImage &Image,
                           size_t &Work, ImageFrameEffects &Effects,
                           bool BorrowECX,
                           RegistrationThrowCalleeABI *ThrowProof,
                           bool *IndependentScalarReturn) {
  if (IndependentScalarReturn)
    *IndependentScalarReturn = true;
  using namespace registration_state;
  auto Charge = [&](size_t Amount) {
    if (Amount > limits::kMaxRegistrationEHStateWork - Work)
      return false;
    Work += Amount;
    return true;
  };
  std::map<int, const LowBlock *> Blocks;
  PrivateFrameState Initial;
  for (unsigned Index = 0; Index != Initial.Frame.Registers.size(); ++Index)
    Initial.Frame.Registers[Index] = {{},   {},    false,
                                      true, false, uint8_t(Index + 1)};
  Initial.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride] =
      FrameValue::frame(4);
  FrameValue CallerPC;
  CallerPC.MayBeFrame = CallerPC.ReturnPC = true;
  Initial.Frame.Cells[4] = CallerPC;
  if (BorrowECX) {
    Initial.Frame.Registers[x86reg::RCX / x86reg::GeneralRegStride] = {};
    Initial.Borrow.Registers[x86reg::RCX / x86reg::GeneralRegStride] =
        FrameValue::frame(0);
  }
  for (const auto &Block : Function.Blocks) {
    Blocks.emplace(Block.Id, &Block);
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
  std::deque<int> Pending{Entry->Id};
  while (!Pending.empty()) {
    const int Id = Pending.front();
    Pending.pop_front();
    if (!Blocks.count(Id) ||
        !Charge(1 + Incoming.at(Id).Frame.Cells.size() +
                Incoming.at(Id).Frame.OtherRegisterBytes.size() +
                Incoming.at(Id).Borrow.Cells.size() +
                Incoming.at(Id).BorrowSpills.Cells.size() +
                Incoming.at(Id).Borrow.OtherRegisterBytes.size() +
                Incoming.at(Id).Initialized.size()))
      return false;
    PrivateFrameState State = Incoming.at(Id);
    FrameTransfer Transfer(State.Frame, 0);
    FrameTransfer BorrowTransfer(State.Borrow, 0);
    bool Threw = false;
    for (const auto &Op : Blocks.at(Id)->Ops) {
      size_t RegisterBytes = Op.Output.Size;
      for (unsigned Index = 0; Index != Op.NumInputs; ++Index)
        RegisterBytes += Op.Inputs[Index].Size;
      if (!Charge(1 + RegisterBytes) || Op.Opcode == NdOp::INTRINSIC ||
          Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
        return false;
      Transfer.beginInstruction(Op.Addr);
      BorrowTransfer.beginInstruction(Op.Addr);
      auto BorrowValue = BorrowTransfer.evaluate(Op, false);
      const auto Memory = lowMemoryOperands(Op);
      if (Memory.Address) {
        if (!Memory.Complete ||
            !Charge(State.Frame.Cells.size() + State.Borrow.Cells.size() +
                    State.BorrowSpills.Cells.size() + Memory.AccessSize))
          return false;
        const auto Address = Transfer.read(*Memory.Address);
        const auto Object = BorrowTransfer.read(*Memory.Address);
        if (Object.MayBeFrame && !Object.Offset)
          return false;
        if (BorrowECX && Object.Offset) {
          const int64_t End = int64_t(*Object.Offset) + Memory.AccessSize;
          if (Address.Offset || Address.Constant || Address.MayBeFrame ||
              *Object.Offset < 0 || End > INT32_MAX ||
              End > limits::kMaxRegistrationEHStateWork)
            return false;
          if (Op.Opcode != NdOp::STORE)
            Effects.ECXReads.emplace(*Object.Offset, int32_t(End));
          if (Memory.StoredValue) {
            const auto Stored = Transfer.read(*Memory.StoredValue);
            const auto Borrowed = BorrowTransfer.read(*Memory.StoredValue);
            if (Op.Opcode != NdOp::STORE || Stored.MayBeFrame ||
                Borrowed.MayBeFrame)
              return false;
            Effects.ECXWrites.emplace(*Object.Offset, int32_t(End));
            State.Borrow.store(*Object.Offset, Memory.AccessSize, Borrowed);
          }
        } else {
          // An unknown address can point into the caller frame. Stack arguments
          // also require a separate extent/initialization proof, not a guessed
          // parameter list from an arbitrary positive EBP displacement.
          if ((!Address.Offset && !Address.Constant) ||
              (Address.MayBeFrame && !Address.Offset) ||
              (Address.Offset &&
               int64_t(*Address.Offset) + Memory.AccessSize > 8))
            return false;
          if (Address.Constant && *Address.Constant != 0) {
            const auto *Owner = Image.getSegmentFor(*Address.Constant);
            if (!Owner || *Address.Constant < Owner->VA ||
                uint64_t(*Address.Constant) - Owner->VA > Owner->Size ||
                Memory.AccessSize >
                    Owner->Size - (uint64_t(*Address.Constant) - Owner->VA))
              return false;
          }
          if (Address.Constant && Op.Opcode != NdOp::STORE)
            Effects.Reads.emplace(*Address.Constant,
                                  va_t(*Address.Constant) + Memory.AccessSize);
          if (Address.Constant && Memory.StoredValue)
            Effects.Writes.emplace(*Address.Constant,
                                   va_t(*Address.Constant) + Memory.AccessSize);
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
            if (Reads && !CallerPC)
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
              for (int64_t Byte = *Address.Offset; Byte < End; ++Byte)
                State.Initialized.insert(int32_t(Byte));
            }
            if (Op.Opcode == NdOp::LOAD)
              BorrowValue =
                  State.BorrowSpills.load(Address.Offset, Memory.AccessSize);
          } else if (Memory.StoredValue &&
                     Transfer.read(*Memory.StoredValue).MayBeFrame) {
            if (!Transfer.read(*Memory.StoredValue).ReturnPC)
              return false;
            Effects.CallerPCWrites.emplace(
                *Address.Constant, va_t(*Address.Constant) + Memory.AccessSize);
          }
          if (Memory.StoredValue && !Address.Offset &&
              BorrowTransfer.read(*Memory.StoredValue).MayBeFrame)
            return false;
        }
      }
      std::optional<int32_t> AfterCallSP;
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
        if (BorrowECX)
          return false;
        if (ThrowProof) {
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
              !Table.Constant || Table.MayBeFrame)
            return false;
          auto Info = coff_loader::getCheckedX86SimpleCxxThrowInfo(
              Image, *Table.Constant);
          if (!Info ||
              !Charge(4096 + Info->ObjectSize + State.Frame.Cells.size()))
            return false;
          const int64_t End = int64_t(*Object.Offset) + Info->ObjectSize;
          if (*Object.Offset < *SP + 8 || End > 4)
            return false;
          for (int64_t Byte = *Object.Offset; Byte != End; ++Byte)
            if (!State.Initialized.count(int32_t(Byte)))
              return false;
          for (const auto &[Cell, Value] : State.Frame.Cells)
            if (Value.MayBeFrame && int64_t(Cell) < End &&
                int64_t(*Object.Offset) < int64_t(Cell) + 4)
              return false;
          for (const auto &Range : Info->ReadOnlyRanges)
            Effects.Reads.emplace(Range.Begin, Range.End);
          Effects.Reads.emplace(Info->TypeDescriptorRange.Begin,
                                Info->TypeDescriptorRange.End);
          ThrowProof->ObjectOffset = *Object.Offset;
          ThrowProof->ThrowInfo = std::move(*Info);
          Threw = true;
          break;
        }
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
        unsigned Pop = 0;
        if (Op.NumInputs == 1 && Op.Inputs[0].isConst())
          if (const auto *Import = Image.findImportAt(Op.Inputs[0].Offset))
            if (Import->Name == "RaiseException")
              Pop = 16;
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
      if (Op.Opcode == NdOp::COND_BR || Op.Opcode == NdOp::INDIR_BR ||
          Op.Opcode == NdOp::RETURN || Op.Opcode == NdOp::INTRINSIC)
        for (unsigned Index = 0; Index != Op.NumInputs; ++Index) {
          const auto Value = Transfer.read(Op.Inputs[Index]);
          if (BorrowTransfer.read(Op.Inputs[Index]).MayBeFrame)
            return false;
          if (Value.MayBeFrame &&
              !(Op.Opcode == NdOp::RETURN &&
                (Value.FrameOnlyFromCall || Value.ReturnPC ||
                 Value.SavedRegister ==
                     x86reg::RAX / x86reg::GeneralRegStride + 1)))
            return false;
        }
      if (Op.Opcode == NdOp::RETURN) {
        if (IndependentScalarReturn)
          *IndependentScalarReturn &=
              Op.NumInputs == 1 && Op.Inputs[0].Size == 4 &&
              !Transfer.read(Op.Inputs[0]).MayBeFrame &&
              !BorrowTransfer.read(Op.Inputs[0]).MayBeFrame;
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
        }
      }
      Transfer.write(Op, Transfer.evaluate(Op, false));
      if (BorrowECX)
        BorrowTransfer.write(Op, BorrowValue);
      if (AfterCallSP)
        State.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride] =
            FrameValue::frame(*AfterCallSP);
    }
    if (Threw)
      continue;
    for (int Successor : Blocks.at(Id)->Succs) {
      const auto Existing = Incoming.find(Successor);
      const auto StateSize = [](const PrivateFrameState &S) {
        return S.Frame.Cells.size() + S.Frame.OtherRegisterBytes.size() +
               S.Borrow.Cells.size() + S.BorrowSpills.Cells.size() +
               S.Borrow.OtherRegisterBytes.size() + S.Initialized.size();
      };
      if (!Charge(
              1 + StateSize(State) +
              (Existing == Incoming.end() ? 0 : StateSize(Existing->second))))
        return false;
      auto [It, New] = Incoming.emplace(Successor, State);
      if (New || It->second.merge(State))
        Pending.push_back(Successor);
    }
  }
  return true;
}

} // namespace neverd::registration_abi
