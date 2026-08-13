//===- COFFExceptionGS.cpp - GS handler decoding and inference ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "COFFExceptionDetail.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/support/BinaryEncoding.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace neverd::coff_loader::detail {

bool parseGSCookie(ExceptionFunction &F, const BinaryImage &Img,
                   va_t CookieVA) {
  GSCookieInfo Cookie;
  const uint8_t *Header = Img.readVA(CookieVA, sizeof(uint32_t));
  if (!Header) {
    Cookie.ParseStatus = ExceptionParseStatus::Malformed;
    diagnose(F, ExceptionParseStatus::Malformed, "truncated GS handler header");
    F.GSCookie = std::move(Cookie);
    return false;
  }
  // The flags ride in the spare low bits of the cookie's frame offset, so how
  // many of them exist is decided by the alignment of the slot the cookie sits
  // in.  A 64-bit CRT gets three: `__GSHandlerCheckCommon` masks the word with
  // -8, tests bit 2 for an aligned frame, and reads a base and an alignment out
  // of the two words behind it.  A 32-bit CRT gets two, so it spends its one
  // remaining bit on the aligned form and derives the adjustment arithmetically
  // -- the ARM routine masks with -4, tests bit 0, and never looks past the
  // first word.  Reading the 64-bit shape out of a 32-bit record both misreads
  // the offset and then runs off the end of the record into whichever .xdata
  // happens to follow.
  const bool WideFlags = Img.is64Bit();
  const uint32_t Flags = readLE<uint32_t>(Header);
  const uint32_t FlagMask = WideFlags ? 7u : 3u;
  Cookie.HasExceptionHandler = WideFlags && (Flags & 1u) != 0;
  Cookie.HasUnwindHandler = WideFlags && (Flags & 2u) != 0;
  Cookie.HasAlignment = (Flags & (WideFlags ? 4u : 1u)) != 0;
  Cookie.CookieOffset = static_cast<int32_t>(Flags & ~FlagMask);
  size_t Size = sizeof(uint32_t);
  if (Cookie.HasAlignment && WideFlags) {
    const uint8_t *Alignment = Img.readVA(CookieVA, 3 * sizeof(uint32_t));
    if (!Alignment) {
      Cookie.ParseStatus = ExceptionParseStatus::Malformed;
      diagnose(F, ExceptionParseStatus::Malformed,
               "truncated aligned GS handler data");
      F.GSCookie = std::move(Cookie);
      return false;
    }
    Cookie.AlignmentBaseOffset = readLE<int32_t>(Alignment + 4);
    Cookie.Alignment = readLE<uint32_t>(Alignment + 8);
    if (Cookie.Alignment == 0 ||
        (Cookie.Alignment & (Cookie.Alignment - 1)) != 0) {
      Cookie.ParseStatus = ExceptionParseStatus::Malformed;
      diagnose(F, ExceptionParseStatus::Malformed,
               "invalid GS stack alignment");
      F.GSCookie = std::move(Cookie);
      return false;
    }
    Size = 3 * sizeof(uint32_t);
  }
  const uint8_t *Payload = Img.readVA(CookieVA, Size);
  if (!Payload) {
    Cookie.ParseStatus = ExceptionParseStatus::Malformed;
    diagnose(F, ExceptionParseStatus::Malformed,
             "truncated GS handler payload");
    F.GSCookie = std::move(Cookie);
    return false;
  }
  Cookie.Payload.assign(Payload, Payload + Size);
  Cookie.ParseStatus = ExceptionParseStatus::Complete;
  F.GSCookie = std::move(Cookie);
  return true;
}

std::optional<va_t> sehGSCookieAddress(const ExceptionFunction &F,
                                       const BinaryImage &Img) {
  auto Count = readScalar<uint32_t>(Img, F.HandlerDataVA);
  if (!Count || *Count > MaxLanguageRecords)
    return std::nullopt;
  uint64_t ScopeBytes = uint64_t(*Count) * 16;
  if (F.HandlerDataVA > InvalidVA - sizeof(uint32_t) ||
      ScopeBytes > InvalidVA - (F.HandlerDataVA + sizeof(uint32_t)))
    return std::nullopt;
  return F.HandlerDataVA + sizeof(uint32_t) + ScopeBytes;
}

/// Every routine a wrapper body calls or tail-jumps to, in whichever
/// instruction encoding \p Arch uses.  Only the direct forms are decoded: an
/// indirect call names nothing at this level, and a wrapper that reaches its
/// base handler indirectly is left unclassified rather than guessed at.
void collectDirectCallTargets(const BinaryImage &Img, Arch A, va_t BodyVA,
                              const uint8_t *Code, size_t CodeSize,
                              std::vector<std::string> &Names) {
  // A Thumb routine is named at its odd, interworking-tagged address while a
  // branch resolves to the even one, so both spellings are offered.
  const bool IsThumb = A == Arch::ARM;
  auto record = [&](std::optional<va_t> Target) {
    if (!Target)
      return;
    Names.push_back(resolvePersonality(Img, *Target).second);
    if (IsThumb && Names.back().empty())
      Names.back() = resolvePersonality(Img, *Target | 1).second;
  };

  switch (A) {
  case Arch::X64:
  case Arch::X86:
    for (size_t Offset = 0; Offset + 5 <= CodeSize; ++Offset) {
      if (Code[Offset] == 0xe8) {
        record(addSignedOffset(BodyVA + Offset + 5,
                               readLE<int32_t>(Code + Offset + 1)));
      } else if (Offset + 6 <= CodeSize && Code[Offset] == 0xff &&
                 Code[Offset + 1] == 0x15) {
        if (auto Slot = addSignedOffset(BodyVA + Offset + 6,
                                        readLE<int32_t>(Code + Offset + 2)))
          Names.push_back(directNameAt(Img, *Slot));
      }
    }
    break;

  case Arch::AArch64:
    // `bl`/`b` share the imm26 form and differ only in bit 31, and a GS
    // wrapper reaches its base handler both ways: it calls the cookie check
    // and tail-jumps to the handler it wraps.
    for (size_t Offset = 0; Offset + 4 <= CodeSize; Offset += 4) {
      const uint32_t Word = readLE<uint32_t>(Code + Offset);
      if ((Word & 0x7c000000u) != 0x14000000u)
        continue;
      const int64_t Imm =
          static_cast<int64_t>(static_cast<int32_t>(Word << 6) >> 6) * 4;
      record(addSignedOffset(BodyVA + Offset, Imm));
    }
    break;

  case Arch::ARM:
    // Thumb-2 `bl` and `b.w`, which share a first halfword and differ in bit
    // 12 of the second.  The branch is relative to the address of the
    // instruction plus four, and the two `J` bits are stored inverted
    // relative to the sign.
    for (size_t Offset = 0; Offset + 4 <= CodeSize; Offset += 2) {
      const uint16_t Hi = readLE<uint16_t>(Code + Offset);
      const uint16_t Lo = readLE<uint16_t>(Code + Offset + 2);
      if ((Hi & 0xf800u) != 0xf000u || (Lo & 0xc000u) != 0xc000u)
        continue;
      const uint32_t S = (Hi >> 10) & 1;
      const uint32_t J1 = (Lo >> 13) & 1;
      const uint32_t J2 = (Lo >> 11) & 1;
      const uint32_t I1 = (~(J1 ^ S)) & 1;
      const uint32_t I2 = (~(J2 ^ S)) & 1;
      uint32_t Value = (S << 24) | (I1 << 23) | (I2 << 22) |
                       ((Hi & 0x3ffu) << 12) | ((Lo & 0x7ffu) << 1);
      int64_t Imm = static_cast<int32_t>(Value << 7) >> 7;
      // Thumb code addresses carry the interworking bit, which is not part of
      // the address the branch resolves to.
      record(addSignedOffset((BodyVA & ~va_t(1)) + Offset + 4, Imm));
    }
    break;

  default:
    break;
  }
}

std::optional<ExceptionPersonality>
inferGSPersonality(const ExceptionFunction &F, const BinaryImage &Img) {
  if (F.PersonalityVA == 0 || F.HandlerDataVA == 0)
    return std::nullopt;

  // On ARM the handler RVA carries the Thumb interworking bit but the runtime
  // function it names does not, so the two spellings have to meet in the
  // middle before the wrapper can be found at all.
  const va_t WrapperVA =
      Img.Arch == Arch::ARM ? (F.PersonalityVA & ~va_t(1)) : F.PersonalityVA;
  const ExceptionFunction *Wrapper = nullptr;
  for (const ExceptionFunction &Candidate : Img.ExceptionMetadata.Functions) {
    if (Candidate.Kind != RuntimeFunctionKind::Primary ||
        !Candidate.CodeRange.isValid())
      continue;
    const va_t CandidateVA = Img.Arch == Arch::ARM
                                 ? (Candidate.CodeRange.Begin & ~va_t(1))
                                 : Candidate.CodeRange.Begin;
    if (CandidateVA == WrapperVA) {
      Wrapper = &Candidate;
      break;
    }
  }
  if (!Wrapper ||
      Wrapper->CodeRange.size() > std::numeric_limits<size_t>::max())
    return std::nullopt;

  // The body starts at the untagged address; reading from the tagged one would
  // shift every instruction by a byte.
  const va_t BodyVA = Img.Arch == Arch::ARM
                          ? (Wrapper->CodeRange.Begin & ~va_t(1))
                          : Wrapper->CodeRange.Begin;
  const size_t CodeSize = static_cast<size_t>(Wrapper->CodeRange.size());
  const uint8_t *Code = Img.readVA(BodyVA, CodeSize);
  if (!Code)
    return std::nullopt;

  // Static runtime wrappers may be stripped of their COFF names.  Require two
  // independent signals before recovering GS provenance: a bounded call from
  // the wrapper runtime function to a named base handler, and a payload that
  // is valid for that handler followed by valid GS cookie data.
  std::vector<std::string> Names;
  collectDirectCallTargets(Img, Img.Arch, BodyVA, Code, CodeSize, Names);

  ExceptionPersonality BasePersonality = ExceptionPersonality::Unknown;
  for (const std::string &Name : Names) {
    ExceptionPersonality Candidate = classifyPersonality(Name);
    if (Candidate != ExceptionPersonality::CSpecificHandler &&
        Candidate != ExceptionPersonality::CxxFrameHandler3 &&
        Candidate != ExceptionPersonality::CxxFrameHandler4)
      continue;
    if (BasePersonality != ExceptionPersonality::Unknown &&
        BasePersonality != Candidate)
      return std::nullopt;
    BasePersonality = Candidate;
  }
  if (BasePersonality == ExceptionPersonality::Unknown)
    return std::nullopt;

  ExceptionFunction Probe = F;
  Probe.ParseStatus = ExceptionParseStatus::Complete;
  Probe.Diagnostics.clear();
  Probe.SEH.reset();
  Probe.Cxx.reset();
  Probe.GSCookie.reset();
  bool PayloadMatches = false;
  switch (BasePersonality) {
  case ExceptionPersonality::CSpecificHandler:
    if (parseSEH(Probe, Img)) {
      std::optional<va_t> CookieVA = sehGSCookieAddress(Probe, Img);
      PayloadMatches = CookieVA && parseGSCookie(Probe, Img, *CookieVA);
    }
    if (PayloadMatches)
      return ExceptionPersonality::GSHandlerCheckSEH;
    break;
  case ExceptionPersonality::CxxFrameHandler3:
    PayloadMatches =
        parseFH3(Probe, Img) &&
        F.HandlerDataVA <= InvalidVA - sizeof(uint32_t) &&
        parseGSCookie(Probe, Img, F.HandlerDataVA + sizeof(uint32_t));
    if (PayloadMatches)
      return ExceptionPersonality::GSHandlerCheckEH;
    break;
  case ExceptionPersonality::CxxFrameHandler4:
    PayloadMatches =
        parseFH4(Probe, Img) &&
        F.HandlerDataVA <= InvalidVA - sizeof(uint32_t) &&
        parseGSCookie(Probe, Img, F.HandlerDataVA + sizeof(uint32_t));
    if (PayloadMatches)
      return ExceptionPersonality::GSHandlerCheckEH4;
    break;
  default:
    break;
  }
  return std::nullopt;
}

} // namespace neverd::coff_loader::detail
