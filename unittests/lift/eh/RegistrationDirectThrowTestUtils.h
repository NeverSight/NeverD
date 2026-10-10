//===- RegistrationDirectThrowTestUtils.h - CRT throw edits ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Mutation checks for reconstructed x86 C++ runtime throws.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_TESTS_REGISTRATIONDIRECTTHROWTESTUTILS_H
#define NEVERD_TESTS_REGISTRATIONDIRECTTHROWTESTUTILS_H
namespace llvm {
class Function;
} // namespace llvm
namespace neverd {
struct BinaryImage;
struct MedFunc;
namespace registration_test {
void checkDirectThrowEdits(const MedFunc &Med, const llvm::Function &Parent,
                           const BinaryImage &Image);
} // namespace registration_test
} // namespace neverd
#endif
