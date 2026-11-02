//===- WindowsMixedScalarFixture.h - Mixed scalar import ABI -----*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// An independent provider checks every integer, pointer and floating input.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_LIFT_CORE_WINDOWSMIXEDSCALARFIXTURE_H
#define NEVERD_UNITTESTS_LIFT_CORE_WINDOWSMIXEDSCALARFIXTURE_H

#include "neverd/debug/DebugContext.h"

#include <optional>
#include <string>

namespace windows_mixed_scalar_test {

class Header : public neverd::NullDebugContext {
  neverd::va_t Slot;

public:
  explicit Header(neverd::va_t Address) : Slot(Address) {}
  std::optional<neverd::FunctionSym>
  resolvePrototype(llvm::StringRef Name) const override {
    using namespace neverd;
    if (Name != "fixture_mixed")
      return std::nullopt;
    FunctionSym Function;
    Function.Name = Name.str();
    Function.PrototypeFromHeader = true;
    Function.PrototypeLibrary = "test-header";
    Function.CallConv = DebugCallConv::Cdecl;
    Function.ReturnType = NdType::makeFloat(8);
    Function.Params = {{"flags", NdType::makeInt(4, false)},
                       {"operation", NdType::makeInt(4, false)},
                       {"left", NdType::makeFloat(8)},
                       {"right", NdType::makeFloat(8)},
                       {"mask", NdType::makeInt(4, false)},
                       {"context", NdType::makePtr(NdType::makeVoid())}};
    Function.PortablePrototypeDeclaration =
        "double fixture_mixed(uint32_t flags, uint32_t operation, double left, "
        "double right, uint32_t mask, void *context)";
    return Function;
  }
  std::optional<neverd::FunctionSym>
  resolveFunction(neverd::va_t Address) const override {
    auto Function =
        Address == Slot ? resolvePrototype("fixture_mixed") : std::nullopt;
    if (Function)
      Function->Addr = Address;
    return Function;
  }
};

inline constexpr const char *Provider = R"(
double fixture_mixed(uint32_t flags, uint32_t operation, double left,
                     double right, uint32_t mask, void *context) {
  if (flags != 7 || operation != 9 || left != 2.5 || right != -6.5 ||
      mask != 11 || (uintptr_t)context != 0x1234) __builtin_trap();
  return 42.25;
}
)";

} // namespace windows_mixed_scalar_test

#endif // NEVERD_UNITTESTS_LIFT_CORE_WINDOWSMIXEDSCALARFIXTURE_H
