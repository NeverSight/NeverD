//===- GGUF.cpp - GGUF directories, packing and unreferenced bytes --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// GGUF directories, packing and unreferenced bytes.
//===----------------------------------------------------------------------===//

#include "ModelInternal.h"

#include "neverd/support/SHA256.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <tuple>

namespace neverd::model {
namespace {
struct Layout {
  unsigned Id;
  const char *Name;
  unsigned Block, Bytes;
};
// GGML on-disk block dimensions. Retired and unrecognized IDs stay opaque.
constexpr Layout Storage[] = {
    {0, "F32", 1, 4},         {1, "F16", 1, 2},
    {2, "Q4_0", 32, 18},      {3, "Q4_1", 32, 20},
    {6, "Q5_0", 32, 22},      {7, "Q5_1", 32, 24},
    {8, "Q8_0", 32, 34},      {9, "Q8_1", 32, 36},
    {10, "Q2_K", 256, 84},    {11, "Q3_K", 256, 110},
    {12, "Q4_K", 256, 144},   {13, "Q5_K", 256, 176},
    {14, "Q6_K", 256, 210},   {15, "Q8_K", 256, 292},
    {16, "IQ2_XXS", 256, 66}, {17, "IQ2_XS", 256, 74},
    {18, "IQ3_XXS", 256, 98}, {19, "IQ1_S", 256, 50},
    {20, "IQ4_NL", 32, 18},   {21, "IQ3_S", 256, 110},
    {22, "IQ2_S", 256, 82},   {23, "IQ4_XS", 256, 136},
    {24, "I8", 1, 1},         {25, "I16", 1, 2},
    {26, "I32", 1, 4},        {27, "I64", 1, 8},
    {28, "F64", 1, 8},        {29, "IQ1_M", 256, 56},
    {30, "BF16", 1, 2},       {34, "TQ1_0", 256, 54},
    {35, "TQ2_0", 256, 66},   {39, "MXFP4", 32, 17},
    {40, "NVFP4", 64, 36},    {41, "Q1_0", 128, 18},
    {42, "Q2_0", 64, 18},     {43, "F8_E4M3", 1, 1}};
constexpr std::array<unsigned, 13> Width = {1, 1, 2, 2, 4, 4, 4,
                                            1, 0, 0, 8, 8, 8};
constexpr std::array<const char *, 13> Types = {
    "u8",   "i8",     "u16",   "i16", "u32", "i32", "f32",
    "bool", "string", "array", "u64", "i64", "f64"};
uint64_t align(uint64_t V, uint64_t A) { return sum(V, (A - V % A) % A); }
uint64_t bits(std::string_view S, bool Big) {
  uint64_t V = 0;
  for (size_t I = 0; I < S.size(); ++I)
    V = (V << 8) | static_cast<unsigned char>(S[Big ? I : S.size() - 1 - I]);
  return V;
}
class DirectoryReader {
  File &Input;
  Budget &ReadBudget;
  uint64_t Offset = 0;
  uint64_t MinimumEnd = 0;
  uint64_t CacheStart = 0;
  uint64_t HashedThrough = 0;
  std::string Cache;
  bool BigEndian = false;
  SHA256Stream DirectoryHash;
  SHA256Stream ValueHash;
  bool InMetadataValue = false;

  // Both digests consume the same contiguous bytes while inside a value.
  // Flush at digest boundaries and before replacing the backing storage.
  void hashConsumedBytes() {
    if (HashedThrough == Offset)
      return;
    llvm::StringRef Bytes = llvm::StringRef(Cache).substr(
        HashedThrough - CacheStart, Offset - HashedThrough);
    DirectoryHash.update(Bytes);
    if (InMetadataValue)
      ValueHash.update(Bytes);
    HashedThrough = Offset;
  }

public:
  DirectoryReader(File &Input, Budget &ReadBudget)
      : Input(Input), ReadBudget(ReadBudget) {}

  uint64_t position() const { return Offset; }
  bool isBigEndian() const { return BigEndian; }
  void setBigEndian() { BigEndian = true; }

  void expectBytes(uint64_t Size) {
    MinimumEnd = std::max(MinimumEnd, sum(Offset, Size));
  }

  llvm::StringRef readBytes(uint64_t Size) {
    uint64_t End = sum(Offset, Size);
    MinimumEnd = std::max(MinimumEnd, End);
    if (End > CacheStart + Cache.size()) {
      hashConsumedBytes();
      CacheStart = Offset;
      // Look ahead only into bytes already required by directory records.
      uint64_t Lookahead =
          std::min<uint64_t>(limits::kChunk, MinimumEnd - Offset);
      Cache = Input.header(Offset, std::max(Size, Lookahead), ReadBudget);
    }
    llvm::StringRef Bytes =
        llvm::StringRef(Cache).substr(Offset - CacheStart, Size);
    Offset = End;
    return Bytes;
  }

  uint64_t readUInt(unsigned Size) { return bits(readBytes(Size), BigEndian); }
  llvm::StringRef readString(uint64_t Max) {
    uint64_t N = readUInt(8);
    if (N > Max)
      throw Error("GGUF string exceeds inspection limit");
    auto V = readBytes(N);
    if (!llvm::json::isUTF8(V))
      throw Error("invalid UTF-8 in GGUF string");
    return V;
  }
  std::pair<Json, std::string> readMetadataValue(unsigned Type,
                                                 bool KeepString) {
    hashConsumedBytes();
    InMetadataValue = true;
    Json Value = readValue(Type, 0, KeepString);
    hashConsumedBytes();
    InMetadataValue = false;
    return {std::move(Value), llvm::toHex(ValueHash.final(), true)};
  }

  std::string finishDirectoryHash() {
    hashConsumedBytes();
    return llvm::toHex(DirectoryHash.final(), true);
  }

private:
  Json readValue(unsigned Type, unsigned Depth, bool KeepString) {
    ReadBudget.tick();
    if (Depth > limits::kDepth || Type >= Width.size())
      throw Error("unsupported GGUF metadata type or nesting");
    if (Type == 8) {
      auto S = readString(KeepString ? limits::kTemplateBytes
                                     : limits::kHeaderBytes);
      return KeepString ? Json(S.str()) : Json(nullptr);
    }
    if (Type == 9) {
      uint64_t Child = readUInt(4), N = readUInt(8);
      if (Child >= Width.size())
        throw Error("unknown GGUF array element type");
      if (Width[Child]) {
        auto Raw = readBytes(product(N, Width[Child]));
        if (Child == 7 && std::any_of(Raw.begin(), Raw.end(),
                                      [](unsigned char C) { return C > 1; }))
          throw Error("invalid GGUF boolean array");
      } else {
        if (N > ReadBudget.WorkLeft)
          throw Stop("GGUF array exceeds work budget");
        for (uint64_t I = 0; I < N; ++I) {
          expectBytes(product(N - I, Child == 8 ? 8 : 12));
          readValue(Child, Depth + 1, false);
        }
      }
      return Json{{"element_type", Types[Child]}, {"count", N}};
    }
    uint64_t V = readUInt(Width[Type]);
    if (Type == 7) {
      if (V > 1)
        throw Error("invalid GGUF boolean");
      return bool(V);
    }
    if (Type == 6)
      return std::bit_cast<float>(static_cast<uint32_t>(V));
    if (Type == 12)
      return std::bit_cast<double>(V);
    if (Type == 1 || Type == 3 || Type == 5 || Type == 11) {
      unsigned Shift = 64 - Width[Type] * 8;
      return std::bit_cast<int64_t>(V << Shift) >> Shift;
    }
    return V;
  }
};
enum class Region { Alignment, Gap, FinalAlignment, Trailer };
void inspectRange(Package &P, File &F, const std::string &Name, uint64_t At,
                  uint64_t N, Region Kind) {
  if (!N)
    return;
  const uint64_t Inspect = std::min(N, P.B.GapLeft);
  uint64_t Done = 0;
  std::optional<uint64_t> Nonzero;
  std::string Carry;
  static constexpr std::pair<std::string_view, std::string_view> Signatures[] =
      {{"PK\x03\x04", "ZIP local header"},
       {"\x7f"
        "ELF",
        "ELF magic"},
       {"%PDF-", "PDF prefix"},
       {"\x89PNG\r\n\x1a\n", "PNG signature"},
       {"GGUF", "GGUF magic"}};
  bool Signature = false;
  while (Done < Inspect) {
    P.B.tick();
    std::string Chunk(std::min<uint64_t>(limits::kChunk, Inspect - Done), '\0');
    F.read(At + Done, Chunk);
    auto I = Chunk.find_first_not_of('\0');
    if (!Nonzero && I != Chunk.npos)
      Nonzero = At + Done + I;
    if (!Signature) {
      std::string Window = Carry + Chunk;
      for (const auto &[Magic, Label] : Signatures) {
        auto Hit = Window.find(Magic);
        if (Hit == Window.npos)
          continue;
        P.finding(
            "layout.secondary-signature", Name, "review", "container",
            "observed",
            "Recognizable signature in unreferenced storage. Inspect this "
            "range.",
            {{"offset", At + Done - Carry.size() + Hit}, {"signature", Label}});
        Signature = true;
        break;
      }
      Carry = Window.substr(Window.size() - std::min<size_t>(7, Window.size()));
    }
    Done += Chunk.size();
    P.B.GapLeft -= Chunk.size();
  }
  const bool Padding =
      Kind == Region::Alignment || Kind == Region::FinalAlignment;
  if (Nonzero || !Padding) {
    const char *RangeKind = Kind == Region::Alignment ? "alignment-padding"
                            : Kind == Region::FinalAlignment
                                ? "possible-final-alignment"
                            : Kind == Region::Trailer ? "trailer"
                                                      : "gap";
    const char *Message = Kind == Region::Alignment
                              ? "Alignment padding contains nonzero bytes."
                          : Kind == Region::FinalAlignment
                              ? "Nonzero bytes follow the final tensor before "
                                "the next alignment boundary."
                              : "Bytes outside declared tensor storage.";
    P.finding(Padding ? "layout.nonzero-padding" : "layout.unreferenced", Name,
              Nonzero ? "review" : "info", "container", "observed", Message,
              {{"range_kind", RangeKind},
               {"offset", At},
               {"length", N},
               {"inspected_bytes", Inspect},
               {"first_nonzero", Nonzero ? Json(*Nonzero) : Json(nullptr)}});
  }
  if (Inspect < N)
    P.gap(Name, "unreferenced-byte budget exhausted",
          {{"offset", At + Inspect}, {"length", N - Inspect}});
}
void scales(Package &P, File &F, const std::string &Name,
            const std::string &TensorName, const Tensor &T, bool Big,
            Json &Coverage) {
  if (!P.B.Opts.ScaleBytes || !T.Bytes)
    return;
  unsigned Block = T.Type == "Q4_0" ? 18 : T.Type == "Q8_0" ? 34 : 0;
  if (!Block) {
    Coverage["unsupported_tensors"] =
        Coverage["unsupported_tensors"].get<unsigned>() + 1;
    return;
  }
  uint64_t Blocks = *T.Bytes / Block,
           Checked = std::min(Blocks, P.B.ScaleLeft / Block), Hits = 0,
           First = 0, NegativeZeros = 0;
  for (uint64_t I = 0; I < Checked;) {
    P.B.tick();
    auto Count = std::min<uint64_t>(Checked - I, limits::kChunk / Block);
    std::string Data(Count * Block, '\0');
    F.read(T.Offset + I * Block, Data);
    P.WeightBytesRead += Data.size();
    P.B.ScaleLeft -= Data.size();
    for (uint64_t J = 0; J < Count; ++J) {
      std::string_view Row(Data.data() + J * Block, Block);
      auto ScaleBits = bits(Row.substr(0, 2), Big);
      NegativeZeros += ScaleBits == 0x8000;
      if ((ScaleBits & 0x7fff) != 0)
        continue;
      const auto FirstByte = static_cast<unsigned char>(Row[2]);
      bool DifferentNibbles =
          Block == 18 && (FirstByte & 15) != (FirstByte >> 4);
      if (DifferentNibbles || Row.find_first_not_of(Row[2], 3) != Row.npos) {
        if (!Hits)
          First = T.Offset + (I + J) * Block;
        ++Hits;
      }
    }
    I += Count;
  }
  if (Checked) {
    auto Range = Coverage["range_count"].get<uint64_t>();
    Coverage["range_count"] = Range + 1;
    if (Range >= P.B.Opts.Offset && Coverage["ranges"].size() < P.B.Opts.Limit)
      Coverage["ranges"].push_back({{"tensor", TensorName},
                                    {"offset", T.Offset},
                                    {"length", Checked * Block},
                                    {"total_blocks", Blocks}});
  }
  Coverage["negative_zero_scales"] =
      Coverage["negative_zero_scales"].get<uint64_t>() + NegativeZeros;
  Coverage["blocks_checked"] =
      Coverage["blocks_checked"].get<uint64_t>() + Checked;
  Coverage["eligible_blocks"] =
      Coverage["eligible_blocks"].get<uint64_t>() + Blocks;
  if (Hits)
    P.finding(
        "quant.zero-scale-varying-codes", Name, "review", "quantization",
        "observed",
        "Zero scale with varying quantized codes. Check conversion provenance.",
        {{"tensor", TensorName}, {"first_offset", First}, {"blocks", Hits}});
}
} // namespace

void readGGUF(Package &P, const std::string &Name) {
  File &F = P.file(Name);
  DirectoryReader R(F, P.B);
  if (R.readBytes(4) != "GGUF")
    throw Error("invalid GGUF magic");
  auto V = R.readBytes(4);
  uint64_t Version = bits(V, false);
  if (Version != 2 && Version != 3) {
    Version = bits(V, true);
    R.setBigEndian();
    if (Version != 3)
      throw Error("unsupported GGUF version or byte order");
  }
  uint64_t NT = R.readUInt(8), NM = R.readUInt(8), A = 32;
  if (NT > limits::kEntries || NM > limits::kEntries)
    throw Error("GGUF directory count exceeds limit");
  std::set<std::string> Keys;
  Json Split = Json::object();
  for (uint64_t I = 0; I < NM; ++I) {
    P.B.entry();
    R.expectBytes(sum(product(NM - I, 13), product(NT, 24)));
    uint64_t At = R.position();
    std::string Key(R.readString(limits::kNameBytes));
    validName(Key);
    if (!Keys.insert(Key).second)
      throw Error("duplicate GGUF metadata key");
    uint64_t Type = R.readUInt(4), ValueAt = R.position();
    bool Template = Key == "tokenizer.chat_template" ||
                    Key.starts_with("tokenizer.chat_template.");
    bool Arch = Key == "general.architecture";
    if ((Template || Arch) && Type != 8)
      throw Error("invalid GGUF template or architecture type");
    auto [Value, H] = R.readMetadataValue(Type, Template || Arch);
    if (Key == "general.alignment") {
      if (Type != 4 || !(A = natural(Value)) || A % 8)
        throw Error("invalid GGUF alignment");
    }
    if (Arch) {
      auto S = Value.get<std::string>();
      validName(S);
      P.Architectures.insert(S);
    }
    if (Template)
      inspectTemplate(
          P, Name, Key, Value.get_ref<const std::string &>(), ValueAt + 8,
          Key == "tokenizer.chat_template"
              ? "default"
              : Key.substr(
                    std::string_view("tokenizer.chat_template.").size()));
    if (Key.starts_with("split."))
      Split[Key] = Value;
    if (Key.starts_with("tokenizer.") || Key.starts_with("adapter.") || Arch ||
        Key == "general.file_type" || Key == "general.quantization_version") {
      auto Fingerprint = digest(std::to_string(Type) + "/" + H);
      auto [It, New] = P.Fingerprints.emplace("gguf/" + Key, Fingerprint);
      if (!New && It->second != Fingerprint)
        P.gap(Name, "conflicting GGUF metadata across shards", {{"key", Key}});
    }
    if (P.MetadataCount++ >= P.B.Opts.Offset &&
        P.Metadata.size() < P.B.Opts.Limit) {
      Json Entry = {{"artifact", Name},    {"key", Key},
                    {"type", Types[Type]}, {"offset", At},
                    {"end", R.position()}, {"encoded_value_sha256", H}};
      if (Type != 8 || Arch)
        Entry["value"] = Value;
      P.Metadata.push_back(std::move(Entry));
    }
  }
  std::vector<std::pair<std::string, Tensor>> List;
  for (uint64_t I = 0; I < NT; ++I) {
    P.B.tick();
    R.expectBytes(product(NT - I, 24));
    std::string Id(R.readString(64));
    validName(Id);
    unsigned Rank = R.readUInt(4);
    if (!Rank || Rank > 4)
      throw Error("invalid GGUF tensor rank");
    Tensor T;
    T.FileId = Name;
    uint64_t Elements = 1;
    for (unsigned J = 0; J < Rank; ++J) {
      auto D = R.readUInt(8);
      if (!D)
        throw Error("zero GGUF dimension");
      T.Shape.push_back(D);
      Elements = product(Elements, D);
    }
    T.Elements = Elements;
    uint64_t Type = R.readUInt(4);
    T.Offset = R.readUInt(8);
    if (T.Offset % A)
      throw LayoutError(
          "unaligned GGUF tensor offset",
          {{"tensor", Id}, {"relative_offset", T.Offset}, {"alignment", A}});
    auto L = std::find_if(std::begin(Storage), std::end(Storage),
                          [&](auto &E) { return E.Id == Type; });
    if (L == std::end(Storage)) {
      T.Type = "ggml:" + std::to_string(Type);
      P.gap(Name, "unknown tensor storage layout",
            {{"tensor", Id}, {"type", Type}, {"relative_offset", T.Offset}});
    } else {
      if (T.Shape.front() % L->Block)
        throw LayoutError("quantized row is not block aligned",
                          {{"tensor", Id},
                           {"row_elements", T.Shape.front()},
                           {"block_elements", L->Block}});
      T.Type = L->Name;
      if (L->Block > 1)
        T.Packing = BlockLayout{L->Block, L->Bytes};
      T.Bytes = product(Elements / L->Block, L->Bytes);
    }
    List.emplace_back(std::move(Id), std::move(T));
  }
  auto Start = align(R.position(), A);
  if (!NT && R.position() == F.size())
    Start = R.position();
  if (Start > F.size())
    throw Error("truncated GGUF directory padding");
  inspectRange(P, F, Name, R.position(), Start - R.position(),
               Region::Alignment);
  std::sort(List.begin(), List.end(), [](const auto &L, const auto &R) {
    return std::tie(L.second.Offset, L.first) <
           std::tie(R.second.Offset, R.first);
  });
  std::optional<uint64_t> End = Start, Previous;
  // An opaque tensor can extend past subsequent declared tensors. Once seen,
  // their end offsets cannot prove that later gaps or trailers are unowned.
  bool OpaquePredecessor = false;
  Json Scale = {{"enabled", bool(P.B.Opts.ScaleBytes)},
                {"eligible_blocks", 0},
                {"blocks_checked", 0},
                {"range_count", 0},
                {"ranges", Json::array()},
                {"negative_zero_scales", 0},
                {"unsupported_tensors", 0}};
  for (auto &[Id, T] : List) {
    T.Offset = sum(Start, T.Offset);
    if (T.Offset > F.size() || (T.Bytes && *T.Bytes > F.size() - T.Offset))
      throw LayoutError(
          "truncated GGUF tensor payload",
          {{"tensor", Id}, {"offset", T.Offset}, {"file_size", F.size()}});
    if ((Previous && *Previous == T.Offset) || (End && T.Offset < *End))
      throw LayoutError("overlapping GGUF tensor offsets",
                        {{"tensor", Id}, {"offset", T.Offset}});
    if (End && !OpaquePredecessor) {
      auto Min = std::min(align(*End, A), T.Offset);
      inspectRange(P, F, Name, *End, Min - *End, Region::Alignment);
      inspectRange(P, F, Name, Min, T.Offset - Min, Region::Gap);
    }
    OpaquePredecessor |= !T.Bytes.has_value();
    Previous = T.Offset;
    End = T.Bytes ? std::optional<uint64_t>(T.Offset + *T.Bytes) : std::nullopt;
    scales(P, F, Name, Id, T, R.isBigEndian(), Scale);
    P.addTensor(std::move(Id), std::move(T));
  }
  if (End && !OpaquePredecessor) {
    auto Min = std::min(align(*End, A), F.size());
    inspectRange(P, F, Name, *End, Min - *End, Region::FinalAlignment);
    inspectRange(P, F, Name, Min, F.size() - Min, Region::Trailer);
  }
  Scale["coverage"] = !P.B.Opts.ScaleBytes            ? "not-requested"
                      : Scale["eligible_blocks"] == 0 ? "no-supported-blocks"
                      : Scale["blocks_checked"] == Scale["eligible_blocks"]
                          ? "all-eligible-blocks"
                          : "prefix-sample";
  P.Artifacts.push_back(
      {{"id", Name},
       {"format", "gguf"},
       {"directory_end", R.position()},
       {"directory_sha256", R.finishDirectoryHash()},
       {"version", Version},
       {"byte_order", R.isBigEndian() ? "big" : "little"},
       {"size", F.size()},
       {"metadata_count", NM},
       {"tensor_count", NT},
       {"alignment", A},
       {"data_offset", Start},
       {"verified_tensor_end",
        NT && End && !OpaquePredecessor ? Json(*End) : Json(nullptr)},
       {"split", Split},
       {"zero_scale_scan", Scale}});
}
} // namespace neverd::model
