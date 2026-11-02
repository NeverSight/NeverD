//===- HighIRPrint.h - Statement-aware HighIR text emission ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// One HighIR statement printer for CLI dumps, legacy text and mapped pages.
/// Each row includes its newline. Only actual statement rows carry a statement;
/// braces, case labels and exception metadata never borrow a parent's anchor.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_HIGH_HIGHIRPRINT_H
#define NEVERD_IR_HIGH_HIGHIRPRINT_H

#include "neverd/ir/NdOps.h"

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"

namespace neverd {
struct HighStmt;
const char *memoryAddressSpaceQualifier(NdMemoryAddressSpace AddressSpace);
using HighIRRowSink =
    llvm::function_ref<void(llvm::StringRef, const HighStmt *)>;
void printHighStatementRows(const HighStmt &Statement, int Indent,
                            HighIRRowSink Sink);
} // namespace neverd

#endif // NEVERD_IR_HIGH_HIGHIRPRINT_H
