//===- WindowsVariadicFixture.h - Windows varargs ---------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A header-only variadic import and an independent integer-tail provider.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TEST_WINDOWSVARIADICFIXTURE_H
#define NEVERD_TEST_WINDOWSVARIADICFIXTURE_H

#include "neverd/debug/DebugContext.h"

#include <optional>
#include <string>

namespace neverd::windows_variadic_test {
class Header : public NullDebugContext {
public:
  va_t Slot = 0;
  unsigned Fixed = 2;
  std::optional<FunctionSym>
  resolvePrototype(llvm::StringRef Name) const override {
    if (Name != "fixture_variadic")
      return std::nullopt;
    FunctionSym Function;
    Function.Name = Name.str();
    Function.ReturnType = NdType::makeInt(4, true);
    Function.Params = {{"first", NdType::makeInt(4, true)},
                       {"count", NdType::makeInt(4, true)}};
    for (unsigned I = Function.Params.size(); I < Fixed; ++I)
      Function.Params.emplace_back("fixed" + std::to_string(I),
                                   NdType::makeInt(4, true));
    Function.IsVariadic = true;
    Function.PrototypeFromHeader = true;
    Function.PrototypeLibrary = "test-windows-header";
    return Function;
  }
  std::optional<FunctionSym> resolveFunction(va_t Address) const override {
    auto Result =
        Address == Slot ? resolvePrototype("fixture_variadic") : std::nullopt;
    if (Result)
      Result->Addr = Address;
    return Result;
  }
};

inline std::string oracle(unsigned Tail, llvm::StringRef Entry) {
  return "\n#include <stdarg.h>\n"
         "int32_t fixture_variadic(int32_t first, int32_t count, ...) {\n"
         "  if (first != -7 || count != " +
         std::to_string(Tail) +
         ") __builtin_trap();\n"
         "  va_list args; va_start(args, count);\n"
         "  for (int i = 0; i < count; ++i)\n"
         "    if (va_arg(args, int) != 11 * (i + 1)) __builtin_trap();\n"
         "  va_end(args); return -19;\n}\n"
         "int main(void) { return (int32_t)" +
         Entry.str() + "(-7) != -19; }\n";
}
} // namespace neverd::windows_variadic_test
#endif // NEVERD_TEST_WINDOWSVARIADICFIXTURE_H
