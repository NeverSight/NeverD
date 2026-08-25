//===- COFFExceptionFH4.cpp - MSVC C++ EH4 decoding -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "COFFExceptionDetail.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/support/BinaryEncoding.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace neverd::coff_loader::detail {
namespace {

class FH4Reader {
public:
  FH4Reader(const BinaryImage &Image, va_t Address)
      : Img(Image), Cursor(Address) {}

  va_t position() const { return Cursor; }

  bool readByte(uint8_t &Value) {
    if (BytesRead >= MaxBytes || Cursor == InvalidVA)
      return false;
    const uint8_t *P = Img.readVA(Cursor, 1);
    if (!P)
      return false;
    Value = *P;
    ++Cursor;
    ++BytesRead;
    return true;
  }

  bool readUInt32(uint32_t &Value) {
    if (BytesRead > MaxBytes - sizeof(uint32_t) ||
        Cursor > InvalidVA - sizeof(uint32_t))
      return false;
    const uint8_t *P = Img.readVA(Cursor, sizeof(uint32_t));
    if (!P)
      return false;
    Value = readLE<uint32_t>(P);
    Cursor += sizeof(uint32_t);
    BytesRead += sizeof(uint32_t);
    return true;
  }

  /// FH4 integers use a low-bit length tag: 0, 01, 011, 0111, or
  /// 1111 followed by a full little-endian word.  Reject overlong forms so
  /// corrupt data has one deterministic interpretation.
  bool readCompressedUInt(uint32_t &Value) {
    uint8_t First = 0;
    if (!readByte(First))
      return false;
    unsigned Extra = 0;
    unsigned InitialShift = 0;
    uint32_t Minimum = 0;
    if ((First & 1u) == 0) {
      Value = First >> 1;
      return true;
    }
    if ((First & 3u) == 1) {
      Extra = 1;
      InitialShift = 2;
      Minimum = 1u << 7;
    } else if ((First & 7u) == 3) {
      Extra = 2;
      InitialShift = 3;
      Minimum = 1u << 14;
    } else if ((First & 15u) == 7) {
      Extra = 3;
      InitialShift = 4;
      Minimum = 1u << 21;
    } else {
      if (First != 15)
        return false;
      if (!readUInt32(Value))
        return false;
      return Value >= (1u << 28);
    }

    Value = First >> InitialShift;
    unsigned Shift = 8 - InitialShift;
    for (unsigned I = 0; I < Extra; ++I, Shift += 8) {
      uint8_t Next = 0;
      if (!readByte(Next))
        return false;
      Value |= uint32_t(Next) << Shift;
    }
    return Value >= Minimum;
  }

private:
  static constexpr uint64_t MaxBytes = 1u << 20;
  const BinaryImage &Img;
  va_t Cursor = 0;
  uint64_t BytesRead = 0;
};

bool readFH4ImageAddress(FH4Reader &Reader, va_t ImageBase, va_t &Address,
                         bool AllowZero = true) {
  uint32_t RVA = 0;
  if (!Reader.readUInt32(RVA) || (!AllowZero && RVA == 0))
    return false;
  if (RVA == 0) {
    Address = 0;
    return true;
  }
  return addRVA(ImageBase, RVA, Address);
}

bool parseFH4HandlerMap(ExceptionFunction &F, const BinaryImage &Img,
                        va_t HandlerMapVA, va_t FuncInfoVA, bool IsSeparated,
                        CxxTryBlock &Try,
                        LanguageRecordBudget &Budget) {
  FH4Reader Reader(Img, HandlerMapVA);
  uint32_t Count = 0;
  if (!Reader.readCompressedUInt(Count) || Count > MaxLanguageRecords) {
    diagnose(F, ExceptionParseStatus::Malformed,
             "invalid FH4 handler-map count");
    return false;
  }
  if (!Budget.consume(Count)) {
    diagnose(F, ExceptionParseStatus::Partial,
             "FH4 aggregate language graph exceeds decode budget");
    return false;
  }
  Try.Handlers.reserve(Count);
  for (uint32_t I = 0; I < Count; ++I) {
    uint8_t Header = 0;
    if (!Reader.readByte(Header) || (Header & 0xc0u) != 0) {
      diagnose(F, ExceptionParseStatus::Malformed,
               "invalid FH4 handler header");
      return false;
    }
    unsigned ContinuationCount = (Header >> 4) & 3u;
    if (ContinuationCount == 3) {
      diagnose(F, ExceptionParseStatus::Malformed,
               "reserved FH4 continuation count");
      return false;
    }

    CxxCatchHandler Catch;
    uint32_t Value = 0;
    if ((Header & 1u) != 0) {
      if (!Reader.readCompressedUInt(Catch.Adjectives)) {
        diagnose(F, ExceptionParseStatus::Malformed,
                 "truncated FH4 catch adjectives");
        return false;
      }
    }
    if ((Header & 2u) != 0) {
      if (!readFH4ImageAddress(Reader, Img.Base, Catch.TypeDescriptorVA)) {
        diagnose(F, ExceptionParseStatus::Malformed,
                 "invalid FH4 type-descriptor RVA");
        return false;
      }
      if (Catch.TypeDescriptorVA != 0 &&
          !Img.readVA(Catch.TypeDescriptorVA, 1)) {
        diagnose(F, ExceptionParseStatus::Malformed,
                 "FH4 type descriptor is not mapped");
        return false;
      }
    }
    if ((Header & 4u) != 0) {
      if (!Reader.readCompressedUInt(Value)) {
        diagnose(F, ExceptionParseStatus::Malformed,
                 "truncated FH4 catch-object displacement");
        return false;
      }
      Catch.CatchObjectOffset = static_cast<int32_t>(Value);
    }
    if (!readFH4ImageAddress(Reader, Img.Base, Catch.HandlerVA,
                             /*AllowZero=*/false) ||
        !isExecutableAddress(Img, Catch.HandlerVA)) {
      diagnose(F, ExceptionParseStatus::Malformed,
               "invalid FH4 catch handler RVA");
      return false;
    }

    Catch.ContinuationVAs.reserve(ContinuationCount);
    for (unsigned J = 0; J < ContinuationCount; ++J) {
      va_t Continuation = 0;
      if ((Header & 8u) != 0) {
        if (!readFH4ImageAddress(Reader, Img.Base, Continuation,
                                 /*AllowZero=*/false)) {
          diagnose(F, ExceptionParseStatus::Malformed,
                   "invalid FH4 image-relative continuation");
          return false;
        }
      } else {
        uint32_t Offset = 0;
        if (!Reader.readCompressedUInt(Offset) ||
            Offset > InvalidVA - F.CodeRange.Begin) {
          diagnose(F, ExceptionParseStatus::Malformed,
                   "invalid FH4 function-relative continuation");
          return false;
        }
        Continuation = F.CodeRange.Begin + Offset;
        if (!F.CodeRange.contains(Continuation)) {
          diagnose(F, ExceptionParseStatus::Malformed,
                   "FH4 function-relative continuation leaves its runtime "
                   "function");
          return false;
        }
      }
      if (!isExecutableAddress(Img, Continuation)) {
        diagnose(F, ExceptionParseStatus::Malformed,
                 "FH4 continuation is not executable");
        return false;
      }
      if (!F.CodeRange.contains(Continuation)) {
        if (!IsSeparated) {
          diagnose(F, ExceptionParseStatus::Malformed,
                   "FH4 image-relative continuation leaves a non-separated "
                   "runtime function");
          return false;
        }
        size_t MatchingContributions = 0;
        for (const ExceptionFunction &Candidate :
             Img.ExceptionMetadata.Functions) {
          if (Candidate.Kind != RuntimeFunctionKind::Primary ||
              !Candidate.CodeRange.contains(Continuation) ||
              Candidate.HandlerDataVA == 0)
            continue;
          auto CandidateRVA =
              readScalar<uint32_t>(Img, Candidate.HandlerDataVA);
          va_t CandidateFuncInfoVA = 0;
          if (CandidateRVA && *CandidateRVA != 0 &&
              addRVA(Img.Base, *CandidateRVA, CandidateFuncInfoVA) &&
              CandidateFuncInfoVA == FuncInfoVA)
            ++MatchingContributions;
        }
        if (MatchingContributions != 1) {
          diagnose(F, ExceptionParseStatus::Malformed,
                   "FH4 continuation has no unique contribution in its "
                   "FuncInfo group");
          return false;
        }
      }
      Catch.ContinuationVAs.push_back(Continuation);
    }
    Try.Handlers.push_back(std::move(Catch));
  }
  return true;
}

} // namespace

bool parseFH4(ExceptionFunction &F, const BinaryImage &Img) {
  auto FuncInfoRVA = readScalar<uint32_t>(Img, F.HandlerDataVA);
  va_t FuncInfoVA = 0;
  if (!FuncInfoRVA || *FuncInfoRVA == 0 ||
      !addRVA(Img.Base, *FuncInfoRVA, FuncInfoVA)) {
    diagnose(F, ExceptionParseStatus::Malformed,
             "invalid FH4 FuncInfo reference");
    return false;
  }

  FH4Reader HeaderReader(Img, FuncInfoVA);
  uint8_t Header = 0;
  if (!HeaderReader.readByte(Header) || (Header & 0x80u) != 0) {
    diagnose(F, ExceptionParseStatus::Malformed, "invalid FH4 FuncInfo header");
    return false;
  }

  CxxExceptionInfo Info;
  LanguageRecordBudget Budget;
  Info.NativeEncoding = CxxExceptionInfo::Encoding::FH4;
  Info.NativeFuncInfoVA = FuncInfoVA;
  Info.Flags = Header;
  Info.IsCatchFunclet = (Header & 1u) != 0;
  Info.IsSeparated = (Header & 2u) != 0;
  Info.IsSynchronous = (Header & 0x20u) != 0;
  Info.IsNoExcept = (Header & 0x40u) != 0;

  if ((Header & 4u) != 0 && !HeaderReader.readCompressedUInt(Info.BBTFlags)) {
    diagnose(F, ExceptionParseStatus::Malformed, "truncated FH4 BBT flags");
    return false;
  }

  va_t UnwindMapVA = 0;
  va_t TryMapVA = 0;
  va_t IPMapVA = 0;
  if (((Header & 8u) != 0 &&
       !readFH4ImageAddress(HeaderReader, Img.Base, UnwindMapVA,
                            /*AllowZero=*/false)) ||
      ((Header & 0x10u) != 0 &&
       !readFH4ImageAddress(HeaderReader, Img.Base, TryMapVA,
                            /*AllowZero=*/false)) ||
      !readFH4ImageAddress(HeaderReader, Img.Base, IPMapVA,
                           /*AllowZero=*/false)) {
    diagnose(F, ExceptionParseStatus::Malformed, "invalid FH4 map reference");
    return false;
  }
  if (Info.IsCatchFunclet &&
      !HeaderReader.readCompressedUInt(Info.FrameOffset)) {
    diagnose(F, ExceptionParseStatus::Malformed,
             "truncated FH4 catch-frame displacement");
    return false;
  }
  // When the function's code was split into several contributions -- POGO and
  // BBT both do this -- the IP-to-state field does not name a map.  It names a
  // directory keyed by the start of each contribution, and the map that
  // applies is the one filed under the runtime function being dispatched.
  // Every contribution shares one unwind map and one try map, so only this
  // lookup differs from the monolithic case; the deltas inside the selected
  // map are still relative to the contribution it belongs to, which is exactly
  // this record's code range.
  bool HasSeparatedStates = true;
  if (Info.IsSeparated) {
    FH4Reader Directory(Img, IPMapVA);
    uint32_t SegmentCount = 0;
    if (!Directory.readCompressedUInt(SegmentCount) || SegmentCount == 0 ||
        SegmentCount > MaxLanguageRecords) {
      diagnose(F, ExceptionParseStatus::Malformed,
               "invalid separated FH4 segment-map count");
      return false;
    }
    if (!Budget.consume(SegmentCount)) {
      diagnose(F, ExceptionParseStatus::Partial,
               "FH4 aggregate language graph exceeds decode budget");
      return false;
    }
    // A contribution the directory does not list simply has no states, which
    // is what the runtime concludes as well.  That is a complete answer, not a
    // failed lookup.
    HasSeparatedStates = false;
    for (uint32_t I = 0; I < SegmentCount; ++I) {
      va_t SegmentStartVA = 0;
      va_t SegmentMapVA = 0;
      if (!readFH4ImageAddress(Directory, Img.Base, SegmentStartVA,
                               /*AllowZero=*/true) ||
          !readFH4ImageAddress(Directory, Img.Base, SegmentMapVA,
                               /*AllowZero=*/true)) {
        diagnose(F, ExceptionParseStatus::Malformed,
                 "truncated separated FH4 segment-map entry");
        return false;
      }
      if (normalizeTableCodeAddress(Img, SegmentStartVA) != F.CodeRange.Begin)
        continue;
      if (SegmentMapVA == 0)
        break;
      IPMapVA = SegmentMapVA;
      HasSeparatedStates = true;
      break;
    }
  }

  if (UnwindMapVA != 0) {
    FH4Reader Reader(Img, UnwindMapVA);
    uint32_t Count = 0;
    if (!Reader.readCompressedUInt(Count) || Count > MaxLanguageRecords) {
      diagnose(F, ExceptionParseStatus::Malformed,
               "invalid FH4 unwind-map count");
      return false;
    }
    if (!Budget.consume(Count)) {
      diagnose(F, ExceptionParseStatus::Partial,
               "FH4 aggregate language graph exceeds decode budget");
      return false;
    }
    Info.MaxState = Count;
    Info.UnwindMap.reserve(Count);
    std::vector<va_t> EntryStarts;
    EntryStarts.reserve(Count);
    std::optional<va_t> EmptyStateTarget;
    for (uint32_t I = 0; I < Count; ++I) {
      va_t EntryStart = Reader.position();
      if (!EmptyStateTarget) {
        if (EntryStart == 0) {
          diagnose(F, ExceptionParseStatus::Malformed,
                   "FH4 unwind-map base underflows");
          return false;
        }
        EmptyStateTarget = EntryStart - 1;
      }
      uint32_t Encoded = 0;
      if (!Reader.readCompressedUInt(Encoded)) {
        diagnose(F, ExceptionParseStatus::Malformed,
                 "truncated FH4 unwind entry");
        return false;
      }
      uint32_t Kind = Encoded & 3u;
      uint32_t NextOffset = Encoded >> 2;
      CxxUnwindAction Action;
      if (NextOffset != 0) {
        if (NextOffset > EntryStart) {
          diagnose(F, ExceptionParseStatus::Malformed,
                   "FH4 unwind predecessor underflows");
          return false;
        }
        va_t Target = EntryStart - NextOffset;
        if (Target != *EmptyStateTarget) {
          auto It = std::find(EntryStarts.begin(), EntryStarts.end(), Target);
          if (It == EntryStarts.end()) {
            diagnose(F, ExceptionParseStatus::Malformed,
                     "FH4 unwind predecessor is not an entry boundary");
            return false;
          }
          Action.ToState =
              static_cast<int32_t>(std::distance(EntryStarts.begin(), It));
        }
      }

      uint32_t ObjectOffset = 0;
      switch (Kind) {
      case 0:
        Action.Kind = CxxUnwindAction::ActionKind::None;
        break;
      case 1:
        Action.Kind = CxxUnwindAction::ActionKind::DestructorWithObject;
        if (!readFH4ImageAddress(Reader, Img.Base, Action.ActionVA,
                                 /*AllowZero=*/false) ||
            !Reader.readCompressedUInt(ObjectOffset)) {
          diagnose(F, ExceptionParseStatus::Malformed,
                   "truncated FH4 object destructor action");
          return false;
        }
        Action.ObjectOffset = static_cast<int32_t>(ObjectOffset);
        break;
      case 2:
        Action.Kind = CxxUnwindAction::ActionKind::DestructorWithObjectPointer;
        if (!readFH4ImageAddress(Reader, Img.Base, Action.ActionVA,
                                 /*AllowZero=*/false) ||
            !Reader.readCompressedUInt(ObjectOffset)) {
          diagnose(F, ExceptionParseStatus::Malformed,
                   "truncated FH4 pointer destructor action");
          return false;
        }
        Action.ObjectOffset = static_cast<int32_t>(ObjectOffset);
        break;
      case 3:
        Action.Kind = CxxUnwindAction::ActionKind::Direct;
        if (!readFH4ImageAddress(Reader, Img.Base, Action.ActionVA,
                                 /*AllowZero=*/false)) {
          diagnose(F, ExceptionParseStatus::Malformed,
                   "truncated FH4 direct unwind action");
          return false;
        }
        break;
      }
      if (Action.ActionVA != 0 && !isExecutableAddress(Img, Action.ActionVA)) {
        diagnose(F, ExceptionParseStatus::Malformed,
                 "FH4 unwind action is not executable");
        return false;
      }
      EntryStarts.push_back(EntryStart);
      Info.UnwindMap.push_back(Action);
    }
  }

  if (TryMapVA != 0) {
    FH4Reader Reader(Img, TryMapVA);
    uint32_t Count = 0;
    if (!Reader.readCompressedUInt(Count) || Count > MaxLanguageRecords) {
      diagnose(F, ExceptionParseStatus::Malformed, "invalid FH4 try-map count");
      return false;
    }
    if (!Budget.consume(Count)) {
      diagnose(F, ExceptionParseStatus::Partial,
               "FH4 aggregate language graph exceeds decode budget");
      return false;
    }
    Info.TryBlocks.reserve(Count);
    for (uint32_t I = 0; I < Count; ++I) {
      uint32_t TryLow = 0;
      uint32_t TryHigh = 0;
      uint32_t CatchHigh = 0;
      va_t HandlerMapVA = 0;
      if (!Reader.readCompressedUInt(TryLow) ||
          !Reader.readCompressedUInt(TryHigh) ||
          !Reader.readCompressedUInt(CatchHigh) ||
          TryLow > std::numeric_limits<int32_t>::max() ||
          TryHigh > std::numeric_limits<int32_t>::max() ||
          CatchHigh > std::numeric_limits<int32_t>::max() ||
          !readFH4ImageAddress(Reader, Img.Base, HandlerMapVA,
                               /*AllowZero=*/false)) {
        diagnose(F, ExceptionParseStatus::Malformed,
                 "invalid FH4 try-map entry");
        return false;
      }
      CxxTryBlock Try;
      Try.TryLow = static_cast<int32_t>(TryLow);
      Try.TryHigh = static_cast<int32_t>(TryHigh);
      Try.CatchHigh = static_cast<int32_t>(CatchHigh);
      if (!parseFH4HandlerMap(F, Img, HandlerMapVA, FuncInfoVA,
                              Info.IsSeparated, Try, Budget))
        return false;
      Info.TryBlocks.push_back(std::move(Try));
    }
  }

  if (HasSeparatedStates) {
    FH4Reader Reader(Img, IPMapVA);
    uint32_t Count = 0;
    if (!Reader.readCompressedUInt(Count) || Count == 0 ||
        Count > MaxLanguageRecords) {
      diagnose(F, ExceptionParseStatus::Malformed,
               "invalid FH4 IP-to-state count");
      return false;
    }
    if (!Budget.consume(Count)) {
      diagnose(F, ExceptionParseStatus::Partial,
               "FH4 aggregate language graph exceeds decode budget");
      return false;
    }
    Info.IPMap.reserve(Count);
    va_t IP = F.CodeRange.Begin;
    for (uint32_t I = 0; I < Count; ++I) {
      uint32_t Delta = 0;
      uint32_t EncodedState = 0;
      if (!Reader.readCompressedUInt(Delta) ||
          !Reader.readCompressedUInt(EncodedState) || Delta > InvalidVA - IP) {
        diagnose(F, ExceptionParseStatus::Malformed,
                 "truncated FH4 IP-to-state entry");
        return false;
      }
      IP += Delta;
      if (!F.CodeRange.contains(IP) && IP != F.CodeRange.End) {
        diagnose(F, ExceptionParseStatus::Malformed,
                 "FH4 IP-to-state entry leaves its runtime function");
        return false;
      }
      if (EncodedState >
          static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) + 1u) {
        diagnose(F, ExceptionParseStatus::Malformed,
                 "FH4 state number exceeds signed range");
        return false;
      }
      int32_t State =
          EncodedState == 0 ? -1 : static_cast<int32_t>(EncodedState - 1);
      Info.IPMap.push_back({IP, State});
    }
  }

  if (!Info.hasValidStateGraph()) {
    diagnose(F, ExceptionParseStatus::Malformed,
             "invalid FH4 exception state graph");
    return false;
  }
  F.Cxx = std::move(Info);
  return true;
}

} // namespace neverd::coff_loader::detail
