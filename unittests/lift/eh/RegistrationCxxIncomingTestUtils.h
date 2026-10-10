//===- RegistrationCxxIncomingTestUtils.h - Caller argument proofs --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_REGISTRATIONCXXINCOMINGTESTUTILS_H
#define NEVERD_REGISTRATIONCXXINCOMINGTESTUTILS_H

namespace llvm {
class Function;
}
namespace neverd {
struct BinaryImage;
struct ExceptionFunction;
namespace registration_test {
void checkCxxIncomingEdits(const llvm::Function &Parent,
                           const ExceptionFunction &Source,
                           const BinaryImage &Image);
} // namespace registration_test
} // namespace neverd
#endif
