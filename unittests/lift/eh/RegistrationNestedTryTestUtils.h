//===- RegistrationNestedTryTestUtils.h - PE32 search checks ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TEST_REGISTRATIONNESTEDTRYTESTUTILS_H
#define NEVERD_TEST_REGISTRATIONNESTEDTRYTESTUTILS_H

namespace llvm {
class Function;
}
namespace neverd {
struct BinaryImage;
struct ExceptionFunction;
struct HighFunc;
struct MedFunc;
namespace registration_test {
void checkNestedTryHigh(const MedFunc &Med, const HighFunc &High);
void checkNestedTrySource(const BinaryImage &Image,
                          const ExceptionFunction &Source);
void checkNestedTryEdits(const llvm::Function &Parent,
                         const ExceptionFunction &Source,
                         const BinaryImage &Image, bool Secondary);
} // namespace registration_test
} // namespace neverd

#endif
