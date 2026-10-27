//===- RegistrationSEHJumpNativeTestUtils.h - Jump edits --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TEST_REGISTRATIONSEHJUMPNATIVETESTUTILS_H
#define NEVERD_TEST_REGISTRATIONSEHJUMPNATIVETESTUTILS_H

namespace llvm {
class Function;
}
namespace neverd {
struct RegistrationStateAnalysis;
struct BinaryImage;
struct ExceptionFunction;
namespace registration_test {
void checkSEHJumpCaptureStateEdits(const llvm::Function &Parent,
                                   const ExceptionFunction &Source,
                                   const BinaryImage &Image);
void checkSEHJumpStateEdits(const llvm::Function &Parent,
                            const ExceptionFunction &Source,
                            const BinaryImage &Image);
void checkSEHJumpEdits(const llvm::Function &Parent,
                       const ExceptionFunction &Source,
                       const BinaryImage &Image);
void checkSEHBoundEdits(const llvm::Function &Parent,
                        const ExceptionFunction &Source,
                        const BinaryImage &Image);
void checkSEHBoundSourceEdits(const llvm::Function &Parent,
                              const ExceptionFunction &Source,
                              const BinaryImage &Image);
void checkSEHBoundCookieEdits(const llvm::Function &Parent,
                              const ExceptionFunction &Source,
                              const ExceptionFunction &Helper,
                              const BinaryImage &Image);
void checkSEHRuntimeEdits(const llvm::Function &Parent,
                          const ExceptionFunction &Source,
                          const BinaryImage &Image);
void checkSEHStartupInfoEdits(const llvm::Function &Parent,
                              const ExceptionFunction &Source,
                              const BinaryImage &Image);
void checkSEHSynchronizationEdits(const llvm::Function &Parent,
                                  const ExceptionFunction &Source,
                                  const BinaryImage &Image);
void checkSEHThreadEdits(const llvm::Function &Parent,
                         const ExceptionFunction &Source,
                         const BinaryImage &Image,
                         const RegistrationStateAnalysis &States);
void checkSEHHeapPolicyEdits(const llvm::Function &Parent,
                             const ExceptionFunction &Source,
                             const BinaryImage &Image);
void checkSEHComputedStateEdits(const llvm::Function &Parent,
                                const ExceptionFunction &Source,
                                const BinaryImage &Image,
                                const RegistrationStateAnalysis &States);
} // namespace registration_test
} // namespace neverd

#endif // NEVERD_TEST_REGISTRATIONSEHJUMPNATIVETESTUTILS_H
