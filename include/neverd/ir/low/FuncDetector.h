//===- FuncDetector.h - Function entry-point detection --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares FuncDetector, which discovers function entry points from symbol
/// tables, exports, call-target scanning, and heuristic validation.
/// Architecture-specific scanners live in FuncDetectorX86.cpp,
/// FuncDetectorARM.cpp, and FuncDetectorAArch64.cpp.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_FUNCDETECTOR_H
#define NEVERD_IR_LOW_FUNCDETECTOR_H

#include "neverd/decode/Decoder.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/ArrayRef.h"

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace neverd {

namespace func_detect_detail {
struct Cancellation;
}

class FuncDetector {
public:
  /// Detect function entry points from the binary image.
  std::vector<std::pair<va_t, std::string>> detect(const BinaryImage &Img,
                                                   Decoder &Dec);

  /// The same discovery with cooperative cancellation. The probe may run on
  /// scan workers: it must be thread-safe, nonblocking and nonthrowing. A
  /// stopped scan joins all workers and returns nullopt without publishing
  /// candidate entries into the image. Empty completed discovery is distinct.
  std::optional<std::vector<std::pair<va_t, std::string>>>
  detect(const BinaryImage &Img, Decoder &Dec,
         const std::function<bool()> &Cancelled);

private:
  friend class RelocatedFunctionEntryVerifier;
  const func_detect_detail::Cancellation *Cancellation = nullptr;
  std::vector<std::pair<va_t, std::string>>
  detectImpl(const BinaryImage &Img, Decoder &Dec, bool RelocationsOnly);
  void scanCallTargets(const BinaryImage &Img, Decoder &Dec,
                       std::set<va_t> &Out,
                       std::vector<std::pair<va_t, va_t>> &CodeAddresses);
  /// Recover VC6 / MSVC functions that have no PDB symbol: EBP-frame
  /// prologues at any alignment, plus 16-byte-aligned SEH / callee-saved /
  /// stack-adjust starts.  Tagged entries skip the bounded verify walk so a
  /// large body is not dropped for exhausting kMaxVerifyInsns.
  void scanX86UnsymbolizedEntries(const BinaryImage &Img, Decoder &Dec,
                                  std::set<va_t> &Out);
  /// Drop the unsymbolized x86 entries, and the \p Guessed starts the loader
  /// only inferred from padding, that a function with other entry evidence
  /// reaches as its own code: by falling into one, by a conditional branch,
  /// or as a case a pointer table among its code names.  A target only an
  /// unconditional jump reaches stays an entry, since the jump may be a tail
  /// call, and so does a frame prologue right after a call that may not
  /// return.  \p Evidenced holds the entries with other evidence and
  /// \p TableCases each code address naming code with the code it names.
  /// Returns the entries dropped.
  std::set<va_t> rejectX86InteriorEntries(
      const BinaryImage &Img, Decoder &Dec, const std::set<va_t> &Evidenced,
      const std::set<va_t> &Guessed,
      const std::vector<std::pair<va_t, va_t>> &TableCases);
  /// The x86 code the function at \p Entry reaches before \p End: falling
  /// through, by conditional and direct jumps, past calls that may return,
  /// and into the \p Cases its tables among the code name.  \p Computed is
  /// set when the code has an indirect jump the walk cannot follow.
  std::set<va_t> x86OwnCodeReached(const BinaryImage &Img, Decoder &Dec,
                                   va_t Entry, va_t End,
                                   llvm::ArrayRef<va_t> Cases, bool &Computed);
  /// Validate a heuristic entry.  When KeepInconclusive is true, bounded
  /// probes that exhaust their budget remain candidates for the formal audit;
  /// definite decode or mapping failures are still rejected. Relocation-only
  /// evidence additionally needs a supported, bounded CFG with a callable exit;
  /// trap-only bytes and an exhausted proof do not establish that role.
  bool verifyFunctionDecode(const BinaryImage &Img, Decoder &Dec, va_t Addr,
                            bool KeepInconclusive = false,
                            bool RequireCallableExit = false);

  std::set<va_t> Entries;
  std::set<va_t> UnsymbolizedX86Entries;
  std::set<va_t> RelocatedCandidates;
};

/// Callable entry evidence from one immutable image's metadata or relocations.
/// Candidate ownership uses the detector's filters, without a call/prologue
/// sweep. Verify only requested targets and retain positive and negative
/// answers locally; concurrent LLVM shards never publish into the image's
/// mutable entry set.
class RelocatedFunctionEntryVerifier {
public:
  explicit RelocatedFunctionEntryVerifier(const BinaryImage &Image)
      : Img(Image) {}
  bool accepts(va_t Address);

private:
  const BinaryImage &Img;
  bool Prepared = false;
  bool MetadataPrepared = false;
  std::vector<va_t> MetadataEntries;
  Decoder Dec;
  FuncDetector Detector;
  std::map<va_t, bool> Answers;
};

} // namespace neverd

#endif // NEVERD_IR_LOW_FUNCDETECTOR_H
