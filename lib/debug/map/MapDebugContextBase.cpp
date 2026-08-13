//===- MapDebugContextBase.cpp - Shared COFF MAP parser -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared COFF /MAP parser used by both MSVCMapLoader and LLDMapLoader.
///
//===----------------------------------------------------------------------===//

#include "neverd/debug/MapDebugContextBase.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <algorithm>

namespace neverd {

//===----------------------------------------------------------------------===//
// MapDebugContextBase virtual method implementations
//===----------------------------------------------------------------------===//

std::optional<FunctionSym>
MapDebugContextBase::resolveFunction(va_t Addr) const {
  auto It = Functions.find(Addr);
  if (It != Functions.end())
    return It->second;
  auto LB = Functions.lower_bound(Addr);
  if (LB != Functions.begin()) {
    --LB;
    if (LB->second.contains(Addr))
      return LB->second;
  }
  return std::nullopt;
}

std::optional<VariableSym> MapDebugContextBase::resolveVariable(va_t,
                                                                int64_t) const {
  return std::nullopt;
}

std::optional<TypeSym> MapDebugContextBase::resolveType(uint64_t) const {
  return std::nullopt;
}

std::optional<SourceLoc> MapDebugContextBase::sourceLocation(va_t Addr) const {
  if (SourceLocations.empty())
    return std::nullopt;
  auto It = SourceLocations.upper_bound(Addr);
  if (It == SourceLocations.begin())
    return std::nullopt;
  --It;
  if (It->first == Addr || (Addr - It->first) < 256)
    return It->second;
  return std::nullopt;
}

std::vector<FunctionSym> MapDebugContextBase::allFunctions() const {
  std::vector<FunctionSym> Result;
  if (!Loaded)
    return Result;
  Result.reserve(Functions.size());
  for (const auto &[_, FS] : Functions)
    Result.push_back(FS);
  std::sort(Result.begin(), Result.end(),
            [](const FunctionSym &A, const FunctionSym &B) {
              return A.Addr < B.Addr;
            });
  return Result;
}

bool MapDebugContextBase::hasInfo() const { return Loaded; }

bool MapDebugContextBase::isCOFFMapHeader(llvm::StringRef Line) {
  llvm::StringRef Trimmed = Line.trim();
  return Trimmed.starts_with("Address") && Trimmed.contains("Publics by Value");
}

bool MapDebugContextBase::parseSegOffset(llvm::StringRef Token, uint16_t &Seg,
                                         uint32_t &Offset) {
  auto ColonPos = Token.find(':');
  if (ColonPos == llvm::StringRef::npos)
    return false;
  unsigned SegVal;
  if (Token.substr(0, ColonPos).getAsInteger(16, SegVal) ||
      SegVal > 0xFFFFu)
    return false;
  unsigned OffVal;
  if (Token.substr(ColonPos + 1).getAsInteger(16, OffVal))
    return false;
  Seg = static_cast<uint16_t>(SegVal);
  Offset = OffVal;
  return true;
}

void MapDebugContextBase::inferFunctionSizes() {
  if (Functions.size() < 2)
    return;
  for (auto It = Functions.begin(); It != Functions.end(); ++It) {
    if (It->second.Size != 0)
      continue;
    auto Next = std::next(It);
    if (Next == Functions.end())
      break;
    uint64_t Gap = Next->first - It->first;
    if (Gap > 0 && Gap <= 16 * 1024 * 1024)
      It->second.Size = Gap;
  }
}

namespace {

struct SectionEntry {
  uint16_t Seg;
  uint32_t Offset;
  uint32_t Length;
  std::string Name;
  std::string Class;
};

bool isCodeSegment(const std::vector<SectionEntry> &Sections, uint16_t Seg) {
  for (auto &S : Sections) {
    if (S.Seg == Seg)
      return S.Class == "CODE";
  }
  return Seg == 1;
}

va_t resolveAddress(const std::vector<SectionEntry> &Sections, uint16_t Seg,
                    uint32_t Offset, uint64_t ImageBase) {
  if (Seg == 0)
    return 0;
  for (auto &S : Sections) {
    if (S.Seg == Seg)
      return ImageBase + S.Offset + Offset;
  }
  return ImageBase + Offset;
}

} // anonymous namespace

void MapDebugContextBase::parseCOFFMapContent(
    llvm::StringRef Content, std::map<va_t, FunctionSym> &Functions,
    uint64_t ImageBase) {

  enum class ParseState { Header, Sections, Symbols, Done };
  ParseState State = ParseState::Header;
  uint64_t PreferredBase = ImageBase;
  std::vector<SectionEntry> Sections;

  llvm::StringRef Remaining = Content;
  while (!Remaining.empty()) {
    auto [Line, Rest] = Remaining.split('\n');
    Remaining = Rest;

    llvm::StringRef Trimmed = Line.trim();
    if (Trimmed.empty())
      continue;

    if (Trimmed.starts_with("Preferred load address is")) {
      llvm::StringRef AddrStr = Trimmed.substr(25).trim();
      unsigned long long Val;
      if (!AddrStr.getAsInteger(16, Val))
        PreferredBase = Val;
      continue;
    }

    if (Trimmed.starts_with("Start         Length") ||
        Trimmed.starts_with("Start ")) {
      State = ParseState::Sections;
      continue;
    }

    if (MapDebugContextBase::isCOFFMapHeader(Trimmed)) {
      State = ParseState::Symbols;
      continue;
    }

    if (Trimmed.starts_with("Static symbols")) {
      State = ParseState::Symbols;
      continue;
    }

    if (Trimmed.starts_with("entry point at")) {
      State = ParseState::Done;
      continue;
    }

    if (State == ParseState::Sections) {
      llvm::SmallVector<llvm::StringRef, 8> Tokens;
      Trimmed.split(Tokens, ' ', -1, false);
      if (Tokens.size() >= 3) {
        uint16_t Seg;
        uint32_t Offset;
        if (parseSegOffset(Tokens[0], Seg, Offset)) {
          SectionEntry Entry;
          Entry.Seg = Seg;
          Entry.Offset = Offset;
          llvm::StringRef LenStr = Tokens[1];
          if (LenStr.ends_with("H") || LenStr.ends_with("h"))
            LenStr = LenStr.drop_back();
          unsigned Len;
          Entry.Length = !LenStr.getAsInteger(16, Len) ? Len : 0;
          if (Tokens.size() > 2)
            Entry.Name = Tokens[2].str();
          if (Tokens.size() > 3)
            Entry.Class = Tokens[3].str();
          Sections.push_back(std::move(Entry));
        }
      }
    } else if (State == ParseState::Symbols) {
      llvm::SmallVector<llvm::StringRef, 8> Tokens;
      Trimmed.split(Tokens, ' ', -1, false);
      if (Tokens.size() < 2)
        continue;

      uint16_t Seg;
      uint32_t Offset;
      if (!parseSegOffset(Tokens[0], Seg, Offset))
        continue;

      bool HasFuncFlag = std::find(Tokens.begin() + 2, Tokens.end(),
                                   llvm::StringRef("f")) != Tokens.end();
      if (!HasFuncFlag && !isCodeSegment(Sections, Seg))
        continue;
      if (!HasFuncFlag && Tokens.size() >= 3 && Tokens.back() == "data")
        continue;

      std::string Name = Tokens[1].str();

      va_t VA = 0;
      if (Tokens.size() >= 3) {
        llvm::StringRef RvaStr = Tokens[2];
        if (RvaStr.starts_with("0x") || RvaStr.starts_with("0X"))
          RvaStr = RvaStr.drop_front(2);
        unsigned long long RvaBase;
        if (!RvaStr.getAsInteger(16, RvaBase))
          VA = static_cast<va_t>(RvaBase);
      }
      if (VA == 0)
        VA = resolveAddress(Sections, Seg, Offset, PreferredBase);

      if (VA != 0 && Functions.find(VA) == Functions.end()) {
        FunctionSym FS;
        FS.Name = std::move(Name);
        FS.Addr = VA;
        Functions[VA] = std::move(FS);
      }
    }
  }
}

void MapDebugContextBase::parseCOFFMapLineNumbers(
    llvm::StringRef Content, std::map<va_t, SourceLoc> &Locations,
    uint64_t ImageBase) {

  std::string CurrentFile;
  bool InLineNumbers = false;

  std::vector<SectionEntry> Sections;
  llvm::StringRef Remaining = Content;
  while (!Remaining.empty()) {
    auto [Line, Rest] = Remaining.split('\n');
    Remaining = Rest;

    llvm::StringRef Trimmed = Line.trim();
    if (Trimmed.empty()) {
      if (InLineNumbers)
        InLineNumbers = false;
      continue;
    }

    if (Trimmed.starts_with("Start         Length") ||
        Trimmed.starts_with("Start ")) {
      llvm::StringRef SRemaining = Rest;
      while (!SRemaining.empty()) {
        auto [SLine, SRest] = SRemaining.split('\n');
        SRemaining = SRest;
        llvm::StringRef STrimmed = SLine.trim();
        if (STrimmed.empty())
          break;
        llvm::SmallVector<llvm::StringRef, 8> Tokens;
        STrimmed.split(Tokens, ' ', -1, false);
        if (Tokens.size() >= 3) {
          uint16_t Seg;
          uint32_t Offset;
          if (parseSegOffset(Tokens[0], Seg, Offset)) {
            SectionEntry Entry;
            Entry.Seg = Seg;
            Entry.Offset = Offset;
            Sections.push_back(std::move(Entry));
          }
        }
      }
      continue;
    }

    if (Trimmed.starts_with("Line numbers for ")) {
      llvm::StringRef FileInfo = Trimmed.substr(17);
      auto ParenPos = FileInfo.find('(');
      if (ParenPos != llvm::StringRef::npos)
        CurrentFile = FileInfo.substr(0, ParenPos).trim().str();
      else
        CurrentFile = FileInfo.str();
      InLineNumbers = true;
      continue;
    }

    if (!InLineNumbers || CurrentFile.empty())
      continue;

    llvm::SmallVector<llvm::StringRef, 16> Tokens;
    Trimmed.split(Tokens, ' ', -1, false);

    for (size_t I = 0; I + 1 < Tokens.size(); I += 2) {
      unsigned LineNum;
      if (Tokens[I].getAsInteger(10, LineNum))
        continue;

      uint16_t Seg;
      uint32_t Offset;
      if (!parseSegOffset(Tokens[I + 1], Seg, Offset))
        continue;

      va_t VA = resolveAddress(Sections, Seg, Offset, ImageBase);
      if (VA == 0)
        continue;

      if (Locations.find(VA) == Locations.end()) {
        SourceLoc Loc;
        Loc.File = CurrentFile;
        Loc.Line = LineNum;
        Locations[VA] = std::move(Loc);
      }
    }
  }
}

} // namespace neverd
