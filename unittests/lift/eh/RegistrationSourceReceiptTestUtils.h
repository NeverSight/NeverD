//===- RegistrationSourceReceiptTestUtils.h - PE32 receipts ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared capture identity for independently replayed source reconstructions.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_TEST_REGISTRATIONSOURCERECEIPTTESTUTILS_H
#define NEVERD_TEST_REGISTRATIONSOURCERECEIPTTESTUTILS_H

#include "llvm/Support/JSON.h"

namespace neverd {
struct BinaryImage;
struct ExceptionFunction;
struct RegistrationStateAnalysis;
namespace registration_test {
void writeSourceReceipt(const char *Input, const char *Output,
                        const BinaryImage &Image,
                        const ExceptionFunction &Source,
                        const RegistrationStateAnalysis &States,
                        const ExceptionFunction &Generated,
                        llvm::json::Object Context);
} // namespace registration_test
} // namespace neverd

#endif
