// Typed Dalvik verification and standalone Java source generation.
// This is a native translation of NeverD's first-party Dalvik source engine.
#include "MobileDalvik.h"

#include "llvm/Support/JSON.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <initializer_list>
#include <iomanip>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace neverd::mobile::dalvik {
namespace {
using Strings = std::set<std::string>;
using State = std::vector<Strings>;

[[noreturn]] void javaError(const std::string &Message) {
  throw std::runtime_error(Message);
}

bool starts(std::string_view Text, std::string_view Prefix) {
  return Text.starts_with(Prefix);
}
bool anyPrefix(std::string_view Text,
               std::initializer_list<std::string_view> Prefixes) {
  return std::any_of(Prefixes.begin(), Prefixes.end(),
                     [&](auto Prefix) { return starts(Text, Prefix); });
}
std::string join(const std::vector<std::string> &Values,
                 std::string_view Separator) {
  std::string Result;
  for (const auto &Value : Values) {
    if (!Result.empty())
      Result += Separator;
    Result += Value;
  }
  return Result;
}
std::vector<std::string> split(std::string_view Text, char Separator) {
  std::vector<std::string> Result;
  size_t Begin = 0;
  for (;;) {
    size_t End = Text.find(Separator, Begin);
    Result.emplace_back(
        Text.substr(Begin, End == Text.npos ? End : End - Begin));
    if (End == Text.npos)
      return Result;
    Begin = End + 1;
  }
}
std::string baseOpcode(std::string_view Opcode) {
  return std::string(Opcode.substr(0, Opcode.find('/')));
}
const std::map<std::string, std::string> Primitives = {
    {"V", "void"}, {"Z", "boolean"}, {"B", "byte"},
    {"C", "char"}, {"S", "short"},   {"I", "int"},
    {"J", "long"}, {"F", "float"},   {"D", "double"}};
const std::map<std::string, std::string> ArithmeticTypes = {
    {"int", "I"},  {"long", "J"}, {"float", "F"}, {"double", "D"},
    {"byte", "B"}, {"char", "C"}, {"short", "S"}};

std::string javaIdentifier(const std::string &Value) {
  static const Strings Keywords = [] {
    Strings Result;
    std::istringstream Words("abstract assert boolean break byte case catch "
                             "char class const continue default "
                             "do double else enum extends final finally float "
                             "for goto if implements import "
                             "instanceof int interface long native new package "
                             "private protected public return "
                             "short static strictfp super switch synchronized "
                             "this throw throws transient try "
                             "void volatile while true false null _ record "
                             "sealed permits yield var");
    for (std::string Word; Words >> Word;)
      Result.insert(Word);
    return Result;
  }();
  auto First = [](unsigned char C) {
    return (C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') || C == '_' ||
           C == '$';
  };
  if (Value.empty() || !First(Value[0]) || Keywords.contains(Value) ||
      !std::all_of(Value.begin() + 1, Value.end(), [&](unsigned char C) {
        return First(C) || (C >= '0' && C <= '9');
      }))
    javaError("Android identifier cannot be represented in Java: " + Value);
  return Value;
}

std::string hexLiteral(uint64_t Value, unsigned Bits) {
  if (Bits == 32)
    Value &= UINT64_C(0xffffffff);
  std::ostringstream Out;
  Out << "0x" << std::hex << std::setfill('0') << std::setw(Bits / 4) << Value;
  if (Bits == 64)
    Out << 'L';
  return Out.str();
}

// The readers retain lone UTF-16 surrogates as WTF-8. Java source must retain
// those exact code units rather than replacing them through a UTF-8 serializer.
std::string javaString(const std::string &Value) {
  std::string Out = "\"";
  auto Unit = [&](uint32_t U) {
    constexpr char Digits[] = "0123456789abcdef";
    Out += "\\u";
    for (int Shift = 12; Shift >= 0; Shift -= 4)
      Out += Digits[(U >> Shift) & 15];
  };
  for (size_t I = 0; I < Value.size();) {
    uint8_t Byte = static_cast<uint8_t>(Value[I++]);
    uint32_t Code = Byte;
    unsigned Extra = 0;
    uint32_t Minimum = 0;
    if (Byte >= 0xc2 && Byte <= 0xdf) {
      Code = Byte & 31;
      Extra = 1;
      Minimum = 0x80;
    } else if (Byte >= 0xe0 && Byte <= 0xef) {
      Code = Byte & 15;
      Extra = 2;
      Minimum = 0x800;
    } else if (Byte >= 0xf0 && Byte <= 0xf4) {
      Code = Byte & 7;
      Extra = 3;
      Minimum = 0x10000;
    } else if (Byte >= 0x80)
      javaError("invalid UTF-8/WTF-8 Java string");
    if (Extra > Value.size() - I)
      javaError("truncated UTF-8/WTF-8 Java string");
    for (unsigned J = 0; J < Extra; ++J) {
      uint8_t Continuation = static_cast<uint8_t>(Value[I++]);
      if ((Continuation & 0xc0) != 0x80)
        javaError("invalid UTF-8/WTF-8 Java string continuation");
      Code = (Code << 6) | (Continuation & 63);
    }
    if (Code < Minimum || Code > 0x10ffff)
      javaError("invalid UTF-8/WTF-8 Java string value");
    switch (Code) {
    case '"':
      Out += "\\\"";
      break;
    case '\\':
      Out += "\\\\";
      break;
    case '\b':
      Out += "\\b";
      break;
    case '\f':
      Out += "\\f";
      break;
    case '\n':
      Out += "\\n";
      break;
    case '\r':
      Out += "\\r";
      break;
    case '\t':
      Out += "\\t";
      break;
    default:
      if (Code >= 0x20 && Code <= 0x7e)
        Out += static_cast<char>(Code);
      else if (Code <= 0xffff)
        Unit(Code);
      else {
        Code -= 0x10000;
        Unit(0xd800 + (Code >> 10));
        Unit(0xdc00 + (Code & 1023));
      }
    }
  }
  return Out + '"';
}

bool isReference(std::string_view Type) {
  return anyPrefix(Type, {"L", "[", "self:"});
}
bool isWide(std::string_view Type) {
  return Type == "J" || Type == "D" || Type == "bits64";
}
unsigned wordWidth(std::string_view Type) {
  return Type == "J" || Type == "D" ? 2 : 1;
}

class Lines {
  Budget &budget;
  bool cumulative;
  uint64_t bytes = 0;
  std::vector<std::string> values;

public:
  explicit Lines(Budget &B, bool Cumulative = false)
      : budget(B), cumulative(Cumulative) {}
  void append(std::string Value) {
    uint64_t Size = Value.size() + 1;
    if (Size > budget.limits.max_bytes ||
        bytes > budget.limits.max_bytes - Size)
      javaError("Android generated source exceeded its byte budget");
    bytes += Size;
    if (cumulative)
      budget.output(Size);
    values.push_back(std::move(Value));
  }
  void extend(std::initializer_list<std::string> Values) {
    for (const auto &Value : Values)
      append(Value);
  }
  const std::vector<std::string> &get() const { return values; }
  std::string text() const { return join(values, "\n"); }
};

struct JavaScope {
  const Class &owner;
  const MethodRef *method = nullptr;
};

const MethodRef *localScope(const JavaScope &Scope) {
  return Scope.owner.enclosing_method ? &*Scope.owner.enclosing_method
                                      : Scope.method;
}

std::string qualifiedTypeName(const std::string &Type,
                              const ClassMap &Classes) {
  if (starts(Type, "["))
    return qualifiedTypeName(Type.substr(1), Classes) + "[]";
  if (auto I = Primitives.find(Type); I != Primitives.end())
    return I->second;
  if (!starts(Type, "L") || Type.size() < 3 || Type.back() != ';')
    javaError("invalid Java projection type");
  if (auto I = Classes.find(Type);
      I != Classes.end() && I->second.enclosing_method)
    javaError("local-class reference has no qualified source name: " + Type);
  if (auto I = Classes.find(Type); I != Classes.end() && I->second.enclosing) {
    const auto &C = I->second;
    if (!C.inner_name || C.inner_name->empty() ||
        !Classes.contains(*C.enclosing))
      javaError("nested Android type has no available enclosing declaration");
    return qualifiedTypeName(*C.enclosing, Classes) + "." +
           javaIdentifier(*C.inner_name);
  }
  auto Parts = split(std::string_view(Type).substr(1, Type.size() - 2), '/');
  for (auto &Part : Parts)
    Part = javaIdentifier(Part);
  return join(Parts, ".");
}

// A fully qualified Java type can still be obscured by a type whose simple
// name matches the first package component. Resolve names from descriptors
// and lexical type scopes before producing source, never by rewriting text.
class JavaTypeNames {
  const ClassMap &classes;
  Budget &budget;
  std::map<std::string, std::map<std::string, const Class *>> members;
  std::map<MethodRef, std::map<std::string, const Class *>> locals;
  std::map<std::string, std::map<std::string, std::string>> packages;
  struct Lookup {
    Strings types;
    bool unknown_inheritance = false;
  };
  mutable std::map<std::tuple<std::string, std::string, std::string, bool>, Lookup>
      cache;

  static std::string package(const std::string &Type) {
    auto End = Type.rfind('/');
    return End == Type.npos ? "" : Type.substr(1, End - 1);
  }
  static std::string simple(const Class &C) {
    if ((C.enclosing || C.enclosing_method) && C.inner_name)
      return *C.inner_name;
    auto Start = C.name.rfind('/');
    Start = Start == C.name.npos ? 1 : Start + 1;
    return C.name.substr(Start, C.name.size() - Start - 1);
  }
  Lookup memberTypes(const Class &Scope, const std::string &Name) const {
    Lookup Result;
    std::deque<std::string> Pending{Scope.name};
    Strings Seen;
    while (!Pending.empty()) {
      budget.tick();
      auto Owner = std::move(Pending.front());
      Pending.pop_front();
      if (!Seen.insert(Owner).second)
        continue;
      auto C = classes.find(Owner);
      if (C == classes.end()) {
        // Object contributes no member types; other missing declarations
        // cannot prove that a source name has no inherited namesake.
        Result.unknown_inheritance |= Owner != "Ljava/lang/Object;";
        continue;
      }
      auto M = members.find(Owner);
      if (M != members.end()) {
        auto Match = M->second.find(Name);
        if (Match != M->second.end()) {
          const auto &Child = *Match->second;
          const auto &Flags = Child.inner_access;
          bool Inherited = Owner != Scope.name;
          bool Accessible =
              !Inherited ||
              (!Flags.contains("private") &&
               (Flags.contains("public") || Flags.contains("protected") ||
                package(Owner) == package(Scope.name)));
          if (Accessible)
            Result.types.insert(Child.name);
          // A declaration in this class hides any inherited namesake.
          continue;
        }
      }
      if (C->second.superclass)
        Pending.push_back(*C->second.superclass);
      Pending.insert(Pending.end(), C->second.interfaces.begin(),
                     C->second.interfaces.end());
    }
    return Result;
  }
  const Lookup &lookup(const JavaScope &Context, const std::string &Name,
                       bool InSupertypeHeader = false) const {
    const MethodRef *Method = localScope(Context);
    if (Method && !locals.contains(*Method))
      Method = nullptr;
    auto Key = std::tuple{Context.owner.name,
                          Method ? Method->identity() : std::string(), Name,
                          InSupertypeHeader};
    if (auto I = cache.find(Key); I != cache.end())
      return I->second;
    Lookup Result;
    if (Method)
      if (auto I = locals.find(*Method); I != locals.end())
        if (auto L = I->second.find(Name); L != I->second.end()) {
          Result.types.insert(L->second->name);
          return cache.emplace(std::move(Key), std::move(Result)).first->second;
        }
    const Class *Scope = &Context.owner;
    while (Scope) {
      budget.tick();
      // Declared and inherited members are in scope within the class body,
      // not within its own extends/implements clause. Enclosing class bodies
      // still contribute their complete lexical scopes to a nested header.
      auto Local = InSupertypeHeader && Scope == &Context.owner
                       ? Lookup{}
                       : memberTypes(*Scope, Name);
      Result.unknown_inheritance |= Local.unknown_inheritance;
      if (simple(*Scope) == Name)
        Local.types.insert(Scope->name);
      if (!Local.types.empty()) {
        Result.types = std::move(Local.types);
        return cache.emplace(std::move(Key), std::move(Result)).first->second;
      }
      if (Scope->enclosing)
        Scope = &classes.at(*Scope->enclosing);
      else if (Scope->enclosing_method)
        Scope = &classes.at(Scope->enclosing_method->owner);
      else
        Scope = nullptr;
    }
    if (auto P = packages.find(package(Context.owner.name)); P != packages.end())
      if (auto I = P->second.find(Name); I != P->second.end())
        Result.types.insert(I->second);
    return cache.emplace(std::move(Key), std::move(Result)).first->second;
  }

public:
  JavaTypeNames(const ClassMap &Classes, Budget &B)
      : classes(Classes), budget(B) {
    for (const auto &[Name, C] : classes) {
      budget.tick();
      if (C.enclosing_method) {
        if (!locals[*C.enclosing_method].emplace(simple(C), &C).second)
          javaError("ambiguous Java type: duplicate local source name");
      } else if (C.enclosing) {
        if (!members[*C.enclosing].emplace(simple(C), &C).second)
          javaError("ambiguous Java type: duplicate nested source name");
      } else
        packages[package(Name)].emplace(simple(C), Name);
    }
  }
  std::string render(const std::string &Type, const JavaScope &Context,
                     bool InSupertypeHeader = false) const {
    budget.tick();
    if (starts(Type, "["))
      return render(Type.substr(1), Context, InSupertypeHeader) + "[]";
    if (auto I = classes.find(Type);
        I != classes.end() && I->second.enclosing_method) {
      const auto &C = I->second;
      const auto *Method = localScope(Context);
      if (!Method || *Method != *C.enclosing_method || !C.inner_name)
        javaError("local-class reference is outside its exact method scope: " +
                  Type + " in " + Context.owner.name);
      const auto &Binding = lookup(Context, *C.inner_name, InSupertypeHeader);
      if (Binding.unknown_inheritance || Binding.types != Strings{Type})
        javaError("local-class reference has no unique source binding: " + Type);
      return javaIdentifier(*C.inner_name);
    }
    auto Full = qualifiedTypeName(Type, classes);
    if (Primitives.contains(Type))
      return Full;
    auto Top = Type;
    while (classes.contains(Top) && classes.at(Top).enclosing) {
      budget.tick();
      Top = *classes.at(Top).enclosing;
    }
    auto Package = package(Top);
    auto Root = Full.substr(0, Full.find('.'));
    const auto &QualifiedBinding = lookup(Context, Root, InSupertypeHeader);
    if (!QualifiedBinding.unknown_inheritance &&
        (QualifiedBinding.types.empty() ||
         (Package.empty() && QualifiedBinding.types == Strings{Top})))
      return Full;
    if (Package == package(Context.owner.name) && classes.contains(Top)) {
      const auto &Binding =
          lookup(Context, simple(classes.at(Top)), InSupertypeHeader);
      if (!Binding.unknown_inheritance && Binding.types == Strings{Top})
        return Package.empty() ? Full : Full.substr(Package.size() + 1);
    }
    if (QualifiedBinding.unknown_inheritance)
      javaError("unresolved inherited Java type: " + Type + " in " +
                Context.owner.name +
                " (external declarations are required to prove source "
                "name binding)");
    javaError("ambiguous Java type: " + Type + " in " + Context.owner.name +
              " (package qualifier is shadowed and no unique source name "
              "is proven)");
  }
  std::string render(const std::string &Type, const Class &Context,
                     bool InSupertypeHeader = false) const {
    return render(Type, JavaScope{Context}, InSupertypeHeader);
  }
  void requireRuntimePackage(const Class &Context) const {
    const auto &Binding = lookup(JavaScope{Context}, "java");
    if (Binding.unknown_inheritance)
      javaError("unresolved inherited Java type: java in " + Context.name +
                " (external declarations are required to prove runtime "
                "helper name binding)");
    if (!Binding.types.empty())
      javaError("ambiguous Java type: required Java runtime package is "
                "shadowed in " +
                Context.name);
  }
};

template <typename Ref>
auto member(const Ref &Reference, const ClassMap &Classes, Budget &B) -> const
    std::conditional_t<std::is_same_v<Ref, MethodRef>, Method, Field> * {
  if (!Classes.contains(Reference.owner))
    return nullptr;
  std::deque<std::string> Pending{Reference.owner};
  Strings Seen;
  while (!Pending.empty()) {
    B.tick();
    std::string Owner = std::move(Pending.front());
    Pending.pop_front();
    auto I = Classes.find(Owner);
    if (I == Classes.end() || !Seen.insert(Owner).second)
      continue;
    const auto &C = I->second;
    if constexpr (std::is_same_v<Ref, MethodRef>) {
      for (const auto &M : C.methods) {
        const auto &Actual = M.reference;
        if (Actual.name != Reference.name ||
            Actual.parameters != Reference.parameters)
          continue;
        if (Actual.returns != Reference.returns)
          javaError(
              Reference.owner +
              ": member type differs from the exact referenced declaration");
        return &M;
      }
      if (Reference.name == "<init>")
        break;
    } else {
      for (const auto &F : C.fields) {
        const auto &Actual = F.reference;
        if (Actual.name != Reference.name)
          continue;
        if (Actual.type != Reference.type)
          javaError(
              Reference.owner +
              ": member type differs from the exact referenced declaration");
        return &F;
      }
    }
    if (C.superclass)
      Pending.push_back(*C.superclass);
    for (const auto &Interface : C.interfaces)
      Pending.push_back(Interface);
  }
  javaError(Reference.owner +
            ": referenced member has no proven local declaration");
}

std::string helperName(const Class &C, std::string Base) {
  Strings Names;
  for (const auto &M : C.methods)
    Names.insert(M.reference.name);
  while (Names.contains(Base))
    Base += '_';
  return Base;
}
bool constructorThrows(const Method &Input, const ClassMap &Classes,
                       Budget &B) {
  const Method *M = &Input;
  std::set<MethodRef> Seen;
  for (;;) {
    B.tick();
    if (!Seen.insert(M->reference).second)
      javaError("recursive constructor delegation");
    const auto *R = M->instructions.empty()
                        ? nullptr
                        : std::get_if<MethodRef>(&M->instructions[0].reference);
    if (!R || R->name != "<init>")
      return false;
    const auto *Target = member(*R, Classes, B);
    if (!Target)
      return *R != MethodRef{"Ljava/lang/Object;", "<init>", {}, "V"};
    M = Target;
  }
}

bool sameHandlers(const std::vector<Handler> &A,
                  const std::vector<Handler> &B) {
  if (A.size() != B.size())
    return false;
  for (size_t I = 0; I < A.size(); ++I)
    if (A[I].type != B[I].type || A[I].target != B[I].target)
      return false;
  return true;
}

// The shared model validates source shape and reference scope. This index adds
// only lexical Java naming and emission/accounting facts.
struct LocalProjection {
  std::map<MethodRef, std::vector<const Class *>> by_method;
  std::map<std::string, const Class *> by_class;
  std::set<MethodRef> projected_methods;

  [[noreturn]] static void fail(const Class &C, const std::string &Reason) {
    javaError(C.name + " [" + C.source_id + "] enclosing " +
              (C.enclosing_method ? C.enclosing_method->identity() : "unknown") +
              ": local-class source shape: " + Reason);
  }
  static bool scalar(const std::string &Type, bool Void = false) {
    return Primitives.contains(Type) && (Void || Type != "V");
  }
  LocalProjection(const ClassMap &Classes, Budget &B) {
    for (const auto &[Name, C] : Classes) {
      B.tick();
      if (C.enclosing_method)
        by_class.emplace(Name, &C);
    }
    if (by_class.empty())
      return;
    for (const auto &[Name, CP] : by_class) {
      B.tick();
      const auto &C = *CP;
      try {
        javaIdentifier(*C.inner_name);
      } catch (const std::runtime_error &Error) {
        fail(C, Error.what());
      }
      if (*C.inner_name == "java")
        fail(C, "local name shadows the required Java runtime package");
      auto Owner = Classes.find(C.enclosing_method->owner);
      // A local class cannot have the same source name as an enclosing class.
      const Class *Ancestor = &Owner->second;
      Strings Seen;
      while (Ancestor) {
        B.tick();
        if (!Seen.insert(Ancestor->name).second || Seen.size() > 128)
          fail(C, "invalid enclosing class chain");
        auto Start = Ancestor->name.rfind('/');
        Start = Start == std::string::npos ? 1 : Start + 1;
        auto Simple = Ancestor->inner_name.value_or(Ancestor->name.substr(
            Start, Ancestor->name.size() - Start - 1));
        if (*C.inner_name == Simple)
          fail(C, "local source name repeats an enclosing class name");
        if (!Ancestor->enclosing)
          break;
        auto Next = Classes.find(*Ancestor->enclosing);
        if (Next == Classes.end())
          fail(C, "unresolved enclosing class");
        Ancestor = &Next->second;
      }
      for (const auto &Body : C.methods) {
        B.tick();
        projected_methods.insert(Body.reference);
      }
      by_method[*C.enclosing_method].push_back(&C);
      projected_methods.insert(*C.enclosing_method);
    }
  }
};

class Body {
  const Method &method;
  const ClassMap &classes;
  Budget &budget;
  const JavaTypeNames &type_names;
  const LocalProjection &local_projection;
  const MethodRef *local_closure = nullptr;
  const std::vector<Instruction> &code;
  std::map<uint32_t, size_t> by_pc;
  std::map<uint32_t, State> states;
  std::string prefix;
  size_t first = 0;
  std::map<uint32_t, std::string> pending_results;
  std::map<uint32_t, uint32_t> constructors;
  // This domain records origins, independently of the verifier's type sets.
  // Only admitted producers introduce a local or exception object. Joins are
  // bitwise unions, and exceptional edges retain the pre-instruction origins.
  enum Origin : uint8_t { Scalar = 1, Local = 2, Exception = 4, Null = 8 };
  using Origins = std::vector<uint8_t>;
  std::map<uint32_t, Origins> origins;

  [[noreturn]] void fail(const std::string &Message) const {
    if (local_closure) {
      auto I = classes.find(method.reference.owner);
      if (I != classes.end())
        javaError(I->second.name + " [" + I->second.source_id +
                  "] enclosing " + local_closure->identity() + ": " +
                  method.reference.identity() + ": " + Message);
    }
    javaError(method.reference.identity() + ": " + Message);
  }
  const Class &ownerClass() const {
    auto I = classes.find(method.reference.owner);
    if (I == classes.end())
      fail("method owner has no local declaration");
    return I->second;
  }
  std::string sourceType(const std::string &Type) const {
    return type_names.render(Type, JavaScope{ownerClass(), &method.reference});
  }
  const std::string &referenceType(const Instruction &Op) const {
    if (auto *T = std::get_if<std::string>(&Op.reference))
      return *T;
    fail("instruction has no type descriptor");
  }
  uint32_t target(const Instruction &Op) const {
    if (!Op.target)
      fail("branch has no destination");
    return *Op.target;
  }
  const std::vector<Handler> &handlers(uint32_t PC) const {
    for (const auto &R : method.tries)
      if (R.start <= PC && PC < R.end)
        return R.handlers;
    static const std::vector<Handler> Empty;
    return Empty;
  }
  void validateShape();
  const Method *validateInvocation(const Instruction &Op);
  std::vector<uint32_t> successors(size_t Index) const;
  bool compatible(std::string Kind, const std::string &Wanted) const;
  std::string read(const State &S, unsigned Reg, const std::string &Type,
                   bool Strict) const;
  std::string write(State &S, unsigned Reg, const std::string &Type,
                    const std::string &Expression) const;
  std::string staticOwner(const std::string &Owner, bool Field = false);
  std::string arrayType(const State &S, unsigned Reg, bool Strict) const;
  std::pair<State, std::vector<std::string>>
  operation(const Instruction &Op, const State &Incoming, bool Strict);
  std::string condition(const Instruction &Op, const State &S,
                        bool Strict) const;
  State initial() const;
  void validateLocalOperations() const;
  Origins localOrigins(const Instruction &Op, const Origins &Incoming,
                       bool Strict) const;
  void verify();
  static std::string resultRead(const std::string &Type);
  static std::string resultWrite(const std::string &Type,
                                 const std::string &Value);
  std::string throwHelper() const {
    return helperName(ownerClass(), "__neverdThrow");
  }

public:
  Body(const Method &M, const ClassMap &C, Budget &B,
       const JavaTypeNames &TypeNames, const LocalProjection &Projection)
      : method(M), classes(C), budget(B), type_names(TypeNames),
        local_projection(Projection), code(M.instructions) {
    if (auto I = Projection.by_class.find(M.reference.owner);
        I != Projection.by_class.end())
      local_closure = &*I->second->enclosing_method;
    else if (Projection.by_method.contains(M.reference))
      local_closure = &M.reference;
    for (size_t I = 0; I < code.size(); ++I)
      by_pc.emplace(code[I].pc, I);
    validateShape();
    if (local_closure)
      validateLocalOperations();
  }
  std::string emit();
};

void Body::validateShape() {
  if (code.empty() || by_pc.size() != code.size())
    fail("missing or duplicate instruction positions");
  for (size_t I = 1; I < code.size(); ++I)
    if (code[I - 1].pc >= code[I].pc)
      fail("invalid method code boundaries");
  if (method.code_end <= code.back().pc)
    fail("invalid method code boundaries");
  if (method.registers > 1024)
    fail("register frame exceeds the bounded Java projection limit");
  if (method.incomingWords() > method.registers)
    fail("incoming arguments exceed the register frame");
  for (const auto &Op : code) {
    budget.tick();
    for (unsigned R : Op.registers)
      if (R >= method.registers)
        fail("instruction register is outside its frame");
    std::vector<uint32_t> Targets;
    if (Op.opcode.find("switch") != std::string::npos)
      Targets = Op.targets;
    else if (anyPrefix(Op.opcode, {"goto", "if-"}))
      Targets.push_back(target(Op));
    for (uint32_t T : Targets)
      if (!by_pc.contains(T))
        fail("branch does not target an executable instruction");
    if (Op.opcode.find("switch") != std::string::npos &&
        (Op.keys.size() != Op.targets.size() ||
         std::set<int32_t>(Op.keys.begin(), Op.keys.end()).size() !=
             Op.keys.size()))
      fail("switch keys and destinations disagree");
  }
  uint32_t PreviousEnd = 0;
  for (const auto &R : method.tries) {
    if (!by_pc.contains(R.start) ||
        (!by_pc.contains(R.end) && R.end != method.code_end) ||
        R.start >= R.end || R.start < PreviousEnd || R.handlers.empty())
      fail("invalid or overlapping exception regions");
    PreviousEnd = R.end;
    for (size_t I = 0; I < R.handlers.size(); ++I) {
      const auto &H = R.handlers[I];
      if (!by_pc.contains(H.target) ||
          code[by_pc.at(H.target)].opcode != "move-exception")
        fail("exception handler must start with move-exception");
      if (!H.type && I + 1 != R.handlers.size())
        fail("catch-all must be the final exception handler");
    }
  }
  for (size_t I = 0; I < code.size(); ++I) {
    const auto &Op = code[I];
    if (!starts(Op.opcode, "move-result"))
      continue;
    if (!I || !anyPrefix(code[I - 1].opcode, {"invoke-", "filled-new-array"}))
      fail("move-result does not immediately follow its producer");
    std::string Type;
    if (auto *M = std::get_if<MethodRef>(&code[I - 1].reference))
      Type = M->returns;
    else if (auto *T = std::get_if<std::string>(&code[I - 1].reference))
      Type = *T;
    if (Type.empty() || Type == "V")
      fail("move-result has no value-producing signature");
    std::string Wanted = isReference(Type)      ? "move-result-object"
                         : wordWidth(Type) == 2 ? "move-result-wide"
                                                : "move-result";
    if (Op.opcode != Wanted)
      fail("move-result carrier disagrees with the native prototype");
    pending_results[Op.pc] = Type;
  }
  if (method.reference.name != "<init>")
    return;
  const auto &Head = code.front();
  const auto *Ref = std::get_if<MethodRef>(&Head.reference);
  unsigned SelfReg = method.registers - method.incomingWords();
  if ((Head.opcode != "invoke-direct" &&
       Head.opcode != "invoke-direct/range") ||
      !Ref || Ref->name != "<init>" || Head.registers.empty() ||
      Head.registers[0] != SelfReg)
    fail("constructor requires a representable leading super/this call");
  const auto &C = ownerClass();
  if (Ref->owner != C.name && (!C.superclass || Ref->owner != *C.superclass))
    fail("constructor receiver does not match its owner or superclass");
  validateInvocation(Head);
  std::map<unsigned, std::pair<std::string, std::string>> Original;
  unsigned Reg = SelfReg + 1;
  for (size_t I = 0; I < method.reference.parameters.size(); ++I) {
    const auto &T = method.reference.parameters[I];
    Original[Reg] = {T, "arg" + std::to_string(I)};
    Reg += wordWidth(T);
  }
  std::vector<std::string> Args;
  size_t Cursor = 1;
  for (const auto &T : Ref->parameters) {
    if (Cursor >= Head.registers.size() ||
        !Original.contains(Head.registers[Cursor]))
      fail("constructor prefix requires unavailable argument evaluation");
    const auto &[Actual, Name] = Original.at(Head.registers[Cursor]);
    if (Actual != T)
      fail("constructor prefix argument types disagree");
    Args.push_back(Name);
    if (wordWidth(T) == 2 &&
        (Cursor + 1 >= Head.registers.size() ||
         Head.registers[Cursor + 1] != Head.registers[Cursor] + 1))
      fail("constructor prefix has a broken wide register pair");
    Cursor += wordWidth(T);
  }
  if (Cursor != Head.registers.size())
    fail("constructor prefix argument count disagrees");
  prefix = std::string(Ref->owner == C.name ? "this" : "super") + "(" +
           join(Args, ", ") + ");";
  first = 1;
  if (first == code.size())
    fail("constructor has no return boundary");
  if (!method.tries.empty() && method.tries[0].start == Head.pc)
    fail("constructor prefix has unrepresentable exception handling");
}

std::vector<uint32_t> Body::successors(size_t Index) const {
  const auto &Op = code[Index];
  if (anyPrefix(Op.opcode, {"return", "throw"}))
    return {};
  if (starts(Op.opcode, "goto"))
    return {target(Op)};
  if (Index + 1 == code.size())
    fail("method can fall through its code boundary");
  std::vector<uint32_t> Result{code[Index + 1].pc};
  if (starts(Op.opcode, "if-"))
    Result.push_back(target(Op));
  else if (Op.opcode.find("switch") != std::string::npos)
    Result.insert(Result.end(), Op.targets.begin(), Op.targets.end());
  std::set<uint32_t> Seen;
  std::erase_if(Result, [&](uint32_t P) { return !Seen.insert(P).second; });
  return Result;
}

bool Body::compatible(std::string Kind, const std::string &Wanted) const {
  if (starts(Kind, "self:"))
    Kind = Kind.substr(5);
  if (Kind == "?")
    return false;
  if (Wanted == "I")
    return Strings{"I", "Z", "B", "S", "C", "bits32", "zero"}.contains(Kind);
  if (Strings{"Z", "B", "S", "C"}.contains(Wanted))
    return compatible(Kind, "I");
  if (Wanted == "F")
    return Kind == "F" || Kind == "bits32" || Kind == "zero";
  if (Wanted == "J" || Wanted == "D")
    return Kind == Wanted || Kind == "bits64";
  if (isReference(Wanted)) {
    if (Kind == "zero" || Kind == "null")
      return true;
    if (!isReference(Kind))
      return false;
    if (Wanted == "Ljava/lang/Object;" || Wanted == Kind)
      return true;
    Strings Seen;
    for (;;) {
      auto I = classes.find(Kind);
      if (I == classes.end() || !Seen.insert(Kind).second)
        break;
      const auto &C = I->second;
      if (std::find(C.interfaces.begin(), C.interfaces.end(), Wanted) !=
          C.interfaces.end())
        return true;
      if (!C.superclass)
        break;
      Kind = *C.superclass;
      if (Kind == Wanted)
        return true;
    }
    return false;
  }
  return Kind == Wanted;
}

std::string Body::read(const State &S, unsigned Reg, const std::string &Type,
                       bool Strict) const {
  if (Reg >= S.size())
    fail("instruction register is outside its frame");
  const auto &Kinds = S[Reg];
  if (Strict &&
      (!std::all_of(Kinds.begin(), Kinds.end(),
                    [&](const auto &K) { return compatible(K, Type); }) ||
       (wordWidth(Type) == 2 &&
        (Reg + 1 >= S.size() || S[Reg + 1] != Strings{"wide-high"}))))
    fail("undefined or incompatible register v" + std::to_string(Reg) +
         " for " + Type);
  auto R = std::to_string(Reg);
  if (isReference(Type))
    return "((" + sourceType(Type) + ") o" + R + ")";
  if (Type == "F")
    return "((java.lang.Float) null).intBitsToFloat(v" + R + ")";
  if (Type == "D")
    return "((java.lang.Double) null).longBitsToDouble(w" + R + ")";
  if (Type == "J")
    return "w" + R;
  if (Type == "Z")
    return "(v" + R + " != 0)";
  if (Type == "B" || Type == "S" || Type == "C")
    return "((" + Primitives.at(Type) + ") v" + R + ")";
  return "v" + R;
}

std::string Body::write(State &S, unsigned Reg, const std::string &Type,
                        const std::string &Expression) const {
  unsigned Size = wordWidth(Type) == 2 || Type == "bits64" ? 2 : 1;
  if (Reg >= S.size() || Size > S.size() - Reg)
    fail("wide result exceeds its register frame");
  std::vector<unsigned> Pairs;
  for (unsigned Low = Reg ? Reg - 1 : 0;
       Low < std::min<size_t>(S.size() - 1, Reg + Size); ++Low)
    if (std::any_of(S[Low].begin(), S[Low].end(),
                    [](const auto &K) { return isWide(K); }))
      Pairs.push_back(Low);
  for (unsigned Low : Pairs)
    S[Low] = S[Low + 1] = Strings{"?"};
  if (Size == 2)
    S[Reg + 1] = Strings{"wide-high"};
  S[Reg] = Strings{Type};
  auto R = std::to_string(Reg);
  if (isReference(Type) || starts(Type, "new:") || Type == "null")
    return "o" + R + " = " + Expression + ";";
  if (Type == "F")
    return "v" + R + " = ((java.lang.Float) null).floatToRawIntBits(" +
           Expression + ");";
  if (Type == "D")
    return "w" + R + " = ((java.lang.Double) null).doubleToRawLongBits(" +
           Expression + ");";
  if (Type == "J" || Type == "bits64")
    return "w" + R + " = " + Expression + ";";
  if (Type == "Z")
    return "v" + R + " = (" + Expression + ") ? 1 : 0;";
  if (Type == "zero")
    return "v" + R + " = 0; o" + R + " = null;";
  return "v" + R + " = " + Expression + ";";
}

const Method *Body::validateInvocation(const Instruction &Op) {
  const auto *Ref = std::get_if<MethodRef>(&Op.reference);
  if (!Ref)
    fail("invocation has no method identity");
  std::string Style = baseOpcode(Op.opcode);
  if (!Strings{"invoke-static", "invoke-virtual", "invoke-interface",
               "invoke-direct", "invoke-super"}
           .contains(Style))
    fail("unsupported invocation dispatch");
  if (Ref->name == "<init>" &&
      (Style != "invoke-direct" || Ref->returns != "V"))
    fail("constructor has invalid dispatch or return type");
  const Method *Declaration = member(*Ref, classes, budget);
  if (Declaration) {
    bool Static = Declaration->access.contains("static");
    if (Static != (Style == "invoke-static"))
      fail("invocation dispatch disagrees with the declared static/instance "
           "kind");
    bool Private = Declaration->access.contains("private");
    if (Style == "invoke-direct" && Ref->name != "<init>" && !Private)
      fail("direct invocation requires a private method or constructor");
    if (Private && Style != "invoke-direct" && Style != "invoke-static")
      fail("virtual invocation cannot dispatch to a private method");
    bool Interface = classes.at(Ref->owner).access.contains("interface");
    if ((Style == "invoke-interface" || Style == "invoke-virtual") &&
        Interface != (Style == "invoke-interface"))
      fail("invocation dispatch disagrees with the declared class/interface "
           "kind");
  }
  if (Style == "invoke-super" &&
      (!ownerClass().superclass || Ref->owner != *ownerClass().superclass))
    fail("super invocation cannot be proven to bind the immediate superclass");
  return Declaration;
}

std::string Body::staticOwner(const std::string &Owner, bool Field) {
  std::string Qualified = sourceType(Owner);
  auto I = classes.find(Owner);
  if (Field || (I != classes.end() && !I->second.access.contains("interface")))
    return "((" + Qualified + ") null)";
  std::string Root = Qualified.substr(0, Qualified.find('.'));
  Strings Names{"pc",       "caught",   "failure",
                "result32", "result64", "resultObject"};
  for (size_t N = 0; N < method.reference.parameters.size(); ++N)
    Names.insert("arg" + std::to_string(N));
  for (unsigned N = 0; N < method.registers; ++N)
    for (const auto *P : {"v", "w", "o"})
      Names.insert(P + std::to_string(N));
  std::string Current = method.reference.owner;
  Strings Seen;
  for (;;) {
    auto C = classes.find(Current);
    if (C == classes.end() || !Seen.insert(Current).second)
      break;
    budget.tick();
    for (const auto &F : C->second.fields)
      Names.insert(F.reference.name);
    if (!C->second.superclass)
      break;
    Current = *C->second.superclass;
  }
  if (Names.contains(Root))
    fail("static method type qualifier is shadowed without a proven class "
         "binding");
  return Qualified;
}

std::string Body::arrayType(const State &S, unsigned Reg, bool Strict) const {
  Strings Kinds = S.at(Reg);
  for (const auto *K : {"zero", "null", "?"})
    Kinds.erase(K);
  if (Kinds.size() == 1 && starts(*Kinds.begin(), "["))
    return *Kinds.begin();
  if (Strict)
    fail("array operation lacks one proven element type");
  return "[I";
}

std::pair<State, std::vector<std::string>>
Body::operation(const Instruction &Op, const State &Incoming, bool Strict) {
  budget.tick();
  State S = Incoming;
  Lines Out(budget);
  const auto &Name = Op.opcode;
  const auto &Regs = Op.registers;
  const auto Base = baseOpcode(Name);
  auto arity = [&](size_t N) {
    if (Regs.size() != N)
      fail("wrong register count for " + Name);
  };
  auto get = [&](unsigned R, const std::string &T) {
    return read(Incoming, R, T, Strict);
  };
  auto put = [&](unsigned R, const std::string &T, const std::string &V) {
    Out.append(write(S, R, T, V));
  };
  if (Name == "nop") {
    arity(0);
  } else if (Base == "move" || Base == "move-object" || Base == "move-wide") {
    arity(2);
    const auto &Kinds = Incoming[Regs[1]];
    if (Base == "move-object") {
      if (Strict && !std::all_of(Kinds.begin(), Kinds.end(), [](const auto &K) {
            return isReference(K) || K == "zero" || K == "null" ||
                   starts(K, "new:");
          }))
        fail("move-object reads an undefined or scalar carrier");
      put(Regs[0], "Ljava/lang/Object;", "o" + std::to_string(Regs[1]));
    } else if (Base == "move-wide") {
      if (Strict && (!std::all_of(Kinds.begin(), Kinds.end(),
                                  [](const auto &K) { return isWide(K); }) ||
                     Regs[1] + 1 >= Incoming.size() ||
                     Incoming[Regs[1] + 1] != Strings{"wide-high"}))
        fail("move-wide reads an undefined or broken wide carrier");
      put(Regs[0], "bits64", "w" + std::to_string(Regs[1]));
    } else {
      static const Strings Allowed{"I", "B", "C",      "S",
                                   "Z", "F", "bits32", "zero"};
      if (Strict &&
          !std::all_of(Kinds.begin(), Kinds.end(),
                       [&](const auto &K) { return Allowed.contains(K); }))
        fail("move reads an undefined or incompatible carrier");
      put(Regs[0], "bits32", "v" + std::to_string(Regs[1]));
      if (Kinds.contains("zero"))
        Out.append("o" + std::to_string(Regs[0]) + " = o" +
                   std::to_string(Regs[1]) + ";");
    }
    S[Regs[0]] = Kinds;
  } else if (starts(Name, "move-result")) {
    arity(1);
    const auto &T = pending_results.at(Op.pc);
    put(Regs[0], T, resultRead(T));
  } else if (Name == "move-exception") {
    arity(1);
    Strings Catches;
    for (const auto &R : method.tries)
      for (const auto &H : R.handlers)
        if (H.target == Op.pc)
          Catches.insert(H.type.value_or("Ljava/lang/Throwable;"));
    if (Catches.empty())
      fail("move-exception is outside a handler entry");
    put(Regs[0], "Ljava/lang/Throwable;", "caught");
    S[Regs[0]] = std::move(Catches);
  } else if (starts(Name, "const-string")) {
    arity(1);
    const auto *V = std::get_if<std::string>(&Op.literal);
    if (!V)
      fail("const-string lacks its decoded string");
    put(Regs[0], "Ljava/lang/String;", javaString(*V));
  } else if (Name == "const-class") {
    arity(1);
    put(Regs[0], "Ljava/lang/Class;", sourceType(referenceType(Op)) + ".class");
  } else if (starts(Name, "const")) {
    arity(1);
    const auto *V = std::get_if<int64_t>(&Op.literal);
    if (!V)
      fail("constant has no exact integer bits");
    bool Wide = starts(Name, "const-wide");
    put(Regs[0],
        Wide      ? "bits64"
        : *V == 0 ? "zero"
                  : "bits32",
        hexLiteral(static_cast<uint64_t>(*V), Wide ? 64 : 32));
  } else if (starts(Base, "return")) {
    const auto &T = method.reference.returns;
    if (Name == "return-void") {
      arity(0);
      if (T != "V")
        fail("void return disagrees with method signature");
      Out.append(method.reference.name == "<clinit>" ? "break dispatch;"
                                                     : "return;");
    } else {
      arity(1);
      std::string Expected = isReference(T)      ? "return-object"
                             : wordWidth(T) == 2 ? "return-wide"
                                                 : "return";
      if (T == "V" || Name != Expected)
        fail("return carrier disagrees with method signature");
      Out.append("return " + get(Regs[0], T) + ";");
    }
  } else if (Name == "array-length") {
    arity(2);
    std::string T = arrayType(Incoming, Regs[1], Strict);
    put(Regs[0], "I", get(Regs[1], T) + ".length");
  } else if (Name == "new-array") {
    arity(2);
    const auto &T = referenceType(Op);
    if (!starts(T, "["))
      fail("new-array has no array type");
    size_t Dimensions = T.find_first_not_of('[');
    if (Dimensions == std::string::npos)
      fail("new-array has no element type");
    std::string Expr = "new " + sourceType(T.substr(Dimensions)) + "[" +
                       get(Regs[1], "I") + "]";
    for (size_t I = 1; I < Dimensions; ++I)
      Expr += "[]";
    put(Regs[0], T, Expr);
  } else if (Name == "filled-new-array" || Name == "filled-new-array/range") {
    const auto &T = referenceType(Op);
    if (!starts(T, "[") || T.substr(1) == "J" || T.substr(1) == "D")
      fail("filled-new-array requires single-word elements");
    std::vector<std::string> Values;
    for (unsigned R : Regs)
      Values.push_back(get(R, T.substr(1)));
    Out.append("resultObject = new " + sourceType(T) + " {" +
               join(Values, ", ") + "};");
  } else if (Name == "new-instance") {
    arity(1);
    const auto &T = referenceType(Op);
    if (!starts(T, "L"))
      fail("new-instance requires a class descriptor");
    size_t Index = by_pc.at(Op.pc);
    if (Index + 1 >= code.size())
      fail("new-instance has no initializing invocation");
    const auto &Next = code[Index + 1];
    const auto *R = std::get_if<MethodRef>(&Next.reference);
    if ((Next.opcode != "invoke-direct" &&
         Next.opcode != "invoke-direct/range") ||
        !R || R->name != "<init>" || Next.registers.empty() ||
        Next.registers[0] != Regs[0] || R->owner != T ||
        !sameHandlers(handlers(Op.pc), handlers(Next.pc)))
      fail("allocation requires an adjacent initializer in the same exception "
           "region");
    write(S, Regs[0], "new:" + std::to_string(Op.pc) + ":" + T, "null");
    constructors[Next.pc] = Op.pc;
  } else if (Name == "fill-array-data") {
    arity(1);
    const auto T = arrayType(Incoming, Regs[0], Strict);
    const auto Element = T.substr(1);
    static const std::map<std::string, unsigned> Widths{
        {"Z", 1}, {"B", 1}, {"C", 2}, {"S", 2},
        {"I", 4}, {"F", 4}, {"J", 8}, {"D", 8}};
    auto W = Widths.find(Element);
    if (W == Widths.end() || Op.element_width != W->second)
      fail("array payload element width disagrees with its array type");
    const auto Address = get(Regs[0], T);
    Out.append("if (" + Address + ".length < " +
               std::to_string(Op.data.size()) +
               ") throw new java.lang.ArrayIndexOutOfBoundsException();");
    for (size_t I = 0; I < Op.data.size(); ++I) {
      budget.tick();
      uint64_t V = Op.data[I];
      std::string Expr;
      if (Element == "F")
        Expr = "((java.lang.Float) null).intBitsToFloat(" + hexLiteral(V, 32) +
               ")";
      else if (Element == "D")
        Expr = "((java.lang.Double) null).longBitsToDouble(" +
               hexLiteral(V, 64) + ")";
      else if (Element == "Z") {
        if (V > 1)
          fail("boolean array payload contains a non-boolean value");
        Expr = V ? "true" : "false";
      } else
        Expr = "(" + Primitives.at(Element) + ") " +
               hexLiteral(V, Element == "J" ? 64 : 32);
      Out.append(Address + "[" + std::to_string(I) + "] = " + Expr + ";");
    }
  } else if (anyPrefix(Name, {"aget", "aput"})) {
    arity(3);
    const auto T = arrayType(Incoming, Regs[1], Strict);
    const auto Element = T.substr(1);
    std::string Suffix;
    if (isReference(Element))
      Suffix = "-object";
    else if (Element == "J" || Element == "D")
      Suffix = "-wide";
    else if (Element == "Z")
      Suffix = "-boolean";
    else if (Element == "B")
      Suffix = "-byte";
    else if (Element == "C")
      Suffix = "-char";
    else if (Element == "S")
      Suffix = "-short";
    if (Strict && Name != Name.substr(0, 4) + Suffix)
      fail("array opcode disagrees with its element width/type");
    const auto Address = get(Regs[1], T) + "[" + get(Regs[2], "I") + "]";
    if (starts(Name, "aget"))
      put(Regs[0], Element, Address);
    else
      Out.append(Address + " = " + get(Regs[0], Element) + ";");
  } else if (Name == "check-cast") {
    arity(1);
    get(Regs[0], "Ljava/lang/Object;");
    const auto &T = referenceType(Op);
    put(Regs[0], T,
        "((" + sourceType(T) + ") o" + std::to_string(Regs[0]) + ")");
  } else if (Name == "instance-of") {
    arity(2);
    put(Regs[0], "Z",
        get(Regs[1], "Ljava/lang/Object;") + " instanceof " +
            sourceType(referenceType(Op)));
  } else if (anyPrefix(Name, {"iget", "iput", "sget", "sput"})) {
    const auto *R = std::get_if<FieldRef>(&Op.reference);
    if (!R)
      fail("field instruction has no field reference");
    bool Static = starts(Name, "s");
    arity(Static ? 1 : 2);
    std::string Suffix;
    if (isReference(R->type))
      Suffix = "-object";
    else if (wordWidth(R->type) == 2)
      Suffix = "-wide";
    else if (R->type == "Z")
      Suffix = "-boolean";
    else if (R->type == "B")
      Suffix = "-byte";
    else if (R->type == "C")
      Suffix = "-char";
    else if (R->type == "S")
      Suffix = "-short";
    if (Name.substr(4) != Suffix)
      fail("field opcode disagrees with its type");
    const auto *Declaration = member(*R, classes, budget);
    if (Declaration && Static != Declaration->access.contains("static"))
      fail("field opcode disagrees with the declared static/instance kind");
    const auto Address =
        (Static ? staticOwner(R->owner, true) : get(Regs[1], R->owner)) + "." +
        javaIdentifier(R->name);
    if (Name.substr(1, 3) == "get")
      put(Regs[0], R->type, Address);
    else {
      if (Declaration && Declaration->access.contains("final"))
        fail("final field assignment requires a structured initialization "
             "proof");
      Out.append(Address + " = " + get(Regs[0], R->type) + ";");
    }
  } else if (starts(Name, "invoke-")) {
    const auto *R = std::get_if<MethodRef>(&Op.reference);
    if (!R || (starts(R->name, "<") && R->name != "<init>"))
      fail("unmodelled constructor or dynamic invocation");
    validateInvocation(Op);
    bool Static = starts(Name, "invoke-static");
    size_t Cursor = Static ? 0 : 1;
    if (!Static && Regs.empty())
      fail("instance invocation lacks its receiver");
    std::vector<std::string> Args;
    for (const auto &T : R->parameters) {
      if (Cursor >= Regs.size())
        fail("invocation has too few argument words");
      Args.push_back(get(Regs[Cursor], T));
      if (wordWidth(T) == 2 &&
          (Cursor + 1 >= Regs.size() || Regs[Cursor + 1] != Regs[Cursor] + 1))
        fail("invocation has a broken wide argument");
      Cursor += wordWidth(T);
    }
    if (Cursor != Regs.size())
      fail("invocation has too many argument words");
    if (R->name == "<init>") {
      auto N = constructors.find(Op.pc);
      if (N == constructors.end())
        fail("initializer is not bound to its uninitialized allocation");
      std::string Expected =
          "new:" + std::to_string(N->second) + ":" + R->owner;
      if (Incoming[Regs[0]] != Strings{Expected} || R->returns != "V")
        fail("initializer is not bound to its uninitialized allocation");
      put(Regs[0], R->owner,
          "new " + sourceType(R->owner) + "(" + join(Args, ", ") + ")");
      return {std::move(S), Out.get()};
    }
    std::string Owner;
    if (starts(Name, "invoke-super")) {
      if (method.access.contains("static") ||
          Incoming[Regs[0]] != Strings{"self:" + method.reference.owner})
        fail("super invocation lacks its actual receiver");
      Owner = "super";
    } else
      Owner = Static ? staticOwner(R->owner) : get(Regs[0], R->owner);
    std::string Call =
        Owner + "." + javaIdentifier(R->name) + "(" + join(Args, ", ") + ")";
    Out.append(R->returns == "V" ? Call + ";" : resultWrite(R->returns, Call));
  } else if (anyPrefix(Name, {"goto", "if-"}) ||
             Name.find("switch") != std::string::npos) {
    if (starts(Name, "if-"))
      condition(Op, Incoming, Strict);
    else if (Name.find("switch") != std::string::npos) {
      arity(1);
      get(Regs[0], "I");
    }
  } else if (Name == "throw") {
    arity(1);
    std::string Value = get(Regs[0], "Ljava/lang/Object;");
    Out.append("throw " + throwHelper() + "((java.lang.Throwable) " + Value +
               ");");
  } else {
    static const std::regex Unary("(neg|not)-(int|long|float|double)");
    static const std::regex Conversion(
        "(int|long|float|double)-to-(int|long|float|double|byte|char|short)");
    static const std::regex Arithmetic(
        "(add|sub|rsub|mul|div|rem|and|or|xor|shl|shr|ushr)-(int|long|float|"
        "double)(/2addr|/lit8|/lit16)?");
    std::smatch Match;
    if (std::regex_match(Name, Match, Unary)) {
      arity(2);
      std::string Type = ArithmeticTypes.at(Match[2].str());
      bool Not = Match[1] == "not";
      if (Not && Type != "I" && Type != "J")
        fail("invalid floating bitwise negation");
      put(Regs[0], Type,
          std::string(Not ? "~" : "-") + "(" + get(Regs[1], Type) + ")");
    } else if (std::regex_match(Name, Match, Conversion)) {
      arity(2);
      const auto &Source = ArithmeticTypes.at(Match[1].str());
      const auto &Dest = ArithmeticTypes.at(Match[2].str());
      put(Regs[0], Dest,
          "((" + Primitives.at(Dest) + ") (" + get(Regs[1], Source) + "))");
    } else if (anyPrefix(Name, {"cmp-long", "cmpl-", "cmpg-"})) {
      arity(3);
      std::string Type = Name == "cmp-long"        ? "J"
                         : Name.ends_with("float") ? "F"
                                                   : "D";
      auto A = get(Regs[1], Type), B = get(Regs[2], Type);
      auto Expr = "((" + A + ") > (" + B + ") ? 1 : (" + A + ") == (" + B +
                  ") ? 0 : -1)";
      if (starts(Name, "cmpg"))
        Expr = "((" + A + ") < (" + B + ") ? -1 : (" + A + ") == (" + B +
               ") ? 0 : 1)";
      put(Regs[0], "I", Expr);
    } else {
      if (!std::regex_match(Name, Match, Arithmetic))
        fail("unsupported instruction: " + Name);
      std::string Operator = Match[1].str(),
                  Type = ArithmeticTypes.at(Match[2].str()),
                  Form = Match[3].str();
      if (Operator == "rsub" && Form.empty() &&
          !std::holds_alternative<std::monostate>(Op.literal))
        Form = "/lit16";
      if (Strings{"and", "or", "xor", "shl", "shr", "ushr", "rsub"}.contains(
              Operator) &&
          Type != "I" && Type != "J")
        fail("invalid arithmetic carrier");
      arity(Form.empty() ? 3 : 2);
      std::string Left = get(Form == "/2addr" ? Regs[0] : Regs[1], Type), Right;
      if (Form == "/lit8" || Form == "/lit16") {
        const auto *V = std::get_if<int64_t>(&Op.literal);
        if (Type != "I" || !V)
          fail("literal operation has no signed integer literal");
        Right = hexLiteral(static_cast<uint64_t>(*V), 32);
      } else {
        std::string RightType =
            Operator == "shl" || Operator == "shr" || Operator == "ushr" ? "I"
                                                                         : Type;
        Right = get(Form == "/2addr" ? Regs[1] : Regs[2], RightType);
      }
      if (Operator == "rsub")
        std::swap(Left, Right);
      static const std::map<std::string, std::string> Symbols = {
          {"add", "+"}, {"sub", "-"},  {"rsub", "-"}, {"mul", "*"},
          {"div", "/"}, {"rem", "%"},  {"and", "&"},  {"or", "|"},
          {"xor", "^"}, {"shl", "<<"}, {"shr", ">>"}, {"ushr", ">>>"}};
      put(Regs[0], Type,
          "((" + Left + ") " + Symbols.at(Operator) + " (" + Right + "))");
    }
  }
  return {std::move(S), Out.get()};
}

std::string Body::condition(const Instruction &Op, const State &S,
                            bool Strict) const {
  bool Zero = Op.opcode.ends_with('z');
  const auto &Regs = Op.registers;
  if (Regs.size() != (Zero ? 1 : 2))
    fail("conditional branch register count disagrees");
  std::string Relation =
      Op.opcode.substr(3, Op.opcode.size() - 3 - (Zero ? 1 : 0));
  static const std::map<std::string, std::string> Symbols{
      {"eq", "=="}, {"ne", "!="}, {"lt", "<"},
      {"ge", ">="}, {"gt", ">"},  {"le", "<="}};
  auto Symbol = Symbols.find(Relation);
  if (Symbol == Symbols.end())
    fail("unsupported conditional relation");
  Strings Kinds = S.at(Regs[0]);
  if (!Zero)
    Kinds.insert(S.at(Regs[1]).begin(), S.at(Regs[1]).end());
  bool Reference = std::any_of(Kinds.begin(), Kinds.end(), [](const auto &K) {
    return isReference(K) || K == "null";
  });
  std::string Left, Right;
  if (Reference) {
    if (Relation != "eq" && Relation != "ne")
      fail("ordered comparison of object references");
    Left = read(S, Regs[0], "Ljava/lang/Object;", Strict);
    Right = Zero ? "null" : read(S, Regs[1], "Ljava/lang/Object;", Strict);
  } else {
    Left = read(S, Regs[0], "I", Strict);
    Right = Zero ? "0" : read(S, Regs[1], "I", Strict);
  }
  return Left + " " + Symbol->second + " " + Right;
}

State Body::initial() const {
  State S(method.registers, Strings{"?"});
  unsigned Reg = method.registers - method.incomingWords();
  if (!method.access.contains("static"))
    S.at(Reg++) = Strings{"self:" + method.reference.owner};
  for (const auto &T : method.reference.parameters) {
    S.at(Reg) = Strings{T};
    if (wordWidth(T) == 2)
      S.at(Reg + 1) = Strings{"wide-high"};
    Reg += wordWidth(T);
  }
  return S;
}

void Body::validateLocalOperations() const {
  for (size_t I = 0; I < code.size(); ++I) {
    budget.tick();
    const auto &Op = code[I];
    const auto &Name = Op.opcode;
    if (anyPrefix(Name, {"check-cast", "instance-of", "const-string",
                         "const-class", "new-array", "filled-new-array",
                         "fill-array-data", "array-length", "aget", "aput",
                         "iget", "iput", "monitor-", "return-object",
                         "move-result-object"}))
      fail("local-class object operation is outside the bounded projection: " +
           Name);
    if (Name == "new-instance") {
      auto Local = local_projection.by_class.find(referenceType(Op));
      if (Local == local_projection.by_class.end() ||
          *Local->second->enclosing_method != *local_closure)
        fail("local-class object allocation has no proven lexical owner");
    }
    if (anyPrefix(Name, {"sget", "sput"})) {
      const auto *Ref = std::get_if<FieldRef>(&Op.reference);
      if (!Ref || !LocalProjection::scalar(Ref->type))
        fail("local-class object field access is unsupported");
    }
    if (!starts(Name, "invoke-"))
      continue;
    const auto *Ref = std::get_if<MethodRef>(&Op.reference);
    if (!Ref || !LocalProjection::scalar(Ref->returns, true) ||
        !std::all_of(Ref->parameters.begin(), Ref->parameters.end(),
                     [](const auto &T) { return LocalProjection::scalar(T); }))
      fail("local-class object invocation argument or return is unsupported");
    if (baseOpcode(Name) == "invoke-static")
      continue;
    if (I == 0 && first == 1 &&
        *Ref == MethodRef{"Ljava/lang/Object;", "<init>", {}, "V"})
      continue;
    auto Local = local_projection.by_class.find(Ref->owner);
    if (Local == local_projection.by_class.end() ||
        *Local->second->enclosing_method != *local_closure)
      fail("local-class object invocation receiver has no proven local body");
  }
}

Body::Origins Body::localOrigins(const Instruction &Op,
                                 const Origins &Incoming, bool Strict) const {
  Origins Out = Incoming;
  const auto &Name = Op.opcode;
  const auto Base = baseOpcode(Name);
  const auto &Regs = Op.registers;
  auto require = [&](unsigned Reg, uint8_t Allowed, const char *Reason) {
    if (Strict &&
        (Incoming.at(Reg) == 0 || (Incoming.at(Reg) & ~Allowed) != 0))
      fail(std::string("local-class object ") + Reason);
  };
  if (Base == "move-object") {
    require(Regs.at(1), Local | Exception | Null,
            "copy has no proven local, exception, or null origin");
    Out.at(Regs.at(0)) = Incoming.at(Regs.at(1));
  } else if (Name == "move-exception")
    Out.at(Regs.at(0)) = Exception;
  else if (Name == "new-instance")
    Out.at(Regs.at(0)) = Local;
  else if (starts(Name, "invoke-")) {
    if (Base != "invoke-static")
      require(Regs.at(0), Local, "receiver is not a proven local instance");
  } else if (Name == "throw")
    require(Regs.at(0), Exception | Null,
            "throw operand is not a proven exception carrier");
  else if (starts(Name, "if-")) {
    for (unsigned R : Regs)
      require(R, Scalar | Null, "identity comparison is unsupported");
  } else if (Name == "nop" || starts(Name, "goto") ||
             Name.find("switch") != std::string::npos ||
             starts(Name, "return") || starts(Name, "sput")) {
    // No register write. Scalar compatibility and definedness are still
    // checked by operation(), independently of this object-origin domain.
  } else if (!Regs.empty()) {
    unsigned Dest = Regs[0];
    bool Wide = starts(Name, "const-wide") || Base == "move-wide" ||
                Name == "move-result-wide" || Name == "sget-wide" ||
                (!starts(Name, "cmp") &&
                 (Base.ends_with("-long") || Base.ends_with("-double")));
    Out.at(Dest) = Scalar;
    if (Base == "move")
      Out.at(Dest) = Incoming.at(Regs.at(1));
    else if (starts(Name, "const") && !Wide) {
      const auto *Value = std::get_if<int64_t>(&Op.literal);
      if (Value && *Value == 0)
        Out.at(Dest) = Null;
    }
    if (Wide)
      Out.at(Dest + 1) = Scalar;
  }
  return Out;
}

void Body::verify() {
  uint32_t Start = code[first].pc;
  states[Start] = initial();
  if (local_closure) {
    Origins Entry(method.registers, Scalar);
    if (!method.access.contains("static"))
      Entry.at(method.registers - method.incomingWords()) = Local;
    origins[Start] = std::move(Entry);
  }
  std::deque<uint32_t> Queue{Start};
  std::set<uint32_t> Queued{Start};
  std::map<uint32_t, std::set<uint32_t>> NormalPredecessors;
  while (!Queue.empty()) {
    budget.tick(method.registers * (local_closure ? 2 : 1) + 1);
    uint32_t PC = Queue.front();
    Queue.pop_front();
    Queued.erase(PC);
    size_t Index = by_pc.at(PC);
    State Incoming = states.at(PC);
    State Outgoing = operation(code[Index], Incoming, false).first;
    Origins IncomingOrigins, OutgoingOrigins;
    if (local_closure) {
      IncomingOrigins = origins.at(PC);
      OutgoingOrigins = localOrigins(code[Index], IncomingOrigins, false);
    }
    auto edge = [&](uint32_t Target, const State &S, const Origins &O,
                    bool Normal) {
      if (first && Target == code[0].pc)
        fail("control flow re-enters its constructor prefix");
      if (Normal)
        NormalPredecessors[Target].insert(PC);
      auto Old = states.find(Target);
      State Merged = S;
      bool Changed = Old == states.end();
      if (Old != states.end()) {
        for (size_t I = 0; I < Merged.size(); ++I)
          Merged[I].insert(Old->second[I].begin(), Old->second[I].end());
        Changed |= Merged != Old->second;
      }
      if (local_closure) {
        Origins MergedOrigins = O;
        auto Previous = origins.find(Target);
        if (Previous != origins.end()) {
          for (size_t I = 0; I < O.size(); ++I)
            MergedOrigins[I] |= Previous->second[I];
          Changed |= MergedOrigins != Previous->second;
        } else
          Changed = true;
        origins[Target] = std::move(MergedOrigins);
      }
      if (!Changed)
        return;
      states[Target] = std::move(Merged);
      if (Queued.insert(Target).second)
        Queue.push_back(Target);
    };
    for (auto T : successors(Index))
      edge(T, Outgoing, OutgoingOrigins, true);
    if (anyPrefix(code[Index].opcode, {"invoke-",
                                       "new-",
                                       "filled-",
                                       "aget",
                                       "aput",
                                       "iget",
                                       "iput",
                                       "sget",
                                       "sput",
                                       "array-length",
                                       "fill-array-data",
                                       "check-cast",
                                       "throw",
                                       "div-int",
                                       "rem-int",
                                       "div-long",
                                       "rem-long",
                                       "monitor-",
                                       "const-string",
                                       "const-class"}))
      for (const auto &H : handlers(PC))
        edge(H.target, Incoming, IncomingOrigins, false);
  }
  for (const auto &[PC, S] : states) {
    size_t Index = by_pc.at(PC);
    const auto &Op = code[Index];
    const auto &Predecessors = NormalPredecessors[PC];
    if (Op.opcode == "move-exception" && (PC == Start || !Predecessors.empty()))
      fail("normal flow enters an exception-only value");
    if (starts(Op.opcode, "move-result") &&
        Predecessors != std::set<uint32_t>{code[Index - 1].pc})
      fail("branch bypasses an invocation result producer");
    if (auto I = constructors.find(PC);
        I != constructors.end() &&
        Predecessors != std::set<uint32_t>{I->second})
      fail("branch bypasses an uninitialized allocation");
    operation(Op, S, true);
    if (local_closure)
      localOrigins(Op, origins.at(PC), true);
  }
  for (size_t I = first; I < code.size(); ++I)
    if (!states.contains(code[I].pc) && code[I].opcode != "nop")
      fail("unreachable instructions have no verified source projection");
}

std::string Body::resultRead(const std::string &Type) {
  if (isReference(Type))
    return "resultObject";
  if (Type == "F")
    return "((java.lang.Float) null).intBitsToFloat(result32)";
  if (Type == "D")
    return "((java.lang.Double) null).longBitsToDouble(result64)";
  if (Type == "J")
    return "result64";
  if (Type == "Z")
    return "(result32 != 0)";
  return "result32";
}
std::string Body::resultWrite(const std::string &Type,
                              const std::string &Value) {
  if (isReference(Type))
    return "resultObject = " + Value + ";";
  if (Type == "F")
    return "result32 = ((java.lang.Float) null).floatToRawIntBits(" + Value +
           ");";
  if (Type == "D")
    return "result64 = ((java.lang.Double) null).doubleToRawLongBits(" + Value +
           ");";
  return std::string(Type == "J" ? "result64" : "result32") + " = " +
         (Type == "Z" ? "(" + Value + ") ? 1 : 0" : Value) + ";";
}

std::string Body::emit() {
  verify();
  Lines Out(budget, true);
  if (!prefix.empty())
    Out.append(prefix);
  for (unsigned Reg = 0; Reg < method.registers; ++Reg) {
    std::string R = std::to_string(Reg);
    Out.extend({"int v" + R + " = 0;", "long w" + R + " = 0L;",
                "java.lang.Object o" + R + " = null;"});
  }
  unsigned Reg = method.registers - method.incomingWords();
  State Initial = initial();
  if (!method.access.contains("static"))
    Out.append("o" + std::to_string(Reg++) + " = this;");
  for (size_t I = 0; I < method.reference.parameters.size(); ++I) {
    const auto &T = method.reference.parameters[I];
    Out.append(write(Initial, Reg, T, "arg" + std::to_string(I)));
    Reg += wordWidth(T);
  }
  Out.extend({"int result32 = 0;", "long result64 = 0L;",
              "java.lang.Object resultObject = null;",
              "java.lang.Throwable caught = null;",
              "int pc = " + std::to_string(code[first].pc) + ";",
              "dispatch: while (true) {", "  try {", "    switch (pc) {"});
  for (size_t Index = first; Index < code.size(); ++Index) {
    const auto &Op = code[Index];
    auto S = states.find(Op.pc);
    if (S == states.end())
      continue;
    auto Statements = operation(Op, S->second, true).second;
    Out.append("      case " + std::to_string(Op.pc) + ": {");
    for (const auto &Statement : Statements)
      Out.append("        " + Statement);
    if (starts(Op.opcode, "if-")) {
      auto Condition = condition(Op, S->second, true);
      Out.append("        pc = (" + Condition + ") ? " +
                 std::to_string(target(Op)) + " : " +
                 std::to_string(code.at(Index + 1).pc) + ";");
    } else if (Op.opcode.find("switch") != std::string::npos) {
      Out.append("        switch (v" + std::to_string(Op.registers[0]) + ") {");
      for (size_t I = 0; I < Op.keys.size(); ++I)
        Out.append("          case " +
                   hexLiteral(static_cast<uint64_t>(Op.keys[I]), 32) +
                   ": pc = " + std::to_string(Op.targets[I]) + "; break;");
      Out.extend({"          default: pc = " +
                      std::to_string(code.at(Index + 1).pc) + ";",
                  "        }"});
    } else if (starts(Op.opcode, "goto"))
      Out.append("        pc = " + std::to_string(target(Op)) + ";");
    else if (!anyPrefix(Op.opcode, {"return", "throw"}))
      Out.append("        pc = " + std::to_string(code.at(Index + 1).pc) + ";");
    if (!anyPrefix(Op.opcode, {"return", "throw"}))
      Out.append("        continue dispatch;");
    Out.append("      }");
  }
  Out.extend({"      default: throw new java.lang.AssertionError(\"Invalid "
              "recovered control flow\");",
              "    }", "  } catch (java.lang.Throwable failure) {"});
  for (const auto &R : method.tries) {
    Out.append("    if (pc >= " + std::to_string(R.start) + " && pc < " +
               std::to_string(R.end) + ") {");
    for (const auto &H : R.handlers) {
      auto Test = H.type ? "failure instanceof " + sourceType(*H.type) : "true";
      Out.append("      if (" + Test + ") { caught = failure; pc = " +
                 std::to_string(H.target) + "; continue dispatch; }");
    }
    Out.append("    }");
  }
  Out.extend({"    throw " + throwHelper() + "(failure);", "  }", "}"});
  return Out.text();
}

std::optional<std::string> fieldValue(const FieldValue &Value,
                                      const std::string &Type) {
  if (std::holds_alternative<std::monostate>(Value))
    return std::nullopt;
  if (const auto *Bits = std::get_if<FloatBits>(&Value)) {
    if (!Bits->wide && Type == "F")
      return "((java.lang.Float) null).intBitsToFloat(" +
             hexLiteral(Bits->bits, 32) + ")";
    if (Bits->wide && Type == "D")
      return "((java.lang.Double) null).longBitsToDouble(" +
             hexLiteral(Bits->bits, 64) + ")";
    javaError("unsupported encoded static field value");
  }
  if (const auto *Text = std::get_if<std::string>(&Value);
      Text && Type == "Ljava/lang/String;")
    return javaString(*Text);
  if (Type == "Z") {
    if (const auto *V = std::get_if<bool>(&Value))
      return *V ? "true" : "false";
    if (const auto *V = std::get_if<int64_t>(&Value))
      return *V ? "true" : "false";
  }
  if (const auto *V = std::get_if<int64_t>(&Value);
      V && Strings{"B", "C", "S", "I", "J"}.contains(Type)) {
    auto Text = hexLiteral(static_cast<uint64_t>(*V), Type == "J" ? 64 : 32);
    return Type == "B" || Type == "C" || Type == "S"
               ? "(" + Primitives.at(Type) + ") " + Text
               : Text;
  }
  javaError("static field value disagrees with its declared type");
}

std::vector<std::string> modifiers(const Access &Flags,
                                   std::initializer_list<const char *> Order) {
  std::vector<std::string> Result;
  for (const auto *Flag : Order)
    if (Flags.contains(Flag))
      Result.emplace_back(Flag);
  return Result;
}

} // namespace

llvm::json::Object recoverJava(const ClassMap &Classes, Budget &B) {
  validateSourceScopes(Classes, B);
  llvm::json::Array Units, Methods, Bindings, Helpers;
  uint64_t Recovered = 0, Declared = 0, Projected = 0;
  LocalProjection Projection(Classes, B);
  std::map<std::string, std::vector<const Class *>> Enclosing;
  for (const auto &[Name, C] : Classes) {
    B.tick();
    if (C.enclosing) {
      if (*C.enclosing == Name || !Classes.contains(*C.enclosing) ||
          !C.inner_name || C.inner_name->empty())
        javaError("unresolved or recursive nested class");
      Enclosing[*C.enclosing].push_back(&C);
    }
  }
  for (const auto &[Name, C] : Classes) {
    std::optional<std::string> Current = Name;
    Strings Seen;
    while (Current) {
      B.tick();
      if (!Seen.insert(*Current).second)
        javaError("recursive nested-class ownership");
      if (Seen.size() > 128)
        javaError("nested-class ownership exceeds the source depth limit");
      const auto &Parent = Classes.at(*Current);
      Current = Parent.enclosing_method
                    ? std::optional<std::string>(Parent.enclosing_method->owner)
                    : Parent.enclosing;
    }
  }
  JavaTypeNames TypeNames(Classes, B);
  Strings Emitting, Emitted;
  enum class DeclarationSite { TopLevel, Member, MethodLocal };
  auto auxiliary = [&](const Class &C, const std::string &Name,
                       const std::string &Prototype, bool Static,
                       const std::string &Unit, const char *Kind) {
    if (Projection.by_class.empty())
      return;
    B.tick();
    Helpers.push_back(llvm::json::Object{{"class", C.name},
                                        {"name", Name},
                                        {"prototype", Prototype},
                                        {"static", Static},
                                        {"source_unit", Unit},
                                        {"kind", Kind}});
  };
  std::function<std::string(const Class &, DeclarationSite,
                            const std::string &)>
      emitClass;
  emitClass = [&](const Class &C, DeclarationSite Site,
                  const std::string &Unit) -> std::string {
    B.tick();
    if (!Emitting.insert(C.name).second)
      javaError("recursive nested-class ownership");
    if (!Emitted.insert(C.name).second)
      javaError("Android class ownership emitted a declaration more than once");
    bool Nested = Site == DeclarationSite::Member;
    bool Local = Site == DeclarationSite::MethodLocal;
    if (C.access.contains("annotation") || C.access.contains("enum"))
      javaError("unsupported Java declaration shape: " + C.name);
    const auto &Flags = Nested ? C.inner_access : C.access;
    if (Nested && !Flags.contains("static"))
      javaError("non-static inner class requires an outer-instance "
                "initialization proof");
    auto Slash = C.name.rfind('/');
    std::string Name;
    if (Nested || Local)
      Name = javaIdentifier(*C.inner_name);
    else
      Name = javaIdentifier(C.name.substr(
          Slash == std::string::npos ? 1 : Slash + 1,
          C.name.size() - (Slash == std::string::npos ? 1 : Slash + 1) - 1));
    if (Name == "java")
      javaError("class name shadows the required Java runtime package");
    auto Mods = modifiers(Flags, {"public", "protected", "private", "static",
                                  "abstract", "final", "strictfp"});
    if (Local)
      Mods = modifiers(Flags, {"final"});
    if (!Nested)
      std::erase(Mods, "static");
    std::string Kind = C.access.contains("interface") ? "interface" : "class";
    Mods.push_back(Kind);
    Mods.push_back(Name);
    std::string Header = join(Mods, " ");
    if (Kind == "class" && C.superclass &&
        *C.superclass != "Ljava/lang/Object;")
      Header += " extends " + TypeNames.render(*C.superclass, C, true);
    if (!C.interfaces.empty()) {
      std::vector<std::string> Interfaces;
      for (const auto &T : C.interfaces)
        Interfaces.push_back(TypeNames.render(T, C, true));
      Header += (Kind == "interface" ? " extends " : " implements ") +
                join(Interfaces, ", ");
    }
    Lines BodyLines(B, true);
    BodyLines.append(Header + " {");
    if (Local) {
      const auto &R = *C.enclosing_method;
      llvm::json::Array Parameters;
      for (const auto &T : R.parameters) {
        B.tick();
        Parameters.push_back(T);
      }
      llvm::json::Object EnclosingMethod{{"identity", R.identity()},
                                         {"owner", R.owner},
                                         {"name", R.name},
                                         {"prototype", R.signature()},
                                         {"parameters", std::move(Parameters)},
                                         {"returns", R.returns}};
      Bindings.push_back(llvm::json::Object{
          {"class", C.name},
          {"input", C.source_id},
          {"enclosing_method", std::move(EnclosingMethod)},
          {"source_unit", Unit},
          {"source_name", Name},
          {"binding_kind", "named-method-local"},
          {"binary_name_status", "unverified"}});
    }
    Strings MemberNames, ConstantTypes;
    bool HasStaticFieldInitializer = false;
    bool HasClassInitializer = false;
    for (const auto &F : C.fields) {
      B.tick();
      if (!MemberNames.insert(F.reference.name).second)
        javaError("Java cannot represent field names overloaded only by type");
      auto FieldMods =
          modifiers(F.access, {"public", "protected", "private", "static",
                               "final", "volatile", "transient"});
      if (std::holds_alternative<FloatBits>(F.value))
        TypeNames.requireRuntimePackage(C);
      auto Value = fieldValue(F.value, F.reference.type);
      HasStaticFieldInitializer |=
          Value.has_value() && F.access.contains("static");
      if (F.access.contains("final") && !Value)
        javaError("final field needs a verified initialization expression");
      if (Value && F.access.contains("static") && F.access.contains("final")) {
        ConstantTypes.insert(F.reference.type);
        *Value = helperName(C, "__neverdConstant") + "(" + *Value + ")";
      }
      FieldMods.push_back(TypeNames.render(F.reference.type, C));
      FieldMods.push_back(javaIdentifier(F.reference.name));
      BodyLines.append("  " + join(FieldMods, " ") +
                       (Value ? " = " + *Value : "") + ";");
    }
    std::set<std::pair<std::string, std::vector<std::string>>> JavaSignatures;
    for (const auto &M : C.methods) {
      B.tick();
      const auto &R = M.reference;
      HasClassInitializer |= R.name == "<clinit>";
      if (!JavaSignatures.emplace(R.name, R.parameters).second)
        javaError(
            "Java cannot represent methods overloaded only by return type");
      llvm::json::Object Row{
          {"identity", R.identity()},
          {"class", C.name},
          {"name", R.name},
          {"prototype", R.signature()},
          {"input", C.source_id},
          {"instruction_count", static_cast<int64_t>(M.instructions.size())}};
      auto MethodMods = modifiers(M.access, {"public", "protected", "private",
                                             "static", "final", "synchronized",
                                             "native", "abstract", "strictfp"});
      std::vector<std::string> Params;
      for (size_t I = 0; I < R.parameters.size(); ++I)
        Params.push_back(TypeNames.render(R.parameters[I], C) + " arg" +
                         std::to_string(I));
      std::string Declaration;
      if (R.name == "<clinit>") {
        if (!R.parameters.empty() || R.returns != "V" ||
            !M.access.contains("static"))
          javaError("invalid class initializer signature");
        Declaration = "static";
      } else if (R.name == "<init>") {
        if (R.returns != "V" || M.access.contains("static"))
          javaError("invalid instance initializer signature");
        MethodMods.push_back(Name);
        Declaration = join(MethodMods, " ") + "(" + join(Params, ", ") + ")";
        if (constructorThrows(M, Classes, B))
          Declaration += " throws java.lang.Throwable";
      } else {
        MethodMods.push_back(TypeNames.render(R.returns, C));
        MethodMods.push_back(javaIdentifier(R.name));
        Declaration = join(MethodMods, " ") + "(" + join(Params, ", ") + ")";
      }
      if (M.access.contains("abstract") || M.access.contains("native")) {
        BodyLines.append("  " + Declaration + ";");
        Row["status"] = "declaration-only";
        Row["reason"] =
            "original abstract/native declaration has no Dalvik body";
        ++Declared;
      } else {
        if (Kind == "interface")
          javaError("interface method body requires a compatible Java "
                    "default/static declaration");
        Body Emitter(M, Classes, B, TypeNames, Projection);
        std::string Source;
        if (auto I = Projection.by_method.find(R);
            I != Projection.by_method.end()) {
          Lines MethodSource(B);
          for (const auto *Child : I->second)
            MethodSource.append(
                emitClass(*Child, DeclarationSite::MethodLocal, Unit));
          MethodSource.append(Emitter.emit());
          Source = MethodSource.text();
        } else
          Source = Emitter.emit();
        auto SourceLines = split(Source, '\n');
        for (auto &Line : SourceLines)
          Line = "    " + Line;
        BodyLines.append("  " + Declaration + " {\n" + join(SourceLines, "\n") +
                         "\n  }");
        if (Projection.projected_methods.contains(R)) {
          Row["status"] = "source-projected";
          Row["projection_kind"] = "named-method-local";
          Row["reason"] =
              "Method body and lexical class scope were emitted; recompiled "
              "local-class binary identity and access flags are unverified.";
          ++Projected;
        } else {
          Row["status"] = "recovered";
          ++Recovered;
        }
      }
      Methods.push_back(std::move(Row));
    }
    if (HasStaticFieldInitializer && !HasClassInitializer)
      auxiliary(C, "<clinit>", "()V", true, Unit, "field-initializer");
    if (Kind == "class") {
      // Every emitted class contains runtime helper references, even when
      // its methods only mention primitive types. Validate their fixed root
      // just like descriptor-derived source types before publication.
      TypeNames.requireRuntimePackage(C);
      bool HasConstructor =
          std::any_of(C.methods.begin(), C.methods.end(), [](const auto &M) {
            return M.reference.name == "<init>";
          });
      if (!HasConstructor) {
        BodyLines.append("  private " + Name + "() {}");
        auxiliary(C, "<init>", "()V", false, Unit, "default-constructor");
      }
      std::string Helper = helperName(C, "__neverdThrow");
      BodyLines.extend({"  @java.lang.SuppressWarnings(\"unchecked\")",
                        std::string("  private ") + (Local ? "" : "static ") +
                            "<E extends java.lang.Throwable> "
                            "java.lang.RuntimeException " +
                            Helper + "(java.lang.Throwable failure) throws E {",
                        "    if (failure == null) throw new "
                        "java.lang.NullPointerException();",
                        "    throw (E) failure;", "  }"});
      auxiliary(C, Helper, "(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;",
                !Local, Unit, "throw-helper");
    }
    for (const auto &T : ConstantTypes) {
      auto NameType = TypeNames.render(T, C);
      BodyLines.append("  private static " + NameType + " " +
                       helperName(C, "__neverdConstant") + "(" + NameType +
                       " value) { return value; }");
      auxiliary(C, helperName(C, "__neverdConstant"), "(" + T + ")" + T, true,
                Unit, "constant-helper");
    }
    if (auto I = Enclosing.find(C.name); I != Enclosing.end()) {
      auto Children = I->second;
      std::sort(Children.begin(), Children.end(),
                [](const auto *A, const auto *B) { return A->name < B->name; });
      for (const auto *Child : Children)
        for (const auto &Line :
             split(emitClass(*Child, DeclarationSite::Member, Unit), '\n'))
          BodyLines.append("  " + Line);
    }
    BodyLines.append("}");
    Emitting.erase(C.name);
    return BodyLines.text();
  };
  uint64_t MethodCount = 0;
  for (const auto &[Name, C] : Classes) {
    MethodCount += C.methods.size();
    if (C.enclosing || C.enclosing_method)
      continue;
    std::string Path = C.name.substr(1, C.name.size() - 2);
    size_t Slash = Path.rfind('/');
    std::string Prefix;
    if (Slash != std::string::npos) {
      auto Package = split(std::string_view(Path).substr(0, Slash), '/');
      for (auto &Part : Package)
        Part = javaIdentifier(Part);
      Prefix = "package " + join(Package, ".") + ";\n\n";
    }
    std::string Source =
        Prefix + emitClass(C, DeclarationSite::TopLevel, Path + ".java") + "\n";
    Units.push_back(llvm::json::Object{{"path", Path + ".java"},
                                       {"class", C.name},
                                       {"source", std::move(Source)}});
  }
  if (Methods.size() != MethodCount || Emitted.size() != Classes.size() ||
      MethodCount != Recovered + Projected + Declared)
    javaError("Android class ownership omitted declared methods");
  if (Units.empty())
    javaError("Android input contains no source classes");
  llvm::json::Object Report{
      {"schema_version", 1},
      {"status", Projected ? "partial" : "recovered"},
      {"class_count", static_cast<int64_t>(Classes.size())},
      {"method_count", static_cast<int64_t>(Methods.size())},
      {"recovered_method_count", static_cast<int64_t>(Recovered)},
      {"declaration_only_method_count", static_cast<int64_t>(Declared)},
      {"unrecovered_method_count", 0},
      {"methods", std::move(Methods)},
      {"source_units", std::move(Units)}};
  if (!Projection.by_class.empty()) {
    if (Bindings.size() != Projection.by_class.size() ||
        Projected != Projection.projected_methods.size())
      javaError("Android local-class projection omitted an original identity");
    Report["projected_method_count"] = static_cast<int64_t>(Projected);
    Report["class_source_bindings"] = std::move(Bindings);
    Report["generated_source_helpers"] = std::move(Helpers);
  }
  return Report;
}
} // namespace neverd::mobile::dalvik
