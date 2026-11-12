//===- MachOExportLayout.cpp - Source patch support ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Relocate checked Mach-O export tries.
//===----------------------------------------------------------------------===//

#include "MachOExportLayout.h"

#include "neverd/backend/Limits.h"

#include "llvm/Support/Errc.h"
#include "llvm/Support/LEB128.h"

#include <set>
namespace neverd {
namespace {
using namespace llvm::MachO;
llvm::Error fail() {
  return llvm::createStringError(
      llvm::errc::not_supported,
      "Mach-O export layout: malformed or unmodeled trie");
}
void uleb(std::vector<uint8_t> &B, uint64_t V) {
  uint8_t T[10];
  auto N = llvm::encodeULEB128(V, T);
  B.insert(B.end(), T, T + N);
}
std::vector<uint8_t> terminal(const MachOExportLayout &E) {
  std::vector<uint8_t> B;
  uleb(B, E.Flags);
  if (E.Flags & EXPORT_SYMBOL_FLAGS_REEXPORT) {
    uleb(B, E.Ordinal);
    B.insert(B.end(), E.ImportName.begin(), E.ImportName.end());
    B.push_back(0);
  } else {
    uleb(B, E.Address);
    if (E.Flags & EXPORT_SYMBOL_FLAGS_STUB_AND_RESOLVER)
      uleb(B, E.Resolver);
  }
  return B;
}
} // namespace
llvm::Expected<MachOExportMap>
readMachOExportLayout(llvm::ArrayRef<uint8_t> B) {
  MachOExportMap Result;
  if (B.empty())
    return Result;
  std::vector<std::pair<uint64_t, std::string>> Work{{0, {}}};
  std::set<uint64_t> Seen;
  std::vector<std::pair<uint64_t, uint64_t>> Ranges;
  while (!Work.empty()) {
    auto [Off, Name] = std::move(Work.back());
    Work.pop_back();
    if (Off >= B.size() || !Seen.insert(Off).second ||
        Name.size() > ::neverd::limits::backend::
                          kMachOExportLayoutReadMachOExportLayoutNameSizeLimit)
      return fail();
    uint64_t At = Off;
    bool Bad = false;
    auto Read = [&](uint64_t Limit) {
      if (At >= Limit) {
        Bad = true;
        return uint64_t(0);
      }
      unsigned N = 0;
      const char *Error = nullptr;
      auto V = llvm::decodeULEB128(B.data() + At, &N, B.data() + Limit, &Error);
      At += N;
      Bad |= Error != nullptr || !N;
      return V;
    };
    const auto Size = Read(B.size());
    if (Bad || !rangeInBounds(At, Size, B.size()))
      return fail();
    const auto End = At + Size;
    if (Size) {
      MachOExportLayout E;
      E.Flags = Read(End);
      const auto Kind = E.Flags & EXPORT_SYMBOL_FLAGS_KIND_MASK;
      if (Bad || Name.empty() || E.Flags & ~uint64_t(0x1f) || Kind == 3 ||
          ((E.Flags & EXPORT_SYMBOL_FLAGS_REEXPORT) &&
           (Kind || (E.Flags & EXPORT_SYMBOL_FLAGS_STUB_AND_RESOLVER))))
        return fail();
      if (E.Flags & EXPORT_SYMBOL_FLAGS_REEXPORT) {
        E.Ordinal = Read(End);
        const auto Start = At;
        while (At < End && B[At])
          ++At;
        if (At == End)
          return fail();
        E.ImportName.assign(reinterpret_cast<const char *>(B.data() + Start),
                            At - Start);
        ++At;
      } else {
        E.Address = Read(End);
        if (E.Flags & EXPORT_SYMBOL_FLAGS_STUB_AND_RESOLVER)
          E.Resolver = Read(End);
      }
      if (Bad || At != End || !Result.emplace(Name, E).second)
        return fail();
    }
    if (At >= B.size())
      return fail();
    unsigned Count = B[At++];
    std::set<std::string> Edges;
    for (unsigned I = 0; I < Count; ++I) {
      const auto Start = At;
      while (At < B.size() && B[At])
        ++At;
      if (At == B.size() || At == Start)
        return fail();
      std::string Edge(reinterpret_cast<const char *>(B.data() + Start),
                       At - Start);
      ++At;
      if (!Edges.insert(Edge).second)
        return fail();
      const auto Child = Read(B.size());
      if (Bad)
        return fail();
      Work.emplace_back(Child, Name + Edge);
    }
    Ranges.emplace_back(Off, At);
  }
  std::sort(Ranges.begin(), Ranges.end());
  uint64_t End = 0;
  for (const auto &[Begin, Last] : Ranges) {
    if (Begin < End ||
        llvm::any_of(B.slice(End, Begin - End), [](uint8_t V) { return V; }))
      return fail();
    End = Last;
  }
  if (llvm::any_of(B.drop_front(End), [](uint8_t V) { return V; }))
    return fail();
  return Result;
}
llvm::Expected<std::vector<uint8_t>>
writeMachOExportLayout(const MachOExportMap &Exports) {
  if (Exports.empty())
    return std::vector<uint8_t>{};
  struct Node {
    std::map<uint8_t, size_t> Edges;
    std::vector<uint8_t> Terminal;
    size_t Offset = 0;
  };
  std::vector<Node> Nodes(1);
  for (const auto &[Name, E] : Exports) {
    size_t I = 0;
    for (uint8_t C : Name) {
      if (!C || Nodes.size() >
                    ::neverd::limits::backend::
                        kMachOExportLayoutWriteMachOExportLayoutNodesSizeLimit)
        return fail();
      auto It = Nodes[I].Edges.find(C);
      if (It != Nodes[I].Edges.end())
        I = It->second;
      else {
        auto N = Nodes.size();
        Nodes[I].Edges.emplace(C, N);
        Nodes.emplace_back();
        I = N;
      }
    }
    Nodes[I].Terminal = terminal(E);
  }
  for (unsigned Pass = 0;
       Pass < ::neverd::limits::backend::
                  kMachOExportLayoutWriteMachOExportLayoutPassLimit;
       ++Pass) {
    std::vector<uint8_t> B;
    bool Changed = false;
    for (auto &N : Nodes) {
      Changed |= N.Offset != B.size();
      N.Offset = B.size();
      uleb(B, N.Terminal.size());
      B.insert(B.end(), N.Terminal.begin(), N.Terminal.end());
      if (N.Edges.size() > 255)
        return fail();
      B.push_back(N.Edges.size());
      for (auto [C, I] : N.Edges) {
        B.push_back(C);
        B.push_back(0);
        uleb(B, Nodes[I].Offset);
      }
    }
    if (!Changed) {
      auto Check = readMachOExportLayout(B);
      if (!Check)
        return Check.takeError();
      if (*Check != Exports)
        return fail();
      return B;
    }
  }
  return fail();
}
llvm::Expected<std::vector<uint8_t>>
machOExportIdentities(llvm::ArrayRef<uint8_t> B, uint64_t HeaderVA,
                      uint32_t FileType,
                      std::map<std::string, uint64_t> *WeakDefinitions) {
  auto Exports = readMachOExportLayout(B);
  if (!Exports)
    return Exports.takeError();
  for (auto &[Name, E] : *Exports) {
    if (E.Flags & EXPORT_SYMBOL_FLAGS_REEXPORT)
      continue;
    if ((E.Flags & EXPORT_SYMBOL_FLAGS_KIND_MASK) !=
        EXPORT_SYMBOL_FLAGS_KIND_ABSOLUTE) {
      if (E.Address > UINT64_MAX - HeaderVA ||
          E.Resolver > UINT64_MAX - HeaderVA)
        return fail();
      if (!(isMachOHeaderSymbol(Name, FileType) && E.Address == 0))
        E.Address += HeaderVA;
      if (E.Flags & EXPORT_SYMBOL_FLAGS_STUB_AND_RESOLVER)
        E.Resolver += HeaderVA;
    }
    if (WeakDefinitions && E.Flags == EXPORT_SYMBOL_FLAGS_WEAK_DEFINITION) {
      auto [Entry, Added] = WeakDefinitions->emplace(Name, E.Address);
      if (!Added && Entry->second != E.Address)
        return fail();
    }
  }
  return writeMachOExportLayout(*Exports);
}
} // namespace neverd
