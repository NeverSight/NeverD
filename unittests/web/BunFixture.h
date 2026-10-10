#pragma once

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace neverd::web::test {
// Synthetic, inert C++ fixtures exercise malformed layouts. They are not
// evidence that a real Bun compiler emitted a given native image.
inline void put(std::string &B, uint64_t At, uint64_t V, unsigned Width) {
  if (At > B.size() || Width > B.size() - At)
    throw std::out_of_range("fixture range");
  for (unsigned I = 0; I < Width; ++I)
    B[At + I] = char(V >> (I * 8));
}
inline uint64_t get(std::string_view B, uint64_t At, unsigned Width) {
  if (At > B.size() || Width > B.size() - At)
    throw std::out_of_range("fixture range");
  uint64_t V = 0;
  for (unsigned I = 0; I < Width; ++I)
    V |= uint64_t(uint8_t(B[At + I])) << (I * 8);
  return V;
}
inline std::string bunELF(std::string Graph) {
  const uint64_t End = (4096 + 8 + Graph.size() + 4095) & ~uint64_t(4095);
  std::string B(End + 192, '\0');
  B.replace(0, 7, "\177ELF\2\1\1", 7);
  put(B, 16, 2, 2);
  put(B, 18, 62, 2);
  put(B, 20, 1, 4);
  put(B, 32, 64, 8);
  put(B, 40, End, 8);
  put(B, 52, 64, 2);
  put(B, 54, 56, 2);
  put(B, 56, 1, 2);
  put(B, 58, 64, 2);
  put(B, 60, 3, 2);
  put(B, 62, 1, 2);
  put(B, 64, 1, 4);
  put(B, 68, 6, 4);
  put(B, 80, 0x400000, 8);
  put(B, 88, 0x400000, 8);
  put(B, 96, End, 8);
  put(B, 104, End, 8);
  put(B, 112, 4096, 8);
  const std::string Names("\0.shstrtab\0.bun\0", 16);
  B.replace(128, Names.size(), Names);
  put(B, End + 64, 1, 4);
  put(B, End + 68, 3, 4);
  put(B, End + 88, 128, 8);
  put(B, End + 96, Names.size(), 8);
  put(B, End + 112, 1, 8);
  put(B, End + 128, 11, 4);
  put(B, End + 132, 1, 4);
  put(B, End + 136, 3, 8);
  put(B, End + 144, 0x401000, 8);
  put(B, End + 152, 4096, 8);
  put(B, End + 160, Graph.size() + 8, 8);
  put(B, End + 176, 8, 8);
  put(B, 4096, Graph.size(), 8);
  B.replace(4104, Graph.size(), Graph);
  return B;
}

struct BunFixture {
  std::string Graph;
  uint64_t Table = 0, Footer = 0;
  std::string Bytes;
  explicit BunFixture(bool Rich = true,
                      std::string_view MapBytes = "OPAQUE_MAP_CANARY",
                      std::string_view AssetBytes =
                          std::string_view("\0\xff\r\nASSET_CANARY", 16)) {
    using Pointer = std::pair<uint32_t, uint32_t>;
    auto Add = [&](std::string_view Text, unsigned Nuls = 0) -> Pointer {
      Pointer P{uint32_t(Graph.size()), uint32_t(Text.size())};
      Graph += Text;
      Graph.append(Nuls, '\0');
      return P;
    };
    auto Aligned = [&](std::string_view Text) {
      while (Graph.size() % 128 != 120)
        Graph.push_back('\0');
      return Add(Text);
    };
    const auto Bytecode = Rich ? Aligned("CACHE_CANARY") : Pointer{};
    const auto Info = Rich ? Add("INFO_CANARY") : Pointer{};
    const auto Builtin = Rich ? Aligned("BUILTIN_CANARY") : Pointer{};
    const auto Strings = Rich ? Aligned("SHARED_CACHE_CANARY") : Pointer{};
    const auto InfoStrings = Rich ? Add("INFO_STRINGS_CANARY") : Pointer{};
    const auto Map = Rich ? Add(MapBytes) : Pointer{};
    const auto Code = Add("export const secret = 'BUN_SOURCE_CANARY';", 1);
    while (Graph.size() & 1)
      Graph.push_back('\0');
    std::string UTF16;
    for (char16_t C : std::u16string_view(u"export const x = '中文🌱';")) {
      UTF16.push_back(char(C));
      UTF16.push_back(char(C >> 8));
    }
    const auto Unicode = Add(UTF16, 2);
    const auto Asset = Add(AssetBytes, 1);
    const auto Name0 = Add("/$bunfs/root/SECRET.js", 1);
    const auto Origin = Rich ? Add("/$bunfs/root/ORIGIN.js", 1) : Pointer{};
    const auto Name1 = Add("/$bunfs/root/UNICODE.js", 1);
    const auto Name2 = Add("/$bunfs/root/../opaque.asset", 1);
    Table = Graph.size();
    auto Record = [&](Pointer Name, Pointer Content, Pointer SM, Pointer BC,
                      Pointer MI, Pointer Origin, uint8_t Encoding,
                      uint8_t Loader, uint8_t Format) {
      for (auto P : {Name, Content, SM, BC, MI, Origin}) {
        const auto At = Graph.size();
        Graph.resize(At + 8);
        put(Graph, At, P.first, 4);
        put(Graph, At + 4, P.second, 4);
      }
      Graph.push_back(char(Encoding));
      Graph.push_back(char(Loader));
      Graph.push_back(char(Format));
      Graph.push_back(0);
    };
    Record(Name0, Code, Map, Bytecode, Info, Origin, 1, 1, 1);
    Record(Name1, Unicode, {}, {}, {}, {}, 2, 1, 1);
    Record(Name2, Asset, {}, {}, {}, {}, 0, 5, 0);
    Graph.append(12, '\0');
    auto U32 = [&](uint32_t V) {
      const auto At = Graph.size();
      Graph.resize(At + 4);
      put(Graph, At, V, 4);
    };
    U32(Rich ? 1 : 0);
    if (Rich) {
      U32(17);
      U32(Builtin.first);
      U32(Builtin.second);
    }
    if (Rich) {
      U32(Strings.first);
      U32(Strings.second);
    }
    U32(2);
    if (Rich) {
      U32(InfoStrings.first);
      U32(InfoStrings.second);
    }
    const auto Args = Add("--fixture-canary", 1);
    Footer = Graph.size();
    Graph.resize(Footer + 32);
    put(Graph, Footer, Footer, 8);
    put(Graph, Footer + 8, Table, 4);
    put(Graph, Footer + 12, 156, 4);
    put(Graph, Footer + 16, 0, 4);
    put(Graph, Footer + 20, Args.first, 4);
    put(Graph, Footer + 24, Args.second, 4);
    put(Graph, Footer + 28, Rich ? 0x3f0 : 0x170, 4);
    Graph += "\n---- Bun! ----\n";
    Bytes = bunELF(Graph);
  }
};
} // namespace neverd::web::test
