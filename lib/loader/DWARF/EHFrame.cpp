//===- EHFrame.cpp - DWARF call frame information decoding ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Walks an `.eh_frame` / `__eh_frame` section entry by entry, decoding each
/// CIE header and augmentation block and each FDE that names one.  The
/// variable-length value readers live in EHFrameEncoding.cpp and the
/// call-frame instruction decoder in EHFrameCFI.cpp.
///
//===----------------------------------------------------------------------===//

#include "neverd/loader/DWARF/EHFrame.h"

#include "EHFrameDetail.h"

#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/DwarfEH.h"

#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <utility>

#define DEBUG_TYPE "neverd-dwarf-eh"

namespace neverd::dwarf_eh {

using namespace dweh;

namespace {

/// Largest augmentation string accepted.  The defined characters are a short
/// fixed set, so anything longer indicates the string is not NUL terminated
/// inside the entry.
constexpr size_t kMaxAugmentationLength = 32;

/// Parse one CIE body.  \p Cursor points just past the CIE id field.
bool parseCIE(const uint8_t *Buf, size_t Size, size_t Cursor, size_t End,
              va_t BufVA, const PointerBases &Bases, unsigned PtrSize,
              const BinaryImage *Img, const ParseLimits &Limits, DwarfCIE &CIE,
              ExceptionParseStatus &Status,
              std::vector<std::string> &Diagnostics) {
  auto fail = [&](const char *Message) {
    Status = mergeExceptionParseStatus(Status, ExceptionParseStatus::Malformed);
    Diagnostics.emplace_back(Message);
    return false;
  };

  if (Cursor >= End)
    return fail("truncated DWARF CIE");
  CIE.Version = Buf[Cursor++];
  if (CIE.Version != 1 && CIE.Version != 3 && CIE.Version != 4)
    return fail("unsupported DWARF CIE version");

  size_t AugStart = Cursor;
  while (Cursor < End && Buf[Cursor] != 0)
    ++Cursor;
  if (Cursor >= End)
    return fail("unterminated DWARF CIE augmentation string");
  if (Cursor - AugStart > kMaxAugmentationLength)
    return fail("DWARF CIE augmentation string is implausibly long");
  CIE.Augmentation.assign(reinterpret_cast<const char *>(Buf + AugStart),
                          Cursor - AugStart);
  ++Cursor;

  if (CIE.Version >= 4) {
    if (!rangeInBounds(Cursor, 2, End))
      return fail("truncated DWARF CIE address-size fields");
    CIE.AddressSize = Buf[Cursor++];
    CIE.SegmentSelectorSize = Buf[Cursor++];
    if (CIE.AddressSize != 4 && CIE.AddressSize != 8)
      return fail("unsupported DWARF CIE address size");
    if (CIE.SegmentSelectorSize != 0)
      return fail("segmented DWARF CIE addresses are not modeled");
  }

  // The legacy "eh" augmentation places a pointer-sized language-specific
  // field immediately after the string, outside any 'z' block.
  if (CIE.Augmentation == "eh") {
    if (PtrSize == 0 || !rangeInBounds(Cursor, PtrSize, End))
      return fail("truncated legacy DWARF CIE eh field");
    Cursor += PtrSize;
  }

  if (!readULEB128(Buf, End, Cursor, CIE.CodeAlignmentFactor))
    return fail("truncated DWARF CIE code alignment factor");
  if (!readSLEB128(Buf, End, Cursor, CIE.DataAlignmentFactor))
    return fail("truncated DWARF CIE data alignment factor");
  if (CIE.CodeAlignmentFactor == 0)
    return fail("zero DWARF CIE code alignment factor");

  if (CIE.Version == 1) {
    if (Cursor >= End)
      return fail("truncated DWARF CIE return address register");
    CIE.ReturnAddressRegister = Buf[Cursor++];
  } else if (!readULEB128(Buf, End, Cursor, CIE.ReturnAddressRegister)) {
    return fail("truncated DWARF CIE return address register");
  }

  size_t ProgramStart = Cursor;
  if (!CIE.Augmentation.empty() && CIE.Augmentation[0] == 'z') {
    CIE.HasAugmentationData = true;
    uint64_t AugLength = 0;
    if (!readULEB128(Buf, End, Cursor, AugLength))
      return fail("truncated DWARF CIE augmentation length");
    if (!rangeInBounds(Cursor, AugLength, End))
      return fail("DWARF CIE augmentation data leaves its entry");
    const size_t AugEnd = Cursor + static_cast<size_t>(AugLength);
    ProgramStart = AugEnd;

    for (size_t I = 1; I < CIE.Augmentation.size(); ++I) {
      switch (CIE.Augmentation[I]) {
      case 'L':
        if (Cursor >= AugEnd)
          return fail("truncated DWARF CIE LSDA encoding");
        CIE.LSDAPointerEncoding = Buf[Cursor++];
        break;
      case 'P': {
        if (Cursor >= AugEnd)
          return fail("truncated DWARF CIE personality encoding");
        CIE.PersonalityEncoding = Buf[Cursor++];
        va_t Personality = 0;
        va_t Slot = 0;
        if (!readEncodedPointer(Buf, AugEnd, Cursor, BufVA,
                                CIE.PersonalityEncoding, Bases, PtrSize, Img,
                                Personality, &Slot)) {
          Status =
              mergeExceptionParseStatus(Status, ExceptionParseStatus::Partial);
          Diagnostics.emplace_back("unresolved DWARF CIE personality pointer");
          Cursor = AugEnd;
        } else {
          CIE.PersonalityVA = Personality;
        }
        // A dynamically bound personality slot holds nothing in the file
        // image, so the slot address is what names the routine later.
        CIE.PersonalitySlotVA = Slot;
        break;
      }
      case 'R':
        if (Cursor >= AugEnd)
          return fail("truncated DWARF CIE FDE pointer encoding");
        CIE.FDEPointerEncoding = Buf[Cursor++];
        break;
      case 'S':
        CIE.IsSignalFrame = true;
        break;
      case 'B':
        CIE.HasPointerAuth = true;
        break;
      case 'G':
        CIE.HasMTETaggedFrame = true;
        break;
      default:
        // An unknown augmentation character makes the remaining augmentation
        // bytes uninterpretable.  The 'z' length still bounds them exactly, so
        // the entry stays parseable; only the augmentation is unknown.
        Status =
            mergeExceptionParseStatus(Status, ExceptionParseStatus::Partial);
        Diagnostics.emplace_back("unknown DWARF CIE augmentation character");
        I = CIE.Augmentation.size();
        break;
      }
    }
    Cursor = AugEnd;
  } else if (!CIE.Augmentation.empty() && CIE.Augmentation != "eh") {
    // Without 'z' there is no length prefix, so an augmentation this decoder
    // does not know makes the rest of the entry unparseable.
    Status = mergeExceptionParseStatus(Status, ExceptionParseStatus::Partial);
    Diagnostics.emplace_back(
        "DWARF CIE augmentation has no self-describing length");
    return true;
  }

  Cursor = ProgramStart;
  // An .eh_frame CIE that never declared an FDE encoding leaves FDE initial
  // locations as absolute pointers, which is the psABI default.
  const uint8_t FDEEncoding =
      CIE.FDEPointerEncoding == Omit ? uint8_t(Absptr) : CIE.FDEPointerEncoding;
  detail::decodeCFIProgram(Buf, Size, Cursor, End, CIE, FDEEncoding, BufVA,
                           Bases, PtrSize, Img, Limits, CIE.InitialInstructions,
                           Status, Diagnostics);
  return true;
}

} // namespace

ParseResult parseEHFrame(const BinaryImage &Img, const FrameSection &Sec,
                         const PointerBases &Bases, const ParseLimits &Limits) {
  ParseResult Result;
  if (!Sec.Data || Sec.Size < 4)
    return Result;

  const unsigned PtrSize = Img.getPointerSize();
  const uint8_t *Buf = Sec.Data;
  const size_t Size = Sec.Size;

  auto malformed = [&](const char *Message) {
    Result.ParseStatus = mergeExceptionParseStatus(
        Result.ParseStatus, ExceptionParseStatus::Malformed);
    Result.Diagnostics.emplace_back(Message);
  };

  size_t Offset = 0;
  size_t Entries = 0;
  while (Offset + 4 <= Size) {
    if (++Entries > Limits.MaxEntries) {
      Result.ParseStatus = mergeExceptionParseStatus(
          Result.ParseStatus, ExceptionParseStatus::Partial);
      Result.Diagnostics.emplace_back(
          "DWARF frame section exceeds entry decode budget");
      break;
    }

    const size_t EntryStart = Offset;
    uint64_t Length = readLE<uint32_t>(Buf + Offset);
    Offset += 4;
    bool Is64BitDwarf = false;
    if (Length == 0xffffffffu) {
      if (!rangeInBounds(Offset, 8, Size)) {
        malformed("truncated 64-bit DWARF frame entry length");
        break;
      }
      Length = readLE<uint64_t>(Buf + Offset);
      Offset += 8;
      Is64BitDwarf = true;
    }
    // A zero length is the section terminator.  Padding after it is normal.
    if (Length == 0)
      break;
    if (!rangeInBounds(Offset, Length, Size)) {
      malformed("DWARF frame entry length leaves its section");
      break;
    }

    const size_t EntryEnd = Offset + static_cast<size_t>(Length);
    const size_t IdSize = Is64BitDwarf ? 8 : 4;
    if (!rangeInBounds(Offset, IdSize, EntryEnd)) {
      malformed("truncated DWARF frame entry identifier");
      break;
    }
    const size_t IdOffset = Offset;
    const uint64_t Id = Is64BitDwarf ? readLE<uint64_t>(Buf + Offset)
                                     : uint64_t(readLE<uint32_t>(Buf + Offset));
    Offset += IdSize;

    if (Id == 0) {
      DwarfCIE CIE;
      CIE.SectionOffset = EntryStart;
      if (parseCIE(Buf, Size, Offset, EntryEnd, Sec.VA, Bases, PtrSize, &Img,
                   Limits, CIE, Result.ParseStatus, Result.Diagnostics))
        Result.CIEs.push_back(std::move(CIE));
      Offset = EntryEnd;
      continue;
    }

    // In .eh_frame the identifier is the distance back from the identifier
    // field to the start of the owning CIE.
    if (Id > IdOffset) {
      malformed("DWARF FDE names a CIE before its section");
      Offset = EntryEnd;
      continue;
    }
    const uint64_t CIEOffset = IdOffset - Id;
    const DwarfCIE *CIE = nullptr;
    for (const DwarfCIE &Candidate : Result.CIEs)
      if (Candidate.SectionOffset == CIEOffset)
        CIE = &Candidate;
    if (!CIE) {
      Result.ParseStatus = mergeExceptionParseStatus(
          Result.ParseStatus, ExceptionParseStatus::Partial);
      Result.Diagnostics.emplace_back("DWARF FDE names an undecoded CIE");
      Offset = EntryEnd;
      continue;
    }

    DwarfFDE FDE;
    FDE.SectionOffset = EntryStart;
    FDE.CIESectionOffset = CIEOffset;

    const uint8_t Encoding = CIE->FDEPointerEncoding == Omit
                                 ? uint8_t(Absptr)
                                 : CIE->FDEPointerEncoding;
    va_t Initial = 0;
    FDE.InitialLocationOffset = Offset;
    if (!readEncodedPointer(Buf, EntryEnd, Offset, Sec.VA, Encoding, Bases,
                            PtrSize, &Img, Initial)) {
      malformed("unreadable DWARF FDE initial location");
      Offset = EntryEnd;
      continue;
    }
    FDE.InitialLocation = Initial;

    // The address range uses the same value format but is never relative:
    // it is a length, so the application nibble must not be applied to it.
    va_t Range = 0;
    if (!readEncodedPointer(Buf, EntryEnd, Offset, Sec.VA, getFormat(Encoding),
                            Bases, PtrSize, &Img, Range)) {
      malformed("unreadable DWARF FDE address range");
      Offset = EntryEnd;
      continue;
    }
    FDE.AddressRange = Range;

    size_t ProgramStart = Offset;
    if (CIE->HasAugmentationData) {
      uint64_t AugLength = 0;
      if (!readULEB128(Buf, EntryEnd, Offset, AugLength)) {
        malformed("truncated DWARF FDE augmentation length");
        Offset = EntryEnd;
        continue;
      }
      if (!rangeInBounds(Offset, AugLength, EntryEnd)) {
        malformed("DWARF FDE augmentation data leaves its entry");
        Offset = EntryEnd;
        continue;
      }
      const size_t AugEnd = Offset + static_cast<size_t>(AugLength);
      ProgramStart = AugEnd;
      if (CIE->LSDAPointerEncoding != Omit) {
        PointerBases FDEBases = Bases;
        FDEBases.Func = FDE.InitialLocation;
        va_t LSDA = 0;
        if (readEncodedPointer(Buf, AugEnd, Offset, Sec.VA,
                               CIE->LSDAPointerEncoding, FDEBases, PtrSize,
                               &Img, LSDA)) {
          FDE.LSDAVA = LSDA;
        } else {
          Result.ParseStatus = mergeExceptionParseStatus(
              Result.ParseStatus, ExceptionParseStatus::Partial);
          Result.Diagnostics.emplace_back("unresolved DWARF FDE LSDA pointer");
        }
      }
      Offset = AugEnd;
    }

    PointerBases FDEBases = Bases;
    FDEBases.Func = FDE.InitialLocation;
    detail::decodeCFIProgram(Buf, Size, ProgramStart, EntryEnd, *CIE, Encoding,
                             Sec.VA, FDEBases, PtrSize, &Img, Limits,
                             FDE.Instructions, Result.ParseStatus,
                             Result.Diagnostics);

    Result.FDEs.push_back(std::move(FDE));
    Offset = EntryEnd;
  }

  LLVM_DEBUG(llvm::dbgs() << "dwarf-eh: decoded " << Result.CIEs.size()
                          << " CIEs and " << Result.FDEs.size() << " FDEs\n");
  return Result;
}

} // namespace neverd::dwarf_eh
