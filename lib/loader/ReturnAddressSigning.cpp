//===- ReturnAddressSigning.cpp - Unified unwind signing query ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Resolve normalized source metadata through its authoritative validators.
//===----------------------------------------------------------------------===//

#include "neverd/loader/ReturnAddressSigning.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ExceptionFunction.h"
#include "neverd/loader/MachO/CompactUnwind.h"

namespace neverd {
namespace {

using dwarf_eh::AArch64RAQueryResult;
using dwarf_eh::AArch64RAQueryStatus;

#define NEVERD_RA_QUERY_TEXT(Name, Text) constexpr char Name[] = Text;
#include "neverd/loader/ReturnAddressSigning.def"

AArch64RAQueryResult fail(AArch64RAQueryStatus Status, const char *Diagnostic) {
  return {Status, std::nullopt, Diagnostic};
}

AArch64RAQueryResult compactRecipe(const CompactUnwindEntry &Entry,
                                   const ExceptionFunction &Function, va_t PC) {
  using Status = AArch64RAQueryStatus;
  if (!Function.CodeRange.contains(Entry.CodeRange) ||
      !macho_unwind::isValidEncodingRange(Arch::AArch64, Entry.NativeEncoding,
                                          Entry.CodeRange))
    return fail(Status::Malformed, kInvalidCompact);
  if (!Entry.CodeRange.contains(PC))
    return fail(Status::OutOfRange, kOutOfRange);
  if (Entry.SemanticStatus != CompactUnwindSemanticStatus::Complete)
    return fail(Status::Unsupported, kIncompleteCompact);
  CompactUnwindEntry Decoded;
  if (!macho_unwind::decodeEncoding(Arch::AArch64, Entry.NativeEncoding,
                                    Decoded) ||
      Decoded.SemanticStatus != CompactUnwindSemanticStatus::Complete)
    return fail(Status::Unsupported, kInvalidCompact);
  if (Decoded.Kind != Entry.Kind ||
      Decoded.ReturnAddressSigningOffset != Entry.ReturnAddressSigningOffset)
    return fail(Status::Malformed, kStaleCompact);
  if (!Decoded.ReturnAddressSigningOffset)
    return fail(Status::Unknown, kNoCompactRecipe);
  // Shared range validation proved the full instruction fits before adding
  // its offset. A present zero offset continues to name the first instruction.
  dwarf_eh::AArch64ReturnAddressState Recipe;
  Recipe.Kind = dwarf_eh::AArch64RASigningKind::SPAndPC;
  Recipe.Key = dwarf_eh::AArch64RAKey::B;
  Recipe.Recipe = dwarf_eh::AArch64RARecipe::NonHintPAuthLR;
  Recipe.SigningPC =
      Entry.CodeRange.Begin + *Decoded.ReturnAddressSigningOffset;
  return {Status::Known, Recipe, nullptr};
}

AArch64RAQueryResult dwarfState(const BinaryImage &Image,
                                const ExceptionFunction &Function, va_t PC,
                                const AArch64UnwindSigningLimits &Limits) {
  using Status = AArch64RAQueryStatus;
  const auto &FDE = *Function.Dwarf;
  const auto Range = ExceptionAddressRange::fromStartAndSize(
      FDE.InitialLocation, FDE.AddressRange);
  if (!Range || !Function.CodeRange.contains(*Range))
    return fail(Status::Malformed, kInvalidOwner);
  if (Image.ExceptionMetadata.CIEs.size() > Limits.MaxCIEs)
    return fail(Status::ResourceLimit, kCIEBudget);
  const DwarfCIE *CIE = nullptr;
  for (const auto &Candidate : Image.ExceptionMetadata.CIEs) {
    if (Candidate.SectionOffset != FDE.CIESectionOffset)
      continue;
    if (CIE)
      return fail(Status::Malformed, kInvalidCIE);
    CIE = &Candidate;
  }
  if (!CIE)
    return fail(Status::Malformed, kInvalidCIE);
  return dwarf_eh::evaluateAArch64ReturnAddressState(Image.Arch, *CIE, FDE, PC,
                                                     Limits.Dwarf);
}

} // namespace

AArch64UnwindSigningQuery
queryAArch64UnwindSigning(const BinaryImage &Image,
                          const ExceptionFunction &Function, va_t PC,
                          const AArch64UnwindSigningLimits &Limits) {
  using Status = AArch64RAQueryStatus;
  AArch64UnwindSigningQuery Result;
  Result.QueryPC = PC;
  const auto Reject = [&](Status State, const char *Diagnostic) {
    if (Function.Dwarf)
      Result.DwarfStateAtPC = fail(State, Diagnostic);
    if (Function.Compact)
      Result.CompactRecipe = fail(State, Diagnostic);
    return Result;
  };
  if (Image.Arch != Arch::AArch64)
    return Reject(Status::Unsupported, kUnsupportedArchitecture);
  if (!Function.CodeRange.isValid())
    return Reject(Status::Malformed, kInvalidOwner);
  if (!Function.CodeRange.contains(PC))
    return Reject(Status::OutOfRange, kOutOfRange);
  auto ParseStatus = mergeExceptionParseStatus(
      Function.ParseStatus, Image.ExceptionMetadata.ParseStatus);
  if (Function.DecodeProvenance) {
    ParseStatus = mergeExceptionParseStatus(
        ParseStatus, Function.DecodeProvenance->Structural.ParseStatus);
    ParseStatus = mergeExceptionParseStatus(
        ParseStatus, Function.DecodeProvenance->Language.ParseStatus);
  }
  if (Image.ExceptionMetadata.StructuralDecode)
    ParseStatus = mergeExceptionParseStatus(
        ParseStatus, Image.ExceptionMetadata.StructuralDecode->ParseStatus);
  if (ParseStatus != ExceptionParseStatus::Complete)
    return Reject(ParseStatus == ExceptionParseStatus::Malformed
                      ? Status::Malformed
                      : Status::Unsupported,
                  kIncompleteOwner);
  if (Function.Dwarf)
    Result.DwarfStateAtPC = dwarfState(Image, Function, PC, Limits);
  if (Function.Compact)
    Result.CompactRecipe = compactRecipe(*Function.Compact, Function, PC);
  return Result;
}

} // namespace neverd
