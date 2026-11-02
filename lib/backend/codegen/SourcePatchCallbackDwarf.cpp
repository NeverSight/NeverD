//===- SourcePatchCallbackDwarf.cpp - Relocated entry frame rules ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Re-encode authenticated DWARF rules for the copied callback instructions.
//===----------------------------------------------------------------------===//

#include "neverd/backend/Limits.h"
#include "neverd/backend/codegen/SourcePatchCallback.h"
#include "neverd/loader/DWARF/AArch64ReturnAddressState.h"
#include "neverd/loader/LanguageEHDwarf.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/MathExtras.h"

namespace neverd {
namespace {
llvm::Error fail(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::errc::not_supported,
                                 "original callback unwind: " + Message);
}

void uleb(std::vector<uint8_t> &Out, uint64_t Value) {
  uint8_t Encoded[16];
  const unsigned Size = llvm::encodeULEB128(Value, Encoded);
  Out.insert(Out.end(), Encoded, Encoded + Size);
}
void sleb(std::vector<uint8_t> &Out, int64_t Value) {
  uint8_t Encoded[16];
  const unsigned Size = llvm::encodeSLEB128(Value, Encoded);
  Out.insert(Out.end(), Encoded, Encoded + Size);
}
void word(std::vector<uint8_t> &Out, uint32_t Value) {
  const size_t Offset = Out.size();
  Out.resize(Offset + 4);
  writeLE<uint32_t>(Out.data() + Offset, Value);
}

llvm::Error encodeRules(std::vector<uint8_t> &Out,
                        llvm::ArrayRef<CFIInstruction> Instructions,
                        const SourceCallbackPrefix &Prefix, int64_t DataAlign,
                        bool Initial) {
  uint64_t Current = 0;
  unsigned RememberDepth = 0;
  for (const auto &I : Instructions) {
    if (I.CodeOffset < Current)
      return fail("frame rules move backwards");
    if (Initial && I.CodeOffset)
      return fail("CIE rules have a code location");
    if (!Initial && I.CodeOffset > Prefix.CopiedSize)
      break;
    if (I.CodeOffset != Current) {
      bool Boundary = I.CodeOffset == Prefix.CopiedSize;
      for (const auto &Instruction : Prefix.Instructions)
        Boundary |= I.CodeOffset == Instruction.Offset;
      if (!Boundary)
        return fail("frame rule begins inside a relocated instruction");
      Out.push_back(llvm::dwarf::DW_CFA_advance_loc4);
      word(Out, I.CodeOffset - Current);
      Current = I.CodeOffset;
    }
    const auto Factored = [&]() -> llvm::Expected<int64_t> {
      if (!DataAlign || (I.Offset == INT64_MIN && DataAlign == -1) ||
          I.Offset % DataAlign)
        return fail("unrepresentable factored frame rule");
      return I.Offset / DataAlign;
    };
    switch (I.Kind) {
    case CFIOpKind::Nop:
    case CFIOpKind::AdvanceLoc:
      break;
    case CFIOpKind::DefCFA:
    case CFIOpKind::DefCFAOffset:
      if (I.Offset < 0)
        return fail("negative canonical frame offset");
      Out.push_back(I.Kind == CFIOpKind::DefCFA
                        ? llvm::dwarf::DW_CFA_def_cfa
                        : llvm::dwarf::DW_CFA_def_cfa_offset);
      if (I.Kind == CFIOpKind::DefCFA)
        uleb(Out, I.Register);
      uleb(Out, I.Offset);
      break;
    case CFIOpKind::DefCFARegister:
      Out.push_back(llvm::dwarf::DW_CFA_def_cfa_register);
      uleb(Out, I.Register);
      break;
    case CFIOpKind::Offset:
    case CFIOpKind::ValOffset: {
      auto Offset = Factored();
      if (!Offset)
        return Offset.takeError();
      Out.push_back(I.Kind == CFIOpKind::Offset
                        ? llvm::dwarf::DW_CFA_offset_extended_sf
                        : llvm::dwarf::DW_CFA_val_offset_sf);
      uleb(Out, I.Register);
      sleb(Out, *Offset);
      break;
    }
    case CFIOpKind::Register:
      Out.push_back(llvm::dwarf::DW_CFA_register);
      uleb(Out, I.Register);
      uleb(Out, I.Register2);
      break;
    case CFIOpKind::Restore:
    case CFIOpKind::Undefined:
    case CFIOpKind::SameValue:
      Out.push_back(
          I.Kind == CFIOpKind::Restore ? llvm::dwarf::DW_CFA_restore_extended
          : I.Kind == CFIOpKind::Undefined ? llvm::dwarf::DW_CFA_undefined
                                           : llvm::dwarf::DW_CFA_same_value);
      uleb(Out, I.Register);
      break;
    case CFIOpKind::RememberState:
      if (++RememberDepth >
          ::neverd::limits::backend::
              kSourcePatchCallbackDwarfEncodeRulesRememberDepthLimit)
        return fail("frame-state nesting exceeds the callback bound");
      Out.push_back(llvm::dwarf::DW_CFA_remember_state);
      break;
    case CFIOpKind::RestoreState:
      if (!RememberDepth)
        return fail("frame-state restore has no saved state");
      --RememberDepth;
      Out.push_back(llvm::dwarf::DW_CFA_restore_state);
      break;
    case CFIOpKind::GnuArgsSize:
      if (I.Offset < 0)
        return fail("negative stack argument size");
      Out.push_back(llvm::dwarf::DW_CFA_GNU_args_size);
      uleb(Out, I.Offset);
      break;
    case CFIOpKind::NegateRAState:
      Out.push_back(llvm::dwarf::DW_CFA_AARCH64_negate_ra_state);
      break;
    case CFIOpKind::SetRAState:
    case CFIOpKind::NegateRAStateWithPC:
      return fail(dwarf_eh::aarch64::kUnsafeRewrite);
    default:
      return fail("frame rule needs an unmodeled expression or PC identity");
    }
  }
  return llvm::Error::success();
}
} // namespace

llvm::Expected<std::vector<uint8_t>>
encodeSourceCallbackDwarfFrame(const SourceCallbackPrefix &Prefix,
                               const DwarfCIE &CIE, const DwarfFDE &FDE,
                               uint64_t FrameVA) {
  if (Prefix.Bytes.empty() || !Prefix.CopiedSize ||
      Prefix.CopiedSize > Prefix.Bytes.size() || Prefix.Instructions.empty() ||
      FDE.InitialLocation != Prefix.OriginalVA ||
      FDE.AddressRange < Prefix.CopiedSize || !CIE.CodeAlignmentFactor ||
      !CIE.DataAlignmentFactor || CIE.ReturnAddressRegister > 255 ||
      FDE.LSDAVA || CIE.IsSignalFrame || CIE.HasPointerAuth ||
      CIE.UsesArm64eReturnAddressABI || CIE.UsesNonHintPAuthLR ||
      CIE.ParseStatus != ExceptionParseStatus::Complete ||
      FDE.ParseStatus != ExceptionParseStatus::Complete ||
      CIE.HasMTETaggedFrame || CIE.SegmentSelectorSize ||
      FrameVA > uint64_t(INT64_MAX) / 2 ||
      Prefix.CallbackVA > uint64_t(INT64_MAX) / 2)
    return fail(
        "frame identity or augmentation requires an unmodeled contract");
  std::vector<uint8_t> Out;
  word(Out, 0); // CIE length.
  word(Out, 0); // .eh_frame CIE identifier.
  Out.insert(Out.end(), {1, 'z', 'R', 0});
  uleb(Out, 1); // Code offsets are normalized to byte units by the loader.
  sleb(Out, CIE.DataAlignmentFactor);
  Out.push_back(CIE.ReturnAddressRegister);
  uleb(Out, 1);
  Out.push_back(llvm::dwarf::DW_EH_PE_pcrel | llvm::dwarf::DW_EH_PE_sdata4);
  if (auto Error = encodeRules(Out, CIE.InitialInstructions, Prefix,
                               CIE.DataAlignmentFactor, true))
    return Error;
  Out.resize(llvm::alignTo(Out.size(), 8), llvm::dwarf::DW_CFA_nop);
  writeLE<uint32_t>(Out.data(), Out.size() - 4);

  const size_t FDEOffset = Out.size();
  word(Out, 0);
  word(Out, FDEOffset + 4); // Backward CIE distance, never an absolute address.
  const int64_t Initial =
      int64_t(Prefix.CallbackVA) - int64_t(FrameVA + Out.size());
  if (!llvm::isInt<32>(Initial) || Prefix.Bytes.size() > UINT32_MAX)
    return fail("FDE code range exceeds its encoding");
  word(Out, uint32_t(Initial));
  word(Out, Prefix.Bytes.size());
  uleb(Out, 0); // No language handler: the admitted prefix contains no call.
  if (auto Error = encodeRules(Out, FDE.Instructions, Prefix,
                               CIE.DataAlignmentFactor, false))
    return Error;
  Out.resize(llvm::alignTo(Out.size(), 8), llvm::dwarf::DW_CFA_nop);
  writeLE<uint32_t>(Out.data() + FDEOffset, Out.size() - FDEOffset - 4);
  return Out;
}

} // namespace neverd
