//===- FunctionNames.h - Function names of many addresses ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Batch function naming preserves the origin of the selected image evidence.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_FUNCTIONNAMES_H
#define NEVERD_LOADER_FUNCTIONNAMES_H

#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"

#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace neverd {

struct FunctionNameEvidence {
  std::string Name;
  NameOrigin Origin = NameOrigin::Synthesized;
};

/// Undo only the display numbering recorded for this function. A producer's
/// literal helper_0 is evidence; a suffix added to distinguish two helper
/// symbols in the listing is not. The returned view borrows Img or Name.
inline llvm::StringRef functionNameBeforeNumbering(const BinaryImage &Img,
                                                   va_t Addr,
                                                   llvm::StringRef Name) {
  const auto It = Img.SharedFunctionNames.find(Addr);
  if (It != Img.SharedFunctionNames.end() && It->second.second == Name)
    return It->second.first;
  return Name;
}

/// BinaryImage::getFunctionNameAt() for each of \p Addrs, from one pass over
/// the exports and symbols rather than one per address: the first export
/// there when it has a name, else the symbol of the highest functionNameRank
/// (the first of equal rank), as the function is listed.
inline std::map<va_t, FunctionNameEvidence>
functionNameEvidenceAt(const BinaryImage &Img, const std::set<va_t> &Addrs) {
  if (Addrs.empty())
    return {};
  std::map<va_t, const Export *> FirstExports;
  for (const Export &Exp : Img.Exports)
    if (Addrs.count(Exp.Addr))
      FirstExports.try_emplace(Exp.Addr, &Exp);
  std::map<va_t, const Symbol *> BestSymbols;
  for (const Symbol &Sym : Img.Symbols) {
    if (Sym.Name.empty() || Sym.IsSection || !Addrs.count(Sym.Addr))
      continue;
    auto [It, Inserted] = BestSymbols.try_emplace(Sym.Addr, &Sym);
    if (!Inserted && BinaryImage::functionNameRank(Sym) >
                         BinaryImage::functionNameRank(*It->second))
      It->second = &Sym;
  }
  std::map<va_t, FunctionNameEvidence> Names;
  for (va_t Addr : Addrs) {
    const auto Exp = FirstExports.find(Addr);
    const auto Sym = BestSymbols.find(Addr);
    if (Exp != FirstExports.end() && !Exp->second->Name.empty())
      Names.emplace(
          Addr, FunctionNameEvidence{Exp->second->Name, NameOrigin::Stated});
    else if (Sym != BestSymbols.end())
      Names.emplace(Addr, FunctionNameEvidence{
                              Img.functionListName(Addr, Sym->second->Name),
                              Sym->second->Origin});
    else
      Names.emplace(Addr, FunctionNameEvidence{
                              (kAutoFuncPrefix + llvm::utohexstr(Addr)).str(),
                              NameOrigin::Synthesized});
  }
  return Names;
}

inline std::map<va_t, std::string>
functionNamesAt(const BinaryImage &Img, const std::set<va_t> &Addrs) {
  std::map<va_t, std::string> Names;
  for (auto &[Addr, Evidence] : functionNameEvidenceAt(Img, Addrs))
    Names.emplace(Addr, std::move(Evidence.Name));
  return Names;
}

/// A recovered entry does not itself supply a name. Preserve the image naming
/// producer's origin, including its generated fallback; only an additional
/// name supplied by recovery is analysis evidence. A stated sub_<va> remains
/// stated, regardless of its resemblance to the fallback spelling.
inline std::map<va_t, NameOrigin>
recoveredFunctionNameOrigins(const BinaryImage &Img,
                             const std::map<va_t, llvm::StringRef> &Recovered) {
  std::set<va_t> Addrs;
  for (const auto &[Addr, Name] : Recovered)
    Addrs.insert(Addr);
  std::map<va_t, NameOrigin> Origins;
  for (const auto &[Addr, Evidence] : functionNameEvidenceAt(Img, Addrs)) {
    const auto Name = Recovered.at(Addr);
    Origins.emplace(Addr, Name.empty()            ? NameOrigin::Synthesized
                          : Name == Evidence.Name ? Evidence.Origin
                                                  : NameOrigin::Analysis);
  }
  return Origins;
}

/// BinaryImage::statedFunctionNamesAt() for every address that has any,
/// from one pass over the exports and one over the symbols rather than
/// three for each address: the name the function is shown under, from the
/// first export there or else the symbol of the highest functionNameRank
/// (the first of equal rank) where a producer stated it, then the names of
/// the exports there and of the stated function symbols there, in order.
inline std::unordered_map<va_t, std::vector<std::string>>
statedFunctionNamesByAddress(const BinaryImage &Img) {
  std::unordered_map<va_t, const Export *> FirstExports;
  FirstExports.reserve(Img.Exports.size());
  for (const Export &Exp : Img.Exports)
    FirstExports.try_emplace(Exp.Addr, &Exp);
  std::unordered_map<va_t, const Symbol *> BestSymbols;
  BestSymbols.reserve(Img.Symbols.size());
  for (const Symbol &Sym : Img.Symbols) {
    if (Sym.Name.empty() || Sym.IsSection)
      continue;
    const auto [Best, Added] = BestSymbols.try_emplace(Sym.Addr, &Sym);
    if (!Added && BinaryImage::functionNameRank(Sym) >
                      BinaryImage::functionNameRank(*Best->second))
      Best->second = &Sym;
  }
  const auto Shown = [&](va_t Addr) -> llvm::StringRef {
    if (const auto Exp = FirstExports.find(Addr);
        Exp != FirstExports.end() && !Exp->second->Name.empty())
      return Exp->second->Name;
    if (const auto Best = BestSymbols.find(Addr);
        Best != BestSymbols.end() && Best->second->Origin == NameOrigin::Stated)
      return Best->second->Name;
    return {};
  };
  std::unordered_map<va_t, std::vector<std::string>> Names;
  for (const auto &[Addr, Exp] : FirstExports)
    if (const llvm::StringRef Name = Shown(Addr); !Name.empty())
      Names.try_emplace(Addr, std::vector<std::string>{Name.str()});
  for (const auto &[Addr, Best] : BestSymbols)
    if (const llvm::StringRef Name = Shown(Addr); !Name.empty())
      Names.try_emplace(Addr, std::vector<std::string>{Name.str()});
  const auto Add = [&](va_t Addr, llvm::StringRef Name) {
    const auto Known = Names.find(Addr);
    if (Known != Names.end() && !Name.empty() &&
        !llvm::is_contained(Known->second, Name))
      Known->second.push_back(Name.str());
  };
  for (const Export &Exp : Img.Exports)
    Add(Exp.Addr, Exp.Name);
  for (const Symbol &Sym : Img.Symbols)
    if (Sym.IsFunc && Sym.Origin == NameOrigin::Stated)
      Add(Sym.Addr, Sym.Name);
  return Names;
}

} // namespace neverd

#endif // NEVERD_LOADER_FUNCTIONNAMES_H
