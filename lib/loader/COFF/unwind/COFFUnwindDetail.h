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

#include "neverd/loader/ExceptionTable.h"

#include "llvm/Support/Compiler.h"

namespace neverd::coff_loader {
namespace LLVM_LIBRARY_VISIBILITY_NAMESPACE unwind_detail {

/// Resolve directory-backed x64 chained-unwind references and validate that
/// each chain reaches a primary record.
void resolveX64UnwindChains(ExceptionInfo &Info);

} // namespace LLVM_LIBRARY_VISIBILITY_NAMESPACE unwind_detail
} // namespace neverd::coff_loader

#endif // NEVERD_LIB_LOADER_COFF_UNWIND_COFFUNWINDDETAIL_H
