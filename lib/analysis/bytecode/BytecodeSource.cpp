//===- BytecodeSource.cpp - Explicit bytecode state lowering --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/analysis/BytecodeSource.h"

#include "BytecodeInternal.h"

#include "neverd/analysis/Limits.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/lift/AArch64/AArch64Regs.h"
#include "neverd/lift/X86/X86Regs.h"

#include <iterator>
#include <map>

using namespace neverd;
using namespace neverd::analysis;

namespace {
// These are lexical values, not architectural registers. In particular a
// call may clobber physical registers but cannot redefine its caller's state
// pointer. Keep them separate from profile and scratch temporary offsets.
constexpr uint64_t StatePointer = UINT64_C(1) << 60;
constexpr uint64_t CallStatus = StatePointer + 8;
constexpr uint64_t ContextPointer = StatePointer + 16;
constexpr uint64_t ScratchBase = UINT64_C(1) << 61;

llvm::Error invalid(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::errc::invalid_argument,
                                 "bytecode source: " + Message);
}
} // namespace

llvm::Expected<BytecodeStateFunction> neverd::analysis::lowerBytecodeState(
    const LowFunc &Function, uint32_t RegisterBytes, Arch SourceArch,
    uint64_t MaxOperations, llvm::ArrayRef<va_t> BoundCalls, bool WithContext) {
  if ((SourceArch != Arch::X64 && SourceArch != Arch::AArch64) ||
      !RegisterBytes ||
      RegisterBytes >
          ::neverd::limits::analysis::kMaxBytecodeRegisterBankBytes ||
      Function.Blocks.empty() || !Function.hasCompleteLiftCoverage() ||
      !MaxOperations)
    return invalid("invalid function, bank or architecture");
  uint64_t Parameter = SourceArch == Arch::AArch64 ? a64reg::X0 : x86reg::RDI;
  uint64_t ContextParameter =
      SourceArch == Arch::AArch64 ? a64reg::X1 : x86reg::RSI;
  uint64_t Return = SourceArch == Arch::AArch64 ? a64reg::X0 : x86reg::RAX;
  BytecodeStateFunction Result;
  Result.Function = Function;
  uint64_t Count = 0;
  bool HasEntry = false;
  bool HasCall = false;
  va_t FailureAddress = UINT64_MAX - 16;
  va_t BodyEntryAddress = UINT64_MAX - 32;
  int FailureID = 0;
  for (const auto &B : Function.Blocks) {
    if (B.StartAddr >= BodyEntryAddress || B.EndAddr >= BodyEntryAddress ||
        B.Id < 0 || B.Id >= std::numeric_limits<int>::max() - 2 ||
        B.Ops.empty() || B.Id != FailureID || B.StartAddr >= B.EndAddr)
      return invalid("invalid block identity");
    for (int S : B.Succs)
      if (S < 0 || static_cast<size_t>(S) >= Function.Blocks.size() ||
          std::find(Function.Blocks[S].Preds.begin(),
                    Function.Blocks[S].Preds.end(),
                    B.Id) == Function.Blocks[S].Preds.end())
        return invalid("invalid successor");
    for (int P : B.Preds)
      if (P < 0 || static_cast<size_t>(P) >= Function.Blocks.size() ||
          !Function.Blocks[P].hasSucc(B.Id))
        return invalid("invalid predecessor");
    FailureID = std::max(FailureID, B.Id + 1);
  }
  if (Function.Blocks.front().StartAddr != Function.Entry)
    return invalid("entry must be the first block");
  for (auto &Block : Result.Function.Blocks) {
    std::vector<LowOp> Ops;
    // Source registers live in ordinary memory. Reuse an exact view only
    // inside this block, until a possibly aliasing guest write or overlapping
    // state write invalidates it. Values use fresh lexical temporaries, so
    // later instruction-local definitions cannot change a cached read.
    std::map<std::pair<uint64_t, uint16_t>, NdVar> RegisterValues;
    std::map<std::pair<uint64_t, uint16_t>, size_t> PendingStateWrites;
    std::set<size_t> SupersededWrites;
    std::map<size_t, BytecodeStateFunction::StoreOrigin> StoreOrigins;
    va_t PreviousPC = InvalidVA;
    unsigned InstructionOperation = 0;
    uint64_t Scratch = ScratchBase;
    auto Temp = [&](uint16_t Size = 8) {
      NdVar V = NdVar::tmp(Scratch, Size);
      Scratch += 8;
      return V;
    };
    auto Emit = [&](NdOp Opcode, NdVar Output,
                    std::initializer_list<NdVar> Inputs, va_t PC) {
      LowOp O;
      O.Opcode = Opcode;
      O.Output = Output;
      O.Addr = PC;
      O.Seq = Ops.size();
      for (NdVar V : Inputs)
        O.addInput(V);
      Ops.push_back(O);
    };
    auto Address = [&](uint64_t Offset, va_t PC) {
      NdVar V = Temp();
      Emit(NdOp::INT_ADD, V,
           {NdVar::tmp(StatePointer, 8), NdVar::scalar(Offset, 8)}, PC);
      return V;
    };
    if (Block.StartAddr == Function.Entry) {
      if (HasEntry)
        return invalid("duplicate entry block");
      HasEntry = true;
    }
    for (const LowOp &Original : Block.Ops) {
      if (Original.Addr != PreviousPC) {
        PreviousPC = Original.Addr;
        InstructionOperation = 0;
      } else
        ++InstructionOperation;
      if (auto Error = detail::validateBytecodeOperation(Original))
        return std::move(Error);
      if (Original.Opcode == NdOp::INDIR_CALL ||
          Original.Opcode == NdOp::INDIR_BR ||
          Original.Opcode == NdOp::INTRINSIC)
        return invalid("unbound call, indirect control or intrinsic");
      LowOp O = Original;
      if (O.NumInputs > std::size(O.Inputs) ||
          O.MemoryOrdering != NdMemoryOrdering::None ||
          O.MemoryAddressSpace != NdMemoryAddressSpace::Default)
        return invalid("unsupported operation metadata");
      if (O.Opcode == NdOp::CALL) {
        if (O.NumInputs != 1 || !O.Inputs[0].isConst() || O.Output.Size ||
            &Original != &Block.Ops.back() || Block.Succs.size() != 1 ||
            std::find(BoundCalls.begin(), BoundCalls.end(),
                      O.Inputs[0].Offset) == BoundCalls.end())
          return invalid("missing or malformed state call binding");
        HasCall = true;
        Emit(NdOp::COPY, NdVar::reg(Parameter, 8),
             {NdVar::tmp(StatePointer, 8)}, O.Addr);
        if (WithContext)
          Emit(NdOp::COPY, NdVar::reg(ContextParameter, 8),
               {NdVar::tmp(ContextPointer, 8)}, O.Addr);
        Emit(NdOp::CALL, NdVar::reg(Return, 8), {O.Inputs[0]}, O.Addr);
        Emit(NdOp::COPY, NdVar::tmp(CallStatus, 8), {NdVar::reg(Return, 8)},
             O.Addr);
        NdVar Failed = Temp(1);
        Emit(NdOp::INT_NOTEQUAL, Failed,
             {NdVar::tmp(CallStatus, 8), NdVar::scalar(0, 8)}, O.Addr);
        Emit(NdOp::COND_BR, {}, {NdVar::scalar(FailureAddress, 8), Failed},
             O.Addr);
        Block.Succs.insert(Block.Succs.begin(), FailureID);
        if (Ops.size() > MaxOperations - Count)
          return invalid("lowering operation budget exhausted");
        continue;
      }
      auto Check = [&](NdVar V, bool Output) {
        if (!V.Size)
          return Output;
        if (V.Size != 1 && V.Size != 2 && V.Size != 4 && V.Size != 8)
          return false;
        if (V.isReg())
          return V.Offset < RegisterBytes && V.Size <= RegisterBytes - V.Offset;
        if (V.isTemp())
          return V.Offset < StatePointer && V.Size <= StatePointer - V.Offset;
        return !Output && V.isConst();
      };
      if (!Check(O.Output, true))
        return invalid("invalid output storage");
      for (unsigned I = 0; I != O.NumInputs; ++I) {
        if (!Check(O.Inputs[I], false))
          return invalid("invalid input storage");
        if (O.Inputs[I].isReg()) {
          const auto Key = std::make_pair(O.Inputs[I].Offset, O.Inputs[I].Size);
          auto It = RegisterValues.find(Key);
          if (It == RegisterValues.end()) {
            // A different-width read can observe prior bytes even when an
            // exact view is cached. Keep stores visible to this real load.
            PendingStateWrites.clear();
            NdVar V = Temp(O.Inputs[I].Size);
            Emit(NdOp::LOAD, V, {Address(O.Inputs[I].Offset, O.Addr)}, O.Addr);
            It = RegisterValues.emplace(Key, V).first;
          }
          O.Inputs[I] = It->second;
        }
      }
      NdVar Output = O.Output;
      if (Output.isReg())
        O.Output = Temp(Output.Size);
      if (O.Opcode == NdOp::RETURN) {
        if (O.NumInputs || O.Output.Size)
          return invalid("return needs an explicit bytecode exit contract");
        Emit(NdOp::COPY, NdVar::reg(Return, 8), {NdVar::scalar(0, 8)}, O.Addr);
        O.addInput(NdVar::reg(Return, 8));
      }
      O.Seq = Ops.size();
      if (O.Opcode == NdOp::STORE)
        StoreOrigins.emplace(Ops.size(),
                             BytecodeStateFunction::StoreOrigin{
                                 O.Addr, InstructionOperation, O.Seq});
      Ops.push_back(O);
      if (O.Opcode == NdOp::LOAD || O.Opcode == NdOp::STORE)
        PendingStateWrites.clear();
      if (O.Opcode == NdOp::STORE)
        RegisterValues.clear();
      if (Output.isReg()) {
        // Valid views are at most eight bytes. Search only starts which can
        // overlap, rather than scanning the entire bank on every write.
        const uint64_t FirstOverlap = Output.Offset < 7 ? 0 : Output.Offset - 7;
        for (auto It = RegisterValues.lower_bound({FirstOverlap, 0});
             It != RegisterValues.end() &&
             It->first.first < Output.Offset + Output.Size;) {
          const auto [Offset, Size] = It->first;
          if (Offset < Output.Offset + Output.Size &&
              Output.Offset < Offset + Size)
            It = RegisterValues.erase(It);
          else
            ++It;
        }
        // Eliminate only whole writes overwritten before any actual memory
        // observer. Cached inputs already name captured values; no removed
        // store can be read by a guest access, call or partial state load.
        for (auto It = PendingStateWrites.lower_bound({Output.Offset, 0});
             It != PendingStateWrites.end() &&
             It->first.first < Output.Offset + Output.Size;) {
          const auto [Offset, Size] = It->first;
          if (Output.Offset <= Offset &&
              Offset + Size <= Output.Offset + Output.Size) {
            SupersededWrites.insert(It->second);
            It = PendingStateWrites.erase(It);
          } else {
            ++It;
          }
        }
        Emit(NdOp::STORE, {}, {Address(Output.Offset, O.Addr), O.Output},
             O.Addr);
        PendingStateWrites.emplace(std::make_pair(Output.Offset, Output.Size),
                                   Ops.size() - 1);
        RegisterValues.emplace(std::make_pair(Output.Offset, Output.Size),
                               O.Output);
      }
      if (Ops.size() > MaxOperations - Count)
        return invalid("lowering operation budget exhausted");
    }
    if (!SupersededWrites.empty()) {
      std::vector<LowOp> Retained;
      Retained.reserve(Ops.size() - SupersededWrites.size());
      for (size_t I = 0; I != Ops.size(); ++I)
        if (!SupersededWrites.count(I)) {
          Ops[I].Seq = Retained.size();
          if (auto At = StoreOrigins.find(I); At != StoreOrigins.end())
            At->second.LoweredSequence = Ops[I].Seq;
          Retained.push_back(std::move(Ops[I]));
        }
      Ops = std::move(Retained);
    }
    Count += Ops.size();
    for (const auto &[Index, Origin] : StoreOrigins) {
      (void)Index;
      Result.StoreOrigins.push_back(Origin);
    }
    Block.Ops = std::move(Ops);
    Block.InstructionBoundaries.clear();
  }
  if (!HasEntry)
    return invalid("missing entry block");
  if (HasCall) {
    if (Count > MaxOperations || MaxOperations - Count < 2)
      return invalid("lowering operation budget exhausted");
    LowBlock Failure;
    Failure.Id = FailureID;
    Failure.StartAddr = FailureAddress;
    Failure.EndAddr = FailureAddress + 1;
    LowOp Copy;
    Copy.Opcode = NdOp::COPY;
    Copy.Addr = FailureAddress;
    Copy.Output = NdVar::reg(Return, 8);
    Copy.addInput(NdVar::tmp(CallStatus, 8));
    LowOp Ret;
    Ret.Opcode = NdOp::RETURN;
    Ret.Addr = FailureAddress;
    Ret.Seq = 1;
    Ret.addInput(NdVar::reg(Return, 8));
    Failure.Ops = {Copy, Ret};
    for (const auto &B : Result.Function.Blocks)
      if (B.hasSucc(FailureID))
        Failure.Preds.push_back(B.Id);
    Result.Function.Blocks.push_back(std::move(Failure));
    Count += 2;
  }
  if (Count > MaxOperations || MaxOperations - Count < (WithContext ? 2u : 1u))
    return invalid("lowering operation budget exhausted");
  // Capture the parameter once, even when bytecode branches back to its first
  // instruction. Placing this copy in that instruction would read the native
  // return register after a call on each later loop iteration.
  for (auto &Block : Result.Function.Blocks) {
    ++Block.Id;
    for (int &P : Block.Preds)
      ++P;
    for (int &S : Block.Succs)
      ++S;
    for (auto &Op : Block.Ops)
      if ((Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR) &&
          Op.Inputs[0].isConst() && Op.Inputs[0].Offset == Function.Entry)
        Op.Inputs[0].Offset = BodyEntryAddress;
  }
  auto &BodyEntry = Result.Function.Blocks.front();
  BodyEntry.StartAddr = BodyEntryAddress;
  BodyEntry.EndAddr = BodyEntryAddress + 1;
  BodyEntry.Preds.push_back(0);
  LowBlock Entry;
  Entry.Id = 0;
  Entry.StartAddr = Function.Entry;
  Entry.EndAddr = Function.Entry + 1;
  Entry.Succs = {1};
  LowOp Capture;
  Capture.Opcode = NdOp::COPY;
  Capture.Addr = Function.Entry;
  Capture.Output = NdVar::tmp(StatePointer, 8);
  Capture.addInput(NdVar::reg(Parameter, 8));
  Entry.Ops.push_back(Capture);
  if (WithContext) {
    Capture.Seq = 1;
    Capture.Output = NdVar::tmp(ContextPointer, 8);
    Capture.Inputs[0] = NdVar::reg(ContextParameter, 8);
    Entry.Ops.push_back(Capture);
  }
  Result.Function.Blocks.insert(Result.Function.Blocks.begin(),
                                std::move(Entry));
  auto &ABI = Result.SourceABI;
  ABI.Origin = SourceFunctionTypeHint::OriginKind::ExplicitSource;
  ABI.Architecture = SourceArch;
  ABI.HasExplicitABI = true;
  ABI.ReturnType = NdType::makeInt(8, false);
  ABI.ReturnLocation = {SourceABICarrierKind::IntegerRegister, Return, 0, 8};
  SourceParameterTypeHint Argument;
  Argument.Name = "state";
  Argument.Type = NdType::makePtr(NdType::makeVoid());
  Argument.Location = {SourceABICarrierKind::IntegerRegister, Parameter, 0, 8};
  ABI.Parameters.push_back(std::move(Argument));
  if (WithContext) {
    SourceParameterTypeHint Context;
    Context.Name = "context";
    Context.Type = NdType::makePtr(NdType::makeVoid());
    Context.Location = {SourceABICarrierKind::IntegerRegister, ContextParameter,
                        0, 8};
    ABI.Parameters.push_back(std::move(Context));
  }
  std::string Diagnostic;
  if (!validateSourceABI(ABI, Diagnostic))
    return invalid(Diagnostic);
  return Result;
}
