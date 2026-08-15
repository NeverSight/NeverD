//===- COFFException.cpp - PE exception personality orchestration --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/COFF/COFFException.h"

#include "COFFExceptionDetail.h"

#include "neverd/loader/COFF/COFFDelphiEH.h"
#include "neverd/loader/DWARF/LSDA.h"
#include "neverd/loader/ExceptionInfo.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>

namespace neverd::coff_loader {
namespace {

/// Decode the Itanium language-specific data area a mingw frame carries.
///
/// mingw-w64 keeps the Itanium C++ ABI's language semantics and reaches them
/// through Windows SEH: `.pdata` and `.xdata` describe the frame, and the
/// personality slot names `__gxx_personality_seh0`.  What follows the handler
/// in `.xdata` is not a Windows dialect at all -- GCC emits the same
/// `.gcc_except_table` record it would have put in its own section on ELF,
/// inline, right where the handler data begins.  So the record is read by the
/// decoder that already reads it everywhere else, and only where to start
/// differs.
///
/// The pointer bases are the ones an Itanium record can name.  There is no
/// `.eh_frame_hdr` here for `datarel` to mean anything against, so that base
/// stays zero and a record using it is reported as unresolved rather than
/// resolved against a guess.
bool parseMinGWLSDA(ExceptionFunction &F, const BinaryImage &Img) {
  if (F.HandlerDataVA == 0)
    return false;

  dwarf_eh::LSDAParseRequest Req;
  Req.LSDAVA = F.HandlerDataVA;
  Req.FunctionStart = F.CodeRange.Begin;
  Req.FunctionEnd = F.CodeRange.End;
  Req.Personality = F.Personality;
  Req.MaxRecords = detail::MaxLanguageRecords;

  dwarf_eh::PointerBases Bases;
  Bases.Func = F.CodeRange.Begin;
  if (const Section *Text = Img.getSectionByName(".text"))
    Bases.Text = Text->VA;

  dwarf_eh::LSDAParseResult Parsed = dwarf_eh::parseLSDA(Img, Req, Bases);
  for (const std::string &Diagnostic : Parsed.Diagnostics)
    F.Diagnostics.push_back(Diagnostic);
  if (!Parsed.Info) {
    detail::diagnose(F, ExceptionParseStatus::Partial,
                     "mingw Itanium LSDA at " +
                         llvm::utohexstr(F.HandlerDataVA) + " was not decoded");
    return false;
  }
  F.Itanium = std::move(*Parsed.Info);
  F.ParseStatus = mergeExceptionParseStatus(F.ParseStatus, Parsed.ParseStatus);
  return true;
}

/// Personality addresses the image gives no name to but which are provably
/// mingw's Itanium routine.
///
/// A stripped PE keeps no symbol for `__gxx_personality_seh0`, so the routine a
/// frame installs cannot be classified by name -- and every mingw frame in the
/// image goes uninterpreted along with it.  What such a frame still carries is
/// its language data, and the record it holds is not something arbitrary bytes
/// read as: the decode has to complete, name call sites that all lie inside the
/// frame that declared them, and dispatch on a type.  That last part is the
/// proof rather than the guess.  Windows dispatches on a filter expression or a
/// `FuncInfo`, never on an Itanium type table, and `__gcc_personality_*` never
/// dispatches at all -- it is cleanup-only by construction.  So a type table
/// reached this way can only belong to the C++ routine.
///
/// One address serves every frame that installs it, so proving it once settles
/// the frames whose own data is cleanup-only and could not have proved it.
std::set<va_t> findUnnamedMinGWPersonalities(BinaryImage &Img) {
  // Most frames installing the routine cannot prove anything about it: a scope
  // with destructors and no handler is cleanup-only, and at `-O0` those come
  // first.  So an address is retried on later frames rather than written off by
  // the first one that could not settle it, and the cap is what keeps an
  // address no frame can prove from costing one decode per frame.
  constexpr unsigned MaxProofAttempts = 32;
  std::set<va_t> Proven;
  std::map<va_t, unsigned> Attempts;
  for (ExceptionFunction &F : Img.ExceptionMetadata.Functions) {
    if (F.PersonalityVA == 0 || F.HandlerDataVA == 0)
      continue;
    if (Proven.count(F.PersonalityVA))
      continue;
    unsigned &Tries = Attempts[F.PersonalityVA];
    if (Tries >= MaxProofAttempts)
      continue;
    ++Tries;
    if (detail::classifyPersonality(
            detail::resolvePersonality(Img, F.PersonalityVA).second) !=
        ExceptionPersonality::Unknown)
      continue;

    ExceptionFunction Probe = F;
    if (!parseMinGWLSDA(Probe, Img) ||
        Probe.ParseStatus != ExceptionParseStatus::Complete || !Probe.Itanium ||
        Probe.Itanium->CallSites.empty())
      continue;
    if (Probe.Itanium->TypeTable.empty() || Probe.Itanium->isCleanupOnly())
      continue;
    if (llvm::all_of(Probe.Itanium->CallSites,
                     [&](const ItaniumCallSite &Site) {
                       return Site.GuardedRange.isValid() &&
                              F.CodeRange.contains(Site.GuardedRange);
                     }))
      Proven.insert(F.PersonalityVA);
  }
  return Proven;
}

} // namespace

void resolveExceptionHandlers(BinaryImage &Img) {
  const std::set<va_t> UnnamedMinGW = findUnnamedMinGWPersonalities(Img);
  for (ExceptionFunction &F : Img.ExceptionMetadata.Functions) {
    if (F.PersonalityVA == 0)
      continue;
    auto [ResolvedVA, Name] = detail::resolvePersonality(Img, F.PersonalityVA);
    F.PersonalityName = Name;
    F.Personality = detail::classifyPersonality(Name);
    if (F.Personality == ExceptionPersonality::Unknown)
      if (std::optional<ExceptionPersonality> Inferred =
              detail::inferGSPersonality(F, Img)) {
        F.Personality = *Inferred;
        F.PersonalityName = getExceptionPersonalityName(*Inferred);
        ResolvedVA = F.PersonalityVA;
      }
    if (F.Personality == ExceptionPersonality::Unknown &&
        UnnamedMinGW.count(F.PersonalityVA)) {
      F.Personality = ExceptionPersonality::GxxPersonalitySEH0;
      F.PersonalityName =
          getExceptionPersonalityName(ExceptionPersonality::GxxPersonalitySEH0);
      ResolvedVA = F.PersonalityVA;
    }
    if (F.Personality == ExceptionPersonality::Unknown) {
      // An unknown personality is an incomplete decode only when the record
      // carries language data that went uninterpreted.  A hand-written handler
      // installed with an empty data slot -- the CRT emits several, such as the
      // ARM64 routine that steps over an unsupported `mrs` -- has nothing more
      // in the image to read, so the record is as complete as it will ever be
      // and only the dispatch semantics are unnamed.  Every Windows dialect
      // begins its language data with either a scope count or a table pointer,
      // so a leading zero word is an empty slot under all of them.
      const bool HasLanguageData =
          F.HandlerDataVA != 0 &&
          detail::readScalar<uint32_t>(Img, F.HandlerDataVA).value_or(0) != 0;
      detail::diagnose(
          F,
          HasLanguageData ? ExceptionParseStatus::Partial
                          : ExceptionParseStatus::Complete,
          HasLanguageData
              ? "unknown Windows language personality"
              : "unknown Windows personality, installed with no language "
                "data");
      Img.ExceptionMetadata.ParseStatus = mergeExceptionParseStatus(
          Img.ExceptionMetadata.ParseStatus, F.ParseStatus);
      continue;
    }
    if (!detail::isExecutableAddress(Img, ResolvedVA))
      detail::diagnose(F, ExceptionParseStatus::Partial,
                       "resolved personality is not executable");

    switch (F.Personality) {
    case ExceptionPersonality::CSpecificHandler:
      detail::parseSEH(F, Img);
      break;
    case ExceptionPersonality::CxxFrameHandler3:
      detail::parseFH3(F, Img);
      break;
    case ExceptionPersonality::CxxFrameHandler4:
      detail::parseFH4(F, Img);
      break;
    case ExceptionPersonality::GSHandlerCheckSEH: {
      if (!detail::parseSEH(F, Img))
        break;
      std::optional<va_t> CookieVA = detail::sehGSCookieAddress(F, Img);
      if (!CookieVA) {
        detail::diagnose(F, ExceptionParseStatus::Malformed,
                         "GS SEH payload address overflows");
        break;
      }
      detail::parseGSCookie(F, Img, *CookieVA);
      break;
    }
    case ExceptionPersonality::GSHandlerCheckEH:
      if (detail::parseFH3(F, Img)) {
        if (F.HandlerDataVA > InvalidVA - sizeof(uint32_t))
          detail::diagnose(F, ExceptionParseStatus::Malformed,
                           "GS FH3 payload address overflows");
        else
          detail::parseGSCookie(F, Img, F.HandlerDataVA + sizeof(uint32_t));
      }
      break;
    case ExceptionPersonality::GSHandlerCheckEH4:
      if (detail::parseFH4(F, Img)) {
        if (F.HandlerDataVA > InvalidVA - sizeof(uint32_t))
          detail::diagnose(F, ExceptionParseStatus::Malformed,
                           "GS FH4 payload address overflows");
        else
          detail::parseGSCookie(F, Img, F.HandlerDataVA + sizeof(uint32_t));
      }
      break;
    case ExceptionPersonality::GxxPersonalitySEH0:
    case ExceptionPersonality::GccPersonalitySEH0:
      parseMinGWLSDA(F, Img);
      break;
    case ExceptionPersonality::DelphiExceptionHandler: {
      // Delphi's x86-64 compiler installs no registration record: it uses the
      // ordinary table mechanism and puts a `TExcData` scope array in the
      // handler data.  A frame whose array does not check out stays Partial
      // rather than being reported as fully understood, because a Delphi `try`
      // would then read as a function that installs a handler and has none.
      if (F.HandlerDataVA == 0)
        break;
      std::string Reason;
      if (!parseDelphiScopeTable(Img, F, Reason))
        detail::diagnose(
            F, ExceptionParseStatus::Partial,
            "Delphi x64 scope table at " + llvm::utohexstr(F.HandlerDataVA) +
                " was not decoded: " +
                (Reason.empty() ? "it does not read as a TExcData" : Reason));
      break;
    }
    default:
      break;
    }
    Img.ExceptionMetadata.ParseStatus = mergeExceptionParseStatus(
        Img.ExceptionMetadata.ParseStatus, F.ParseStatus);
  }
  Img.ExceptionMetadata.rebuildIndex();
}

} // namespace neverd::coff_loader
