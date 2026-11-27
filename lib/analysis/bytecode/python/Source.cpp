//===- Source.cpp - Structured CPython source and navigation --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Structure only regions whose normal transfers, stack consumption and
/// exceptional exits have been accounted for. Keep sibling results independent.
//===----------------------------------------------------------------------===//

#include "SourceDetail.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"

#include <set>

using namespace llvm;
using namespace neverd::python;

bool Code::isModule() const {
  return Name->Data == "<module>" ||
         (Id == 0 && (Version == 8 || (Version < 205 && Name->Data == "?")));
}

Emitter::Emitter(Job &J, Function &F, std::vector<Function> &All,
                 std::vector<Body> &Bodies)
    : J(J), F(F), C(*F.Node.Body), Functions(All), Bodies(Bodies),
      Result(Bodies[C.Id]), NextLine(C.isModule() ? 1 : 2),
      Module(C.isModule()) {}
void Emitter::reject(StringRef Why) const {
  unsupported(C.Bytes->DataOffset +
                  (Current < F.Code.size() ? F.Code[Current].Byte : 0),
              Why);
}
void Emitter::invalid(StringRef Why) const {
  malformed(C.Bytes->DataOffset +
                (Current < F.Code.size() ? F.Code[Current].Byte : 0),
            Why);
}
size_t Emitter::target(size_t P) const {
  return F.Starts[*F.Code[P].Target / (C.Version < 306 ? 1 : 2)];
}
size_t Emitter::normalized(size_t P) const {
  auto It = EquivalentExits.find(P);
  return It == EquivalentExits.end() ? P : It->second;
}
bool Emitter::at(size_t P, Op O) const {
  return P < F.Code.size() && F.Code[P].Opcode == O;
}
bool Emitter::same(size_t A, size_t B, size_t N) const {
  if (A > F.Code.size() || B > F.Code.size() || N > F.Code.size() - A ||
      N > F.Code.size() - B)
    return false;
  for (size_t I = 0; I < N; ++I) {
    J.charge(J.Comparisons, 1, limits::kRegionComparisons,
             C.Bytes->DataOffset + F.Code[A + I].Byte);
    const auto &X = F.Code[A + I], &Y = F.Code[B + I];
    if (X.Opcode != Y.Opcode || X.Arg != Y.Arg)
      return false;
    if (X.Target && normalized(target(A + I)) != normalized(target(B + I)))
      return false;
  }
  return true;
}
void Emitter::line(unsigned Indent, StringRef Text) {
  J.charge(J.Text, Indent * 4 + Text.size() + 1, limits::kTextBytes,
           C.Bytes->DataOffset);
  Result.Text.append(Indent * 4, ' ');
  Result.Text += Text;
  Result.Text += '\n';
  ++NextLine;
}

std::string Emitter::signature(const Code &Child, StringRef Name,
                               const Expr &Def) {
  if (!identifier(Name) || Def.Values.size() > Child.Args)
    reject("Unqualified function name or positional defaults");
  std::vector<std::string> Parts;
  std::set<std::string> Seen;
  auto Param = [&](unsigned N) {
    auto S = Child.Vars->Items[N]->Data.str();
    if (!identifier(S) || !Seen.insert(S).second)
      reject("Unqualified or duplicate parameter name");
    return S;
  };
  unsigned FirstDefault = Child.Args - Def.Values.size();
  for (unsigned N = 0; N < Child.Args; ++N) {
    auto S = Param(N);
    if (N >= FirstDefault)
      S += "=" + Def.Values[N - FirstDefault];
    Parts.push_back(std::move(S));
    if (N + 1 == Child.PosOnly)
      Parts.emplace_back("/");
  }
  unsigned Extra = Child.Args + Child.KwOnly;
  if (Child.Flags & 4)
    Parts.push_back("*" + Param(Extra++));
  else if (Child.KwOnly)
    Parts.emplace_back("*");
  unsigned Found = 0;
  for (unsigned N = 0; N < Child.KwOnly; ++N) {
    auto S = Param(Child.Args + N);
    if (auto It = Def.Keywords.find(S); It != Def.Keywords.end()) {
      S += "=" + It->second;
      ++Found;
    }
    Parts.push_back(std::move(S));
  }
  if (Found != Def.Keywords.size())
    reject("Keyword default outside parameter list");
  if (Child.Flags & 8)
    Parts.push_back("**" + Param(Extra));
  return std::string(Child.Flags & (0x80 | 0x200) ? "async def " : "def ") +
         Name.str() + "(" + join(Parts, ", ") + "):";
}
void Emitter::nested(unsigned Indent, const Expr &Def, StringRef Target) {
  auto &Child = *Def.Value->Body;
  if (Functions[Child.Id].Decoded && !Functions[Child.Id].Error &&
      Bodies[Child.Id].Text.empty() && !Bodies[Child.Id].ComprehensionBody)
    throw Failure{"disassembly_only",
                  C.Bytes->DataOffset + F.Code[Current].Byte,
                  "Nested function is available as disassembly"};
  if ((Child.Version != 8 && Child.Name->Data != Target) ||
      Functions[Child.Id].Error || Bodies[Child.Id].ComprehensionBody)
    reject("Nested definition has an unqualified body or binding");
  auto Header = signature(Child, Target, Def);
  auto &Body = Bodies[Child.Id].Text;
  Functions[Child.Id].Source = Header + "\n" + Body;
  Functions[Child.Id].BindingName = Target.str();
  line(Indent, Header);
  StringRef Tail = Body;
  while (!Tail.empty()) {
    auto [L, R] = Tail.split('\n');
    line(Indent, L);
    Tail = R;
  }
}
std::string Comprehension::apply(StringRef Iterable) const {
  return "[" + Element + " for " + Target + " in " + Iterable.str() +
         (Condition.empty() ? "" : " if " + Condition) + "]";
}
void Emitter::comprehension() {
  size_t End = F.Code.size();
  if (C.Args != 1 || C.Vars->Items[0]->Data != ".0" || End < 8 ||
      !at(0, Op::BUILD_LIST) || F.Code[0].Arg || !at(1, Op::LOAD_FAST) ||
      F.Code[1].Arg || !at(2, Op::FOR_ITER) || target(2) != End - 1 ||
      !at(3, Op::STORE_FAST) || !at(End - 3, Op::LIST_APPEND) ||
      F.Code[End - 3].Arg != 2 || !at(End - 2, Op::JUMP_ABSOLUTE) ||
      target(End - 2) != 2 || !at(End - 1, Op::RETURN_VALUE))
    reject("Unqualified comprehension control flow");
  Comprehension Spec;
  Spec.Target = name(F.Code[3]);
  size_t Start = 4;
  for (size_t P = Start; P < End - 3; ++P)
    if (at(P, Op::POP_JUMP_IF_FALSE)) {
      if (target(P) != 2 || !Spec.Condition.empty())
        reject("Unqualified comprehension filter");
      Spec.Condition = expressionRange(Start, P);
      Start = P + 1;
    }
  Spec.Element = expressionRange(Start, End - 3);
  Result.ComprehensionBody = Spec;
  std::string Arg = "_iterable";
  auto Present = [&](const Object *O) {
    return llvm::any_of(O->Items,
                        [&](const Object *V) { return V->Data == Arg; });
  };
  while (Present(C.Vars) || Present(C.Free) || Present(C.Names))
    Arg += "_";
  F.Source = "def _list_comprehension(" + Arg + "):\n    return " +
             Spec.apply(Arg) + "\n";
  std::fill(F.Lines.begin(), F.Lines.end(), 2);
}
void Emitter::block(size_t B, size_t E, unsigned Depth) {
  if (Depth > limits::kDepth)
    throw Failure{"budget_exceeded", C.Bytes->DataOffset,
                  "Structured source nesting limit exceeded"};
  Stack S;
  auto Suite = [&](size_t Start, size_t End) {
    auto Before = NextLine;
    block(Start, End, Depth + 1);
    if (Before == NextLine)
      line(Depth + 1, "pass");
  };
  auto Empty = [&] {
    if (!S.empty())
      reject("An unevaluated operand crosses a statement boundary");
  };
  for (size_t P = B; P < E; ++P) {
    J.check(C.Bytes->DataOffset + F.Code[P].Byte);
    Current = P;
    F.Lines[P] = NextLine;
    auto &I = F.Code[P];
    Op O = I.Opcode;
    if (expression(P, E, S)) {
      if (C.Version >= 105 && S.size() > C.Stack)
        invalid("Operands exceed declared stack size");
      continue;
    }
    switch (O) {
    case Op::NOP:
      break;
    case Op::GEN_START:
      if (P || !(C.Flags & (0x20 | 0x80 | 0x200)) ||
          I.Arg != ((C.Flags & 0x200)  ? 2u
                    : (C.Flags & 0x80) ? 1u
                                       : 0u))
        invalid("Invalid generator entry");
      break;
    case Op::STORE_FAST:
    case Op::STORE_NAME:
    case Op::STORE_GLOBAL:
    case Op::STORE_DEREF: {
      auto A = pop(S);
      auto N = name(I);
      if (A.Type == Expr::Imported) {
        if (S.size() != 1 || S.back().Type != Expr::Import || P + 1 >= E ||
            !at(P + 1, Op::POP_TOP))
          reject("Unqualified from-import sequence");
        auto M = pop(S);
        if (M.Values.size() != 1 || M.Values[0] != A.Text)
          reject("From-import list differs from bound name");
        line(Depth, "from " + M.Text + " import " + A.Text +
                        (A.Text == N ? "" : " as " + N));
        F.Lines[++P] = NextLine - 1;
        break;
      }
      Empty();
      if (A.Type == Expr::Augment) {
        if (A.Text != N)
          reject("In-place operation stores a different binding");
        line(Depth, N + " " + A.Extra);
      } else if (A.Type == Expr::Definition)
        nested(Depth, A, N);
      else if (A.Type == Expr::Import) {
        if (!A.Values.empty() || A.Text.starts_with("."))
          reject("Import form requires a from-list");
        auto Base = StringRef(A.Text).split('.').first;
        if (Base != A.Text && Base != N)
          reject("Dotted import alias has additional attribute steps");
        line(Depth, "import " + A.Text + (Base == N ? "" : " as " + N));
      } else
        line(Depth, N + " = " + text(std::move(A)));
      break;
    }
    case Op::IMPORT_NAME: {
      auto From = pop(S), Level = pop(S);
      if (Level.Type != Expr::Constant || Level.Value->Type != Object::Int ||
          Level.Value->Number < 0 || Level.Value->Number > 255)
        reject("Dynamic relative import level");
      StringRef Mod = C.Names->Items[I.Arg]->Data;
      SmallVector<StringRef> Parts;
      Mod.split(Parts, '.');
      if (!(Mod.empty() && Level.Value->Number) &&
          !llvm::all_of(Parts, identifier))
        reject("Invalid import module name");
      Expr M{Expr::Import, std::string(Level.Value->Number, '.') + Mod.str()};
      if (From.Type != Expr::Constant || From.Value->Type != Object::None)
        M.Values = tuple(std::move(From), true);
      S.push_back(std::move(M));
      break;
    }
    case Op::IMPORT_FROM:
      if (S.empty() || S.back().Type != Expr::Import)
        reject("Missing import module");
      S.push_back({Expr::Imported, name(I)});
      break;
    case Op::POP_TOP: {
      auto A = text(pop(S));
      Empty();
      line(Depth, A);
      break;
    }
    case Op::RETURN_VALUE: {
      auto A = pop(S);
      Empty();
      if (P + 1 != E)
        reject("Unclaimed instructions follow a return");
      if (Module) {
        if (A.Type != Expr::Constant || A.Value->Type != Object::None)
          reject("Non-None module return");
      } else if (C.Flags & 0x200) {
        if (A.Type != Expr::Constant || A.Value->Type != Object::None)
          invalid("Value return in async generator");
        line(Depth, "return");
      } else
        line(Depth, "return " + text(std::move(A)));
      break;
    }
    case Op::RAISE_VARARGS: {
      if (I.Arg > 2 || P + 1 != E)
        reject("Unqualified raise boundary");
      auto Values = args(S, I.Arg);
      Empty();
      line(Depth, Values.empty() ? "raise" : "raise " + join(Values, " from "));
      break;
    }
    case Op::POP_JUMP_IF_FALSE:
    case Op::POP_JUMP_IF_TRUE: {
      auto Cond = text(pop(S));
      Empty();
      size_t Else = normalized(target(P));
      if (Else <= P + 1 || Else > E)
        reject("Nonlocal conditional branch");
      line(Depth,
           "if " + (O == Op::POP_JUMP_IF_TRUE ? "not (" + Cond + ")" : Cond) +
               ":");
      size_t End = Else;
      if (at(Else - 1, Op::JUMP_FORWARD)) {
        End = normalized(target(Else - 1));
        if (End <= Else || End > E)
          reject("Unqualified conditional join");
        Suite(P + 1, Else - 1);
        line(Depth, "else:");
        Suite(Else, End);
      } else
        Suite(P + 1, Else);
      P = End - 1;
      break;
    }
    case Op::FOR_ITER: {
      auto It = pop(S);
      Empty();
      size_t End = target(P);
      if (It.Type != Expr::Iterator || End <= P + 3 || End > E ||
          !(at(P + 1, Op::STORE_FAST) || at(P + 1, Op::STORE_NAME)) ||
          !at(End - 1, Op::JUMP_ABSOLUTE) || target(End - 1) != P)
        reject("Unqualified for-loop latch");
      line(Depth, "for " + name(F.Code[P + 1]) + " in " + It.Text + ":");
      Suite(P + 2, End - 1);
      P = End - 1;
      break;
    }
    case Op::SETUP_WITH:
    case Op::SETUP_FINALLY:
      P = exceptionBlock(P, E, Depth, S) - 1;
      break;
    default:
      reject("Unqualified source operation " + mnemonic(O).str());
    }
  }
  Empty();
}
void Emitter::run() {
  F.Lines.assign(F.Code.size(), 0);
  if (C.Version >= 103 &&
      ((C.Flags & ~0x2ffu) ||
       (Module ? bool(C.Flags & 0x2bf) : (C.Flags & 3) != 3)))
    reject("Unqualified code-object flags or scope");
  if (C.Name->Data == "<listcomp>") {
    comprehension();
    return;
  }
  if (!Module && C.Version != 8 && !identifier(C.Name->Data))
    reject("Unqualified code-object kind");
  unsigned D = Module ? 0 : 1;
  if (!Module && !C.Consts->Items.empty() &&
      C.Consts->Items[0]->Type == Object::Unicode)
    line(D, literal(*C.Consts->Items[0]));
  std::set<std::string> Globals, Free;
  for (const auto &I : F.Code) {
    if (I.Opcode == Op::STORE_GLOBAL || I.Opcode == Op::DELETE_GLOBAL)
      Globals.insert(name(I));
    if ((I.Opcode == Op::STORE_DEREF || I.Opcode == Op::DELETE_DEREF) &&
        I.Arg >= C.Cells->Items.size())
      Free.insert(name(I));
  }
  if (!Globals.empty())
    line(D, "global " + join(Globals, ", "));
  if (!Free.empty())
    line(D, "nonlocal " + join(Free, ", "));
  block(0, F.Code.size(), D);
  if (Result.Text.empty())
    line(D, "pass");
  F.Source = Module ? Result.Text
                    : signature(C, C.Version == 8 ? "_function" : C.Name->Data,
                                Expr{}) +
                          "\n" + Result.Text;
  for (auto &L : F.Lines)
    if (L >= NextLine)
      L = 0;
}
void neverd::python::render(Job &J, std::vector<Function> &Functions,
                            unsigned Component) {
  std::vector<Body> Bodies(Functions.size());
  for (size_t N = Functions.size(); N; --N) {
    auto &F = Functions[N - 1];
    if (F.Error || (F.Node.Body->Version != 310 && !prepareSource(F)))
      continue;
    try {
      Emitter(J, F, Functions, Bodies).run();
    } catch (const Failure &E) {
      if (E.Status == "malformed" || E.Status == "unsupported")
        F.Error = E;
      else if (E.Status != "disassembly_only")
        throw;
      F.Source.clear();
      F.Lines.clear();
      Bodies[N - 1] = {};
    }
  }
  unsigned Base = J.Functions.size();
  auto &R = *J.Components[Component].getAsObject();
  R["root"] = Base;
  if (!J.Root)
    J.Root = Base;
  StringRef Overall = "reconstructed_source";
  for (Function &F : Functions) {
    auto &C = *F.Node.Body;
    json::Array Diagnostics;
    StringRef Status = "reconstructed_source";
    if (F.Error) {
      Status = F.Error->Status == "malformed" ? "malformed"
                                              : "partial_reconstruction";
      Diagnostics.emplace_back(json::Object{{"offset", F.Error->Offset},
                                            {"message", F.Error->Message}});
      if (Overall != "malformed")
        Overall = Status;
    }
    json::Object Row{{"id", Base + C.Id},
                     {"component", Component},
                     {"name", C.Name->Data.str()},
                     {"filename", C.Filename->Data.str()},
                     {"offset", F.Node.Offset},
                     {"end_offset", F.Node.EndOffset},
                     {"bytecode_offset", C.Bytes->DataOffset},
                     {"flags", C.Flags},
                     {"first_line", C.FirstLine},
                     {"status", Status},
                     {"decode_complete", F.Decoded},
                     {"diagnostics", std::move(Diagnostics)}};
    if (C.Version == 8) {
      Row["name"] = C.isModule()            ? "<module>"
                    : F.BindingName.empty() ? "<code>"
                                            : F.BindingName;
      Row["name_evidence"] =
          F.BindingName.empty() ? "unavailable" : "function_binding";
    }
    attachInstructionRows(J, F, Row);
    if (C.Version >= 311) {
      Row["exception_regions"] = std::move(F.ExceptionRegions);
      Row["qualified_name"] = C.QualifiedName->Data.str();
    }
    if (!F.Error && F.Source.empty()) {
      Row["status"] = "disassembly_only";
      if (Overall == "reconstructed_source")
        Overall = "disassembly_only";
    } else if (!F.Error) {
      Row["source"] = std::move(F.Source);
      if (!C.isModule() && C.Name->Data != "<listcomp>") {
        Row["signature_evidence"] =
            F.BindingName.empty() ? "code_parameters" : "function_binding";
        if (F.BindingName.empty())
          Row.getArray("diagnostics")
              ->emplace_back(json::Object{
                  {"message",
                   "The code object supplies parameter names. Default values "
                   "need the enclosing function binding."}});
      }
    }
    J.Functions.emplace_back(std::move(Row));
  }
  R["status"] = Overall;
}
