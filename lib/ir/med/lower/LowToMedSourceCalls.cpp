#include "../../../loader/Swift/SwiftBooleanProjection.h"
#include "../../../loader/Swift/SwiftBooleanSourceBinding.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ObjC/ObjCBlockCallHints.h"
#include "neverd/loader/ObjC/ObjCCallHints.h"
#include "neverd/loader/Swift/SwiftValueWitnessCalls.h"

#include <algorithm>
#include <optional>
#include <set>

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
    auto SwiftHints = buildSwiftValueWitnessCallHints(*Image, Low);
    for (auto &[Address, Hint] : SwiftHints) {
      auto [It, Inserted] = Hints.emplace(Address, std::move(Hint));
      if (!Inserted)
        Hints.erase(It);
    }
  }
  const SourceFunctionTypeHint *EntrySignature = nullptr;
  // Standalone clients historically supplied one shared map. The pipeline
  // supplies a distinct entry map so call-only thunk overrides cannot rewrite
  // the machine body's return and live-in contract.
  const auto *EntryHints =
      SourceEntryTypeHints ? SourceEntryTypeHints : SourceCalleeTypeHints;
  if (EntryHints)
    if (auto It = EntryHints->find(Low.Entry); It != EntryHints->end()) {
      std::string Diagnostic;
      if (It->second.Architecture == TargetArch &&
          validateSourceABI(It->second, Diagnostic))
        EntrySignature = &It->second;
    }
  if (Image) {
    auto BlockHints = buildObjCBlockCallHints(*Image, Low, EntrySignature);
    Hints.insert(BlockHints.begin(), BlockHints.end());
  }
  if (Image && EntrySignature) {
    const auto Booleans =
        qualifySwiftBooleanProjections(*Image, Low, *EntrySignature);
    const auto Signature = swiftBooleanNormalizedSignature();
    for (const auto &Boolean : Booleans) {
      if (!Signature)
        continue;
      SourceCallTypeHint Hint;
      Hint.CallKind = SourceCallTypeHint::Kind::SwiftBooleanProjection;
      Hint.BooleanResult = SourceCallTypeHint::BooleanResultProjection{
          Low.Entry, Boolean.Normalization.Site};
      Hint.Signature = *Signature;
      Hint.TargetAddress = Boolean.Runtime.ImportSlot;
      Hint.TargetName = SwiftBooleanComparisonImport.drop_front().str();
      Hints.emplace(Boolean.Normalization.Site.Instruction, std::move(Hint));
    }
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
      if (Op.Opcode == NdOp::RETURN && EntrySignature &&
          EntrySignature->ReturnType->Kind == NdTypeKind::Void) {
        // RET reads the architecture's conventional result register in the
        // generic LowIR model.  An authenticated source signature returning
        // void makes that machine value unobservable; keeping it would retain
        // an undefined caller-saved carrier and reject an otherwise complete
        // source projection.
        Op.NumInputs = 0;
      } else if (Op.Opcode == NdOp::RETURN && EntrySignature &&
                 EntrySignature->ReturnType->Kind == NdTypeKind::Struct &&
                 EntrySignature->ReturnLocation.Kind !=
                     SourceABICarrierKind::IndirectResultPointer) {
        Op.NumInputs = 0;
        for (const auto &Piece : EntrySignature->ReturnComponents)
          Op.addInput(ndVarToMedVar(
              NdVar::reg(Piece.RegisterOffset, Piece.ValueBytes)));
      } else if (Op.Opcode == NdOp::RETURN && EntrySignature &&
                 !EntrySignature->ReturnComponents.empty()) {
        // Publish both physical dependencies before SSA. They are separate
        // registers, not one contiguous register-bank slice (notably RAX/RDX).
        MedOp Result;
        Result.Opcode = NdOp::CONCAT;
        Result.Addr = Op.Addr;
        Result.Output = Temporary(EntrySignature->ReturnType->Size);
        for (const auto I : {1U, 0U}) {
          const auto &Piece = EntrySignature->ReturnComponents[I];
          Result.addInput(ndVarToMedVar(
              NdVar::reg(Piece.RegisterOffset, Piece.ValueBytes)));
        }
        Op.NumInputs = 0;
        Op.addInput(Result.Output);
        Ops.push_back(std::move(Result));
      } else if (Op.Opcode == NdOp::RETURN && EntrySignature &&
                 EntrySignature->ReturnLocation.Kind ==
                     SourceABICarrierKind::IntegerRegister) {
        // RET itself does not read X0/RAX. Publish the declared source lane
        // before SSA so PHIs carry its low bytes without requiring unknown
        // upper bits from a narrow call result.
        const auto &Return = EntrySignature->ReturnLocation;
        Op.NumInputs = 0;
        Op.addInput(ndVarToMedVar(
            NdVar::reg(Return.RegisterOffset, Return.ValueBytes)));
      }
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
      if (Hint->BooleanResult &&
          (!isSwiftBooleanSourceBinding(*Hint) ||
           Hint->BooleanResult->FunctionEntry != Low.Entry ||
           Hint->BooleanResult->Site.Instruction != Op.Addr ||
           Hint->BooleanResult->Site.Sequence != Op.OriginSeq ||
           Hint->BooleanResult->Site.Opcode != Op.Opcode ||
           !Op.Inputs[0].isConst() ||
           Hint->BooleanResult->Site.StaticTarget != Op.Inputs[0].ConstVal)) {
        Ops.push_back(std::move(Op));
        continue;
      }
      const auto &Signature = Hint->Signature;
      const auto Parameters = sourceABIParameters(Signature);
      const bool HasStack =
          std::any_of(Parameters.begin(), Parameters.end(),
                      [](const SourceABIParameter &P) {
                        return P.Location.Kind == SourceABICarrierKind::Stack;
                      });
      std::optional<int64_t> Bias = 0;
      if (HasStack && TargetArch == Arch::X64)
        Bias = returnAddressBias(Image, Op.Addr);
      if (!Bias || Parameters.size() > 64) {
        Ops.push_back(std::move(Op));
        continue;
      }
      for (size_t I = 0; I < Parameters.size(); ++I) {
        const auto &Location = Parameters[I].Location;
        MedVar Argument;
        if (Parameters[I].ParameterIndex == 1 &&
            Hint->SelectorReferenceAddress) {
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
        } else if (TargetArch == Arch::AArch64 &&
                   Location.Kind == SourceABICarrierKind::IntegerRegister &&
                   Location.ValueBytes < 4) {
          // AArch64 writes to Wn define the complete 32-bit register, even
          // when the source value carried by a call is only a byte or half.
          // Publish that architectural carrier before SSA, then extract the
          // declared source lane. Reading the narrow physical alias directly
          // would create a separate SSA web that misses converging Wn writes.
          MedOp Extract;
          Extract.Opcode = NdOp::SUBBYTES;
          Extract.Addr = Op.Addr;
          Extract.Output = Temporary(Location.ValueBytes);
          Extract.addInput(
              ndVarToMedVar(NdVar::reg(Location.RegisterOffset, 4)));
          Extract.addInput(
              MedVar::makeConst(0, 4, ConstantAddressProvenance::Scalar));
          Argument = Extract.Output;
          Ops.push_back(std::move(Extract));
        } else {
          Argument = ndVarToMedVar(
              NdVar::reg(Location.RegisterOffset, Location.ValueBytes));
        }
        Op.addInput(Argument);
      }
      const auto &Return = Signature.ReturnLocation;
      const bool IndirectResult =
          Return.Kind == SourceABICarrierKind::IndirectResultPointer;
      Op.Output = Return.Kind == SourceABICarrierKind::None || IndirectResult
                      ? MedVar()
                      : ndVarToMedVar(NdVar::reg(Return.RegisterOffset,
                                                 Return.ValueBytes));
      std::vector<MedOp> ReturnOps;
      if (IndirectResult) {
        // Preserve the pre-call hidden pointer before SSA introduces the
        // caller-saved x8 clobber. A declared result initializes every record
        // field; the logical call keeps the ordinary source parameter list.
        MedOp Address;
        Address.Opcode = NdOp::COPY;
        Address.Addr = Op.Addr;
        Address.Output = Temporary(8);
        Address.addInput(ndVarToMedVar(NdVar::reg(Return.RegisterOffset, 8)));
        const auto Buffer = Address.Output;
        Ops.push_back(std::move(Address));
        Op.Output = Temporary(Signature.ReturnType->Size);
        for (const auto &Member :
             sourceAggregateMembers(Signature.ReturnType)) {
          MedOp Extract;
          Extract.Opcode = NdOp::SUBBYTES;
          Extract.Addr = Op.Addr;
          Extract.Output = Temporary(Member.Type->Size);
          Extract.addInput(Op.Output);
          Extract.addInput(MedVar::makeConst(Member.ByteOffset, 4));
          MedOp Field;
          Field.Opcode = NdOp::INT_ADD;
          Field.Addr = Op.Addr;
          Field.Output = Temporary(8);
          Field.addInput(Buffer);
          Field.addInput(MedVar::makeConst(Member.ByteOffset, 8));
          MedOp Store;
          Store.Opcode = NdOp::STORE;
          Store.Addr = Op.Addr;
          Store.addInput(Field.Output);
          Store.addInput(Extract.Output);
          ReturnOps.push_back(std::move(Extract));
          ReturnOps.push_back(std::move(Field));
          ReturnOps.push_back(std::move(Store));
        }
      } else if (!Signature.ReturnComponents.empty()) {
        Op.Output = Temporary(Signature.ReturnType->Size);
        const auto Members = sourceAggregateMembers(Signature.ReturnType);
        for (size_t I = 0; I < Signature.ReturnComponents.size(); ++I) {
          const auto &Piece = Signature.ReturnComponents[I];
          MedOp Extract;
          Extract.Opcode = NdOp::SUBBYTES;
          Extract.Addr = Op.Addr;
          Extract.Output =
              ndVarToMedVar(NdVar::reg(Piece.RegisterOffset, Piece.ValueBytes));
          Extract.addInput(Op.Output);
          Extract.addInput(MedVar::makeConst(
              Members.empty() ? I * Piece.ValueBytes : Members[I].ByteOffset,
              4));
          ReturnOps.push_back(std::move(Extract));
        }
      } else if (Hint->CallKind ==
                 SourceCallTypeHint::Kind::SwiftBooleanProjection) {
        // Only this exact occurrence has a complete caller proof that zeroing
        // undefined bits is unobservable. Ordinary narrow returns retain their
        // unknown upper-byte dependency below.
        Op.Output = Temporary(1);
        MedOp Extend;
        Extend.Opcode = NdOp::INT_ZEXT;
        Extend.Addr = Op.Addr;
        Extend.Output = ndVarToMedVar(NdVar::reg(Return.RegisterOffset, 8));
        Extend.addInput(Op.Output);
        ReturnOps.push_back(std::move(Extend));
      } else if (Return.Kind == SourceABICarrierKind::IntegerRegister &&
                 Return.ValueBytes < 8) {
        // A scalar ABI result does not define the rest of its register. Keep
        // those bits tied to this call's unknown clobber instead of either
        // preserving the pre-call input or inventing an architectural write.
        Op.Output = Temporary(Return.ValueBytes);
        MedVar Value = Op.Output;
        if (Return.ExtendTo32Bits) {
          MedOp Extend;
          Extend.Opcode =
              Signature.ReturnType->IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT;
          Extend.Addr = Op.Addr;
          Extend.Output = Temporary(4);
          Extend.addInput(Value);
          Value = Extend.Output;
          ReturnOps.push_back(std::move(Extend));
        }
        MedOp Upper;
        Upper.Opcode = NdOp::SUBBYTES;
        Upper.Addr = Op.Addr;
        Upper.Output = Temporary(8 - Value.Size);
        Upper.addInput(ndVarToMedVar(NdVar::reg(Return.RegisterOffset, 8)));
        Upper.addInput(MedVar::makeConst(Value.Size, 4));
        MedOp Merge;
        Merge.Opcode = NdOp::CONCAT;
        Merge.Addr = Op.Addr;
        Merge.Output = ndVarToMedVar(NdVar::reg(Return.RegisterOffset, 8));
        Merge.addInput(Upper.Output);
        Merge.addInput(Value);
        ReturnOps.push_back(std::move(Upper));
        ReturnOps.push_back(std::move(Merge));
      } else if (Return.Kind == SourceABICarrierKind::FloatingRegister &&
                 TRI.isVectorReg(Return.RegisterOffset)) {
        auto [WideOffset, WideBytes] =
            TRI.findWideReg(Return.RegisterOffset, Return.ValueBytes);
        if (TRI.FPABIRegWidth)
          WideBytes = std::min(WideBytes, TRI.FPABIRegWidth);
        if (WideBytes > Return.ValueBytes &&
            TRI.subRegByteOffset(Return.RegisterOffset, Return.ValueBytes,
                                 WideOffset, WideBytes) == 0) {
          // A scalar floating result defines only the low lane of its vector
          // return carrier. Preserve that exact dependency when a later
          // instruction copies the full vector: the suffix is this call's
          // unknown clobber, not the pre-call value, while the prefix remains
          // available to a later narrow source projection.
          MedOp Upper;
          Upper.Opcode = NdOp::SUBBYTES;
          Upper.Addr = Op.Addr;
          Upper.Output = Temporary(WideBytes - Return.ValueBytes);
          Upper.addInput(
              ndVarToMedVar(NdVar::reg(WideOffset, WideBytes)));
          Upper.addInput(MedVar::makeConst(Return.ValueBytes, 4));
          MedOp Merge;
          Merge.Opcode = NdOp::CONCAT;
          Merge.Addr = Op.Addr;
          Merge.Output =
              ndVarToMedVar(NdVar::reg(WideOffset, WideBytes));
          Merge.addInput(Upper.Output);
          Merge.addInput(Op.Output);
          ReturnOps.push_back(std::move(Upper));
          ReturnOps.push_back(std::move(Merge));
        }
      }
      // These hints were matched to an exact imported runtime declaration by
      // the loader and passed ABI validation above. Carry its termination
      // effect into MedIR as well as source flow. Native candidate signatures
      // are not declarations of this effect and use the separate fixed point.
      if (Hint->DoesNotReturn &&
          Signature.ReturnType->Kind == NdTypeKind::Void &&
          (Hint->CallKind == SourceCallTypeHint::Kind::ObjCRuntimeCall ||
           Hint->CallKind == SourceCallTypeHint::Kind::DarwinRuntimeCall ||
           Hint->CallKind == SourceCallTypeHint::Kind::SwiftRuntimeCall))
        Op.DoesNotReturn = true;
      Op.SourceCallHint =
          std::make_shared<const SourceCallTypeHint>(std::move(*Hint));
      Ops.push_back(std::move(Op));
      for (auto &ReturnOp : ReturnOps)
        Ops.push_back(std::move(ReturnOp));
    }
    Block.Ops = std::move(Ops);
  }

  if (!EntrySignature ||
      EntrySignature->ReturnLocation.Kind !=
          SourceABICarrierKind::IntegerRegister ||
      EntrySignature->ReturnLocation.ValueBytes >= 8)
    return;
  const auto Bytes = EntrySignature->ReturnLocation.ValueBytes;
  std::set<uint64_t> ReturnRegisters{
      EntrySignature->ReturnLocation.RegisterOffset};
  auto WideCopy = [&](const MedOp &Op) {
    return Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
           Op.Output.Kind == MedVar::Reg && Op.Inputs[0].Kind == MedVar::Reg &&
           Op.Output.Size > Bytes &&
           (Op.Output.Size == 2 || Op.Output.Size == 4 ||
            Op.Output.Size == 8) &&
           Op.Inputs[0].Size == Op.Output.Size &&
           !TRI.isFrameOrLinkReg(Op.Output.RegOff) &&
           !TRI.isStackPointer(Op.Output.RegOff) &&
           !TRI.isVectorReg(Op.Output.RegOff) &&
           !TRI.isFrameOrLinkReg(Op.Inputs[0].RegOff) &&
           !TRI.isStackPointer(Op.Inputs[0].RegOff) &&
           !TRI.isVectorReg(Op.Inputs[0].RegOff);
  };
  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (const auto &Block : Func.Blocks)
      for (const auto &Op : Block.Ops)
        if (WideCopy(Op) && ReturnRegisters.count(Op.Output.RegOff))
          Changed |= ReturnRegisters.insert(Op.Inputs[0].RegOff).second;
  }
  for (auto &Block : Func.Blocks) {
    std::vector<MedOp> Ops;
    for (auto Op : Block.Ops) {
      if (WideCopy(Op) && ReturnRegisters.count(Op.Output.RegOff)) {
        // A saved full register can carry a narrow result through several
        // merges before the final return. Expose its low-lane dependency now
        // so subregister normalization builds narrow PHIs along that path.
        // CONCAT preserves every upper bit; it is not a narrow register write.
        const auto Input = Op.Inputs[0];
        MedOp Low;
        Low.Opcode = NdOp::SUBBYTES;
        Low.Addr = Op.Addr;
        Low.Output = Temporary(Bytes);
        Low.addInput(ndVarToMedVar(NdVar::reg(Input.RegOff, Bytes)));
        Low.addInput(MedVar::makeConst(0, 4));
        MedVar Value = Low.Output;
        Ops.push_back(std::move(Low));
        // Reassemble with ordinary 1/2/4/8-byte values. A seven-byte "high"
        // would leak a nonexistent uint56_t into C when this register also
        // carries a pointer along another path.
        for (uint16_t Width = Bytes; Width < Input.Size; Width *= 2) {
          MedOp Upper;
          Upper.Opcode = NdOp::SUBBYTES;
          Upper.Addr = Op.Addr;
          Upper.Output = Temporary(Width);
          Upper.addInput(Input);
          Upper.addInput(MedVar::makeConst(Width, 4));
          const bool Last = Width * 2 == Input.Size;
          MedOp Piece;
          Piece.Opcode = NdOp::CONCAT;
          Piece.Addr = Op.Addr;
          Piece.Output = Last ? Op.Output : Temporary(Width * 2);
          Piece.addInput(Upper.Output);
          Piece.addInput(Value);
          Value = Piece.Output;
          Ops.push_back(std::move(Upper));
          if (Last) {
            Op.Opcode = Piece.Opcode;
            Op.NumInputs = 0;
            Op.addInput(Piece.Inputs[0]);
            Op.addInput(Piece.Inputs[1]);
          } else {
            Ops.push_back(std::move(Piece));
          }
        }
      }
      Ops.push_back(std::move(Op));
    }
    Block.Ops = std::move(Ops);
  }
}
} // namespace neverd
