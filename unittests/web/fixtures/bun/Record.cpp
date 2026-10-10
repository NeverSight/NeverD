// Development-only corpus recorder. Never linked into NeverD or called by
// tests. It reads fixed compiler outputs; it never invokes a compiler or
// executes input. The independent field reads preserve golden input bytes.
#include "llvm/Support/JSON.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/raw_ostream.h"

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
std::string_view part(std::string_view B, uint64_t O, uint64_t N) {
  if (O > B.size() || N > B.size() - O)
    throw std::runtime_error("range");
  return B.substr(O, N);
}
uint64_t read(std::string_view B, uint64_t O, unsigned N) {
  const auto S = part(B, O, N);
  uint64_t V = 0;
  for (unsigned I = 0; I < N; ++I)
    V |= uint64_t(uint8_t(S[I])) << (8 * I);
  return V;
}
std::string hash(std::string_view B) {
  llvm::SHA256 H;
  H.update(llvm::StringRef(B));
  const auto Digest = H.final();
  constexpr char Hex[] = "0123456789abcdef";
  std::string S;
  for (const auto Byte : Digest) {
    S.push_back(Hex[Byte >> 4]);
    S.push_back(Hex[Byte & 15]);
  }
  return S;
}
void write(const std::string &Path, std::string_view Bytes) {
  std::ofstream F(Path, std::ios::binary);
  F.write(Bytes.data(), Bytes.size());
  if (!F.good())
    throw std::runtime_error("write");
}
} // namespace

int main(int Argc, char **Argv) {
  try {
    if (Argc != 3)
      throw std::runtime_error("usage: record full.elf output-prefix");
    std::ifstream F(Argv[1], std::ios::binary | std::ios::ate);
    if (!F || F.tellg() < 64 || F.tellg() > 256 * 1024 * 1024)
      throw std::runtime_error("fixture input");
    std::string B(size_t(F.tellg()), '\0');
    F.seekg(0);
    F.read(B.data(), B.size());
    if (!F.good())
      throw std::runtime_error("read");
    const auto SH = read(B, 40, 8), Count = read(B, 60, 2);
    const auto NamesAt = SH + read(B, 62, 2) * 64;
    const auto Names =
        part(B, read(B, NamesAt + 24, 8), read(B, NamesAt + 32, 8));
    unsigned Matches = 0;
    for (uint64_t I = 0; I < Count; ++I) {
      const auto S = SH + I * 64, N = read(B, S, 4);
      if (N + 5 > Names.size() ||
          Names.substr(N, 5) != std::string_view(".bun\0", 5))
        continue;
      if (++Matches != 1)
        throw std::runtime_error("duplicate");
      const auto At = read(B, S + 24, 8);
      const auto Graph = part(B, At + 8, read(B, At, 8));
      write(std::string(Argv[2]) + ".graph.bin", Graph);
      const auto Footer = Graph.size() - 48;
      const auto Table = read(Graph, Footer + 8, 4);
      const auto Modules = read(Graph, Footer + 12, 4) / 52;
      llvm::json::Array Records;
      for (uint64_t M = 0; M < Modules; ++M) {
        llvm::json::Array Ranges;
        for (unsigned P = 0; P < 6; ++P) {
          const auto O = read(Graph, Table + M * 52 + P * 8, 4);
          const auto N = read(Graph, Table + M * 52 + P * 8 + 4, 4);
          Ranges.emplace_back(
              llvm::json::Object{{"offset", std::to_string(O)},
                                 {"size", std::to_string(N)},
                                 {"sha256", hash(part(Graph, O, N))}});
        }
        Records.emplace_back(llvm::json::Object{
            {"ranges", std::move(Ranges)},
            {"encoding", read(Graph, Table + M * 52 + 48, 1)},
            {"loader", read(Graph, Table + M * 52 + 49, 1)},
            {"format", read(Graph, Table + M * 52 + 50, 1)},
            {"side", read(Graph, Table + M * 52 + 51, 1)}});
      }
      llvm::json::Object Manifest{
          {"schema_version", 1},
          {"producer", "Bun"},
          {"producer_version", "1.4.2"},
          {"producer_commit", "744846f844374847c902b5e7fd59b4342a51ef99"},
          {"target", "bun-linux-x64-baseline"},
          {"full_elf_sha256", hash(B)},
          {"full_elf_size", std::to_string(B.size())},
          {"graph_offset", std::to_string(At + 8)},
          {"graph_size", std::to_string(Graph.size())},
          {"graph_sha256", hash(Graph)},
          {"executes_fixture", false},
          {"preserved_fixture",
           "exact_graph_only_native_runtime_not_redistributed"},
          {"modules", std::move(Records)}};
      std::string JSON;
      llvm::raw_string_ostream OS(JSON);
      OS << llvm::json::Value(std::move(Manifest)) << '\n';
      write(std::string(Argv[2]) + ".manifest.json", JSON);
      std::cout << "recorded " << Modules << " modules, " << Graph.size()
                << " graph bytes\n";
    }
    if (!Matches)
      throw std::runtime_error("missing .bun");
    return 0;
  } catch (const std::exception &E) {
    std::cerr << E.what() << '\n';
    return 1;
  }
}
