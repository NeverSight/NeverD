//===- RegistrationCleanupTestUtils.h - Cleanup proof edits ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent mutations of compiler-generated cleanups inside a live catch.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_TEST_REGISTRATIONCLEANUPTESTUTILS_H
#define NEVERD_TEST_REGISTRATIONCLEANUPTESTUTILS_H

namespace llvm {
class Function;
}
namespace neverd {
struct BinaryImage;
struct LowFunc;
struct MedFunc;
namespace registration_test {
void checkCatchCleanupReachability(const LowFunc &Low,
                                   const BinaryImage &Image);
void checkCatchCleanupEdits(const MedFunc &Med, const llvm::Function &Function,
                            const BinaryImage &Image);
} // namespace registration_test
} // namespace neverd
#endif
