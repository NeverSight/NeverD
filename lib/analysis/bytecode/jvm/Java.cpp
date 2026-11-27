//===- Java.cpp - Structured Java rendering -------------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Render proven expression order and single-entry regions. The typed CFG
/// remains available when a source structure cannot account for an edge.
//===----------------------------------------------------------------------===//

#include "Flow.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSwitch.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <map>
#include <set>

using namespace llvm;
namespace neverd::jvm {
namespace {
struct Text {
  std::string S;
  std::vector<unsigned> Rows;
};
Text combine(std::string S, ArrayRef<Text> Inputs, unsigned Row) {
  Text Out{std::move(S), {Row}};
  for (auto &T : Inputs)
    Out.Rows.insert(Out.Rows.end(), T.Rows.begin(), T.Rows.end());
  return Out;
}
bool identifier(StringRef S) {
  if (S.empty() || !(isAlpha(S[0]) || S[0] == '_' || S[0] == '$'))
    return false;
  if (!llvm::all_of(S.drop_front(),
                    [](char C) { return isAlnum(C) || C == '_' || C == '$'; }))
    return false;
  static constexpr StringRef Keywords[] = {
      "abstract",   "assert",       "boolean",   "break",      "byte",
      "case",       "catch",        "char",      "class",      "const",
      "continue",   "default",      "do",        "double",     "else",
      "enum",       "extends",      "final",     "finally",    "float",
      "for",        "goto",         "if",        "implements", "import",
      "instanceof", "int",          "interface", "long",       "native",
      "new",        "package",      "private",   "protected",  "public",
      "return",     "short",        "static",    "strictfp",   "super",
      "switch",     "synchronized", "this",      "throw",      "throws",
      "transient",  "try",          "void",      "volatile",   "while",
      "true",       "false",        "null",      "_"};
  return !llvm::is_contained(Keywords, S);
}
std::string dotted(StringRef Raw, size_t At) {
  if (Raw.starts_with("["))
    return javaType(Raw);
  std::string S;
  while (!Raw.empty()) {
    auto [Part, Tail] = Raw.split('/');
    if (!identifier(Part))
      unsupported(At, "java_identifier",
                  "JVM identifier has no qualified Java spelling");
    if (!S.empty())
      S += '.';
    S += Part;
    Raw = Tail;
  }
  return S;
}
std::string quoted(StringRef S) {
  std::string T = "\"";
  for (unsigned char C : S) {
    switch (C) {
    case '"':
      T += "\\\"";
      break;
    case '\\':
      T += "\\\\";
      break;
    case '\n':
      T += "\\n";
      break;
    case '\t':
      T += "\\t";
      break;
    case '\r':
      T += "\\r";
      break;
    case '\b':
      T += "\\b";
      break;
    case '\f':
      T += "\\f";
      break;
    default:
      if (C < 32 || C == 127) {
        T += '\\';
        T += char('0' + (C >> 6));
        T += char('0' + ((C >> 3) & 7));
        T += char('0' + (C & 7));
      } else
        T += char(C);
    }
  }
  return T + '"';
}
bool conditional(unsigned Op) {
  return (Op >= 153 && Op <= 166) || Op == 198 || Op == 199;
}
bool ret(unsigned Op) { return (Op >= 172 && Op <= 177) || Op == 191; }
bool go(unsigned Op) { return Op == 167 || Op == 200; }
bool barrier(unsigned Op) {
  return (Op >= 46 && Op <= 86) || Op == 108 || Op == 109 || Op == 112 ||
         Op == 113 || Op == 132 || Op >= 153 || Op == 18 || Op == 19 ||
         Op == 20;
}
std::string condition(unsigned Op, ArrayRef<Text> V, bool Reverse) {
  static constexpr const char *Ops[]{"==", "!=", "<", ">=", ">", "<="};
  unsigned I = Op >= 198   ? Op - 198
               : Op >= 165 ? Op - 165
               : Op >= 159 ? Op - 159
                           : Op - 153;
  std::string B = V.size() == 2 ? V[1].S : Op >= 198 ? "null" : "0";
  return V[0].S + " " + Ops[Reverse ? (I ^ 1) : I] + " " + B;
}
class Printer {
  const Class &C;
  const Member &M;
  const Instructions &Code;
  const Flow &G;
  const std::vector<Instruction> &Rows;
  const neverd::jvm::Code &Body;
  Signature Sig;
  JavaBody Out;
  unsigned Line = 1, At = 0;
  size_t TextBytes = 0;
  std::map<int64_t, Text> Values;
  std::map<int64_t, unsigned> Uses, LastUse;
  std::vector<unsigned> Epoch;
  std::map<unsigned, std::pair<std::string, std::string>> Locals;
  std::set<unsigned> Parameters;
  std::map<unsigned, std::string> SharedTemps;
  std::vector<bool> Seen;
  std::vector<std::vector<Text>> Actions;
  std::vector<std::vector<Text>> Arguments;
  [[noreturn]] void fail(StringRef Why) const {
    unsupported(Body.At + At, "java_structure", Why.str());
  }
  void line(unsigned Depth, Text T) {
    if (Depth > limits::Nesting ||
        T.S.size() + 2 * Depth + 1 > limits::ReportBytes - TextBytes)
      fail("Java text or region nesting budget exceeded");
    TextBytes += T.S.size() + 2 * Depth + 1;
    Out.Text.append(2 * Depth, ' ');
    Out.Text += T.S;
    Out.Text += '\n';
    for (unsigned R : T.Rows)
      if (Out.Lines[R].empty() || Out.Lines[R].back() != Line)
        Out.Lines[R].push_back(Line);
    ++Line;
  }
  void line(unsigned D, std::string S) { line(D, Text{std::move(S), {}}); }
  bool reference(StringRef T) {
    return T.starts_with("L") || T.starts_with("[") || T == "null";
  }
  std::string sourceType(StringRef T) {
    if (T == "uninitializedThis")
      return dotted(C.Name, Body.At + At);
    if (T == "null")
      return "java.lang.Object";
    auto Base = T.ltrim('[');
    if (Base.starts_with("L"))
      dotted(Base.drop_front().drop_back(), Body.At + At);
    return javaType(T);
  }
  std::string local(unsigned Slot, StringRef T) {
    auto It = Locals.find(Slot);
    if (It == Locals.end()) {
      auto Name = "local" + std::to_string(Slot);
      if (T == "null" || T.starts_with("new@"))
        fail("Local lacks a unique declared Java type");
      Locals[Slot] = {T.str(), Name};
      return Name;
    }
    auto &Declared = It->second.first;
    if (verificationKind(Declared) != verificationKind(T) && T != "null" &&
        T != "uninitializedThis")
      fail("Local slot is reused with incompatible Java types");
    if (reference(T) && reference(Declared) && T != Declared && T != "null" &&
        Declared != "Ljava/lang/Object;")
      fail("Local slot needs distinct reference declarations");
    return It->second.second;
  }
  std::string literal(const Constant &K) {
    switch (K.Tag) {
    case 3:
      return std::to_string(int32_t(K.Bits));
    case 5:
      return std::to_string(int64_t(K.Bits)) + "L";
    case 4:
    case 6: {
      bool F = K.Tag == 4;
      double V = F ? double(std::bit_cast<float>(uint32_t(K.Bits)))
                   : std::bit_cast<double>(K.Bits);
      if (!std::isfinite(V))
        fail("Nonfinite constant needs its exact bit-level construction");
      char Buffer[64];
      auto R = std::to_chars(Buffer, Buffer + sizeof(Buffer), V);
      if (R.ec != std::errc())
        fail("Cannot express floating-point constant");
      return std::string(Buffer, R.ptr) + (F ? "f" : "d");
    }
    case 8: {
      auto &S = C.Pool[K.A];
      if (S.Escaped)
        fail("String contains an unpaired UTF-16 surrogate");
      return quoted(S.Text);
    }
    case 7:
      return dotted(C.text(K.A, K.At), K.At) + ".class";
    default:
      fail("Constant requires linkage or bootstrap evidence");
    }
  }
  Text get(const Value &V, unsigned Row) {
    if (V.Type == "uninitializedThis")
      return {"this", {Row}};
    if (auto I = Values.find(V.Id); I != Values.end())
      return I->second;
    fail("Stack value crosses an unstructured source boundary");
  }
  std::string displayMethod(const Member &Method) const {
    if ((Method.Flags & (0x1000 | 0x0002 | 0x0008)) !=
            (0x1000 | 0x0002 | 0x0008) ||
        !StringRef(Method.Name).starts_with("lambda$"))
      return Method.Name;
    auto N = "recoveredLambda$" + std::to_string(&Method - C.Methods.data());
    while (llvm::any_of(C.Methods,
                        [&](const Member &Other) { return Other.Name == N; }))
      N += '$';
    return N;
  }
  std::string dynamic(const Instruction &R, ArrayRef<Text> V, unsigned B,
                      unsigned Row) {
    const auto &Site = C.Pool[R.Pool], &NT = C.nameAndType(Site);
    auto Call = methodDescriptor(C.text(NT.B, Site.At), Site.At);
    const auto &Boot = C.Bootstraps[Site.A];
    const auto &Handle = C.Pool[Boot.Handle], &Target = C.Pool[Handle.B],
               &TargetNT = C.nameAndType(Target);
    if (Handle.A != 6 || Target.Tag != 10)
      fail("Bootstrap is not a qualified static factory");
    const auto &Owner = C.className(Target.A, Site.At),
               &Name = C.text(TargetNT.A, Site.At),
               &Desc = C.text(TargetNT.B, Site.At);
    if (Owner == "java/lang/invoke/StringConcatFactory" &&
        Name == "makeConcatWithConstants" &&
        Desc == "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/"
                "String;Ljava/lang/invoke/MethodType;Ljava/lang/String;[Ljava/"
                "lang/Object;)Ljava/lang/invoke/CallSite;" &&
        Call.Result == "Ljava/lang/String;" && !Boot.Arguments.empty() &&
        C.Pool[Boot.Arguments[0]].Tag == 8) {
      auto &String = C.Pool[C.Pool[Boot.Arguments[0]].A];
      if (String.Escaped)
        fail("Concatenation recipe has an unpaired surrogate");
      unsigned Arg = 0, Constant = 1;
      std::string Expr = "(\"\"", Literal;
      auto flush = [&] {
        if (!Literal.empty()) {
          Expr += " + " + quoted(Literal);
          Literal.clear();
        }
      };
      for (char Ch : String.Text) {
        if (Ch != 1 && Ch != 2) {
          Literal += Ch;
          continue;
        }
        flush();
        if (Ch == 2) {
          if (Constant >= Boot.Arguments.size())
            fail("Concatenation recipe exceeds its constants");
          auto &K = C.Pool[Boot.Arguments[Constant++]];
          if (K.Tag != 8)
            fail("Concatenation constant conversion needs linkage evidence");
          Expr += " + " + literal(K);
          continue;
        }
        if (Arg >= V.size())
          fail("Concatenation recipe exceeds its argument list");
        auto &T = Call.Arguments[Arg];
        auto E = V[Arg++].S;
        if (reference(T) && T != "Ljava/lang/String;")
          fail("Concatenation would move an observable reference conversion");
        if (T == "Z")
          E = "(" + E + " != 0)";
        else if (T == "C")
          E = "((char) " + E + ")";
        Expr += " + " + E;
      }
      flush();
      if (Arg != V.size() || Constant != Boot.Arguments.size())
        fail("Unused concatenation bootstrap arguments");
      return Expr + ")";
    }
    if (Owner == "java/lang/invoke/LambdaMetafactory" &&
        Name == "metafactory" &&
        Desc == "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/"
                "String;Ljava/lang/invoke/MethodType;Ljava/lang/invoke/"
                "MethodType;Ljava/lang/invoke/MethodHandle;Ljava/lang/invoke/"
                "MethodType;)Ljava/lang/invoke/CallSite;" &&
        Call.Result == "Ljava/util/function/IntUnaryOperator;" &&
        C.text(NT.A, Site.At) == "applyAsInt" && Boot.Arguments.size() == 3) {
      const auto &Sam = C.Pool[Boot.Arguments[0]],
                 &Impl = C.Pool[Boot.Arguments[1]],
                 &Inst = C.Pool[Boot.Arguments[2]];
      if (Sam.Tag != 16 || C.text(Sam.A, Site.At) != "(I)I" || Inst.Tag != 16 ||
          C.text(Inst.A, Site.At) != "(I)I" || Impl.Tag != 15 || Impl.A != 6)
        fail("Lambda adaptation is outside the qualified SAM shape");
      const auto &K = C.Pool[Impl.B], &N = C.nameAndType(K);
      if (K.Tag != 10 || C.className(K.A, K.At) != C.Name)
        fail("Lambda implementation is outside the selected class");
      auto &ImplName = C.text(N.A, N.At), &ImplDesc = C.text(N.B, N.At);
      auto Expected = "(" + std::string(V.size() + 1, 'I') + ")I";
      if (ImplDesc != Expected ||
          !llvm::all_of(Call.Arguments, [](auto &T) { return T == "I"; }))
        fail("Lambda capture types disagree with the implementation handle");
      auto F = llvm::find_if(C.Methods, [&](const Member &Method) {
        return Method.Name == ImplName && Method.Desc == ImplDesc;
      });
      if (F == C.Methods.end() || !(F->Flags & 8) || !identifier(F->Name))
        fail("Lambda target method is unavailable");
      std::string Expr = "(lambdaArg -> " + dotted(C.Name, Site.At) + "." +
                         displayMethod(*F) + "(";
      for (unsigned I = 0; I < V.size(); ++I) {
        std::string Capture =
            "capture" + std::to_string(R.PC) + "_" + std::to_string(I);
        Actions[B].push_back(combine(
            "final int " + Capture + " = " + V[I].S + ";", {V[I]}, Row));
        Expr += Capture + ", ";
      }
      return Expr + "lambdaArg))";
    }
    fail("Unrecognized dynamic bootstrap");
  }
  void expressions() {
    unsigned E = 0;
    for (unsigned I = 0; I < Rows.size(); ++I) {
      Epoch.push_back(E);
      if (barrier(Rows[I].Opcode))
        ++E;
      if (!G.Blocks[G.BlockForRow[I]].Entry)
        continue;
      unsigned Op = Rows[I].Opcode;
      if ((Op >= 21 && Op <= 45) || Op == 132)
        continue;
      for (auto &V : G.Ops[I].Inputs) {
        ++Uses[V.Id];
        LastUse[V.Id] = I;
      }
    }
    for (unsigned B = 0; B < G.Blocks.size(); ++B) {
      auto &Block = G.Blocks[B];
      if (!Block.Entry)
        continue;
      for (auto &V : Block.Entry->Stack)
        if (V.Id <= FirstExceptionId)
          Values[V.Id] = {"caught", {}};
      for (unsigned I = Block.Begin; I < Block.End; ++I) {
        auto &R = Rows[I];
        unsigned Op = R.Opcode;
        At = R.PC;
        auto &Node = G.Ops[I];
        std::vector<Text> V;
        if (!(Op >= 21 && Op <= 45) && Op != 132)
          for (auto &In : Node.Inputs)
            V.push_back(get(In, I));
        auto emit = [&](std::string S) {
          Actions[B].push_back(combine(std::move(S), V, I));
        };
        auto set = [&](std::string S, bool Literal = false) {
          Text X = combine(std::move(S), V, I);
          auto Id = Node.Result->Id;
          unsigned U = LastUse[Id];
          if (!Literal && !(Uses[Id] == 1 && U > I && G.BlockForRow[U] == B &&
                            (Rows[U].Opcode < 87 || Rows[U].Opcode > 95) &&
                            Epoch[U] == Epoch[I] + unsigned(barrier(Op)) &&
                            X.S.size() < limits::InlineExpressionBytes)) {
            auto N = "v" + std::to_string(R.PC);
            if (Node.Result->Type == "null")
              fail("Null temporary lacks a qualified Java type");
            auto Type = sourceType(Node.Result->Type);
            if (U && G.BlockForRow[U] != B) {
              SharedTemps.emplace(R.PC, Type);
              X.S = N + " = " + X.S + ";";
            } else
              X.S = Type + " " + N + " = " + X.S + ";";
            Actions[B].push_back(std::move(X));
            X = {N, {}};
          }
          Values[Id] = std::move(X);
        };
        if (Op == 0) {
          emit("");
          continue;
        }
        if (Op == 1) {
          set("null", true);
          continue;
        }
        if (Op >= 2 && Op <= 17) {
          if (Op <= 8)
            set(std::to_string(int(Op) - 3), true);
          else if (Op <= 10)
            set(std::to_string(Op - 9) + "L", true);
          else if (Op <= 13)
            set(std::to_string(Op - 11) + ".0f", true);
          else if (Op <= 15)
            set(std::to_string(Op - 14) + ".0d", true);
          else
            set(std::to_string(R.Operand), true);
        } else if (Op >= 18 && Op <= 20) {
          auto &K = C.Pool[R.Pool];
          set(literal(K), K.Tag >= 3 && K.Tag <= 6);
        } else if (Op >= 21 && Op <= 45) {
          auto Type = Node.Result->Type;
          if (Type == "uninitializedThis") {
            Values[Node.Result->Id] = {"this", {I}};
            continue;
          }
          auto N = local(R.Local, Type);
          auto &Declared = Locals[R.Local].first;
          if (Declared == "Z")
            N = "(" + N + " ? 1 : 0)";
          set(N);
        } else if (Op >= 54 && Op <= 78) {
          auto N = local(R.Local, Node.Inputs[0].Type);
          if (N == "this")
            fail("Bytecode assigns to the receiver slot");
          auto RHS = V[0].S;
          auto &Decl = Locals[R.Local].first;
          if (Decl == "Z" || Decl == "B" || Decl == "C" || Decl == "S")
            fail(
                "Local reuse changes a narrow Java parameter into an int slot");
          emit(N + " = " + RHS + ";");
        } else if (Op == 132) {
          auto N = local(R.Local, "I");
          if (Locals[R.Local].first != "I")
            fail("Increment requires a proven Java int local");
          emit(N + (R.Increment == 1 ? "++;"
                    : R.Increment == -1
                        ? "--;"
                        : " += " + std::to_string(R.Increment) + ";"));
        } else if (Op >= 87 && Op <= 95) {
          emit("");
        } else if (Op >= 96 && Op <= 115) {
          constexpr StringLiteral Ops("+-*/%");
          set("(" + V[0].S + " " + std::string(1, Ops[(Op - 96) / 4]) + " " +
              V[1].S + ")");
        } else if (Op >= 116 && Op <= 119)
          set("(-" + V[0].S + ")");
        else if (Op >= 120 && Op <= 131) {
          static constexpr const char *Ops[]{"<<", ">>", ">>>", "&", "|", "^"};
          set("(" + V[0].S + " " + Ops[(Op - 120) / 2] + " " + V[1].S + ")");
        } else if (Op >= 133 && Op <= 147) {
          std::string T = Op == 145   ? "B"
                          : Op == 146 ? "C"
                          : Op == 147 ? "S"
                                      : Node.Result->Type;
          set("((" + sourceType(T) + ") " + V[0].S + ")");
        } else if (conditional(Op) || Op == 170 || Op == 171)
          Arguments[B] = std::move(V);
        else if (go(Op))
          continue;
        else if (ret(Op)) {
          if (Op == 177)
            emit("return;");
          else if (Op == 191)
            emit("throw " + V[0].S + ";");
          else {
            std::string Expr = V[0].S;
            if (Sig.Result == "Z")
              Expr = "((" + Expr + " & 1) != 0)";
            if (Sig.Result == "B" || Sig.Result == "C" || Sig.Result == "S")
              Expr = "(" + sourceType(Sig.Result) + ") (" + Expr + ")";
            emit("return " + Expr + ";");
          }
        } else if (Op == 187) {
          Values[Node.Result->Id] = {"", {I}};
        } else if (Op == 186) {
          set(dynamic(R, V, B, I));
        } else if (Op >= 178 && Op <= 185) {
          auto &K = C.Pool[R.Pool], &NT = C.nameAndType(K);
          const auto &N = C.text(NT.A, K.At), &Desc = C.text(NT.B, K.At),
                     &Owner = C.className(K.A, K.At);
          if (N != "<init>" && !identifier(N))
            fail("Member lacks a Java identifier");
          if (Op <= 181) {
            auto Base = Op == 178 || Op == 179 ? dotted(Owner, K.At) : V[0].S;
            if (Op == 178 || Op == 180) {
              auto Field = Base + "." + N;
              if (Desc == "Z")
                Field = "(" + Field + " ? 1 : 0)";
              set(std::move(Field));
            } else {
              auto Val = V.back().S;
              if (Desc == "Z")
                Val = "((" + Val + " & 1) != 0)";
              if (Desc == "C" || Desc == "S" || Desc == "B")
                Val = "(" + sourceType(Desc) + ") (" + Val + ")";
              emit(Base + "." + N + " = " + Val + ";");
            }
          } else {
            if (N == "<init>" &&
                StringRef(Node.Inputs[0].Type).starts_with("new@")) {
              bool Dup = I > 0 && Rows[I - 1].Opcode == 89;
              if (Desc != "()V" || I < unsigned(1 + Dup))
                fail("Allocation has an unqualified argument or alias order");
              unsigned Allocation = I - 1 - unsigned(Dup);
              if (Rows[Allocation].Opcode != 187 ||
                  G.BlockForRow[Allocation] != B ||
                  C.className(Rows[Allocation].Pool, Body.At + At) != Owner ||
                  Node.Inputs[0].Id != int64_t(Rows[Allocation].PC) + 1)
                fail("Allocation and constructor do not form one expression");
              auto Type = dotted(Owner, K.At);
              auto Spell = "new " + Type + "()";
              if (Dup) {
                auto Temp = "v" + std::to_string(Rows[Allocation].PC);
                auto U = LastUse[Node.Inputs[0].Id];
                if (U && G.BlockForRow[U] != B) {
                  SharedTemps.emplace(Rows[Allocation].PC, Type);
                  emit(Temp + " = " + Spell + ";");
                } else
                  emit(Type + " " + Temp + " = " + Spell + ";");
                Values[Node.Inputs[0].Id] = {std::move(Temp), {}};
              } else
                emit(Spell + ";");
              continue;
            }
            if (N == "<init>" && (M.Name != "<init>" || V[0].S != "this" ||
                                  !Actions[B].empty() || B != 0))
              fail("Constructor call needs allocation or ordering evidence");
            if (Op == 183 && N != "<init>" && Owner != C.Name)
              fail(
                  "Special method dispatch needs a qualified super invocation");
            std::string Name = N;
            if (Owner == C.Name && StringRef(N).starts_with("lambda$")) {
              auto Found = llvm::find_if(C.Methods, [&](const Member &Other) {
                return Other.Name == N && Other.Desc == Desc;
              });
              if (Found != C.Methods.end())
                Name = displayMethod(*Found);
            }
            std::string Call =
                N == "<init>"
                    ? (Owner == C.Name ? "this" : "super")
                    : (Op == 184 ? dotted(Owner, K.At) : V[0].S) + "." + Name;
            Call += '(';
            auto Called = methodDescriptor(Desc, K.At);
            for (unsigned A = 0; A < Called.Arguments.size(); ++A) {
              if (A)
                Call += ", ";
              auto &T = Called.Arguments[A];
              auto Arg = V[A + (Op != 184)].S;
              // Explicit argument types select the member identified by the
              // descriptor.
              Call += T == "Z" ? "(" + Arg + " != 0)"
                               : "(" + sourceType(T) + ") (" + Arg + ")";
            }
            Call += ')';
            if (Called.Result == "V")
              emit(Call + ";");
            else
              set(Called.Result == "Z" ? "(" + Call + " ? 1 : 0)" : Call);
          }
        } else if (Op == 190)
          set(V[0].S + ".length");
        else if (Op == 192)
          set("((" + dotted(C.className(R.Pool, Body.At + At), Body.At + At) +
              ") " + V[0].S + ")");
        else if (Op == 193)
          set("(" + V[0].S + " instanceof " +
              dotted(C.className(R.Pool, Body.At + At), Body.At + At) +
              " ? 1 : 0)");
        else
          fail("Instruction needs additional source-level evidence");
      }
    }
  }
  unsigned block(unsigned PC) const { return G.BlockForRow[Code.Index[PC]]; }
  unsigned last(unsigned B) const { return G.Blocks[B].End - 1; }
  void actions(unsigned B, unsigned D) {
    if (Seen[B])
      fail("Structured regions overlap");
    Seen[B] = true;
    for (auto &A : Actions[B])
      if (A.S.empty()) {
        for (auto I : A.Rows)
          Out.Lines[I].push_back(Line);
      } else
        line(D, A);
  }
  void region(unsigned First, unsigned End, unsigned D,
              std::optional<unsigned> Back = {}) {
    if (D > limits::Nesting)
      fail("Structured region nesting exceeds limit");
    for (unsigned B = First; B < End;) {
      auto &Blk = G.Blocks[B];
      if (!Blk.Entry) {
        ++B;
        continue;
      }
      unsigned I = last(B);
      auto &R = Rows[I];
      At = R.PC;
      if (go(R.Opcode)) {
        unsigned Test = block(R.Targets[0]);
        if (Test > B + 1 && Test < End) {
          unsigned Check = last(Test);
          const auto &Guard = Rows[Check];
          if (conditional(Guard.Opcode) && block(Guard.Targets[0]) == B + 1 &&
              block(Guard.Targets[1]) == Test + 1) {
            actions(B, D);
            if (Actions[Test].empty()) {
              actions(Test, D);
              auto Head = combine(
                  "while (" + condition(Guard.Opcode, Arguments[Test], false) +
                      ") {",
                  Arguments[Test], Check);
              Head.Rows.push_back(I);
              line(D, std::move(Head));
            } else {
              line(D, Text{"while (true) {", {I}});
              actions(Test, D + 1);
              line(D + 1,
                   combine("if (" +
                               condition(Guard.Opcode, Arguments[Test], true) +
                               ") break;",
                           Arguments[Test], Check));
            }
            region(B + 1, Test, D + 1);
            line(D, "}");
            B = Test + 1;
            continue;
          }
        }
      }
      if (conditional(R.Opcode)) {
        unsigned Taken = block(R.Targets[0]), Fall = block(R.Targets[1]);
        if (Fall != B + 1 || Taken <= B || Taken > End)
          fail("Conditional edges do not form a forward region");
        bool Loop = Taken > Fall && go(Rows[last(Taken - 1)].Opcode) &&
                    block(Rows[last(Taken - 1)].Targets[0]) == B;
        if (Loop) {
          if (Actions[B].empty()) {
            actions(B, D);
            line(D, combine("while (" +
                                condition(R.Opcode, Arguments[B], true) + ") {",
                            Arguments[B], I));
          } else {
            line(D, "while (true) {");
            actions(B, D + 1);
            line(D + 1,
                 combine("if (" + condition(R.Opcode, Arguments[B], false) +
                             ") break;",
                         Arguments[B], I));
          }
          region(Fall, Taken, D + 1, B);
          line(D, "}");
          B = Taken;
          continue;
        }
        actions(B, D);
        line(D,
             combine("if (" + condition(R.Opcode, Arguments[B], true) + ") {",
                     Arguments[B], I));
        unsigned Join = Taken;
        if (Taken > Fall && go(Rows[last(Taken - 1)].Opcode)) {
          unsigned T = block(Rows[last(Taken - 1)].Targets[0]);
          if (T > Taken && T <= End)
            Join = T;
        }
        region(Fall, Taken, D + 1, Join);
        if (Join > Taken) {
          line(D, "} else {");
          region(Taken, Join, D + 1);
        }
        line(D, "}");
        B = Join;
        continue;
      }
      if (R.Opcode == 170 || R.Opcode == 171) {
        actions(B, D);
        line(D,
             combine("switch (" + Arguments[B][0].S + ") {", Arguments[B], I));
        std::map<unsigned, std::vector<std::string>> Labels;
        Labels[block(R.Targets[0])].push_back("default:");
        for (unsigned K = 0; K < R.Keys.size(); ++K)
          Labels[block(R.Targets[K + 1])].push_back(
              "case " + std::to_string(R.Keys[K]) + ":");
        unsigned Join = End;
        for (auto It = Labels.begin(); It != Labels.end(); ++It) {
          auto Next = std::next(It);
          if (Next == Labels.end())
            break;
          unsigned Tail = last(Next->first - 1);
          if (go(Rows[Tail].Opcode)) {
            unsigned T = block(Rows[Tail].Targets[0]);
            if (T > Labels.rbegin()->first && T <= End)
              Join = std::min(Join, T);
          }
        }
        if (Labels.begin()->first != B + 1 || Labels.rbegin()->first >= Join)
          fail("Switch entries cross their source region");
        for (auto It = Labels.begin(); It != Labels.end(); ++It) {
          auto Next = std::next(It);
          unsigned Limit = Next == Labels.end() ? Join : Next->first;
          for (auto &L : It->second)
            line(D + 1, L);
          auto &Tail = Rows[last(Limit - 1)];
          if (!ret(Tail.Opcode) &&
              !(go(Tail.Opcode) && block(Tail.Targets[0]) == Join))
            fail("Switch path has unresolved fallthrough");
          region(It->first, Limit, D + 2, Join);
          if (go(Tail.Opcode))
            line(D + 2, "break;");
        }
        line(D, "}");
        B = Join;
        continue;
      }
      actions(B, D);
      if (go(R.Opcode)) {
        unsigned T = block(R.Targets[0]);
        if (T == B + 1) {
          Out.Lines[I].push_back(Line);
          ++B;
          continue;
        }
        if (B + 1 == End && (T == End || (Back && T == *Back))) {
          Out.Lines[I].push_back(Line);
          return;
        }
        fail("Jump crosses a structured source boundary");
      }
      if (!ret(R.Opcode) &&
          (R.Targets.size() != 1 || block(R.Targets[0]) != B + 1))
        fail("Block requires unresolved control flow");
      ++B;
    }
  }

public:
  Printer(const Class &C, const Member &M, const Instructions &Code,
          const Flow &G)
      : C(C), M(M), Code(Code), G(G), Rows(Code.Rows), Body(*M.Body),
        Sig(methodDescriptor(M.Desc, M.At)), Seen(G.Blocks.size()),
        Actions(G.Blocks.size()), Arguments(G.Blocks.size()) {}
  JavaBody run() {
    if (M.Name == "<clinit>")
      fail("Class initializer requires a class-level source region");
    auto Owner = dotted(C.Name, Body.At);
    auto Short = StringRef(Owner).rsplit('.').second;
    if (Short.empty())
      Short = Owner;
    if (M.Name != "<init>" && !identifier(M.Name))
      fail("Method lacks a Java identifier");
    std::string Head = M.Flags & 1   ? "public "
                       : M.Flags & 2 ? "private "
                       : M.Flags & 4 ? "protected "
                                     : "";
    unsigned Slot = 0;
    if (M.Flags & 8)
      Head += "static ";
    else {
      Locals[Slot] = {"L" + C.Name + ";", "this"};
      Parameters.insert(Slot++);
    }
    if (M.Flags & 0x20)
      Head += "synchronized ";
    if (M.Flags & 0x800)
      Head += "strictfp ";
    Head += M.Name == "<init>"
                ? Short.str()
                : sourceType(Sig.Result) + " " + displayMethod(M);
    Head += '(';
    for (unsigned I = 0; I < Sig.Arguments.size(); ++I) {
      if (I)
        Head += ", ";
      auto &T = Sig.Arguments[I];
      auto Name = "arg" + std::to_string(I);
      Head += sourceType(T) + " " + Name;
      Locals[Slot] = {T, Name};
      Parameters.insert(Slot);
      Slot += slots(T);
    }
    Head += ')';
    for (auto &A : M.Attributes)
      if (auto *V = A.getAsObject()->getArray("classes");
          V && A.getAsObject()->getString("name") == "Exceptions") {
        bool First = true;
        for (auto &T : *V) {
          Head += First ? " throws " : ", ";
          Head += dotted(*T.getAsString(), M.At);
          First = false;
        }
      }
    Out.Lines.resize(Rows.size());
    expressions();
    line(0, Head + " {");
    if (M.Name == "<init>" && !C.Super.empty()) {
      if (Actions[0].empty() ||
          !(StringRef(Actions[0].front().S).starts_with("super(") ||
            StringRef(Actions[0].front().S).starts_with("this(")))
        fail("Constructor invocation must precede local declarations");
      line(1, std::move(Actions[0].front()));
      Actions[0].erase(Actions[0].begin());
    }
    for (auto &[S, L] : Locals)
      if (!Parameters.count(S))
        line(1, sourceType(L.first) + " " + L.second + ";");
    for (auto &[PC, Type] : SharedTemps)
      line(1, Type + " v" + std::to_string(PC) + ";");
    if (Body.Handlers.empty())
      region(0, G.Blocks.size(), 1);
    else {
      if (Body.Handlers.size() != 1)
        fail("Overlapping exception priority requires another source region");
      auto &H = Body.Handlers[0];
      if (H.Start != 0 || !H.Catch || H.End > H.Target || !H.Target)
        fail("Exception table needs a qualified typed catch");
      for (auto &R : Rows)
        if (R.PC >= H.End && R.PC < H.Target && !ret(R.Opcode) && !go(R.Opcode))
          fail("Moving the catch boundary would include new effects");
      unsigned Handler = block(H.Target);
      if (!ret(Rows[last(Handler - 1)].Opcode))
        fail("Try region needs a shared exit proof");
      line(1, "try {");
      region(0, Handler, 2);
      line(1, "} catch (" + dotted(C.className(H.Catch, Body.At), Body.At) +
                  " caught) {");
      region(Handler, G.Blocks.size(), 2);
      line(1, "}");
    }
    for (unsigned B = 0; B < Seen.size(); ++B)
      if (G.Blocks[B].Entry && !Seen[B])
        fail("Reachable block is outside the rendered regions");
    line(0, "}");
    return std::move(Out);
  }
};
} // namespace
JavaBody renderJava(const Class &C, const Member &M, const Instructions &Code,
                    const Flow &G) {
  return Printer(C, M, Code, G).run();
}
} // namespace neverd::jvm
