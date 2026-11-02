//===- ObjectExterns.h - Addresses of a relocatable object's externs ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A relocatable object names symbols it does not define, and a relocation
/// against one of them has no address until a linker supplies one.  Each
/// format's loader collects them, with the common symbols the object leaves
/// to the linker and the pointer cells some references reach a symbol through
/// (ELF GOT entries, COFF `__imp_` pointers).  This layer gives each an address
/// past the object's sections and adds the segments that hold them, as IDA and
/// Ghidra show an object's externs.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_OBJECTEXTERNS_H
#define NEVERD_LOADER_OBJECTEXTERNS_H

#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace neverd {
namespace object_externs {

/// What a relocatable object references without defining.
struct ExternRequests {
  struct UndefinedSymbol {
    std::string Name;
    bool Called = false;
    /// The furthest offset past the symbol a relocation states.
    uint64_t StatedReach = 0;
  };
  struct CommonSymbol {
    std::string Name;
    uint64_t Bytes = 0;
    uint64_t Alignment = 1;
  };
  /// In the order relocations first name them.
  std::vector<UndefinedSymbol> Undefined;
  std::vector<CommonSymbol> Commons;

  /// An undefined symbol a relocation names: \p Called when a call or branch
  /// reaches it, with \p Addend past it.
  void noteUndefined(llvm::StringRef Name, bool Called, int64_t Addend);
  /// A tentative definition the object leaves common: \p Bytes of storage
  /// aligned to \p Alignment.
  void noteCommon(llvm::StringRef Name, uint64_t Bytes, uint64_t Alignment);
  bool empty() const { return Undefined.empty() && Commons.empty(); }

private:
  std::map<std::string, size_t> UndefinedIndex;
  std::set<std::string> CommonNames;
};

/// The addresses of an object's externs and cells.
struct ExternLayout {
  /// Each undefined symbol, by name: its address in the writable `extern`
  /// segment.  A called one is an import there, data a symbol.
  std::map<std::string, va_t> SymbolSlots;
  std::set<std::string> CalledSymbols;
  /// The bytes each undefined data symbol's address keeps past it.
  std::map<std::string, uint64_t> SymbolBytes;
  /// Each common symbol, by name: its storage in the extern segment, and the
  /// bytes of it.
  std::map<std::string, va_t> CommonSlots;
  std::map<std::string, uint64_t> CommonSizes;
  va_t ExternBase = 0;
  uint64_t ExternSize = 0;
  /// The pointer-sized cells, in a read-only segment past the externs.
  va_t CellBase = 0;
  uint64_t CellSize = 0;
  uint64_t PointerBytes = 0;

  va_t cellAddress(size_t Index) const {
    return CellBase + Index * PointerBytes;
  }
};

/// Place \p Requests and \p Cells pointer cells past \p ImageEnd, the end of
/// the object's allocated sections.  An object whose externs do not fit the
/// address space is refused.
llvm::Expected<ExternLayout> layoutExterns(const ExternRequests &Requests,
                                           size_t Cells, uint64_t PointerBytes,
                                           va_t ImageEnd);

/// Add the extern segment with its section to \p Img, every undefined symbol
/// as a symbol, and the called ones as imports named \p ImportName of their
/// symbols.
void addExternSegment(
    const ExternLayout &Layout, BinaryImage &Img,
    llvm::function_ref<std::string(llvm::StringRef)> ImportName =
        [](llvm::StringRef Symbol) { return Symbol.str(); });

/// Add the cells as a read-only segment and section named \p Name, holding
/// \p Contents.
void addCellSegment(const ExternLayout &Layout, llvm::StringRef Name,
                    std::vector<uint8_t> Contents, BinaryImage &Img);

/// Store \p Address in cell \p Index of \p Contents.
void writeCell(const ExternLayout &Layout, std::vector<uint8_t> &Contents,
               size_t Index, va_t Address);

} // namespace object_externs
} // namespace neverd

#endif // NEVERD_LOADER_OBJECTEXTERNS_H
