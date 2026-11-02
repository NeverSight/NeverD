//===- HighCEmitter.h - High IR to C emitter -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Emitter that translates HighIR to structured C source code.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_HIGHC_HIGHCEMITTER_H
#define NEVERD_BACKEND_C_HIGHC_HIGHCEMITTER_H
#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/debug/DebugContext.h"
#include "neverd/ir/high/HighIR.h"

#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace neverd {

/// What an emission reads of an image's data symbols to bound the objects
/// the code reaches: each object a symbol sizes (the largest at an address),
/// the furthest any object at or before each one reaches, and where each data
/// symbol starts.  It depends on the image alone.
struct HighCImageDataObjects {
  std::vector<std::pair<va_t, uint64_t>> SizedObjects;
  std::vector<va_t> SizedObjectReach;
  std::vector<va_t> DataSymbolStarts;
};

/// \p Image's data objects, as every emission over it reads them.
HighCImageDataObjects collectImageDataObjects(const BinaryImage &Image);

/// The first function symbol with a name at each address of \p Image, in
/// the order of its symbols: the one an address the code takes names.
using HighCImageFunctionSymbols = std::unordered_map<va_t, const Symbol *>;
HighCImageFunctionSymbols collectImageFunctionSymbols(const BinaryImage &Image);

class PreparedPortableX87Function;

class HighCEmitter {
  const HighFunc *PortableBody = nullptr;
  const PreparedPortableX87Function *PortableInterface = nullptr;
  const BinaryImage *PreparedImage = nullptr;
  std::unordered_set<std::string_view> PreparedImageFunctionNames;
  HighCImageDataObjects PreparedDataObjects;
  HighCImageFunctionSymbols PreparedFunctionSymbols;

public:
  /// Reuse the image's defined-function index and data objects across a
  /// batch of emissions: an image holds tens of thousands of symbols.  The
  /// image and its symbols must remain unchanged until the batch ends.
  void prepareImageFunctionNames(const BinaryImage &Image);

  bool emit(const std::vector<HighFunc> &Funcs, llvm::raw_ostream &OS,
            const CEmitterOptions &Opts = {}, DebugContext *Dbg = nullptr);

  /// Emit an isolated prepared portable leaf. Source/debug state on this
  /// emitter is not part of the alternate explicit C interface.
  llvm::Expected<std::string>
  emitPortableX87(const PreparedPortableX87Function &Prepared,
                  llvm::StringRef OutputTriple);

  bool emitToFile(const std::vector<HighFunc> &Funcs, const std::string &Path,
                  const CEmitterOptions &Opts = {},
                  DebugContext *Dbg = nullptr);
};

} // namespace neverd

#endif // NEVERD_BACKEND_C_HIGHC_HIGHCEMITTER_H
