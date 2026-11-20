//===- LowToMedGoResults.cpp - Go register ABI call results ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Models the results a call under Go's internal register ABI returns past
/// its first one.
///
//===----------------------------------------------------------------------===//

#include "LowToMedCallReturnDetail.h"

#include "neverd/ir/CallRegisterContract.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/GoCallContract.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <map>
#include <memory>
#include <tuple>
#include <vector>

namespace neverd {

void LowToMedConverter::modelGoCallResults(MedFunc &Func) {
  // Go's internal ABI (src/cmd/compile/abi-internal.md) returns a call's
  // integer results in RAX, RBX, RCX, RDI, RSI and R8-R11, in that order, and
  // keeps none of those registers but its results ("There are no callee-save
  // registers"): a read of one after the call, before any write, is a result,
  // and so is every register before it.  An indirect call compiled Go code
  // makes enters Go code by the internal ABI, as a function value always
  // does, and stands alone; a direct call shares its callee's shape with the
  // callee's other calls.
  if (TargetArch != Arch::X64 || !Image || !Image->ExceptionMetadata.GoModule ||
      !Image->ExceptionMetadata.GoModule->RegisterArguments)
    return;
  const llvm::ArrayRef<uint64_t> Results =
      goIntegerArgumentRegisters(TargetArch);
  const auto PositionOf = [&](uint64_t RegOff) {
    const auto *It = llvm::find(Results, RegOff);
    return It == Results.end() ? -1 : static_cast<int>(It - Results.begin());
  };
  std::map<uint32_t, std::vector<MedVar>> Clobbers;
  {
    llvm::DenseMap<uint32_t, bool> Calls;
    for (const MedBlock &Block : Func.Blocks)
      for (const MedOp &Op : Block.Ops)
        if (Op.Opcode == NdOp::INDIR_CALL &&
            Op.RegisterContract == CallRegisterContract::GoInternal &&
            goCallEntersInternalABI(*Image, Func.Entry, InvalidVA, nullptr) &&
            !Op.SourceCallHint && !Op.DoesNotReturn &&
            !Op.RegisterReturnFields && Op.Output.Kind == MedVar::Reg &&
            Op.Output.RegOff == x86reg::RAX)
          Calls[Op.CallSiteId] = true;
    for (const MedCallClobber &Clobber : Func.CallClobbers)
      if (Calls.count(Clobber.CallSiteId) &&
          Clobber.Value.Kind == MedVar::Reg &&
          PositionOf(Clobber.Value.RegOff) > 0)
        Clobbers[Clobber.CallSiteId].push_back(Clobber.Value);
  }
  if (Clobbers.empty())
    return;

  // The widths each value of a result register is read at, carried through
  // the PHIs it reaches.  A read that only keeps the bytes a partial write
  // leaves, a self-zeroing idiom, or a sub-register view takes no result.
  const auto Identity = [](const MedVar &V) {
    return std::tuple{V.Kind, V.Id, V.SSAVer};
  };
  using ValueKey = decltype(Identity(MedVar{}));
  llvm::DenseMap<ValueKey, uint16_t> ReadWidths;
  llvm::DenseMap<ValueKey, const PhiNode *> PhiDefinitions;
  llvm::SmallVector<ValueKey, 16> Work;
  const auto Observe = [&](const MedVar &Value, uint16_t Bytes) {
    if (Value.Kind != MedVar::Reg || PositionOf(Value.RegOff) <= 0)
      return;
    const ValueKey Key = Identity(Value);
    Bytes = std::min(Bytes, Value.Size);
    uint16_t &Width = ReadWidths[Key];
    if (Bytes > Width) {
      Width = Bytes;
      Work.push_back(Key);
    }
  };
  const auto SelfView = [](const MedOp &Op) {
    return Op.Opcode == NdOp::SUBBYTES && Op.NumInputs >= 1 &&
           Op.Inputs[0].Kind == MedVar::Reg &&
           Op.Inputs[0].RegOff == Op.Output.RegOff;
  };
  for (const MedBlock &Block : Func.Blocks) {
    for (const PhiNode &Phi : Block.Phis)
      if (Phi.Output.Kind == MedVar::Reg && PositionOf(Phi.Output.RegOff) > 0)
        PhiDefinitions.try_emplace(Identity(Phi.Output), &Phi);
    for (size_t I = 0; I < Block.Ops.size(); ++I) {
      const MedOp &Read = Block.Ops[I];
      if (SelfView(Read) ||
          lowtomed_detail::keepsUpperBytesOfWrite(Block.Ops, I) ||
          ((Read.Opcode == NdOp::INT_XOR || Read.Opcode == NdOp::INT_SUB) &&
           Read.NumInputs == 2 && Read.Inputs[0].Kind == Read.Inputs[1].Kind &&
           Read.Inputs[0].Id == Read.Inputs[1].Id &&
           Read.Inputs[0].SSAVer == Read.Inputs[1].SSAVer))
        continue;
      for (uint8_t J = 0; J < Read.NumInputs; ++J)
        Observe(Read.Inputs[J], Read.Inputs[J].Size);
    }
  }
  while (!Work.empty()) {
    const ValueKey Key = Work.pop_back_val();
    const auto Found = PhiDefinitions.find(Key);
    if (Found == PhiDefinitions.end())
      continue;
    const PhiNode &Phi = *Found->second;
    const uint16_t Width = std::min(ReadWidths[Key], Phi.Output.Size);
    for (const auto &[Pred, Input] : Phi.Args)
      Observe(Input, Width);
  }

  int NextVer = 1, NextId = 1;
  const auto Bump = [&](const MedVar &V) {
    NextVer = std::max(NextVer, V.SSAVer + 1);
    NextId = std::max(NextId, V.Id + 1);
  };
  for (const MedBlock &Block : Func.Blocks) {
    for (const MedOp &Op : Block.Ops) {
      Bump(Op.Output);
      for (uint8_t I = 0; I < Op.NumInputs; ++I)
        Bump(Op.Inputs[I]);
    }
    for (const PhiNode &Phi : Block.Phis) {
      Bump(Phi.Output);
      for (const auto &[Pred, Input] : Phi.Args)
        Bump(Input);
    }
  }

  const uint16_t Word = getTargetRegInfo(TargetArch).PointerSize;
  for (MedBlock &Block : Func.Blocks)
    for (size_t OI = 0; OI < Block.Ops.size(); ++OI) {
      MedOp &Op = Block.Ops[OI];
      if (Op.Opcode != NdOp::INDIR_CALL)
        continue;
      const auto Found = Clobbers.find(Op.CallSiteId);
      if (Found == Clobbers.end())
        continue;
      int Last = 0;
      for (const MedVar &Value : Found->second)
        if (ReadWidths.count(Identity(Value)))
          Last = std::max(Last, PositionOf(Value.RegOff));
      if (Last == 0)
        continue;
      // The call returns the packed results; each result register, at every
      // width the call left it at, is defined from its field, so the reads
      // the call reached keep their values, in any block and through PHIs.
      const MedVar First = Op.Output;
      MedVar Packed;
      Packed.Kind = MedVar::Temp;
      Packed.TheArch = TargetArch;
      Packed.Id = NextId++;
      Packed.SSAVer = 0;
      Packed.Size = static_cast<uint16_t>(Word * (Last + 1));
      Op.Output = Packed;
      auto Fields = std::make_shared<std::vector<MedReturnReg>>();
      std::vector<MedOp> Extracts;
      for (int Position = 0; Position <= Last; ++Position) {
        const uint64_t Register = Results[Position];
        Fields->push_back({Register, Word, false});
        MedVar Field;
        if (Position == 0) {
          Field = First;
        } else {
          const MedVar *Exact = nullptr;
          for (const MedVar &Value : Found->second)
            if (Value.RegOff == Register && (!Exact || Value.Size == Word))
              Exact = &Value;
          if (Exact) {
            Field = *Exact;
          } else {
            Field.Kind = MedVar::Reg;
            Field.TheArch = TargetArch;
            Field.RegOff = Register;
            Field.Id = NextId++;
            Field.SSAVer = NextVer++;
          }
        }
        Field.Size = Word;
        MedOp Extract;
        Extract.Opcode = NdOp::SUBBYTES;
        Extract.Addr = Op.Addr;
        Extract.Output = Field;
        Extract.addInput(Packed);
        Extract.addInput(MedVar::makeConst(Word * Position, Word));
        Extracts.push_back(Extract);
        for (const MedVar &Value : Found->second) {
          if (Value.RegOff != Register || Identity(Value) == Identity(Field))
            continue;
          MedOp Alias;
          Alias.Addr = Op.Addr;
          Alias.Output = Value;
          Alias.Opcode = Value.Size < Word   ? NdOp::SUBBYTES
                         : Value.Size > Word ? NdOp::INT_ZEXT
                                             : NdOp::COPY;
          Alias.addInput(Field);
          if (Alias.Opcode == NdOp::SUBBYTES)
            Alias.addInput(MedVar::makeConst(0, Word));
          Extracts.push_back(Alias);
        }
      }
      Op.RegisterReturnFields = std::move(Fields);
      const uint32_t Site = Op.CallSiteId;
      llvm::erase_if(Func.CallClobbers, [&](const MedCallClobber &Clobber) {
        return Clobber.CallSiteId == Site &&
               Clobber.Value.Kind == MedVar::Reg &&
               PositionOf(Clobber.Value.RegOff) > 0 &&
               PositionOf(Clobber.Value.RegOff) <= Last;
      });
      Block.Ops.insert(Block.Ops.begin() + OI + 1, Extracts.begin(),
                       Extracts.end());
      OI += Extracts.size();
    }
}

} // namespace neverd
