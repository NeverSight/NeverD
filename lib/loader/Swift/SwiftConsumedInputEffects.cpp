#include "neverd/loader/Swift/SwiftConsumedInputEffects.h"

#include "../MachO/ImmutableNativeFrame.h"
#include "../MachO/SourceLocalCall.h"

#include "neverd/lift/AArch64/AArch64Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/Limits.h"
#include "neverd/loader/MachO/DarwinImportVeneer.h"
#include "neverd/loader/MachO/DarwinRuntimeCalls.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/loader/Swift/SwiftRuntimeCalls.h"

#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Support/Endian.h"

namespace neverd {
namespace {
struct Contract {
  const char *Runtime, *Metadata, *Witness, *Provider;
  size_t Bytes;
};
constexpr Contract Contracts[] = {
#include "SwiftConsumedInputs.inc"
};

bool strongImport(const BinaryImage &Image, va_t Slot, const char *Name,
                  const char *Provider) {
  const auto Found = Image.DyldBindSlots.find(Slot);
  const auto *Section = Image.getSectionFor(Slot);
  return Section &&
         (Section->Type & llvm::MachO::SECTION_TYPE) ==
             llvm::MachO::S_NON_LAZY_SYMBOL_POINTERS &&
         isImmutableImageImportSlot(Image, Slot) &&
         Found != Image.DyldBindSlots.end() &&
         Found->second.Name == std::string("_") + Name &&
         Found->second.Module == Provider && !Found->second.WeakImport &&
         !Found->second.Addend;
}

// A deliberately bounded, same-block ADRP/LDR pair immediately before the
// call. The canonical decoder below authenticates every LowIR operand and
// edge. No reaching predecessor or memory reload may supply either identity.
std::optional<va_t> loadedImport(const BinaryImage &Image,
                                 const LowBlock &Block, va_t Address,
                                 unsigned Register) {
  const auto Bytes = readImmutableCodeBytes(Image, Address, 8);
  if (!Bytes)
    return std::nullopt;
  const uint32_t Page = llvm::support::endian::read32le(Bytes->data());
  const uint32_t Load = llvm::support::endian::read32le(Bytes->data() + 4);
  if ((Page & 0x9f00001f) != (0x90000000u | Register) ||
      (Load & 0xffc003ff) != (0xf9400000u | (Register << 5) | Register))
    return std::nullopt;
  for (const auto &Boundary : Block.InstructionBoundaries) {
    if (Boundary.Address != Address || Boundary.OpCount != 1)
      continue;
    const auto &Op = Block.Ops[Boundary.FirstOp];
    if (Op.Opcode != NdOp::COPY || Op.NumInputs != 1 ||
        Op.Output != NdVar::reg(Register * 8, 8) || !Op.Inputs[0].isConst() ||
        Op.Inputs[0].Size != 8)
      return std::nullopt;
    const uint64_t Offset = ((Load >> 10) & 0xfff) * 8;
    if (Op.Inputs[0].Offset > UINT64_MAX - Offset)
      return std::nullopt;
    return Op.Inputs[0].Offset + Offset;
  }
  return std::nullopt;
}
} // namespace

bool isSwiftConsumedInputCallTarget(const BinaryImage &Image, va_t Target) {
  const auto Slot = darwinImportVeneerSlot(Image, Target);
  if (!Slot)
    return false;
  const auto Import = Image.DyldBindSlots.find(*Slot);
  if (Import == Image.DyldBindSlots.end())
    return false;
  for (const auto &Contract : Contracts)
    if (Import->second.Name == std::string("_") + Contract.Runtime)
      return true;
  return false;
}

std::map<va_t, SourceCallTypeHint>
buildSwiftConsumedInputCallHints(const BinaryImage &Image,
                                 const LowFunc &Caller) {
  std::map<va_t, SourceCallTypeHint> Result;
  if (Image.Arch != Arch::AArch64 || Image.Format != BinaryFormat::MachO ||
      Image.Bits != Bitness::Bits64 || Image.IsRelocatable)
    return Result;
  const auto Calls = sourceLocalCalls(Image, Caller);
  bool CheckedMachine = false;
  size_t Budget = ::neverd::limits::loader::
      kSwiftConsumedInputEffectsBuildSwiftConsumedInputCallHintsBudget;
  for (const auto &[Site, Word] : Calls) {
    if (Site.Instruction < 20)
      continue;
    const auto Slot = darwinImportVeneerSlot(Image, *Site.StaticTarget);
    const auto Runtime =
        Slot ? swiftRuntimeSourceCallHint(Image, *Slot) : std::nullopt;
    if (!Runtime || Runtime->WeakImport || Runtime->DoesNotReturn ||
        Runtime->CallKind != SourceCallTypeHint::Kind::SwiftRuntimeCall)
      continue;
    for (const auto &Contract : Contracts) {
      if (Runtime->TargetName != Contract.Runtime ||
          !strongImport(Image, *Slot, Contract.Runtime, Contract.Provider))
        continue;
      if (!CheckedMachine) {
        if (!immutableNativeFrameMachineMatches(Image, Caller, Budget))
          return {};
        CheckedMachine = true;
      }
      for (const auto &Block : Caller.Blocks) {
        // One input-address instruction, optionally preceded by a complete
        // indirect-result-address ADD. Both windows keep the adjacent type
        // and witness loads; no arbitrary reaching-definition search occurs.
        for (unsigned AddressWords : {1U, 2U}) {
          const unsigned PrefixBytes = 16 + AddressWords * 4;
          if (Site.Instruction < PrefixBytes)
            continue;
          std::set<va_t> Prefix;
          for (const auto &B : Block.InstructionBoundaries)
            if (B.Address <= Site.Instruction &&
                Site.Instruction - B.Address <= PrefixBytes)
              Prefix.insert(B.Address);
          if (Prefix.size() != 5 + AddressWords ||
              *Prefix.begin() != Site.Instruction - PrefixBytes ||
              *Prefix.rbegin() != Site.Instruction)
            continue;
          if (AddressWords == 2) {
            const auto Bytes =
                readImmutableCodeBytes(Image, Site.Instruction - 8, 4);
            if (!Bytes || (llvm::support::endian::read32le(Bytes->data()) &
                           0xffc0001f) != 0x91000008u)
              continue; // Only ADD x8, Xn/SP, #imm12; no call or type clobber.
          }
          // The final input-address instruction cannot overwrite either type
          // identity. Reject calls, intrinsics and control transfers here too.
          bool Clobbered = false;
          for (const auto &Op : Block.Ops)
            if (Op.Addr == Site.Instruction - 4) {
              Clobbered |= Op.Opcode == NdOp::CALL ||
                           Op.Opcode == NdOp::INDIR_CALL ||
                           Op.Opcode == NdOp::INTRINSIC ||
                           (Op.Output.isReg() && Op.Output.Size &&
                            Op.Output.Offset < a64reg::X3 &&
                            Op.Output.Offset + Op.Output.Size > a64reg::X1);
            }
          if (Clobbered)
            continue;
          const auto Metadata =
              loadedImport(Image, Block, Site.Instruction - PrefixBytes, 1);
          const auto Witness =
              loadedImport(Image, Block, Site.Instruction - PrefixBytes + 8, 2);
          if (!Metadata || !Witness ||
              !strongImport(Image, *Metadata, Contract.Metadata,
                            Contract.Provider) ||
              !strongImport(Image, *Witness, Contract.Witness,
                            Contract.Provider) ||
              !darwinRuntimeGlobalAddressHint(Image, *Metadata) ||
              !darwinRuntimeGlobalAddressHint(Image, *Witness))
            continue;
          auto Binding = *Runtime;
          Binding.SwiftConsumedInput =
              SourceCallTypeHint::SwiftConsumedInputEvidence{
                  Caller.Entry, Site, *Metadata, *Witness};
          Result.emplace(Site.Instruction, std::move(Binding));
        }
      }
    }
  }
  return Result;
}

std::optional<SourceFrameEffects>
swiftConsumedInputCallEffects(const BinaryImage &Image, const LowFunc &Caller,
                              const SourceCallTypeHint &Binding) {
  if (!Binding.SwiftConsumedInput)
    return std::nullopt;
  const auto Current = buildSwiftConsumedInputCallHints(Image, Caller);
  const auto Found = Current.find(Binding.SwiftConsumedInput->Site.Instruction);
  if (Found == Current.end() || Binding.WeakImport || Binding.DoesNotReturn ||
      Binding.SwiftConsumedInput != Found->second.SwiftConsumedInput ||
      Binding.TargetName != Found->second.TargetName ||
      Binding.TargetAddress != Found->second.TargetAddress ||
      Binding.CallKind != Found->second.CallKind ||
      !equalSourceABIs(Binding.Signature, Found->second.Signature))
    return std::nullopt;
  for (const auto &Contract : Contracts)
    if (Binding.TargetName == Contract.Runtime &&
        strongImport(Image, Binding.SwiftConsumedInput->MetadataSlot,
                     Contract.Metadata, Contract.Provider) &&
        strongImport(Image, Binding.SwiftConsumedInput->WitnessSlot,
                     Contract.Witness, Contract.Provider)) {
      SourceFrameEffects Effects;
      Effects.WritableFrameParameters.emplace(1, Contract.Bytes);
      Effects.InitializedFrameParameters.insert(1);
      if (sourceFrameEffectsMatchABI(Effects, Binding.Signature))
        return Effects;
    }
  return std::nullopt;
}
} // namespace neverd
