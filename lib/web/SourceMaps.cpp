#include "JsonReader.h"

#include "neverd/web/Artifact.h"
#include "neverd/web/Session.h"
#include "neverd/web/SourceMap.h"

#include <algorithm>
#include <array>
#include <limits>

namespace neverd::web {
namespace {
using Object = llvm::json::Object;
using Value = llvm::json::Value;

std::optional<std::string> optionalString(const Object &Object,
                                          const char *Key) {
  const auto *Item = Object.get(Key);
  if (!Item)
    return std::nullopt;
  const auto Text = Item->getAsString();
  if (!Text)
    throw Error("invalid_source_map_field");
  return Text->str();
}

std::optional<std::string> optionalElement(const Value &Value) {
  if (Value.kind() == Value::Null)
    return std::nullopt;
  const auto Text = Value.getAsString();
  if (!Text)
    throw Error("invalid_source_map_field");
  return Text->str();
}

uint64_t coordinate(const Object &Object, const char *Key) {
  const auto Number = Object.getInteger(Key);
  if (!Number || *Number < 0 || uint64_t(*Number) > UINT32_MAX)
    throw Error("invalid_source_map_coordinate");
  return uint64_t(*Number);
}

TextPosition offset(TextPosition Base, TextPosition Relative) {
  TextPosition Result{Base.Line + Relative.Line,
                      Relative.UTF16Column +
                          (Relative.Line ? 0 : Base.UTF16Column)};
  if (Result.Line > UINT32_MAX || Result.UTF16Column > UINT32_MAX)
    throw Error("source_map_coordinate_overflow");
  return Result;
}

int64_t vlq(std::string_view Text, size_t &Cursor) {
  constexpr std::string_view Alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  uint64_t Raw = 0;
  unsigned Shift = 0;
  do {
    if (Cursor == Text.size())
      throw Error("truncated_source_map_vlq");
    if (Shift > 30)
      throw Error("source_map_vlq_budget_exceeded");
    const auto Digit = Alphabet.find(Text[Cursor++]);
    if (Digit == Alphabet.npos)
      throw Error("invalid_source_map_vlq");
    Raw |= uint64_t(Digit & 31) << Shift;
    if (Raw > UINT32_MAX)
      throw Error("source_map_vlq_overflow");
    if (!(Digit & 32))
      break;
    Shift += 5;
  } while (true);
  // ECMA-426 assigns the negative-zero encoding to the minimum int32 value.
  if (Raw == 1)
    return INT32_MIN;
  return Raw & 1 ? -int64_t(Raw >> 1) : int64_t(Raw >> 1);
}

void delta(int64_t &Previous, int64_t Difference) {
  Previous += Difference;
  if (Previous < 0 || uint64_t(Previous) > UINT32_MAX)
    throw Error("invalid_source_map_coordinate");
}

class Decoder {
  SourceMap &Result;

  void mappings(std::string_view Text, TextPosition Base, uint64_t SourceBase,
                uint64_t SourceCount, uint64_t NameBase, uint64_t NameCount) {
    size_t Cursor = 0;
    uint64_t Line = 0;
    int64_t Column = 0, Source = 0, OriginalLine = 0, OriginalColumn = 0,
            Name = 0;
    while (Cursor < Text.size()) {
      if (Text[Cursor] == ';') {
        ++Cursor;
        ++Line;
        Column = 0;
        continue;
      }
      std::array<int64_t, 5> Fields{};
      unsigned Count = 0;
      do {
        if (Count == Fields.size())
          throw Error("invalid_source_map_segment");
        Fields[Count++] = vlq(Text, Cursor);
      } while (Cursor < Text.size() && Text[Cursor] != ',' &&
               Text[Cursor] != ';');
      if (Count != 1 && Count != 4 && Count != 5)
        throw Error("invalid_source_map_segment");
      delta(Column, Fields[0]);
      SourceMapSegment Segment;
      Segment.Generated = offset(Base, {Line, uint64_t(Column)});
      if (Count >= 4) {
        delta(Source, Fields[1]);
        delta(OriginalLine, Fields[2]);
        delta(OriginalColumn, Fields[3]);
        if (uint64_t(Source) >= SourceCount)
          throw Error("invalid_source_map_index");
        Segment.SourceIndex = SourceBase + uint64_t(Source);
        Segment.Original = {uint64_t(OriginalLine), uint64_t(OriginalColumn)};
        if (Count == 5) {
          delta(Name, Fields[4]);
          if (uint64_t(Name) >= NameCount)
            throw Error("invalid_source_map_index");
          Segment.NameIndex = NameBase + uint64_t(Name);
        }
      }
      if (Result.Segments.size() >= MaxSourceMapSegments)
        throw Error("source_map_segment_budget_exceeded");
      Result.Segments.push_back(Segment);
      if (Cursor < Text.size() && Text[Cursor] == ',') {
        ++Cursor;
        if (Cursor == Text.size() || Text[Cursor] == ',' || Text[Cursor] == ';')
          throw Error("invalid_source_map_segment");
      }
    }
  }

  void regular(const Object &Map, TextPosition Base, std::string_view Path) {
    const auto *Sources = Map.getArray("sources");
    const auto Mappings = Map.getString("mappings");
    if (!Sources || !Mappings)
      throw Error("invalid_source_map_field");
    if (Sources->size() > 10000 - Result.Sources.size())
      throw Error("source_map_source_budget_exceeded");
    auto RootText = optionalString(Map, "sourceRoot");
    std::shared_ptr<const std::string> Root;
    if (RootText)
      Root = std::make_shared<const std::string>(std::move(*RootText));
    const auto *Contents = Map.getArray("sourcesContent");
    if (Map.get("sourcesContent") && !Contents)
      throw Error("invalid_source_map_field");
    if (Contents)
      for (const auto &Item : *Contents)
        if (Item.kind() != Value::Null && !Item.getAsString())
          throw Error("invalid_source_map_field");
    const auto SourceBase = Result.Sources.size();
    for (size_t I = 0; I < Sources->size(); ++I) {
      MappedSource Source;
      const auto Index = std::to_string(I);
      Source.ID = identity("map-source", {Result.ID, Path, Index});
      Source.Name = optionalElement((*Sources)[I]);
      Source.Root = Root;
      if (Contents && I < Contents->size())
        Source.Contents = optionalElement((*Contents)[I]);
      Result.Sources.push_back(std::move(Source));
    }
    const auto *Ignore = Map.get("ignoreList");
    if (!Ignore)
      Ignore = Map.get("x_google_ignoreList");
    if (Ignore) {
      const auto *Items = Ignore->getAsArray();
      if (!Items)
        throw Error("invalid_source_map_field");
      for (const auto &Item : *Items) {
        const auto Index = Item.getAsInteger();
        if (!Index || *Index < 0 || uint64_t(*Index) >= Sources->size())
          throw Error("invalid_source_map_index");
        Result.Sources[SourceBase + *Index].Ignored = true;
      }
    }
    const auto NameBase = Result.Names.size();
    if (const auto *Names = Map.get("names")) {
      const auto *Items = Names->getAsArray();
      if (!Items)
        throw Error("invalid_source_map_field");
      if (Items->size() > 100000 - Result.Names.size())
        throw Error("source_map_name_budget_exceeded");
      for (const auto &Item : *Items) {
        const auto Name = Item.getAsString();
        if (!Name)
          throw Error("invalid_source_map_field");
        Result.Names.push_back(Name->str());
      }
    }
    mappings(*Mappings, Base, SourceBase, Sources->size(), NameBase,
             Result.Names.size() - NameBase);
  }

public:
  explicit Decoder(SourceMap &Result) : Result(Result) {}
  void decode(const Object &Map, TextPosition Base, std::string_view Path,
              uint64_t Depth = 0) {
    if (Depth > 4)
      throw Error("source_map_section_depth_exceeded");
    if (Map.getInteger("version") != 3)
      throw Error("unsupported_source_map_version");
    (void)optionalString(Map, "file");
    const auto *SectionsValue = Map.get("sections");
    if (!SectionsValue) {
      regular(Map, Base, Path);
      return;
    }
    if (Map.get("mappings") || Map.get("sources"))
      throw Error("ambiguous_source_map_layout");
    const auto *Sections = SectionsValue->getAsArray();
    if (!Sections)
      throw Error("invalid_source_map_field");
    if (Sections->size() > 1024)
      throw Error("source_map_section_budget_exceeded");
    std::optional<TextPosition> PreviousOffset, PreviousLast;
    uint64_t Index = 0;
    for (const auto &Value : *Sections) {
      const auto *Section = Value.getAsObject();
      if (!Section)
        throw Error("invalid_source_map_field");
      if (Section->get("url"))
        throw Error("external_source_map_section_unsupported");
      const auto *Origin = Section->getObject("offset");
      const auto *Child = Section->getObject("map");
      if (!Origin || !Child)
        throw Error("invalid_source_map_field");
      const auto Start = offset(
          Base, {coordinate(*Origin, "line"), coordinate(*Origin, "column")});
      if ((PreviousOffset && Start <= *PreviousOffset) ||
          (PreviousLast && Start <= *PreviousLast))
        throw Error("overlapping_source_map_sections");
      const auto FirstSegment = Result.Segments.size();
      decode(*Child, Start, std::string(Path) + "/" + std::to_string(Index++),
             Depth + 1);
      for (size_t I = FirstSegment; I < Result.Segments.size(); ++I)
        if (!PreviousLast || *PreviousLast < Result.Segments[I].Generated)
          PreviousLast = Result.Segments[I].Generated;
      PreviousOffset = Start;
    }
  }
};
} // namespace

SourceMap decodeSourceMap(std::string_view ArtifactID, std::string_view Bytes) {
  auto Parsed =
      parseBoundedJSON(Bytes, {MaxSourceMapBytes, 32, 200000, 4 * 1024 * 1024});
  const auto *Object = Parsed.getAsObject();
  if (!Object)
    throw Error("invalid_source_map_field");
  SourceMap Map;
  Map.ArtifactID = ArtifactID;
  Map.BlobHash = sha256(Bytes);
  Map.ID = identity("source-map", {ArtifactID, Map.BlobHash, SourceMapProfile});
  Map.Indexed = Object->get("sections") != nullptr;
  Map.File = optionalString(*Object, "file");
  Decoder(Map).decode(*Object, {}, "");
  std::stable_sort(
      Map.Segments.begin(), Map.Segments.end(),
      [](const auto &A, const auto &B) { return A.Generated < B.Generated; });
  return Map;
}

std::vector<uint64_t> sourceMapAnchors(const SourceMap &Map,
                                       TextPosition Position) {
  const auto End = std::upper_bound(
      Map.Segments.begin(), Map.Segments.end(), Position,
      [](const auto &P, const auto &S) { return P < S.Generated; });
  if (End == Map.Segments.begin() ||
      std::prev(End)->Generated.Line != Position.Line)
    return {};
  const auto Anchor = std::prev(End)->Generated;
  const auto Begin = std::lower_bound(
      Map.Segments.begin(), End, Anchor,
      [](const auto &S, const auto &P) { return S.Generated < P; });
  std::vector<uint64_t> Result;
  for (auto I = Begin; I != End; ++I)
    Result.push_back(uint64_t(std::distance(Map.Segments.begin(), I)));
  return Result;
}
} // namespace neverd::web
