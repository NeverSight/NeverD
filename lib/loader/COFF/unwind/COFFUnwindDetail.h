//===- COFFUnwindDetail.h - Private PE unwind graph helpers ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Private helpers shared by the COFF loader implementation and its focused
/// synthetic tests.  This header is not part of NeverD's installed API.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LOADER_COFF_UNWIND_COFFUNWINDDETAIL_H
#define NEVERD_LIB_LOADER_COFF_UNWIND_COFFUNWINDDETAIL_H

#include "neverd/loader/BinaryImageModel.h"
#include "neverd/loader/ExceptionTable.h"

#include "llvm/Support/Compiler.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace neverd::coff_loader {
namespace LLVM_LIBRARY_VISIBILITY_NAMESPACE unwind_detail {

/// Decode into a freshly constructed record in the owning directory. The
/// value-returning public decoder uses the same implementation.
void decodeX64ExceptionFunctionInto(const BinaryImage &Img, va_t ImageBase,
                                    uint32_t RuntimeFunctionRVA,
                                    uint32_t BeginRVA, uint32_t EndRVA,
                                    uint32_t UnwindInfoRVA,
                                    ExceptionFunction &Function);

/// Resolve directory-backed x64 chained-unwind references and validate that
/// each chain reaches a primary record.
void resolveX64UnwindChains(ExceptionInfo &Info);

/// The x64 RUNTIME_FUNCTION records at \p RFBytes that a restricted load
/// decodes: those covering one of the image's LoadOnlyFunctionEntries and
/// every chained record whose unwind chain leads to one of them.  A
/// function's cold parts belong to it, and its scope table can guard code
/// there.  Chained records are decoded to read their chain; \p Decoded keeps
/// them, by record index, for the caller.
std::vector<bool> selectRestrictedX64Records(
    const BinaryImage &Img, uint64_t ImageBase, const uint8_t *RFBytes,
    size_t Count, uint32_t DirectoryRVA,
    std::unordered_map<size_t, ExceptionFunction> &Decoded);

} // namespace LLVM_LIBRARY_VISIBILITY_NAMESPACE unwind_detail
} // namespace neverd::coff_loader

#endif // NEVERD_LIB_LOADER_COFF_UNWIND_COFFUNWINDDETAIL_H
