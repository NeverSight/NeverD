#ifndef NEVERD_SDK_CAPI_OBJCCONSTANTOBJECTSOURCES_H
#define NEVERD_SDK_CAPI_OBJCCONSTANTOBJECTSOURCES_H

#include "ObjCConstantStringSources.h"

#include "neverd/loader/ObjC/ObjCConstantObjects.h"

namespace neverd::sdk {
inline std::optional<SourceCallTypeHint>
constantObjectSourceHint(const BinaryImage &Image, va_t Address,
                         va_t PointerSlot = 0) {
  if (PointerSlot && readImmutableImagePointer(Image, PointerSlot) != Address)
    return std::nullopt;
  const auto Graph = readObjCConstantObjectGraph(Image, Address);
  if (!Graph || Graph->at(Address).TheKind == ObjCConstantObject::Kind::String)
    return std::nullopt;
  SourceCallTypeHint Hint;
  Hint.CallKind = SourceCallTypeHint::Kind::RuntimeConstantObject;
  Hint.TargetAddress = Address;
  Hint.ImmutablePointerSlot = PointerSlot;
  Hint.Signature.ReturnType = NdType::makePtr(NdType::makeVoid());
  std::string Reason;
  if (!assignDarwinScalarSourceABI(Hint.Signature, Image.Arch, Reason))
    return std::nullopt;
  return Hint;
}

inline std::string renderObjCConstantObjectHelpers(
    const BinaryImage &Image, const std::set<va_t> &Roots,
    std::set<va_t> Strings, std::set<std::string> &SharedFunctions) {
  using Kind = ObjCConstantObject::Kind;
  ObjCConstantObjectGraph Objects;
  size_t BytesLeft = 1024 * 1024, EdgesLeft = 16384;
  for (va_t Root : Roots) {
    if (Objects.count(Root))
      continue;
    auto Graph = readObjCConstantObjectGraph(Image, Root);
    if (!Graph)
      throw std::runtime_error(
          "constant-object source graph is no longer valid");
    for (auto &[Address, Object] : *Graph) {
      if (Objects.count(Address))
        continue;
      const size_t Edges = Object.Elements.size() + Object.Keys.size();
      const size_t Bytes = 40 + Object.String.Units.size() * 2 + Edges * 8;
      if (Objects.size() >= 4096 || Bytes > BytesLeft || Edges > EdgesLeft)
        throw std::runtime_error(
            "constant-object source graph exceeds its budget");
      BytesLeft -= Bytes;
      EdgesLeft -= Edges;
      if (Object.TheKind == Kind::String)
        Strings.insert(Address);
      Objects.emplace(Address, std::move(Object));
    }
  }
  auto Name = [&](va_t Address) {
    return std::string(Objects.at(Address).TheKind == Kind::String
                           ? "neverd_objc_constant_string_"
                           : "neverd_objc_constant_object_") +
           llvm::utohexstr(Address, true) + "_address";
  };
  std::string Source =
      renderObjCConstantStringHelpers(Image, Strings, SharedFunctions);
  for (const auto &[Address, Object] : Objects)
    if (Object.TheKind != Kind::String) {
      SharedFunctions.insert(Name(Address));
      Source += "\nuintptr_t " + Name(Address) + "(void);\n";
    }
  for (const auto &[Address, Object] : Objects) {
    if (Object.TheKind == Kind::String)
      continue;
    if (Object.TheKind == Kind::ImportedBoolean) {
      const auto Symbol =
          "neverd_imported_boolean_" + llvm::utohexstr(Address, true);
      Source += "\nuintptr_t " + Name(Address) +
                "(void) {\n  extern const unsigned char " + Symbol +
                "[] __asm__(\"" + Object.ImportName +
                "\");\n  return (uintptr_t)" + Symbol + ";\n}\n";
      continue;
    }
    const bool Integer = Object.TheKind == Kind::Integer;
    const bool Dictionary = Object.TheKind == Kind::Dictionary;
    const std::string Class = Integer      ? "NSConstantIntegerNumber"
                              : Dictionary ? "NSConstantDictionary"
                                           : "NSConstantArray";
    const std::string ClassVariable = "neverd_constant_class_" + Class;
    Source += "\nuintptr_t " + Name(Address) +
              "(void) {\n  extern unsigned char " + ClassVariable +
              "[] "
              "__asm__(\"_OBJC_CLASS_$_" +
              Class + "\");\n";
    // Each identity definition can be extracted into a shared translation
    // unit. Keep child declarations with it, independent of traversal order
    // and of which method supplied this shared object.
    std::set<va_t> Children(Object.Elements.begin(), Object.Elements.end());
    Children.insert(Object.Keys.begin(), Object.Keys.end());
    for (va_t Child : Children)
      Source += "  extern uintptr_t " + Name(Child) + "(void);\n";
    if (Integer) {
      Source += "  static struct { const void *isa; const char *encoding; "
                "uint64_t bits; } object = { " +
                ClassVariable + ", \"" + std::string(1, Object.Encoding) +
                "\", UINT64_C(0x" + llvm::utohexstr(Object.Bits, true) +
                ") };\n";
    } else {
      const auto Count = Object.Elements.size();
      const std::string Slots = std::to_string(Count ? Count : 1);
      Source += "  static const void *elements[" + Slots + "];\n";
      if (Dictionary)
        Source += "  static const void *keys[" + Slots + "];\n";
      Source += "  static struct { const void *isa; ";
      if (Dictionary)
        Source += "uintptr_t options; ";
      Source += "uintptr_t count; ";
      if (Dictionary)
        Source += "const void *const *keys; ";
      Source +=
          "const void *const *elements; } object = { " + ClassVariable + ", ";
      if (Dictionary)
        Source += std::to_string(Object.Options) + ", ";
      Source += std::to_string(Count) + ", ";
      if (Dictionary)
        Source += Count ? "keys, " : "0, ";
      Source += Count ? "elements };\n" : "0 };\n";
      // Accessors have no callbacks or allocation. A validated acyclic graph
      // makes recursive initialization finite. Publish all slots together so
      // concurrent callers observe the same complete immutable object.
      Source += "  static unsigned state;\n"
                "  if (__atomic_load_n(&state, __ATOMIC_ACQUIRE) != 2) {\n"
                "    unsigned expected = 0;\n"
                "    if (__atomic_compare_exchange_n(&state, &expected, 1, 0, "
                "__ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE)) {\n";
      auto Initialize = [&](const auto &Children, llvm::StringRef Field) {
        for (size_t I = 0; I < Children.size(); ++I)
          Source += "      " + Field.str() + "[" + std::to_string(I) +
                    "] = (const void *)" + Name(Children[I]) + "();\n";
      };
      Initialize(Object.Keys, "keys");
      Initialize(Object.Elements, "elements");
      Source +=
          "      __atomic_store_n(&state, 2, __ATOMIC_RELEASE);\n"
          "    } else {\n"
          "      while (__atomic_load_n(&state, __ATOMIC_ACQUIRE) != 2) {}\n"
          "    }\n  }\n";
    }
    Source += "  return (uintptr_t)&object;\n}\n";
  }
  return Source;
}
} // namespace neverd::sdk
#endif
