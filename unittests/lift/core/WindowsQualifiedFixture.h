//===- WindowsQualifiedFixture.h - Qualified imports -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Independent declarations for pointer qualifiers in Windows header calls.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TEST_WINDOWSQUALIFIEDFIXTURE_H
#define NEVERD_TEST_WINDOWSQUALIFIEDFIXTURE_H

#include "neverd/debug/DebugContext.h"
#include "neverd/debug/UserPrototype.h"

#include <cstdint>
#include <optional>
#include <string>

namespace neverd::windows_qualified_test {
class Header : public NullDebugContext {
public:
  va_t Slot = 0;
  bool Nested = false;
  bool Record = false;
  bool Union = false;
  bool VoidResult = false;
  bool Local = false;
  uint16_t PointerBytes = 8;
  DebugCallConv Convention = DebugCallConv::Cdecl;
  std::string element() const {
    return Record ? (Union ? "union fixture_record" : "struct fixture_record")
                  : "uint16_t";
  }
  std::string declaration() const {
    return (VoidResult ? "void " : "const " + element() + " *") +
           "fixture_qualified(const " + element() +
           (Nested ? " *const *input" : " *input") +
           ", volatile uint32_t *output)";
  }
  std::optional<FunctionSym>
  resolvePrototype(llvm::StringRef Name) const override {
    if (Name != "fixture_qualified")
      return std::nullopt;
    const auto Pointer = [&](TypeRef Type) {
      auto Result = NdType::makePtr(Type);
      Result->Size = PointerBytes;
      return Result;
    };
    auto Element = Record ? NdType::makeNamedRecord("fixture_record", 0)
                          : NdType::makeInt(2, false);
    // Match the C record classification supplied by LocalTypesDebugContext;
    // an unclassified PDB class pointer may encode a hidden return instead.
    if (Record)
      Element->Passing = NdRecordPassing::ByValue;
    auto Result = Pointer(Element);
    FunctionSym Function;
    Function.Name = Name.str();
    Function.ReturnType = VoidResult ? NdType::makeVoid() : Result;
    Function.Params = {{"input", Nested ? Pointer(Result) : Result},
                       {"output", Pointer(NdType::makeInt(4, false))}};
    Function.CallConv = Convention;
    Function.PrototypeFromHeader = !Local;
    Function.PrototypeLibrary =
        Local ? UserPrototypeLibrary.str() : "test-windows-header";
    Function.PortablePrototypeDeclaration = declaration();
    return Function;
  }
  std::optional<FunctionSym> resolveFunction(va_t Address) const override {
    auto Result =
        Address == Slot ? resolvePrototype("fixture_qualified") : std::nullopt;
    if (Result)
      Result->Addr = Address;
    return Result;
  }
};
} // namespace neverd::windows_qualified_test

#endif // NEVERD_TEST_WINDOWSQUALIFIEDFIXTURE_H
