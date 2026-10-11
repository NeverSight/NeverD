//===- RegistrationMultipleCatchTestUtils.h - Catch rejection probes
//-------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_REGISTRATIONMULTIPLECATCHTESTUTILS_H
#define NEVERD_REGISTRATIONMULTIPLECATCHTESTUTILS_H
namespace llvm {
class Function;
}
namespace neverd {
struct ExceptionFunction;
struct BinaryImage;
namespace registration_test {
void checkMultipleCatchEdits(const llvm::Function &Parent,
                             const ExceptionFunction &Source,
                             const BinaryImage &Image);
}
} // namespace neverd
#endif
