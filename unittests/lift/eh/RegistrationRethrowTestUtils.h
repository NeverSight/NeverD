//===- RegistrationRethrowTestUtils.h - Rethrow edits -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Verify current MedIR and LLVM runtime arguments independently of receipts.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_TESTS_REGISTRATIONRETHROWTESTUTILS_H
#define NEVERD_TESTS_REGISTRATIONRETHROWTESTUTILS_H
namespace llvm {
class Function;
} // namespace llvm
namespace neverd {
struct BinaryImage;
struct MedFunc;
namespace registration_test {
void checkRuntimeThrowEdits(const MedFunc &Med, const llvm::Function &Parent,
                            const BinaryImage &Image);
} // namespace registration_test
} // namespace neverd
#endif
