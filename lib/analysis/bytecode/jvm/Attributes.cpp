//===- Attributes.cpp - Bounded JVM attribute readers ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Read each attribute inside its declared extent. Unknown attributes retain
/// their physical locator. Code errors remain local to the owning method.
//===----------------------------------------------------------------------===//

#include "Class.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSwitch.h"

using namespace llvm;
namespace neverd::jvm {
namespace {
void depth(unsigned D, size_t At) {
  if (D > limits::Nesting)
    throw Failure{"budget_exceeded", "attribute_depth",
                  "Attribute nesting limit exceeded", At};
}
unsigned count(Reader &R) {
  unsigned N = R.u2();
  R.J.charge(N, R.at());
  return N;
}
std::string verificationType(Reader &R, const Class &C) {
  unsigned Tag = R.u1();
  static constexpr const char *Kinds[]{
      "top", "I", "F", "D", "J", "null", "uninitializedThis"};
  if (Tag < std::size(Kinds))
    return Kinds[Tag];
  if (Tag == 7) {
    auto &N = C.className(R.u2(), R.at());
    return StringRef(N).starts_with("[") ? N : "L" + N + ";";
  }
  if (Tag == 8)
    return "new@" + std::to_string(R.u2());
  invalid(R.at() - 1, "verification_type",
          "Unknown stack-map verification type");
}
unsigned width(ArrayRef<std::string> V) {
  unsigned Size = 0;
  for (auto &T : V)
    Size += slots(T);
  return Size;
}
void stackMaps(Reader &R, const Class &C, const Member &M, Code &B) {
  auto Locals = methodDescriptor(M.Desc, M.At).Arguments;
  if (!(M.Flags & 8))
    Locals.insert(Locals.begin(), M.Name == "<init>" ? "uninitializedThis"
                                                     : "L" + C.Name + ";");
  for (auto &L : Locals)
    if (verificationKind(L) == 'I')
      L = "I";
  int PC = -1;
  for (unsigned N = count(R); N; --N) {
    unsigned T = R.u1();
    unsigned Delta = T < 64 ? T : T < 128 ? T - 64 : R.u2();
    std::vector<std::string> Stack;
    if ((T >= 64 && T < 128) || T == 247)
      Stack.push_back(verificationType(R, C));
    else if (T >= 248 && T <= 250) {
      if (251 - T > Locals.size())
        invalid(R.at(), "stack_map_chop", "Stack map removes missing locals");
      Locals.resize(Locals.size() - (251 - T));
    } else if (T >= 252 && T <= 254) {
      for (unsigned I = 251; I < T; ++I)
        Locals.push_back(verificationType(R, C));
    } else if (T == 255) {
      Locals.clear();
      for (unsigned I = count(R); I; --I)
        Locals.push_back(verificationType(R, C));
      for (unsigned I = count(R); I; --I)
        Stack.push_back(verificationType(R, C));
    } else if (T >= 128 && T != 251)
      invalid(R.at(), "stack_map_frame", "Reserved stack-map frame type");
    PC += int(Delta) + 1;
    if (unsigned(PC) >= B.Bytes.size() || width(Locals) > B.MaxLocals ||
        width(Stack) > B.MaxStack)
      invalid(R.at(), "stack_map_extent", "Stack map exceeds Code bounds");
    R.J.charge(Locals.size() + Stack.size(), R.at());
    for (auto &T : Locals)
      R.J.text(T.size(), R.at());
    for (auto &T : Stack)
      R.J.text(T.size(), R.at());
    B.Frames.push_back({unsigned(PC), Locals, std::move(Stack)});
  }
}
json::Object annotation(Reader &, Class &, unsigned);
json::Value element(Reader &R, Class &C, unsigned Depth) {
  depth(Depth, R.at());
  R.J.charge(1, R.at());
  char Tag = R.u1();
  switch (Tag) {
  case 'B':
  case 'C':
  case 'I':
  case 'S':
  case 'Z':
    return int64_t(int32_t(C.constant(R.u2(), {3}, R.at()).Bits));
  case 'D':
  case 'F':
  case 'J': {
    unsigned K = Tag == 'D' ? 6 : Tag == 'F' ? 4 : 5;
    const auto &V = C.constant(R.u2(), {K}, R.at());
    return json::Object{{"type", std::string(1, Tag)},
                        {"bits", utohexstr(V.Bits)}};
  }
  case 's': {
    const auto &S = C.constant(R.u2(), {1}, R.at()).Text;
    R.J.text(S.size(), R.at());
    return S;
  }
  case 'c': {
    const auto &Desc = C.text(R.u2(), R.at());
    if (Desc != "V")
      fieldDescriptor(Desc, R.at());
    return json::Object{{"class_descriptor", Desc}};
  }
  case 'e': {
    const auto &T = C.text(R.u2(), R.at());
    const auto &N = C.text(R.u2(), R.at());
    fieldDescriptor(T, R.at());
    return json::Object{{"enum_type", T}, {"constant", N}};
  }
  case '@':
    return annotation(R, C, Depth + 1);
  case '[': {
    json::Array A;
    for (unsigned N = count(R); N; --N)
      A.push_back(element(R, C, Depth + 1));
    return A;
  }
  default:
    invalid(R.at() - 1, "annotation_value", "Unknown annotation value tag");
  }
}
json::Object annotation(Reader &R, Class &C, unsigned Depth) {
  depth(Depth, R.at());
  const auto &Type = C.text(R.u2(), R.at());
  fieldDescriptor(Type, R.at());
  json::Object Values;
  for (unsigned N = count(R); N; --N) {
    const auto &Name = C.text(R.u2(), R.at());
    if (Values.get(Name))
      invalid(R.at(), "annotation_element", "Duplicate annotation element");
    Values[std::string(Name)] = element(R, C, Depth + 1);
  }
  return json::Object{{"type", Type}, {"elements", std::move(Values)}};
}
json::Value target(Reader &R) {
  unsigned T = R.u1();
  json::Object O{{"type", T}};
  if (T == 0 || T == 1 || T == 0x16)
    O["index"] = R.u1();
  else if (T == 0x10 || T == 0x17 || (T >= 0x42 && T <= 0x46))
    O["index"] = R.u2();
  else if (T == 0x11 || T == 0x12) {
    O["parameter"] = R.u1();
    O["bound"] = R.u1();
  } else if (T == 0x40 || T == 0x41) {
    json::Array V;
    for (unsigned N = count(R); N; --N) {
      unsigned P = R.u2(), L = R.u2(), I = R.u2();
      V.emplace_back(json::Object{{"pc", P}, {"length", L}, {"local", I}});
    }
    O["locals"] = std::move(V);
  } else if (T >= 0x47 && T <= 0x4b) {
    O["pc"] = R.u2();
    O["argument"] = R.u1();
  } else if (T < 0x13 || T > 0x15)
    invalid(R.at() - 1, "annotation_target", "Invalid type annotation target");
  json::Array Path;
  for (unsigned N = R.u1(); N; --N) {
    unsigned Kind = R.u1(), Index = R.u1();
    if (Kind > 3 || (Kind != 3 && Index))
      invalid(R.at() - 2, "annotation_path", "Invalid type annotation path");
    Path.emplace_back(json::Array{Kind, Index});
  }
  O["path"] = std::move(Path);
  return O;
}
std::string named(const Class &C, Reader &R, unsigned Tag) {
  auto &K = C.constant(R.u2(), {Tag}, R.at());
  return C.text(K.A, K.At);
}
json::Array names(const Class &C, Reader &R, unsigned Tag) {
  json::Array V;
  for (unsigned N = count(R); N; --N)
    V.emplace_back(named(C, R, Tag));
  return V;
}
json::Value maybeName(Reader &R, const Class &C) {
  unsigned I = R.u2();
  return I ? json::Value(C.text(I, R.at())) : json::Value(nullptr);
}
void module(Reader &R, Class &C) {
  auto &O = C.Module;
  O["name"] = named(C, R, 19);
  O["flags"] = R.u2();
  O["version"] = maybeName(R, C);
  json::Array Required;
  for (unsigned N = count(R); N; --N) {
    auto S = named(C, R, 19);
    unsigned Flags = R.u2();
    Required.emplace_back(json::Object{{"module", std::move(S)},
                                       {"flags", Flags},
                                       {"version", maybeName(R, C)}});
  }
  O["requires"] = std::move(Required);
  for (auto Key : {"exports", "opens"}) {
    json::Array A;
    for (unsigned N = count(R); N; --N) {
      auto P = named(C, R, 20);
      unsigned Flags = R.u2();
      auto To = names(C, R, 19);
      A.emplace_back(json::Object{
          {"package", std::move(P)}, {"flags", Flags}, {"to", std::move(To)}});
    }
    O[Key] = std::move(A);
  }
  O["uses"] = names(C, R, 7);
  json::Array Providers;
  for (unsigned N = count(R); N; --N) {
    auto S = named(C, R, 7);
    auto With = names(C, R, 7);
    if (With.empty())
      invalid(R.at(), "module_provider", "Empty module provider list");
    Providers.emplace_back(
        json::Object{{"service", std::move(S)}, {"with", std::move(With)}});
  }
  O["provides"] = std::move(Providers);
}
void code(Reader &R, Class &C, Member &M, unsigned Depth) {
  auto &B = M.Body.emplace();
  bool Compact = C.Major == 45 && C.Minor <= 2;
  B.MaxStack = Compact ? R.u1() : R.u2();
  B.MaxLocals = Compact ? R.u1() : R.u2();
  unsigned Length = Compact ? R.u2() : R.u4();
  if (!Length || Length > 65535)
    invalid(R.at(), "code_length",
            "Code must contain between one and 65535 bytes");
  B.At = R.at();
  B.Bytes = R.take(Length);
  if (methodDescriptor(M.Desc, M.At).Slots + unsigned(!(M.Flags & 8)) >
      B.MaxLocals)
    invalid(B.At, "argument_locals",
            "Arguments exceed the local variable frame");
  for (unsigned N = count(R); N; --N) {
    Handler H{R.u2(), R.u2(), R.u2(), R.u2()};
    if (H.Start >= H.End || H.End > Length || H.Target >= Length)
      invalid(R.at() - 8, "handler_extent",
              "Exception handler exceeds method code");
    if (H.Catch)
      C.className(H.Catch, R.at() - 2);
    B.Handlers.push_back(H);
  }
  readAttributes(R, C, &M, &B, true, B.Attributes, Depth + 1);
}
bool readAttribute(StringRef Name, Reader &R, Class &C, Member *M, Code *Body,
                   bool Method, json::Object &O, unsigned Depth) {
  bool ClassScope = !M && !Body;
  if (Name == "Code" && Method && M && !Body)
    code(R, C, *M, Depth);
  else if (Name == "StackMapTable" && Body)
    stackMaps(R, C, *M, *Body);
  else if (Name == "LineNumberTable" && Body) {
    for (unsigned N = count(R); N; --N) {
      unsigned PC = R.u2(), Line = R.u2();
      if (PC >= Body->Bytes.size())
        invalid(R.at() - 4, "line_range", "Line table PC exceeds bytecode");
      Body->Lines.emplace_back(PC, Line);
    }
  } else if ((Name == "LocalVariableTable" ||
              Name == "LocalVariableTypeTable") &&
             Body) {
    for (unsigned N = count(R); N; --N) {
      unsigned Start = R.u2(), Length = R.u2();
      const auto &Var = C.text(R.u2(), R.at()), &Desc = C.text(R.u2(), R.at());
      unsigned Slot = R.u2();
      if (Start + Length > Body->Bytes.size() || Start >= Body->Bytes.size() ||
          Slot >= Body->MaxLocals)
        invalid(R.at() - 10, "local_range",
                "Invalid local variable scope or slot");
      if (Name == "LocalVariableTable") {
        fieldDescriptor(Desc, R.at());
        if (Slot + slots(Desc) > Body->MaxLocals)
          invalid(R.at() - 2, "local_range", "Wide local exceeds frame");
      }
      Body->Variables.emplace_back(json::Object{
          {"name", Var},
          {Name == "LocalVariableTable" ? "descriptor" : "signature", Desc},
          {"start_pc", Start},
          {"end_pc", Start + Length},
          {"slot", Slot}});
    }
  } else if (Name == "BootstrapMethods" && ClassScope) {
    for (unsigned N = count(R); N; --N) {
      Bootstrap B{R.u2(), {}};
      auto &H = C.constant(B.Handle, {15}, R.at());
      if (H.A != 6 && H.A != 8)
        invalid(R.at(), "bootstrap_handle",
                "Bootstrap method requires a static or constructor handle");
      for (unsigned I = count(R); I; --I) {
        unsigned K = R.u2();
        C.constant(K, {3, 4, 5, 6, 7, 8, 15, 16, 17}, R.at());
        B.Arguments.push_back(K);
      }
      C.Bootstraps.push_back(std::move(B));
    }
  } else if (Name == "Module" && ClassScope)
    module(R, C);
  else if (Name == "Record" && ClassScope) {
    json::Array A;
    for (unsigned N = count(R); N; --N) {
      Member F;
      F.Name = C.text(R.u2(), R.at());
      F.Desc = C.text(R.u2(), R.at());
      fieldDescriptor(F.Desc, R.at());
      readAttributes(R, C, &F, nullptr, false, F.Attributes, Depth + 1);
      A.emplace_back(json::Object{{"name", F.Name},
                                  {"descriptor", F.Desc},
                                  {"attributes", std::move(F.Attributes)}});
    }
    O["components"] = std::move(A);
  } else if (Name == "ModulePackages" && ClassScope)
    O["packages"] = names(C, R, 20);
  else if ((Name == "NestMembers" || Name == "PermittedSubclasses" ||
            Name == "Exceptions") &&
           !Body)
    O["classes"] = names(C, R, 7);
  else if ((Name == "NestHost" || Name == "ModuleMainClass") && ClassScope)
    O["class"] = named(C, R, 7);
  else if (Name == "Signature" || (Name == "SourceFile" && ClassScope))
    O["value"] = C.text(R.u2(), R.at());
  else if (Name == "ConstantValue" && M && !Method && !Body) {
    unsigned I = R.u2();
    auto &K = C.constant(I, {3, 4, 5, 6, 8}, R.at());
    unsigned Expected = M->Desc == "J"                     ? 5
                        : M->Desc == "F"                   ? 4
                        : M->Desc == "D"                   ? 6
                        : M->Desc == "Ljava/lang/String;"  ? 8
                        : verificationKind(M->Desc) == 'I' ? 3
                                                           : 0;
    if (K.Tag != Expected)
      invalid(R.at() - 2, "constant_value",
              "ConstantValue type differs from field type");
    O["pool_index"] = I;
  } else if (Name == "EnclosingMethod" && ClassScope) {
    O["class"] = named(C, R, 7);
    unsigned I = R.u2();
    if (I)
      C.constant(I, {12}, R.at());
    O["method_index"] = I;
  } else if (Name == "InnerClasses" && ClassScope) {
    json::Array A;
    for (unsigned N = count(R); N; --N) {
      unsigned Inner = R.u2(), Outer = R.u2(), Short = R.u2(), Flags = R.u2();
      auto &Cls = C.className(Inner, R.at());
      A.emplace_back(
          json::Object{{"class", Cls},
                       {"outer", Outer ? json::Value(C.className(Outer, R.at()))
                                       : json::Value(nullptr)},
                       {"inner_name", Short ? json::Value(C.text(Short, R.at()))
                                            : json::Value(nullptr)},
                       {"flags", Flags}});
    }
    O["classes"] = std::move(A);
  } else if (Name == "MethodParameters" && Method && M && !Body) {
    json::Array A;
    for (unsigned N = R.u1(); N; --N) {
      auto Nme = maybeName(R, C);
      unsigned Flags = R.u2();
      A.emplace_back(json::Object{{"name", std::move(Nme)}, {"flags", Flags}});
    }
    O["parameters"] = std::move(A);
  } else if (Name == "AnnotationDefault" && M && Method && !Body)
    O["value"] = element(R, C, Depth + 1);
  else if (Name == "RuntimeVisibleAnnotations" ||
           Name == "RuntimeInvisibleAnnotations") {
    json::Array A;
    for (unsigned N = count(R); N; --N) {
      auto V = annotation(R, C, Depth + 1);
      if (ClassScope && V.getString("type") == "Lkotlin/Metadata;")
        C.Kotlin = *V.getObject("elements");
      A.emplace_back(std::move(V));
    }
    O["annotations"] = std::move(A);
  } else if (Name == "RuntimeVisibleParameterAnnotations" ||
             Name == "RuntimeInvisibleParameterAnnotations") {
    json::Array P;
    for (unsigned I = R.u1(); I; --I) {
      json::Array A;
      for (unsigned N = count(R); N; --N)
        A.emplace_back(annotation(R, C, Depth + 1));
      P.emplace_back(std::move(A));
    }
    O["parameters"] = std::move(P);
  } else if (Name == "RuntimeVisibleTypeAnnotations" ||
             Name == "RuntimeInvisibleTypeAnnotations") {
    json::Array A;
    for (unsigned N = count(R); N; --N) {
      auto Where = target(R);
      auto Ann = annotation(R, C, Depth + 1);
      Ann["target"] = std::move(Where);
      A.emplace_back(std::move(Ann));
    }
    O["annotations"] = std::move(A);
  } else if (Name != "Synthetic" && Name != "Deprecated")
    return false;
  R.end();
  return true;
}
} // namespace
void readAttributes(Reader &R, Class &C, Member *M, Code *Body, bool Method,
                    json::Array &Out, unsigned Depth) {
  depth(Depth, R.at());
  std::set<StringRef> Seen;
  for (unsigned N = count(R); N; --N) {
    size_t At = R.at();
    auto &Name = C.text(R.u2(), At);
    Reader Data = R.slice(R.u4());
    json::Object O{{"name", Name},
                   {"offset", At},
                   {"data_offset", Data.at()},
                   {"length", Data.left()}};
    try {
      auto Minimum =
          StringSwitch<unsigned>(Name)
              .Case("StackMapTable", 50)
              .Case("BootstrapMethods", 51)
              .Cases({"RuntimeVisibleTypeAnnotations",
                      "RuntimeInvisibleTypeAnnotations", "MethodParameters"},
                     52)
              .Cases({"Module", "ModulePackages", "ModuleMainClass"}, 53)
              .Cases({"NestHost", "NestMembers"}, 55)
              .Case("Record", C.Minor == 65535 ? 58 : 60)
              .Case("PermittedSubclasses", C.Minor == 65535 ? 59 : 61)
              .Default(45);
      if (C.Major < Minimum)
        unsupported(At, "attribute_version",
                    "Attribute requires a different class-file version");
      bool Read = readAttribute(Name, Data, C, M, Body, Method, O, Depth);
      if (Read && Name != "LineNumberTable" && Name != "LocalVariableTable" &&
          Name != "LocalVariableTypeTable" && !Seen.insert(Name).second)
        invalid(At, "duplicate_attribute", "Duplicate singleton attribute");
      O["status"] = Read ? "decoded" : "opaque";
    } catch (const Failure &F) {
      if (!M || Body || !Method || StringRef(F.Status) == "cancelled" ||
          StringRef(F.Status) == "budget_exceeded")
        throw;
      if (!M->Error)
        M->Error = F;
      O["status"] = F.Status;
      O["diagnostic"] = diagnostic(F);
    }
    Out.emplace_back(std::move(O));
  }
}
} // namespace neverd::jvm
