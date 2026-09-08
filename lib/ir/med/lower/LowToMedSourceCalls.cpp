#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ObjC/ObjCBlockCallHints.h"
#include "neverd/loader/ObjC/ObjCCallHints.h"

#include <algorithm>
#include <optional>

namespace neverd {
namespace {
// A Darwin x64 CALL pushes the return address; a tail JMP reuses it. Source
// stack locations are relative to callee entry SP, not the caller's current SP.
std::optional<int64_t> returnAddressBias(const BinaryImage *Image,
                                         va_t Address) {
  if (!Image)
    return std::nullopt;
  const auto *P = Image->readVA(Address, 2);
  if (!P)
    return std::nullopt;
  if (P[0] == 0xe8)
    return 8;
  if (P[0] == 0xe9 || P[0] == 0xeb)
    return 0;
  // Optional REX prefix for indirect calls through an extended register.
  if ((P[0] & 0xf0) == 0x40) {
    P = Image->readVA(Address + 1, 2);
    if (!P)
      return std::nullopt;
  }
  if (P[0] == 0xff && ((P[1] >> 3) & 7) == 2)
    return 8;
  if (P[0] == 0xff && ((P[1] >> 3) & 7) == 4)
    return 0;
  return std::nullopt;
}
} // namespace

void LowToMedConverter::bindSourceCalls(MedFunc &Func, const LowFunc &Low,
                                        BinaryFormat Fmt) {
  if (!SourceCallHintsEnabled || Fmt != BinaryFormat::MachO ||
      (Image && (Image->IsRelocatable || Image->Arch != TargetArch)) ||
      (TargetArch != Arch::AArch64 && TargetArch != Arch::X64))
    return;
  auto Hints = Image ? buildObjCSourceCallHints(*Image, Low)
                     : std::map<va_t, SourceCallTypeHint>();
  if (Image) {
    const SourceFunctionTypeHint *EntrySignature = nullptr;
    if (SourceCalleeTypeHints)
      if (auto It = SourceCalleeTypeHints->find(Low.Entry);
          It != SourceCalleeTypeHints->end())
        EntrySignature = &It->second;
    auto BlockHints = buildObjCBlockCallHints(*Image, Low, EntrySignature);
    Hints.insert(BlockHints.begin(), BlockHints.end());
  }
  const auto &TRI = getTargetRegInfo(TargetArch);
  auto Temporary = [&](uint16_t Size) {
    MedVar V;
    V.Kind = MedVar::Temp;
    V.Id = allocVarId();
    V.Size = Size;
    V.TheArch = TargetArch;
    return V;
  };
  for (auto &Block : Func.Blocks) {
    std::vector<MedOp> Ops;
    for (auto Op : Block.Ops) {
      if ((Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL) ||
          Op.NumInputs != 1) {
        Ops.push_back(std::move(Op));
        continue;
      }
      std::optional<SourceCallTypeHint> Hint;
      if (auto It = Hints.find(Op.Addr); It != Hints.end())
        Hint = It->second;
      else if (SourceCalleeTypeHints && Op.Opcode == NdOp::CALL &&
               Op.Inputs[0].isConst()) {
        auto It = SourceCalleeTypeHints->find(Op.Inputs[0].ConstVal);
        if (It != SourceCalleeTypeHints->end()) {
          Hint.emplace();
          Hint->Signature = It->second;
          Hint->TargetAddress = It->first;
          // The real native name is resolved by the ordinary call ABI pass.
        }
      }
      std::string Diagnostic;
      if (!Hint || Hint->Signature.Architecture != TargetArch ||
          !validateSourceABI(Hint->Signature, Diagnostic)) {
        Ops.push_back(std::move(Op));
        continue;
      }
      const auto &Signature = Hint->Signature;
      const bool HasStack =
          std::any_of(Signature.Parameters.begin(), Signature.Parameters.end(),
                      [](const SourceParameterTypeHint &P) {
                        return P.Location.Kind == SourceABICarrierKind::Stack;
                      });
      std::optional<int64_t> Bias = 0;
      if (HasStack && TargetArch == Arch::X64)
        Bias = returnAddressBias(Image, Op.Addr);
      if (!Bias || Signature.Parameters.size() > 64) {
        Ops.push_back(std::move(Op));
        continue;
      }
      for (size_t I = 0; I < Signature.Parameters.size(); ++I) {
        const auto &Location = Signature.Parameters[I].Location;
        MedVar Argument;
        if (I == 1 && Hint->SelectorReferenceAddress) {
          MedOp Load;
          Load.Opcode = NdOp::LOAD;
          Load.Addr = Op.Addr;
          Load.Output = Temporary(8);
          Load.addInput(
              MedVar::makeConst(Hint->SelectorReferenceAddress, 8,
                                ConstantAddressProvenance::DataAddress));
          Argument = Load.Output;
          Ops.push_back(std::move(Load));
        } else if (Location.Kind == SourceABICarrierKind::Stack) {
          MedOp Address;
          Address.Opcode = NdOp::INT_ADD;
          Address.Addr = Op.Addr;
          Address.Output = Temporary(8);
          Address.addInput(ndVarToMedVar(NdVar::reg(TRI.StackPointer, 8)));
          Address.addInput(
              MedVar::makeConst(Location.EntryStackOffset - *Bias, 8,
                                ConstantAddressProvenance::Scalar));
          MedOp Load;
          Load.Opcode = NdOp::LOAD;
          Load.Addr = Op.Addr;
          Load.Output = Temporary(Location.ValueBytes);
          Load.addInput(Address.Output);
          Argument = Load.Output;
          Ops.push_back(std::move(Address));
          Ops.push_back(std::move(Load));
        } else {
          Argument = ndVarToMedVar(
              NdVar::reg(Location.RegisterOffset, Location.ValueBytes));
        }
        Op.addInput(Argument);
      }
      const auto &Return = Signature.ReturnLocation;
      Op.Output = Return.Kind == SourceABICarrierKind::None
                      ? MedVar()
                      : ndVarToMedVar(NdVar::reg(Return.RegisterOffset,
                                                 Return.ValueBytes));
      Op.SourceCallHint =
          std::make_shared<const SourceCallTypeHint>(std::move(*Hint));
      Ops.push_back(std::move(Op));
    }
    Block.Ops = std::move(Ops);
  }
}
} // namespace neverd
