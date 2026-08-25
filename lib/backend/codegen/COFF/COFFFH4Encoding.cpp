//===- COFFFH4Encoding.cpp - Canonical C++ EH4 wire values ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/backend/codegen/COFF/COFFFH4Encoding.h"

#include "llvm/ADT/Twine.h"
#include "llvm/Support/Errc.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace neverd::coff_fh4 {
namespace {

llvm::Error encodingError(const char *Message) {
  return llvm::createStringError(llvm::errc::invalid_argument,
                                 "COFF FH4 encoding: %s", Message);
}

llvm::Error layoutError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::errc::invalid_argument,
                                 "COFF FH4 encoding: %s",
                                 Message.str().c_str());
}

class ParseContext {
public:
  ParseContext(ReadBytes Read, ParseLimits Limits)
      : Read(Read), Limits(Limits) {}

  llvm::Expected<llvm::ArrayRef<uint8_t>> read(uint32_t RVA, uint32_t Size) {
    if (Size == 0)
      return llvm::ArrayRef<uint8_t>();
    if (RVA > std::numeric_limits<uint32_t>::max() - Size)
      return layoutError("RVA range overflows uint32");
    if (BytesConsumed > Limits.MaxBytes ||
        Size > Limits.MaxBytes - BytesConsumed)
      return layoutError("aggregate byte budget exceeded");

    llvm::Expected<llvm::ArrayRef<uint8_t>> Bytes = Read(RVA, Size);
    if (!Bytes)
      return llvm::joinErrors(Bytes.takeError(),
                              layoutError("RVA bytes are not mapped"));
    if (Bytes->size() != Size)
      return layoutError("RVA reader returned an inexact byte range");
    BytesConsumed += Size;
    return *Bytes;
  }

  llvm::Error consumeRecords(uint32_t Count) {
    if (RecordsConsumed > Limits.MaxRecords ||
        Count > Limits.MaxRecords - RecordsConsumed)
      return layoutError("aggregate record budget exceeded");
    RecordsConsumed += Count;
    return llvm::Error::success();
  }

  llvm::Error registerTable(ByteRange Range, llvm::StringRef Name) {
    if (Range.empty() || Range.BeginRVA > Range.EndRVA)
      return layoutError(llvm::Twine(Name) + " has an invalid range");
    Tables.push_back({Range, Name.str()});
    return llvm::Error::success();
  }

  llvm::Error validateTableClosure() {
    std::sort(Tables.begin(), Tables.end(),
              [](const Table &Left, const Table &Right) {
                if (Left.Range.BeginRVA != Right.Range.BeginRVA)
                  return Left.Range.BeginRVA < Right.Range.BeginRVA;
                return Left.Range.EndRVA < Right.Range.EndRVA;
              });
    for (size_t I = 1; I < Tables.size(); ++I) {
      const Table &Previous = Tables[I - 1];
      const Table &Current = Tables[I];
      if (Previous.Range == Current.Range)
        continue;
      if (Previous.Range.EndRVA > Current.Range.BeginRVA)
        return layoutError(llvm::Twine(Previous.Name) + " and " + Current.Name +
                           " overlap");
    }
    return llvm::Error::success();
  }

private:
  struct Table {
    ByteRange Range;
    std::string Name;
  };

  ReadBytes Read;
  ParseLimits Limits;
  uint32_t BytesConsumed = 0;
  uint32_t RecordsConsumed = 0;
  std::vector<Table> Tables;
};

class Cursor {
public:
  Cursor(ParseContext &Context, uint32_t RVA) : Context(Context), RVA(RVA) {}

  uint32_t position() const { return RVA; }

  llvm::Expected<uint8_t> readByte() {
    llvm::Expected<llvm::ArrayRef<uint8_t>> Bytes = Context.read(RVA, 1);
    if (!Bytes)
      return Bytes.takeError();
    ++RVA;
    return Bytes->front();
  }

  llvm::Expected<uint32_t> readUInt32() {
    llvm::Expected<llvm::ArrayRef<uint8_t>> Bytes = Context.read(RVA, 4);
    if (!Bytes)
      return Bytes.takeError();
    RVA += 4;
    return uint32_t((*Bytes)[0]) | (uint32_t((*Bytes)[1]) << 8) |
           (uint32_t((*Bytes)[2]) << 16) | (uint32_t((*Bytes)[3]) << 24);
  }

  llvm::Expected<CompressedUInt> readCompressedUInt() {
    llvm::Expected<uint8_t> First = readByte();
    if (!First)
      return First.takeError();

    unsigned Size = 0;
    if ((*First & 1u) == 0)
      Size = 1;
    else if ((*First & 3u) == 1)
      Size = 2;
    else if ((*First & 7u) == 3)
      Size = 3;
    else if ((*First & 15u) == 7)
      Size = 4;
    else
      Size = 5;

    llvm::SmallVector<uint8_t, 5> Encoded(Size);
    Encoded.front() = *First;
    if (Size != 1) {
      llvm::Expected<llvm::ArrayRef<uint8_t>> Tail =
          Context.read(RVA, Size - 1);
      if (!Tail)
        return Tail.takeError();
      std::copy(Tail->begin(), Tail->end(), Encoded.begin() + 1);
      RVA += Size - 1;
    }
    return decodeCompressedUInt(Encoded);
  }

private:
  ParseContext &Context;
  uint32_t RVA;
};

llvm::Expected<uint32_t> readRequiredRVA(Cursor &Reader,
                                         llvm::StringRef Context) {
  llvm::Expected<uint32_t> RVA = Reader.readUInt32();
  if (!RVA)
    return RVA.takeError();
  if (*RVA == 0)
    return layoutError(llvm::Twine(Context) + " RVA is zero");
  return *RVA;
}

llvm::Expected<HandlerMap> parseHandlerMap(ParseContext &Context,
                                           uint32_t HandlerMapRVA) {
  Cursor Reader(Context, HandlerMapRVA);
  llvm::Expected<CompressedUInt> Count = Reader.readCompressedUInt();
  if (!Count)
    return Count.takeError();
  if (llvm::Error Error = Context.consumeRecords(Count->Value))
    return std::move(Error);

  HandlerMap Result;
  Result.Entries.reserve(Count->Value);
  for (uint32_t I = 0; I != Count->Value; ++I) {
    HandlerEntry Entry;
    Entry.Range.BeginRVA = Reader.position();
    llvm::Expected<uint8_t> Header = Reader.readByte();
    if (!Header)
      return Header.takeError();
    if ((*Header & 0xc0u) != 0)
      return layoutError("invalid FH4 handler header");
    Entry.Header = *Header;

    const unsigned ContinuationCount = (*Header >> 4) & 3u;
    if (ContinuationCount == 3)
      return layoutError("reserved FH4 continuation count");
    Entry.ContinuationsAreImageRelative = (*Header & 8u) != 0;

    if ((*Header & 1u) != 0) {
      llvm::Expected<CompressedUInt> Adjectives = Reader.readCompressedUInt();
      if (!Adjectives)
        return Adjectives.takeError();
      Entry.Adjectives = Adjectives->Value;
    }
    if ((*Header & 2u) != 0) {
      llvm::Expected<uint32_t> TypeRVA = Reader.readUInt32();
      if (!TypeRVA)
        return TypeRVA.takeError();
      Entry.TypeDescriptorRVA = *TypeRVA;
    }
    if ((*Header & 4u) != 0) {
      llvm::Expected<CompressedUInt> ObjectOffset = Reader.readCompressedUInt();
      if (!ObjectOffset)
        return ObjectOffset.takeError();
      Entry.CatchObjectOffset = ObjectOffset->Value;
    }

    llvm::Expected<uint32_t> HandlerRVA =
        readRequiredRVA(Reader, "catch handler");
    if (!HandlerRVA)
      return HandlerRVA.takeError();
    Entry.HandlerRVA = *HandlerRVA;

    Entry.Continuations.reserve(ContinuationCount);
    for (unsigned J = 0; J != ContinuationCount; ++J) {
      uint32_t Continuation = 0;
      if (Entry.ContinuationsAreImageRelative) {
        llvm::Expected<uint32_t> ContinuationRVA =
            readRequiredRVA(Reader, "catch continuation");
        if (!ContinuationRVA)
          return ContinuationRVA.takeError();
        Continuation = *ContinuationRVA;
      } else {
        llvm::Expected<CompressedUInt> FunctionOffset =
            Reader.readCompressedUInt();
        if (!FunctionOffset)
          return FunctionOffset.takeError();
        Continuation = FunctionOffset->Value;
      }
      Entry.Continuations.push_back(Continuation);
    }
    Entry.Range.EndRVA = Reader.position();
    Result.Entries.push_back(std::move(Entry));
  }
  Result.Range = {HandlerMapRVA, Reader.position()};
  if (llvm::Error Error =
          Context.registerTable(Result.Range, "FH4 handler map"))
    return std::move(Error);
  return Result;
}

llvm::Expected<UnwindMap> parseUnwindMap(ParseContext &Context,
                                         uint32_t UnwindMapRVA) {
  Cursor Reader(Context, UnwindMapRVA);
  llvm::Expected<CompressedUInt> Count = Reader.readCompressedUInt();
  if (!Count)
    return Count.takeError();
  if (llvm::Error Error = Context.consumeRecords(Count->Value))
    return std::move(Error);

  UnwindMap Result;
  Result.Entries.reserve(Count->Value);
  std::vector<uint32_t> EntryStarts;
  EntryStarts.reserve(Count->Value);
  std::optional<uint32_t> EmptyStateTarget;
  for (uint32_t I = 0; I != Count->Value; ++I) {
    UnwindEntry Entry;
    Entry.Range.BeginRVA = Reader.position();
    if (!EmptyStateTarget) {
      if (Entry.Range.BeginRVA == 0)
        return layoutError("FH4 unwind-map base underflows");
      EmptyStateTarget = Entry.Range.BeginRVA - 1;
    }

    llvm::Expected<CompressedUInt> Encoded = Reader.readCompressedUInt();
    if (!Encoded)
      return Encoded.takeError();
    Entry.Encoded = Encoded->Value;
    const uint32_t Kind = Entry.Encoded & 3u;
    const uint32_t NextOffset = Entry.Encoded >> 2;
    if (NextOffset != 0) {
      if (NextOffset > Entry.Range.BeginRVA)
        return layoutError("FH4 unwind predecessor underflows");
      const uint32_t Target = Entry.Range.BeginRVA - NextOffset;
      if (Target != *EmptyStateTarget) {
        auto It = std::find(EntryStarts.begin(), EntryStarts.end(), Target);
        if (It == EntryStarts.end())
          return layoutError("FH4 unwind predecessor is not an entry boundary");
        Entry.ToState =
            static_cast<int32_t>(std::distance(EntryStarts.begin(), It));
      }
    }

    switch (Kind) {
    case 0:
      Entry.Kind = UnwindActionKind::None;
      break;
    case 1:
    case 2: {
      Entry.Kind = Kind == 1 ? UnwindActionKind::DestructorWithObject
                             : UnwindActionKind::DestructorWithObjectPointer;
      llvm::Expected<uint32_t> ActionRVA =
          readRequiredRVA(Reader, "unwind action");
      if (!ActionRVA)
        return ActionRVA.takeError();
      Entry.ActionRVA = *ActionRVA;
      llvm::Expected<CompressedUInt> ObjectOffset = Reader.readCompressedUInt();
      if (!ObjectOffset)
        return ObjectOffset.takeError();
      Entry.ObjectOffset = ObjectOffset->Value;
      break;
    }
    case 3: {
      Entry.Kind = UnwindActionKind::Direct;
      llvm::Expected<uint32_t> ActionRVA =
          readRequiredRVA(Reader, "unwind action");
      if (!ActionRVA)
        return ActionRVA.takeError();
      Entry.ActionRVA = *ActionRVA;
      break;
    }
    }

    Entry.Range.EndRVA = Reader.position();
    EntryStarts.push_back(Entry.Range.BeginRVA);
    Result.Entries.push_back(std::move(Entry));
  }
  Result.Range = {UnwindMapRVA, Reader.position()};
  if (llvm::Error Error = Context.registerTable(Result.Range, "FH4 unwind map"))
    return std::move(Error);
  return Result;
}

llvm::Expected<TryMap> parseTryMap(ParseContext &Context, uint32_t TryMapRVA) {
  Cursor Reader(Context, TryMapRVA);
  llvm::Expected<CompressedUInt> Count = Reader.readCompressedUInt();
  if (!Count)
    return Count.takeError();
  if (llvm::Error Error = Context.consumeRecords(Count->Value))
    return std::move(Error);

  TryMap Result;
  Result.Entries.reserve(Count->Value);
  for (uint32_t I = 0; I != Count->Value; ++I) {
    TryEntry Entry;
    Entry.Range.BeginRVA = Reader.position();
    llvm::Expected<CompressedUInt> TryLow = Reader.readCompressedUInt();
    llvm::Expected<CompressedUInt> TryHigh = Reader.readCompressedUInt();
    llvm::Expected<CompressedUInt> CatchHigh = Reader.readCompressedUInt();
    if (!TryLow)
      return TryLow.takeError();
    if (!TryHigh)
      return TryHigh.takeError();
    if (!CatchHigh)
      return CatchHigh.takeError();
    if (TryLow->Value > std::numeric_limits<int32_t>::max() ||
        TryHigh->Value > std::numeric_limits<int32_t>::max() ||
        CatchHigh->Value > std::numeric_limits<int32_t>::max())
      return layoutError("FH4 try state exceeds the signed range");
    Entry.TryLow = TryLow->Value;
    Entry.TryHigh = TryHigh->Value;
    Entry.CatchHigh = CatchHigh->Value;

    llvm::Expected<uint32_t> HandlerMapRVA =
        readRequiredRVA(Reader, "handler map");
    if (!HandlerMapRVA)
      return HandlerMapRVA.takeError();
    Entry.HandlerMapRVA = *HandlerMapRVA;
    Entry.Range.EndRVA = Reader.position();

    llvm::Expected<HandlerMap> Handlers =
        parseHandlerMap(Context, Entry.HandlerMapRVA);
    if (!Handlers)
      return Handlers.takeError();
    Entry.Handlers = std::move(*Handlers);
    Result.Entries.push_back(std::move(Entry));
  }
  Result.Range = {TryMapRVA, Reader.position()};
  if (llvm::Error Error = Context.registerTable(Result.Range, "FH4 try map"))
    return std::move(Error);
  return Result;
}

llvm::Expected<IPMap> parseIPMap(ParseContext &Context, uint32_t IPMapRVA) {
  Cursor Reader(Context, IPMapRVA);
  llvm::Expected<CompressedUInt> Count = Reader.readCompressedUInt();
  if (!Count)
    return Count.takeError();
  if (Count->Value == 0)
    return layoutError("FH4 IP-to-state map is empty");
  if (llvm::Error Error = Context.consumeRecords(Count->Value))
    return std::move(Error);

  IPMap Result;
  Result.Entries.reserve(Count->Value);
  uint32_t FunctionOffset = 0;
  for (uint32_t I = 0; I != Count->Value; ++I) {
    IPEntry Entry;
    Entry.Range.BeginRVA = Reader.position();
    llvm::Expected<CompressedUInt> Delta = Reader.readCompressedUInt();
    llvm::Expected<CompressedUInt> State = Reader.readCompressedUInt();
    if (!Delta)
      return Delta.takeError();
    if (!State)
      return State.takeError();
    if (Delta->Value > std::numeric_limits<uint32_t>::max() - FunctionOffset)
      return layoutError("FH4 IP-to-state offset overflows uint32");
    if (State->Value >
        static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) + 1u)
      return layoutError("FH4 IP-to-state number exceeds the signed range");
    FunctionOffset += Delta->Value;
    Entry.Delta = Delta->Value;
    Entry.EncodedState = State->Value;
    Entry.State =
        State->Value == 0 ? -1 : static_cast<int32_t>(State->Value - 1);
    Entry.FunctionOffset = FunctionOffset;
    Entry.Range.EndRVA = Reader.position();
    Result.Entries.push_back(std::move(Entry));
  }
  Result.Range = {IPMapRVA, Reader.position()};
  if (llvm::Error Error = Context.registerTable(Result.Range, "FH4 IP map"))
    return std::move(Error);
  return Result;
}

llvm::Expected<SeparatedIPMap> parseSeparatedIPMap(ParseContext &Context,
                                                   uint32_t DirectoryRVA) {
  Cursor Reader(Context, DirectoryRVA);
  llvm::Expected<CompressedUInt> Count = Reader.readCompressedUInt();
  if (!Count)
    return Count.takeError();
  if (Count->Value == 0)
    return layoutError("FH4 separated IP directory is empty");
  if (llvm::Error Error = Context.consumeRecords(Count->Value))
    return std::move(Error);

  SeparatedIPMap Result;
  Result.Entries.reserve(Count->Value);
  for (uint32_t I = 0; I != Count->Value; ++I) {
    SeparatedIPEntry Entry;
    Entry.Range.BeginRVA = Reader.position();
    llvm::Expected<uint32_t> FunctionStartRVA = Reader.readUInt32();
    llvm::Expected<uint32_t> IPMapRVA = Reader.readUInt32();
    if (!FunctionStartRVA)
      return FunctionStartRVA.takeError();
    if (!IPMapRVA)
      return IPMapRVA.takeError();
    Entry.FunctionStartRVA = *FunctionStartRVA;
    Entry.IPMapRVA = *IPMapRVA;
    Entry.Range.EndRVA = Reader.position();
    if (Entry.IPMapRVA != 0) {
      llvm::Expected<IPMap> States = parseIPMap(Context, Entry.IPMapRVA);
      if (!States)
        return States.takeError();
      Entry.States = std::move(*States);
    }
    Result.Entries.push_back(std::move(Entry));
  }
  Result.Range = {DirectoryRVA, Reader.position()};
  if (llvm::Error Error =
          Context.registerTable(Result.Range, "FH4 separated IP directory"))
    return std::move(Error);
  return Result;
}

} // namespace

llvm::Expected<CompressedUInt>
decodeCompressedUInt(llvm::ArrayRef<uint8_t> Bytes) {
  if (Bytes.empty())
    return encodingError("truncated compressed integer");

  const uint8_t First = Bytes.front();
  unsigned Size = 0;
  unsigned InitialShift = 0;
  uint32_t Minimum = 0;
  if ((First & 1u) == 0) {
    Size = 1;
    InitialShift = 1;
  } else if ((First & 3u) == 1) {
    Size = 2;
    InitialShift = 2;
    Minimum = 1u << 7;
  } else if ((First & 7u) == 3) {
    Size = 3;
    InitialShift = 3;
    Minimum = 1u << 14;
  } else if ((First & 15u) == 7) {
    Size = 4;
    InitialShift = 4;
    Minimum = 1u << 21;
  } else {
    if (First != 15)
      return encodingError("reserved compressed integer prefix");
    Size = 5;
    Minimum = 1u << 28;
  }

  if (Bytes.size() < Size)
    return encodingError("truncated compressed integer");

  uint32_t Value = 0;
  if (Size == 5) {
    for (unsigned I = 0; I != 4; ++I)
      Value |= uint32_t(Bytes[I + 1]) << (I * 8);
  } else {
    Value = First >> InitialShift;
    unsigned Shift = 8 - InitialShift;
    for (unsigned I = 1; I != Size; ++I, Shift += 8)
      Value |= uint32_t(Bytes[I]) << Shift;
  }

  if (Value < Minimum)
    return encodingError("overlong compressed integer");
  return CompressedUInt{Value, static_cast<uint8_t>(Size)};
}

llvm::SmallVector<uint8_t, 5> encodeCompressedUInt(uint32_t Value) {
  llvm::SmallVector<uint8_t, 5> Bytes;
  auto AppendLE = [&](uint32_t Encoded, unsigned Size) {
    for (unsigned I = 0; I != Size; ++I)
      Bytes.push_back(static_cast<uint8_t>(Encoded >> (I * 8)));
  };

  if (Value < (1u << 7)) {
    AppendLE(Value << 1, 1);
  } else if (Value < (1u << 14)) {
    AppendLE((Value << 2) | 1u, 2);
  } else if (Value < (1u << 21)) {
    AppendLE((Value << 3) | 3u, 3);
  } else if (Value < (1u << 28)) {
    AppendLE((Value << 4) | 7u, 4);
  } else {
    Bytes.push_back(15);
    AppendLE(Value, 4);
  }
  return Bytes;
}

llvm::Expected<FuncInfoLayout>
parseFuncInfoLayout(uint32_t FuncInfoRVA, ReadBytes Read, ParseLimits Limits) {
  if (Limits.MaxRecords == 0)
    return layoutError("record budget is zero");
  if (Limits.MaxBytes == 0)
    return layoutError("byte budget is zero");

  ParseContext Context(Read, Limits);
  Cursor HeaderReader(Context, FuncInfoRVA);
  FuncInfoLayout Result;
  Result.HeaderRange.BeginRVA = FuncInfoRVA;

  llvm::Expected<uint8_t> Header = HeaderReader.readByte();
  if (!Header)
    return Header.takeError();
  if ((*Header & 0x80u) != 0)
    return layoutError("invalid FH4 FuncInfo header");
  Result.Header = *Header;

  if ((*Header & 4u) != 0) {
    llvm::Expected<CompressedUInt> BBTFlags = HeaderReader.readCompressedUInt();
    if (!BBTFlags)
      return BBTFlags.takeError();
    Result.BBTFlags = BBTFlags->Value;
  }
  if ((*Header & 8u) != 0) {
    llvm::Expected<uint32_t> RVA = readRequiredRVA(HeaderReader, "unwind map");
    if (!RVA)
      return RVA.takeError();
    Result.UnwindMapRVA = *RVA;
  }
  if ((*Header & 0x10u) != 0) {
    llvm::Expected<uint32_t> RVA = readRequiredRVA(HeaderReader, "try map");
    if (!RVA)
      return RVA.takeError();
    Result.TryMapRVA = *RVA;
  }
  llvm::Expected<uint32_t> IPMapRVA =
      readRequiredRVA(HeaderReader, "IP-to-state map");
  if (!IPMapRVA)
    return IPMapRVA.takeError();
  Result.IPMapRVA = *IPMapRVA;

  if ((*Header & 1u) != 0) {
    llvm::Expected<CompressedUInt> FrameOffset =
        HeaderReader.readCompressedUInt();
    if (!FrameOffset)
      return FrameOffset.takeError();
    Result.FrameOffset = FrameOffset->Value;
  }
  Result.HeaderRange.EndRVA = HeaderReader.position();
  if (llvm::Error Error =
          Context.registerTable(Result.HeaderRange, "FH4 FuncInfo"))
    return std::move(Error);

  if (Result.UnwindMapRVA != 0) {
    llvm::Expected<UnwindMap> Map =
        parseUnwindMap(Context, Result.UnwindMapRVA);
    if (!Map)
      return Map.takeError();
    Result.Unwind = std::move(*Map);
  }
  if (Result.TryMapRVA != 0) {
    llvm::Expected<TryMap> Map = parseTryMap(Context, Result.TryMapRVA);
    if (!Map)
      return Map.takeError();
    Result.Try = std::move(*Map);
  }
  if ((*Header & 2u) != 0) {
    llvm::Expected<SeparatedIPMap> Map =
        parseSeparatedIPMap(Context, Result.IPMapRVA);
    if (!Map)
      return Map.takeError();
    Result.SeparatedStates = std::move(*Map);
  } else {
    llvm::Expected<IPMap> Map = parseIPMap(Context, Result.IPMapRVA);
    if (!Map)
      return Map.takeError();
    Result.States = std::move(*Map);
  }

  if (llvm::Error Error = Context.validateTableClosure())
    return std::move(Error);
  return Result;
}

} // namespace neverd::coff_fh4
