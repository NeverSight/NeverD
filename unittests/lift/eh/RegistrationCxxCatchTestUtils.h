//===- RegistrationCxxCatchTestUtils.h - Unbound PE32 catches ----*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_REGISTRATIONCXXCATCHTESTUTILS_H
#define NEVERD_REGISTRATIONCXXCATCHTESTUTILS_H

namespace llvm {
class Function;
}
namespace neverd {
struct BinaryImage;
struct ExceptionFunction;
namespace registration_test {
void checkUnboundCxxCatchEdits(const llvm::Function &Parent,
                               const ExceptionFunction &Source,
                               const BinaryImage &Image);
} // namespace registration_test
} // namespace neverd

#endif // NEVERD_REGISTRATIONCXXCATCHTESTUTILS_H
