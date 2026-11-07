//===- COFFException.h - Checked PE exception decoding --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares the bounded PE/COFF exception decoders used by the loader.  The
/// record-level entry point is public so fuzzers and synthetic tests can verify
/// corrupt metadata without manufacturing an entire PE image.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_COFF_COFFEXCEPTION_H
#define NEVERD_LOADER_COFF_COFFEXCEPTION_H

#include "neverd/loader/BinaryImage.h"

#include <algorithm>
#include <set>

namespace neverd::coff_loader {

/// Decode one AMD64 RUNTIME_FUNCTION and the referenced UNWIND_INFO record.
/// All RVA arithmetic and image reads are checked.  Failure is represented by
/// ExceptionFunction::ParseStatus and diagnostics; partial information such as
/// a sound code range remains available to function discovery.
ExceptionFunction decodeX64ExceptionFunction(const BinaryImage &Img,
                                             va_t ImageBase,
                                             uint32_t RuntimeFunctionRVA,
                                             uint32_t BeginRVA, uint32_t EndRVA,
                                             uint32_t UnwindInfoRVA);

/// Checked AMD64 runtime-function data records, including embedded chain rows,
/// in record-address order. EndRVA remains an exclusive boundary, not an entry.
std::vector<BinaryImage::COFFPDataRecord>
x64RuntimeFunctionRecords(const BinaryImage &Img);

/// Resolve and decode language personalities after imports, symbols, and
/// executable import veneers have been discovered.  Unknown personalities are
/// retained without guessing their handler-data schema.  When
/// BinaryImage::LoadOnlyFunctionEntries is nonempty, only those entries and
/// the catch funclets / out-of-line SEH filters they name are materialized.
void resolveExceptionHandlers(BinaryImage &Img);

/// Revisit already-decoded unknown Windows personalities after publishing
/// symbols. Only newly recognized handlers are decoded again; their language
/// diagnostics are replaced independently of native unwind/directory faults.
void refreshExceptionHandlerNames(BinaryImage &Img);

/// Decode unwind and language tables for \p Entries (and their catch
/// funclets and out-of-line SEH filters) if they are not already present.
/// Empty \p Entries with an empty load filter means every already-materialized
/// function.
void ensureExceptionHandlers(BinaryImage &Img, const std::set<va_t> &Entries);

/// What ensureExceptionHandlers can add to an image: runtime-function
/// records (with the code ranges and symbols they bring) and decoded language
/// tables, which a record gets once for its current handler-name evidence.
/// Equal states before
/// and after a call mean the call added nothing analysis reads; it can only
/// have marked records without a personality as resolved.
struct ExceptionHandlerState {
  size_t Functions = 0;
  size_t ResolvedPersonalities = 0;
  size_t KnownCodeRanges = 0;
  size_t Symbols = 0;
  ExceptionParseStatus ParseStatus = ExceptionParseStatus::Complete;
  bool operator==(const ExceptionHandlerState &) const = default;
};
ExceptionHandlerState exceptionHandlerState(const BinaryImage &Img);

/// Whether ensureExceptionHandlers can add nothing to \p Img, whatever
/// functions it is given: no runtime-function record is left to materialize
/// (a whole image decodes every record when it loads) and every record's
/// language tables are resolved. Loading an image or publishing new handler
/// names can change that state.
inline bool exceptionHandlersSettled(const BinaryImage &Img) {
  return Img.COFFPDataRecords.empty() &&
         std::all_of(Img.ExceptionMetadata.Functions.begin(),
                     Img.ExceptionMetadata.Functions.end(),
                     [](const ExceptionFunction &F) {
                       return F.LanguageTablesResolved;
                     });
}

/// Materialize the x64 RUNTIME_FUNCTION covering \p Address from
/// BinaryImage::COFFPDataRecords when `--func` skipped it at load.
bool ensureX64RuntimeFunction(BinaryImage &Img, va_t Address);

} // namespace neverd::coff_loader

#endif // NEVERD_LOADER_COFF_COFFEXCEPTION_H
