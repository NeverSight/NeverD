//===- Class.cpp - JVM class files and constant pools ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Validate the wire types before exposing any pool reference to a consumer.
//===----------------------------------------------------------------------===//

#include "Class.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"

#include <algorithm>

using namespace llvm;
namespace neverd::jvm {
void invalid(size_t At, const char *Code, std::string Why) {
  throw Failure{"malformed", Code, std::move(Why), At};
}
void unsupported(size_t At, const char *Code, std::string Why) {
  throw Failure{"unsupported", Code, std::move(Why), At};
}
json::Object diagnostic(const Failure &F) {
  return json::Object{
      {"code", F.Code}, {"offset", F.At}, {"message", F.Message}};
}
void Job::check(size_t At) const {
  if (Stop())
    throw Failure{"cancelled", "cancelled", "JVM recovery cancelled", At};
}
void Job::text(size_t N, size_t At) {
  if (N > TextBytes)
    throw Failure{"budget_exceeded", "job_bytes",
                  "JVM metadata and IR byte budget exhausted", At};
  TextBytes -= N;
}
void Job::charge(size_t N, size_t At) {
  check(At);
  if (N > Items)
    throw Failure{"budget_exceeded", "job_items",
                  "JVM job item budget exhausted", At};
  Items -= N;
}
StringRef Reader::take(size_t N) {
  if (N > left())
    invalid(at(), "truncated", "Class record exceeds its enclosing extent");
  auto S = Data.substr(Pos, N);
  Pos += N;
  return S;
}
uint8_t Reader::u1() { return take(1).front(); }
uint16_t Reader::u2() { return support::endian::read16be(take(2).data()); }
uint32_t Reader::u4() { return support::endian::read32be(take(4).data()); }
uint64_t Reader::u8() { return support::endian::read64be(take(8).data()); }
Reader Reader::slice(size_t N) {
  auto Start = at();
  return {take(N), J, Start};
}
void Reader::end() const {
  if (left())
    invalid(at(), "trailing_bytes", "Trailing bytes in class record");
}
bool validClassName(StringRef S) {
  if (S.empty() || S.starts_with('/') || S.ends_with('/') || S.contains("//") ||
      S.contains('\0'))
    return false;
  return S.find_first_of(".;[") == StringRef::npos;
}
namespace {
std::string type(StringRef &Text, bool Void, size_t At) {
  auto Start = Text;
  unsigned Rank = 0;
  while (Text.consume_front("[")) {
    if (++Rank > 255)
      invalid(At, "descriptor_rank", "Array descriptor exceeds 255 dimensions");
  }
  if (Text.consume_front("L")) {
    size_t End = Text.find(';');
    if (End == StringRef::npos || !validClassName(Text.take_front(End)))
      invalid(At, "descriptor_class", "Invalid class in descriptor");
    Text = Text.drop_front(End + 1);
  } else {
    if (Text.empty() ||
        (StringRef("BCDFIJSZ").find(Text.front()) == StringRef::npos &&
         !(Void && !Rank && Text.front() == 'V')))
      invalid(At, "descriptor_type", "Invalid JVM type descriptor");
    Text = Text.drop_front();
  }
  return Start.take_front(Start.size() - Text.size()).str();
}
void addReference(Class &C, StringRef T) {
  T = T.ltrim('[');
  if (T.consume_front("L") && T.consume_back(";"))
    C.References.insert(T.str());
}
void visitDescriptor(Class &C, StringRef Desc, bool Method, size_t At) {
  if (Method) {
    auto Sig = methodDescriptor(Desc, At);
    for (auto &Arg : Sig.Arguments)
      addReference(C, Arg);
    addReference(C, Sig.Result);
  } else {
    fieldDescriptor(Desc, At);
    addReference(C, Desc);
  }
}
void appendUTF8(std::string &S, unsigned C) {
  if (C < 0x80) {
    S.push_back(char(C));
    return;
  }
  unsigned Count = C < 0x800 ? 2 : C < 0x10000 ? 3 : 4;
  S.push_back(char((Count == 2   ? 0xc0
                    : Count == 3 ? 0xe0
                                 : 0xf0) |
                   (C >> (6 * (Count - 1)))));
  while (--Count)
    S.push_back(char(0x80 | ((C >> (6 * (Count - 1))) & 63)));
}
void modifiedUTF8(Constant &K, Reader R) {
  std::optional<unsigned> High;
  auto EmitUnpaired = [&](unsigned U) {
    // A String may contain an unpaired UTF-16 code unit. Its raw pool offset
    // and escaped text remain available. Never use it as a resolved identity.
    K.Escaped = true;
    K.Text += "\\u" + utohexstr(U, false, 4);
  };
  K.Text.reserve(R.left());
  while (R.left()) {
    size_t At = R.at();
    unsigned U = R.u1();
    if (!U || U >= 0xf0 || (U >= 0x80 && U < 0xc0))
      invalid(At, "modified_utf8", "Invalid modified UTF-8 leading byte");
    if (U >= 0xc0) {
      unsigned N = U >= 0xe0 ? 2 : 1;
      U &= N == 2 ? 15 : 31;
      for (unsigned I = 0; I < N; ++I) {
        unsigned B = R.u1();
        if ((B & 0xc0) != 0x80)
          invalid(R.at() - 1, "modified_utf8",
                  "Invalid modified UTF-8 continuation");
        U = (U << 6) | (B & 63);
      }
      if ((N == 2 && U < 0x800) || (N == 1 && U && U < 0x80))
        invalid(At, "modified_utf8", "Overlong modified UTF-8 code unit");
    }
    if (High) {
      if (U >= 0xdc00 && U <= 0xdfff) {
        appendUTF8(K.Text, 0x10000 + ((*High - 0xd800) << 10) + U - 0xdc00);
        High.reset();
        continue;
      }
      EmitUnpaired(*High);
      High.reset();
    }
    if (U >= 0xd800 && U <= 0xdbff)
      High = U;
    else if (U >= 0xdc00 && U <= 0xdfff)
      EmitUnpaired(U);
    else
      appendUTF8(K.Text, U);
  }
  if (High)
    EmitUnpaired(*High);
}
void pool(Reader &R, Class &C) {
  unsigned N = R.u2();
  if (!N)
    invalid(R.at() - 2, "pool_count", "A class needs a constant pool count");
  R.J.charge(N, R.at());
  C.Pool.resize(N);
  for (unsigned I = 1; I < N; ++I) {
    auto &K = C.Pool[I];
    K.At = R.at();
    K.Tag = R.u1();
    unsigned Minimum = K.Tag == 17                                 ? 55
                       : K.Tag == 19 || K.Tag == 20                ? 53
                       : K.Tag == 15 || K.Tag == 16 || K.Tag == 18 ? 51
                                                                   : 45;
    if (Minimum > C.Major)
      invalid(K.At, "pool_version",
              "Constant pool tag is unavailable in the class version");
    switch (K.Tag) {
    case 1:
      modifiedUTF8(K, R.slice(R.u2()));
      break;
    case 3:
    case 4:
      K.Bits = R.u4();
      break;
    case 5:
    case 6:
      K.Bits = R.u8();
      if (++I == N)
        invalid(K.At, "pool_wide", "Missing reserved constant-pool slot");
      break;
    case 7:
    case 8:
    case 16:
    case 19:
    case 20:
      K.A = R.u2();
      break;
    case 9:
    case 10:
    case 11:
    case 12:
    case 17:
    case 18:
      K.A = R.u2();
      K.B = R.u2();
      break;
    case 15:
      K.A = R.u1();
      K.B = R.u2();
      break;
    default:
      invalid(K.At, "pool_tag", "Unknown constant-pool tag");
    }
  }
}
void name(StringRef S, bool Method, size_t At) {
  if (S.empty() || S.contains('\0') ||
      S.find_first_of(".;[/") != StringRef::npos ||
      (Method && S != "<init>" && S != "<clinit>" &&
       S.find_first_of("<>") != StringRef::npos))
    invalid(At, "member_name", "Invalid JVM member name");
}
void checkPool(Class &C) {
  for (const auto &K : C.Pool) {
    switch (K.Tag) {
    case 7: {
      const auto &S = C.text(K.A, K.At);
      if (StringRef(S).starts_with("["))
        visitDescriptor(C, S, false, K.At);
      else if (validClassName(S))
        C.References.insert(S);
      else
        invalid(K.At, "class_name", "Invalid internal class name");
      break;
    }
    case 8:
      C.constant(K.A, {1}, K.At);
      break;
    case 19:
    case 20:
      if (C.text(K.A, K.At).empty())
        invalid(K.At, "module_name", "Empty module or package name");
      break;
    case 16:
      visitDescriptor(C, C.text(K.A, K.At), true, K.At);
      break;
    case 12: {
      const auto &N = C.text(K.A, K.At), &D = C.text(K.B, K.At);
      bool Method = StringRef(D).starts_with("(");
      name(N, Method, K.At);
      visitDescriptor(C, D, Method, K.At);
      break;
    }
    case 9:
    case 10:
    case 11:
    case 17:
    case 18: {
      if (K.Tag <= 11)
        C.className(K.A, K.At);
      const auto &NT = C.nameAndType(K);
      const auto &N = C.text(NT.A, K.At), &D = C.text(NT.B, K.At);
      bool Method = K.Tag != 9 && K.Tag != 17;
      if (StringRef(D).starts_with("(") != Method ||
          (Method && N == "<clinit>") ||
          (N == "<init>" && (K.Tag != 10 || !StringRef(D).ends_with(")V"))))
        invalid(K.At, "pool_descriptor",
                "Member reference has incompatible name or descriptor");
      break;
    }
    case 15: {
      if (!K.A || K.A > 9)
        invalid(K.At, "handle_kind", "Invalid method handle reference kind");
      const Constant *Target;
      if (K.A <= 4)
        Target = &C.constant(K.B, {9}, K.At);
      else if (K.A == 9)
        Target = &C.constant(K.B, {11}, K.At);
      else if ((K.A == 6 || K.A == 7) && C.Major >= 52)
        Target = &C.constant(K.B, {10, 11}, K.At);
      else
        Target = &C.constant(K.B, {10}, K.At);
      if (K.A >= 5) {
        auto &NT = C.nameAndType(*Target);
        auto &N = C.text(NT.A, K.At);
        if ((K.A == 8) != (N == "<init>") || N == "<clinit>")
          invalid(K.At, "handle_target",
                  "Method handle has incompatible target name");
      }
      break;
    }
    }
  }
}
std::vector<Member> members(Reader &R, Class &C, bool Method) {
  std::vector<Member> Rows;
  std::set<std::pair<std::string, std::string>> Keys;
  unsigned N = R.u2();
  R.J.charge(N, R.at());
  Rows.reserve(N);
  while (N--) {
    Member M;
    M.At = R.at();
    M.Flags = R.u2();
    M.Name = C.text(R.u2(), M.At);
    M.Desc = C.text(R.u2(), M.At);
    name(M.Name, Method, M.At);
    visitDescriptor(C, M.Desc, Method, M.At);
    if (!Keys.emplace(M.Name, M.Desc).second)
      invalid(M.At, "duplicate_member", "Duplicate member name and descriptor");
    if (Method && M.Name == "<init>" &&
        (!StringRef(M.Desc).ends_with(")V") || (M.Flags & 8)))
      invalid(M.At, "constructor_descriptor",
              "Invalid constructor declaration");
    if (Method && M.Name == "<clinit>" &&
        (M.Desc != "()V" || (C.Major >= 51 && !(M.Flags & 8))))
      invalid(M.At, "initializer_descriptor",
              "Invalid class initializer declaration");
    readAttributes(R, C, &M, nullptr, Method, M.Attributes);
    if (Method && !M.Error && bool(M.Body) == bool(M.Flags & (0x0100 | 0x0400)))
      M.Error = Failure{"malformed", "code_presence",
                        "Method flags and Code attribute disagree", M.At};
    Rows.push_back(std::move(M));
  }
  return Rows;
}
} // namespace
Signature methodDescriptor(StringRef Text, size_t At) {
  Signature S;
  if (!Text.consume_front("("))
    invalid(At, "method_descriptor",
            "Missing method descriptor opening parenthesis");
  while (!Text.consume_front(")")) {
    S.Arguments.push_back(type(Text, false, At));
    S.Slots += slots(S.Arguments.back());
    if (S.Slots > 255)
      invalid(At, "method_slots", "Method argument slot limit exceeded");
  }
  S.Result = type(Text, true, At);
  if (!Text.empty())
    invalid(At, "descriptor_tail", "Trailing method descriptor data");
  return S;
}
void fieldDescriptor(StringRef Text, size_t At) {
  type(Text, false, At);
  if (!Text.empty())
    invalid(At, "descriptor_tail", "Trailing field descriptor data");
}
char verificationKind(StringRef T) {
  if (T.empty())
    return '?';
  char K = T.front();
  if (K == '[' || K == 'L')
    return 'A';
  return StringRef("ZBCS").contains(K) ? 'I' : K;
}
unsigned slots(StringRef T) {
  return T == "J" || T == "D" ? 2 : T == "V" ? 0 : 1;
}
std::string javaType(StringRef T) {
  unsigned Rank = 0;
  while (T.consume_front("["))
    ++Rank;
  std::string S;
  if (T.consume_front("L")) {
    T.consume_back(";");
    S = T.str();
    std::replace(S.begin(), S.end(), '/', '.');
  } else {
    static constexpr StringLiteral Codes("BCDFIJSZV");
    static constexpr const char *Names[]{"byte",  "char",    "double",
                                         "float", "int",     "long",
                                         "short", "boolean", "void"};
    S = Names[Codes.find(T.front())];
  }
  while (Rank--)
    S += "[]";
  return S;
}
const Constant &Class::constant(unsigned I,
                                std::initializer_list<unsigned> Tags,
                                size_t At) const {
  if (!I || I >= Pool.size() || !Pool[I].Tag ||
      (Tags.size() && !is_contained(Tags, Pool[I].Tag)))
    invalid(At, "pool_reference",
            "Constant pool index has an incompatible tag");
  return Pool[I];
}
const std::string &Class::text(unsigned I, size_t At) const {
  const auto &K = constant(I, {1}, At);
  if (Admission)
    Admission->text(K.Text.size(), At);
  if (K.Escaped)
    unsupported(At, "surrogate_identifier",
                "Unpaired surrogate in JVM identifier");
  return K.Text;
}
const std::string &Class::className(unsigned I, size_t At) const {
  return text(constant(I, {7}, At).A, At);
}
const Constant &Class::nameAndType(const Constant &Ref) const {
  return constant(Ref.B, {12}, Ref.At);
}
Class readClass(StringRef Bytes, Job &J) {
  Reader R{Bytes, J};
  Class C;
  C.Admission = &J;
  if (R.u4() != 0xcafebabe)
    invalid(0, "class_magic", "Missing JVM class signature");
  C.Minor = R.u2();
  C.Major = R.u2();
  if (C.Major < 45 || C.Major > 44 + LastRelease)
    unsupported(6, "class_version", "Unqualified class-file major version");
  if (C.Major >= 56 && C.Minor && C.Minor != 65535)
    invalid(4, "class_minor",
            "Minor version must be zero or the preview marker");
  pool(R, C);
  checkPool(C);
  C.Flags = R.u2();
  C.Name = C.className(R.u2(), R.at() - 2);
  unsigned Super = R.u2();
  if (Super)
    C.Super = C.className(Super, R.at() - 2);
  if (!validClassName(C.Name) || (Super && !validClassName(C.Super)) ||
      (!Super && C.Name != "java/lang/Object" && !(C.Flags & 0x8000)))
    invalid(R.at() - 2, "class_hierarchy",
            "Invalid class or superclass identity");
  for (unsigned N = R.u2(); N; --N) {
    const auto &I = C.className(R.u2(), R.at() - 2);
    if (!validClassName(I))
      invalid(R.at() - 2, "interface", "Invalid interface name");
    C.Interfaces.push_back(I);
  }
  C.Fields = members(R, C, false);
  C.Methods = members(R, C, true);
  readAttributes(R, C, nullptr, nullptr, false, C.Attributes);
  R.end();
  for (auto &K : C.Pool) {
    if ((K.Tag == 17 || K.Tag == 18) && K.A >= C.Bootstraps.size())
      invalid(K.At, "bootstrap_index",
              "Bootstrap index exceeds BootstrapMethods table");
    if ((K.Tag == 19 || K.Tag == 20) && !(C.Flags & 0x8000))
      invalid(K.At, "module_constant",
              "Module and package constants require a module descriptor");
  }
  if ((C.Flags & 0x8000) &&
      (C.Major < 53 || C.Name != "module-info" || Super || !C.Fields.empty() ||
       !C.Methods.empty() || !C.Interfaces.empty() || C.Module.empty()))
    invalid(0, "module_layout", "Invalid JVM module descriptor");
  C.Admission = nullptr;
  C.References.erase(C.Name);
  return C;
}
} // namespace neverd::jvm
