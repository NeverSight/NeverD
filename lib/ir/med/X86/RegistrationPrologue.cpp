//===- RegistrationPrologue.cpp - Physical C++ entry projection
//------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationPrologue.h"

#include "neverd/ir/low/RegistrationSharedCall.h"
#include "neverd/lift/X86/X86Lifter.h"

#include <algorithm>

namespace neverd::x86_registration {

std::optional<LowFunc> projectCxxPrologue(const BinaryImage &Image,
                                          const LowFunc &Source) {
  const auto Call = getCheckedX86RegistrationCxxProloguePrefix(Image, Source);
  if (!Call)
    return std::nullopt;
  const auto Entry = std::find_if(
      Source.Blocks.begin(), Source.Blocks.end(),
      [&](const auto &Block) { return Block.StartAddr == Source.Entry; });

  // Keep synthetic scratch storage disjoint from every source temporary,
  // including values that survive across source instruction boundaries.
  uint64_t Next = TmpBase;
  auto Reserve = [&](const NdVar &Value) {
    if (!Value.isTemp())
      return true;
    if (Value.Offset > UINT64_MAX - Value.Size - 4096)
      return false;
    Next = std::max(Next, Value.Offset + Value.Size);
    return true;
  };
  for (const auto &Block : Source.Blocks)
    for (const auto &Op : Block.Ops) {
      if (!Reserve(Op.Output))
        return std::nullopt;
      for (unsigned I = 0; I != Op.NumInputs; ++I)
        if (!Reserve(Op.Inputs[I]))
          return std::nullopt;
    }

  std::vector<LowOp> Ops;
  X86Lifter::LiftState S(Call->CallVA, 5, Ops);
  S.TmpId = (Next - TmpBase + TmpStride - 1) / TmpStride;
  const auto SP = NdVar::reg(x86reg::RSP, 4);
  const auto FP = NdVar::reg(x86reg::RBP, 4);
  const auto AX = NdVar::reg(x86reg::RAX, 4);
  const auto OldFP = S.makeTemp(4);
  S.emit(NdOp::COPY, OldFP, {FP});
  // The allocation PUSH already placed ESP at entry ESP - 4, the helper's
  // eventual EBP. All offsets below come from the loader's checked protocol.
  S.emit(NdOp::COPY, FP, {SP});
  auto Address = [&](int32_t Offset) {
    const auto Result = S.makeTemp(4);
    S.emit(NdOp::INT_ADD, Result,
           {FP, NdVar::scalar(uint64_t(int64_t(Offset)), 4)});
    return Result;
  };
  const auto ReturnPC = NdVar::codeAddress(Call->Caller.End, 4, Source.Entry);
  S.emit(NdOp::STORE, {}, {Address(-4), ReturnPC});
  const auto Chain = S.makeTemp(4);
  const auto Cookie = S.makeTemp(4);
  const auto EncodedCookie = S.makeTemp(4);
  bool ReadCookie = false;
  const auto FinalSP = Address(Call->PostCallStackOffset);
  using Value = coff_loader::X86CxxPrologueWrite::Value;
  for (const auto &Write : Call->Writes) {
    NdVar Input;
    switch (Write.Source) {
    case Value::PreviousEBP:
      Input = OldFP;
      break;
    case Value::Handler:
      Input = NdVar::codeAddress(Call->HandlerVA, 4);
      break;
    case Value::PreviousChain:
      S.emit(NdOp::LOAD, Chain, {NdVar::scalar(0, 4)}, NdMemoryOrdering::None,
             NdMemoryAddressSpace::X86FS);
      Input = Chain;
      break;
    case Value::EBX:
      Input = NdVar::reg(x86reg::RBX, 4);
      break;
    case Value::ESI:
      Input = NdVar::reg(x86reg::RSI, 4);
      break;
    case Value::EDI:
      Input = NdVar::reg(x86reg::RDI, 4);
      break;
    case Value::EncodedCookie:
      if (!ReadCookie) {
        S.emit(NdOp::LOAD, Cookie,
               {NdVar::dataAddress(Call->SecurityCookieVA, 4)});
        S.emit(NdOp::INT_XOR, EncodedCookie, {Cookie, FP});
        X86Lifter::emitFlagsLogic(S, EncodedCookie);
        ReadCookie = true;
      }
      Input = EncodedCookie;
      break;
    case Value::StackPointer:
      Input = FinalSP;
      break;
    case Value::InitialState:
      // The helper moves its return PC before replacing the original word.
      // Retain even the now-dead word below ESP; no liveness claim erases it.
      S.emit(NdOp::STORE, {},
             {Address(Call->PostCallStackOffset - 4), ReturnPC});
      Input = NdVar::scalar(UINT32_MAX, 4);
      break;
    }
    S.emit(NdOp::STORE, {}, {Address(Write.FrameOffset), Input});
  }
  S.emit(NdOp::COPY, SP, {FinalSP});
  S.emit(NdOp::COPY, AX, {Address(Call->RegistrationOffset)});
  S.emit(NdOp::STORE, {}, {NdVar::scalar(0, 4), AX}, NdMemoryOrdering::None,
         NdMemoryAddressSpace::X86FS);

  LowFunc Result = Source;
  auto &Block = Result.Blocks[Entry - Source.Blocks.begin()];
  Block.Ops.pop_back();
  Block.Ops.insert(Block.Ops.end(), Ops.begin(), Ops.end());
  Block.InstructionBoundaries.back().OpCount = Ops.size();
  return Result;
}

} // namespace neverd::x86_registration
