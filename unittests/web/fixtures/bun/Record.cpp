//===- Record.cpp - Pinned Bun fixture recording -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Pinned Bun fixture recording.
///
//===----------------------------------------------------------------------===//

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
#include <vector>

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

// Independent golden recorder: field reads are deliberately separate from
// NeverD's admission logic. Inputs are pinned development compiler outputs.
std::vector<uint64_t> sections(std::string_view B) {
  std::vector<uint64_t> Result;
  if (part(B, 0, 4) == "\177ELF") {
    const auto SH = read(B, 40, 8), Count = read(B, 60, 2);
    const auto NamesAt = SH + read(B, 62, 2) * 64;
    const auto Names =
        part(B, read(B, NamesAt + 24, 8), read(B, NamesAt + 32, 8));
    for (uint64_t I = 0; I < Count; ++I) {
      const auto S = SH + I * 64, N = read(B, S, 4);
      if (N + 5 <= Names.size() &&
          Names.substr(N, 5) == std::string_view(".bun\0", 5))
        Result.push_back(read(B, S + 24, 8));
    }
  } else if (read(B, 0, 4) == 0xfeedfacf) {
    uint64_t At = 32;
    const auto Count = read(B, 16, 4);
    for (uint64_t I = 0; I < Count; ++I) {
      const auto Length = read(B, At + 4, 4);
      part(B, At, Length);
      if (read(B, At, 4) == 0x19 &&
          part(B, At + 8, 6) == std::string_view("__BUN\0", 6)) {
        const auto N = read(B, At + 64, 4);
        for (uint64_t J = 0; J < N; ++J) {
          const auto S = At + 72 + J * 80;
          if (part(B, S, 6) == std::string_view("__bun\0", 6))
            Result.push_back(read(B, S + 48, 4));
        }
      }
      if (Length < 8)
        throw std::runtime_error("command length");
      At += Length;
    }
  } else if (part(B, 0, 2) == "MZ") {
    const auto PE = read(B, 60, 4), Count = read(B, PE + 6, 2);
    const auto SH = PE + 24 + read(B, PE + 20, 2);
    for (uint64_t I = 0; I < Count; ++I) {
      const auto S = SH + I * 40;
      if (part(B, S, 8) == std::string_view(".bun\0\0\0\0", 8))
        Result.push_back(read(B, S + 20, 4));
    }
  }
  return Result;
}
} // namespace

int main(int Argc, char **Argv) {
  try {
    if (Argc != 3 && Argc != 4)
      throw std::runtime_error(
          "usage: record container output-prefix|--probe [target]");
    std::ifstream F(Argv[1], std::ios::binary | std::ios::ate);
    if (!F || F.tellg() < 64 || F.tellg() > 256 * 1024 * 1024)
      throw std::runtime_error("fixture input");
    std::string B(size_t(F.tellg()), '\0');
    F.seekg(0);
    F.read(B.data(), B.size());
    if (!F.good())
      throw std::runtime_error("read");
    const auto Headers = sections(B);
    if (Headers.size() != 1)
      throw std::runtime_error("missing or duplicate graph section");
    unsigned Matches = 0;
    for (const auto At : Headers) {
      if (++Matches != 1)
        throw std::runtime_error("duplicate");
      const auto Graph = part(B, At + 8, read(B, At, 8));
      const auto Footer = Graph.size() - 48;
      const auto Table = read(Graph, Footer + 8, 4);
      if (std::string_view(Argv[2]) == "--probe") {
        llvm::outs() << llvm::json::Value(llvm::json::Object{
                            {"full_elf_sha256", hash(B)},
                            {"graph_offset", At + 8},
                            {"graph_size", Graph.size()},
                            {"byte_count", read(Graph, Footer, 8)},
                            {"module_table_offset", Table},
                            {"module_table_bytes", read(Graph, Footer + 12, 4)},
                            {"entry_point", read(Graph, Footer + 16, 4)},
                            {"args_offset", read(Graph, Footer + 20, 4)},
                            {"args_bytes", read(Graph, Footer + 24, 4)},
                            {"flags", read(Graph, Footer + 28, 4)}})
                     << '\n';
        continue;
      }
      write(std::string(Argv[2]) + ".graph.bin", Graph);
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
          {"target", Argc == 4 ? Argv[3] : "bun-linux-x64-baseline"},
          {"full_container_sha256", hash(B)},
          {"full_container_size", std::to_string(B.size())},
          {"graph_offset", std::to_string(At + 8)},
          {"graph_size", std::to_string(Graph.size())},
          {"graph_sha256", hash(Graph)},
          {"executes_fixture", false},
          {"preserved_fixture",
           "exact_graph_only_native_runtime_not_redistributed"},
          {"modules", std::move(Records)}};
      if (Argc == 3) {
        Manifest["full_elf_sha256"] = hash(B);
        Manifest["full_elf_size"] = std::to_string(B.size());
      }
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
