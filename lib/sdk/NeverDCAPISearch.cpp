//===- NeverDCAPISearch.cpp - C API: search and binary diff ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Byte and string search plus function and decompilation diff queries.
///
//===----------------------------------------------------------------------===//

#include "SessionImpl.h"

#include "llvm/Support/JSON.h"

#include <algorithm>
#include <cctype>

using namespace neverd;
using namespace neverd::sdk;

// ===--------------------------------------------------------------------===//
// Search
// ===--------------------------------------------------------------------===//

const char *neverd_search_bytes(neverd_session_t Sess, const unsigned char *Pat,
                                int PatLen, int MaxResults) {
  auto *S = toSession(Sess);
  if (!S->Loaded || !Pat || PatLen <= 0)
    return dupStr("[]");

  int Limit = MaxResults > 0 ? MaxResults : 256;
  llvm::json::Array Arr;

  for (const auto &Seg : S->Img.Segments) {
    if (Seg.Data.size() < static_cast<size_t>(PatLen))
      continue;
    size_t End = Seg.Data.size() - PatLen;
    for (size_t I = 0; I <= End; ++I) {
      if (std::memcmp(Seg.Data.data() + I, Pat, PatLen) == 0) {
        llvm::json::Object Hit;
        Hit["addr"] = vaHex(Seg.VA + I);
        Hit["segment"] = Seg.Name;
        Hit["offset"] = static_cast<int64_t>(I);
        Arr.push_back(std::move(Hit));
        if (static_cast<int>(Arr.size()) >= Limit)
          break;
      }
    }
    if (static_cast<int>(Arr.size()) >= Limit)
      break;
  }
  return dupStr(jsonToString(llvm::json::Value(std::move(Arr))));
}

const char *neverd_search_string(neverd_session_t Sess, const char *Pattern,
                                 int CaseSensitive, int MaxResults) {
  auto *S = toSession(Sess);
  if (!S->Loaded || !Pattern || Pattern[0] == '\0')
    return dupStr("[]");

  int Limit = MaxResults > 0 ? MaxResults : 256;
  std::string Pat(Pattern);
  std::string PatLower;
  if (!CaseSensitive) {
    PatLower.resize(Pat.size());
    std::transform(Pat.begin(), Pat.end(), PatLower.begin(), [](char Ch) {
      return static_cast<char>(std::tolower(static_cast<unsigned char>(Ch)));
    });
  }

  llvm::json::Array Arr;

  for (const auto &Seg : S->Img.Segments) {
    if (Seg.Data.size() < Pat.size())
      continue;
    size_t End = Seg.Data.size() - Pat.size();
    for (size_t I = 0; I <= End; ++I) {
      bool Match;
      if (CaseSensitive) {
        Match = std::memcmp(Seg.Data.data() + I, Pat.data(), Pat.size()) == 0;
      } else {
        Match = true;
        for (size_t J = 0; J < Pat.size(); ++J) {
          unsigned char Hay = static_cast<unsigned char>(
              std::tolower(static_cast<unsigned char>(Seg.Data[I + J])));
          if (Hay != static_cast<unsigned char>(PatLower[J])) {
            Match = false;
            break;
          }
        }
      }
      if (Match) {
        llvm::json::Object Hit;
        Hit["addr"] = vaHex(Seg.VA + I);
        Hit["segment"] = Seg.Name;
        size_t CtxEnd = std::min(I + Pat.size() + 32, Seg.Data.size());
        std::string Ctx(reinterpret_cast<const char *>(Seg.Data.data() + I),
                        std::min(CtxEnd - I, static_cast<size_t>(64)));
        for (auto &C : Ctx)
          if (static_cast<unsigned char>(C) < 0x20 || C == 0x7f)
            C = '.';
        Hit["context"] = Ctx;
        Arr.push_back(std::move(Hit));
        if (static_cast<int>(Arr.size()) >= Limit)
          break;
      }
    }
    if (static_cast<int>(Arr.size()) >= Limit)
      break;
  }
  return dupStr(jsonToString(llvm::json::Value(std::move(Arr))));
}

// ===--------------------------------------------------------------------===//
// Binary diff
// ===--------------------------------------------------------------------===//

const char *neverd_diff_functions(neverd_session_t SessA,
                                  neverd_session_t SessB) {
  auto *A = toSession(SessA);
  auto *B = toSession(SessB);
  if (!A->Loaded || !B->Loaded)
    return dupStr("{}");
  (void)A->synchronizeFunctions();
  (void)B->synchronizeFunctions();

  std::map<std::string, int> MapA, MapB;
  for (int I = 0; I < static_cast<int>(A->Functions.size()); ++I)
    MapA[A->Functions[I].Name] = I;
  for (int I = 0; I < static_cast<int>(B->Functions.size()); ++I)
    MapB[B->Functions[I].Name] = I;

  llvm::json::Array Matched, Added, Removed;

  for (const auto &[Name, IdxA] : MapA) {
    auto It = MapB.find(Name);
    if (It != MapB.end()) {
      int IdxB = It->second;
      llvm::json::Object Obj;
      Obj["name"] = Name;
      Obj["addr_a"] = vaHex(A->Functions[IdxA].Entry);
      Obj["addr_b"] = vaHex(B->Functions[IdxB].Entry);
      Obj["size_a"] = static_cast<int64_t>(A->Functions[IdxA].Size);
      Obj["size_b"] = static_cast<int64_t>(B->Functions[IdxB].Size);
      Obj["same_size"] = (A->Functions[IdxA].Size == B->Functions[IdxB].Size);
      Matched.push_back(std::move(Obj));
    } else {
      llvm::json::Object Obj;
      Obj["name"] = Name;
      Obj["addr"] = vaHex(A->Functions[IdxA].Entry);
      Obj["size"] = static_cast<int64_t>(A->Functions[IdxA].Size);
      Removed.push_back(std::move(Obj));
    }
  }

  for (const auto &[Name, IdxB] : MapB) {
    if (MapA.find(Name) == MapA.end()) {
      llvm::json::Object Obj;
      Obj["name"] = Name;
      Obj["addr"] = vaHex(B->Functions[IdxB].Entry);
      Obj["size"] = static_cast<int64_t>(B->Functions[IdxB].Size);
      Added.push_back(std::move(Obj));
    }
  }

  llvm::json::Object Result;
  Result["matched"] = std::move(Matched);
  Result["added"] = std::move(Added);
  Result["removed"] = std::move(Removed);
  return dupStr(jsonToString(llvm::json::Value(std::move(Result))));
}

const char *neverd_diff_decompile(neverd_session_t SessA, neverd_va_t EntryA,
                                  neverd_session_t SessB, neverd_va_t EntryB) {
  const char *CodeA = neverd_decompile(SessA, EntryA);
  const char *CodeB = neverd_decompile(SessB, EntryB);

  llvm::json::Object Result;
  Result["code_a"] = CodeA ? CodeA : "";
  Result["code_b"] = CodeB ? CodeB : "";
  bool Identical = CodeA && CodeB && std::strcmp(CodeA, CodeB) == 0;
  Result["identical"] = Identical;

  if (CodeA)
    free(const_cast<char *>(CodeA));
  if (CodeB)
    free(const_cast<char *>(CodeB));

  return dupStr(jsonToString(llvm::json::Value(std::move(Result))));
}
