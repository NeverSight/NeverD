//===- MIPS16EntryAdapter.cpp - Proven classic FP entry transport --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Replay the lifter's pure register transfers, including the actual delay
/// slot, and compare every parameter byte with the shared ABI placements.
//===----------------------------------------------------------------------===//

#include "MIPS16EntryAdapter.h"

#include "neverd/ArchSupport.h"
#include "neverd/ir/low/MIPS/MIPS16EntryTransport.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/libc/LibCFloatingArguments.h"
#include "neverd/lift/MIPS/MIPSRegs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ExecutableCodeOwnerIndex.h"
#include "neverd/loader/FunctionNames.h"

#include "llvm/ADT/DenseMap.h"

#include <algorithm>
#include <map>
#include <vector>

namespace neverd::pipeline_detail {

std::optional<MIPS16EntryAdapter>
proveMIPS16EntryAdapter(const BinaryImage &Image, va_t Entry,
                        const DebugContext &Debug,
                        const ExecutableCodeOwnerIndex *Owners) {
  // An exact declaration for this entry has precedence over inferred adapter
  // identity. A containing function or name-only record supplies no contract.
  if (const auto Declared = Debug.resolveFunction(Entry);
      Declared && Declared->Addr == Entry &&
      libc::hasFixedPlatformCallDeclaration(*Declared, &Debug))
    return std::nullopt;
  const auto Transport = proveMIPS16EntryTransport(Image, Entry, Owners);
  if (!Transport)
    return std::nullopt;
  const va_t Body = Transport->Body;
  const auto Declaration = Debug.resolveFunction(Body);
  if (!Declaration || Declaration->Addr != Body)
    return std::nullopt;
  auto Incoming = libc::floatingCallArguments(*Declaration, Image.Arch,
                                              Image.abiFormat(), &Debug);
  auto Outgoing =
      libc::floatingCallArguments(*Declaration, Image.Arch, Image.abiFormat(),
                                  &Debug, InstructionMode::MIPS16);
  if (!Incoming || !Outgoing || Incoming->ReturnKind == NdTypeKind::Float ||
      Incoming->ReturnElements)
    return std::nullopt;
  const auto Types =
      floatingCallTypes(Image.Arch, Image.abiFormat(), *Incoming);
  if (!Types)
    return std::nullopt;
  const auto Before = placeSourceParameters(Image.Arch, Image.abiFormat(),
                                            Types->Result, Types->Parameters);
  const auto After = placeSourceParameters(
      Image.Arch, Image.abiFormat(), Types->Result, Types->Parameters, false,
      std::nullopt, InstructionMode::MIPS16);
  if (!Before || !After || Before->ResultPointer || After->ResultPointer ||
      Before->Parameters.size() != Types->Parameters.size() ||
      After->Parameters.size() != Types->Parameters.size())
    return std::nullopt;
  bool FloatingTransport = false;
  for (size_t P = 0; P < After->Parameters.size(); ++P)
    for (const auto &Destination : After->Parameters[P]) {
      if (Destination.Indirect)
        return std::nullopt;
      for (unsigned I = 0; I < Destination.Location.ValueBytes; ++I) {
        const unsigned Byte = Destination.Offset + I;
        const auto Source = std::find_if(
            Before->Parameters[P].begin(), Before->Parameters[P].end(),
            [&](const auto &Piece) {
              return !Piece.Indirect && Byte >= Piece.Offset &&
                     Byte - Piece.Offset < Piece.Location.ValueBytes;
            });
        if (Source == Before->Parameters[P].end())
          return std::nullopt;
        const auto &From = Source->Location;
        const auto &To = Destination.Location;
        unsigned Part = Byte - Source->Offset;
        if (To.Kind == SourceABICarrierKind::Stack) {
          if (From.Kind != To.Kind ||
              From.EntryStackOffset + Part != To.EntryStackOffset + I)
            return std::nullopt;
          continue;
        }
        if (To.Kind != SourceABICarrierKind::IntegerRegister ||
            (From.Kind != SourceABICarrierKind::IntegerRegister &&
             From.Kind != SourceABICarrierKind::FloatingRegister))
          return std::nullopt;
        uint64_t Register = From.RegisterOffset;
        if (From.Kind == SourceABICarrierKind::FloatingRegister) {
          FloatingTransport = true;
          if (Image.MIPS->hasScalarFP32ABI(false) && Part >= 4) {
            Register += mipsreg::VectorBytes;
            Part -= 4;
          }
        }
        if (To.RegisterOffset < mipsreg::r(4) ||
            To.RegisterOffset > mipsreg::r(7) ||
            To.RegisterOffset % mipsreg::SlotBytes || I >= 4)
          return std::nullopt;
        const auto Actual =
            Transport->ArgumentBytes
                [(To.RegisterOffset / mipsreg::SlotBytes - 4) * 4 + I];
        if (!Actual || *Actual != Register + Part)
          return std::nullopt;
      }
    }
  if (!FloatingTransport)
    return std::nullopt;
  return MIPS16EntryAdapter{
      Body, std::move(*Incoming), std::move(*Outgoing),
      Declaration->PrototypeLibrary == UserPrototypeLibrary, Declaration->Name};
}

void projectMIPS16EntryCalls(const BinaryImage &Image,
                             std::vector<MedFunc> &Functions,
                             const DebugContext *Debug) {
  if (!Debug || !isMIPS(Image.Arch) || isMIPS64(Image.Arch) || !Image.MIPS ||
      !Image.MIPS->HasMIPS16)
    return;
  llvm::DenseMap<va_t, std::optional<MIPS16EntryAdapter>> Adapters;
  std::optional<ExecutableCodeOwnerIndex> Owners;
  std::optional<std::map<va_t, std::string>> Names;
  for (MedFunc &Function : Functions) {
    llvm::DenseMap<std::pair<int, int>, MedCallInfo *> Calls;
    for (auto &Call : Function.CallInfos)
      Calls.try_emplace({Call.BlockId, Call.OpIdx}, &Call);
    for (auto &Block : Function.Blocks)
      for (size_t I = 0; I < Block.Ops.size(); ++I) {
        auto &Op = Block.Ops[I];
        if (Op.Dead || Op.Opcode != NdOp::CALL || Op.SourceCallHint ||
            !Op.ScalarCallSignature ||
            Op.ScalarCallSignature->MIPS16Parameters || Op.NumInputs == 0 ||
            !Op.Inputs[0].isConst())
          continue;
        const auto Found = Calls.find({Block.Id, int(I)});
        if (Found == Calls.end())
          continue;
        auto &Call = *Found->second;
        if (Call.IsIndirect || Call.SourceCallHint ||
            Call.TargetAddr != Op.Inputs[0].ConstVal)
          continue;
        auto [Known, Inserted] = Adapters.try_emplace(Call.TargetAddr);
        if (Inserted) {
          if (!Owners)
            Owners.emplace(Image);
          Known->second =
              proveMIPS16EntryAdapter(Image, Call.TargetAddr, *Debug, &*Owners);
          if (Known->second && Known->second->BodyName.empty()) {
            // Address-bound user types need not carry a DWARF name. Naming
            // does not establish the transport proof; reuse the image's
            // naming policy once the body is proven, with one shared index.
            if (!Names)
              Names.emplace(imageFunctionNames(Image));
            const auto Named = Names->find(Known->second->Body);
            Known->second->BodyName =
                Named != Names->end()
                    ? Named->second
                    : Image.getFunctionNameAt(Known->second->Body);
          }
        }
        const auto &Adapter = Known->second;
        if (!Adapter || Adapter->BodyName.empty())
          continue;
        const auto Signature = recoveredFloatingCallArguments(
            Op, Call, Image.Arch, Image.abiFormat());
        if (!Signature || *Signature != Adapter->EntryArguments)
          continue;
        // SSA already captured the classic FP words as logical scalar values.
        // The compiler now supplies the compressed callee's argument transport.
        // No live register read or original instruction target is replaced.
        Op.Inputs[0] = MedVar::makeConst(Adapter->Body, Op.Inputs[0].Size,
                                         ConstantAddressProvenance::CodeAddress,
                                         Adapter->Body);
        Op.Inputs[0].TheArch = Image.Arch;
        Op.ScalarCallSignature =
            std::make_shared<FloatingCallArguments>(Adapter->BodyArguments);
        Call.TargetAddr = Adapter->Body;
        Call.TargetName = Adapter->BodyName;
        Call.SourceCalleeType.reset();
        Function.CallUnreadRegisters.erase(Op.CallSiteId);
      }
  }
}

} // namespace neverd::pipeline_detail
