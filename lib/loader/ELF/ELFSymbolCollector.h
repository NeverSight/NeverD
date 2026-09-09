//===- ELFSymbolCollector.h - ELF symbol identity merging -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LOADER_ELF_ELFSYMBOLCOLLECTOR_H
#define NEVERD_LIB_LOADER_ELF_ELFSYMBOLCOLLECTOR_H

#include "neverd/loader/BinaryImageModel.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Compiler.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <tuple>
#include <utility>

namespace neverd {
namespace elf_loader {
namespace LLVM_LIBRARY_VISIBILITY_NAMESPACE detail {

/// Merge repeated definitions while preserving names, addresses and raw kinds.
/// Callers normalize addresses and classify functions for their ELF dialect.
/// Names must refer to file-backed string tables that outlive the collector.
class ELFSymbolCollector {
  using Key = std::tuple<va_t, llvm::StringRef, uint8_t>;
  BinaryImage &Image;
  std::map<Key, size_t> Indices;

public:
  explicit ELFSymbolCollector(BinaryImage &Image) : Image(Image) {}

  void add(llvm::StringRef Name, va_t Address, uint64_t Size, uint8_t Kind,
           bool IsFunction) {
    const auto [Entry, Inserted] =
        Indices.try_emplace(Key{Address, Name, Kind}, Image.Symbols.size());
    if (Inserted) {
      Symbol Definition;
      Definition.Name = Name.str();
      Definition.Addr = Address;
      Definition.Size = Size;
      Definition.IsFunc = IsFunction;
      Image.Symbols.push_back(std::move(Definition));
    } else if (Image.Symbols[Entry->second].Size == 0) {
      // One table can omit the size supplied by another definition.
      Image.Symbols[Entry->second].Size = Size;
    }
  }
};

} // namespace LLVM_LIBRARY_VISIBILITY_NAMESPACE detail
} // namespace elf_loader
} // namespace neverd

#endif // NEVERD_LIB_LOADER_ELF_ELFSYMBOLCOLLECTOR_H
