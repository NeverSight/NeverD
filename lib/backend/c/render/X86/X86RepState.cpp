//===- X86RepState.cpp - REP completion in both C routes -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Validate owned LLVM interfaces before substituting a native builtin.
/// Source offsets, full registers and comparison flags stay in one operation.
//===----------------------------------------------------------------------===//

#include "X86RepState.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Support/ModRef.h"

#include <stdexcept>
#include <utility>

namespace neverd {
namespace {

std::string wordType(const X86RepAsm &P) {
  return "uint" + std::to_string(P.WordBytes * 8) + "_t";
}
std::string localPrefix(const std::vector<std::string> &Arguments,
                        const std::vector<std::string> &Outputs = {}) {
  std::string Prefix = "neverd_rep_";
  for (;;) {
    bool Collision = false;
    for (const auto *List : {&Arguments, &Outputs})
      for (const auto &Text : *List)
        Collision |= Text.find(Prefix) != std::string::npos;
    if (!Collision)
      return Prefix;
    Prefix += '_';
  }
}
std::string declarations(const X86RepAsm &P,
                         const std::vector<std::string> &Outputs) {
  std::string Text;
  for (const auto &Name : Outputs)
    Text += wordType(P) + " " + Name + "; ";
  return Text;
}
std::string gccConstraint(std::string C) {
  for (auto [Fixed, Letter] : {std::pair{"{ax}", "a"},
                               {"{cx}", "c"},
                               {"{dx}", "d"},
                               {"{si}", "S"},
                               {"{di}", "D"}})
    if (const auto At = C.find(Fixed); At != std::string::npos)
      C.replace(At, 4, Letter);
  return C;
}
std::string gccText(const std::string &Asm) {
  std::string Text;
  for (size_t I = 0; I < Asm.size(); ++I) {
    const char C = Asm[I];
    if (C == '$' && I + 1 < Asm.size() && Asm[I + 1] == '$') {
      Text += '$';
      ++I;
    } else
      Text += C == '%'    ? "%%"
              : C == '$'  ? "%"
              : C == '\n' ? "\\n"
              : C == '\t' ? "\\t"
                          : std::string(1, C);
  }
  return Text;
}
std::string fallback(const X86RepAsm &P,
                     const std::vector<std::string> &Outputs,
                     const std::vector<std::string> &Arguments) {
  const auto Prefix = localPrefix(Arguments, Outputs) + "asm_";
  std::vector<std::string> Locals;
  for (unsigned I = 0; I < P.OutputConstraints.size(); ++I)
    Locals.push_back(Prefix + std::to_string(I));
  std::string Text = declarations(P, Locals) + "__asm__ volatile(\"" +
                     gccText(P.Text) + "\" : ";
  for (unsigned I = 0; I < Locals.size(); ++I) {
    if (I)
      Text += ", ";
    Text +=
        "\"" + gccConstraint(P.OutputConstraints[I]) + "\"(" + Locals[I] + ")";
  }
  Text += " : ";
  for (unsigned I = 0; I < Arguments.size(); ++I) {
    if (I)
      Text += ", ";
    const auto Type = I == 3                      ? "uint32_t"
                      : I == 2 && P.Contract.Port ? "uint16_t"
                                                  : wordType(P);
    Text += "\"" + gccConstraint(P.InputConstraints[I]) + "\"((" + Type + ")(" +
            Arguments[I] + "))";
  }
  Text += " : \"cc\", \"memory\"); ";
  for (unsigned I = 0; I < Outputs.size(); ++I)
    Text += Outputs[I] + " = " + Locals[I] + "; ";
  return Text;
}
std::string native(const X86RepAsm &P, const std::vector<std::string> &Outputs,
                   const std::vector<std::string> &Arguments) {
  const auto Prefix = localPrefix(Arguments, Outputs);
  const auto Array = Prefix + "state";
  const auto Flags = Prefix + "flags";
  const bool Scratch =
      P.Id == Intrinsic::X86RepScasState && Outputs.size() == 4;
  auto Inputs = Arguments;
  std::string Text = "unsigned long long " + Array + "[" +
                     std::to_string(P.Contract.WordResults) + "]; ";
  if (Scratch) {
    Inputs[2] = Prefix + "accumulator";
    Text += "unsigned long long " + Inputs[2] + " = (unsigned long long)(" +
            Arguments[2] + "); ";
  }
  if (P.Contract.Compare)
    Text += "unsigned short " + Flags + "; ";
  Text += "__builtin_ia32_rep_" + std::string(P.Contract.Name) + "_state(";
  for (unsigned I = 0; I < Arguments.size(); ++I) {
    const char *Type = I == 3 ? "unsigned char"
                       : I == 4 || (I == 2 && P.Contract.Port)
                           ? "unsigned short"
                           : "unsigned long long";
    // Owned assembly takes an i32 direction condition. Normalize before
    // narrowing to the native API's byte so values such as 256 stay true.
    Text += "(" + std::string(Type) + ")(" +
            (I == 3 ? "(" + Inputs[I] + ") != 0" : Inputs[I]) + "), ";
  }
  Text += std::to_string(P.Control.ElementBytes) + ", " +
          std::to_string(P.Control.AddressBytes * 8) + ", ";
  if (P.Contract.Source)
    Text += std::to_string(P.Control.Segment) + ", ";
  if (P.Contract.Compare)
    Text += std::to_string(P.Control.NotEqual) + ", ";
  Text += Array + (P.Contract.Compare ? ", &" + Flags : "") + "); ";
  for (unsigned I = 0; I < P.Contract.WordResults; ++I)
    Text += Outputs[I] + " = " + Array + "[" + std::to_string(I) + "]; ";
  if (P.Contract.Compare)
    Text += Outputs[P.Contract.WordResults] + " = " + Flags + "; ";
  // The owned SCAS fallback exposes its scratch DX as a fourth LLVM result.
  if (Scratch)
    Text += Outputs[3] + " = (" + wordType(P) + ")(" + Inputs[2] + "); ";
  return Text;
}
std::string dispatch(const X86RepAsm &P,
                     const std::vector<std::string> &Outputs,
                     const std::vector<std::string> &Arguments) {
  const auto Asm = fallback(P, Outputs, Arguments);
  if (P.WordBytes != 8)
    return Asm;
  return "NEVERD_X86_REP(({ " + native(P, Outputs, Arguments) + "}), ({ " +
         Asm + "})); ";
}

uint32_t constant(const llvm::CallInst &Call, unsigned Index) {
  if (Index >= Call.arg_size())
    throw std::invalid_argument("missing REP native control");
  const auto *V = llvm::dyn_cast<llvm::ConstantInt>(Call.getArgOperand(Index));
  if (!V || !V->getType()->isIntegerTy(32))
    throw std::invalid_argument("REP controls must be constant i32 values");
  return static_cast<uint32_t>(V->getZExtValue());
}

} // namespace

std::optional<Intrinsic> x86RepNativeId(std::string_view Name, Arch A) {
  if (A != Arch::X86 && A != Arch::X64)
    return std::nullopt;
#define NEVERD_X86_REP_STATE(Id, ...)                                          \
  if (Name == x86RepNativeName(Intrinsic::Id, A))                              \
    return Intrinsic::Id;
#include "neverd/ir/intrinsics/X86/X86RepState.def"
#undef NEVERD_X86_REP_STATE
  return std::nullopt;
}

std::optional<X86RepAsm> x86RepCallPlan(Arch A, const llvm::CallInst &Call) {
  const auto *F = Call.getCalledFunction();
  if (F && F->getName().starts_with("llvm.x86.rep.")) {
    const auto Id = x86RepNativeId(F->getName().str(), A);
    const auto ID = llvm::Intrinsic::lookupIntrinsicID(F->getName());
    if (!Id || !ID || !F->isDeclaration() || F->getIntrinsicID() != ID ||
        F->getFunctionType() !=
            llvm::Intrinsic::getType(Call.getContext(), ID) ||
        Call.getFunctionType() != F->getFunctionType() ||
        Call.getNumOperandBundles() ||
        Call.getMemoryEffects() != llvm::MemoryEffects::unknown() ||
        Call.doesNotReturn())
      throw std::invalid_argument("invalid REP native type, mode or effects");
    for (unsigned I = 0; I < Call.arg_size(); ++I)
      if (Call.getArgOperand(I)->getType() !=
          F->getFunctionType()->getParamType(I))
        throw std::invalid_argument("invalid REP native operand type");
    const auto C = *x86RepContract(*Id);
    unsigned Index = C.Compare ? 5 : 4;
    const auto Element = constant(Call, Index++);
    const auto Address = constant(Call, Index++);
    const auto Segment = C.Source ? constant(Call, Index++) : 0;
    const auto Repeat = C.Compare ? constant(Call, Index++) : 0;
    const auto Descriptor =
        x86RepDescriptor(*Id, A, Element, Address / 8, Segment, Repeat != 0);
    if (Call.arg_size() != Index ||
        (Address != 16 && Address != 32 && Address != 64) || Repeat > 1 ||
        !Descriptor)
      throw std::invalid_argument("invalid REP native controls");
    return x86RepAsm(*Id, A, *Descriptor);
  }
  const auto *Asm = llvm::dyn_cast<llvm::InlineAsm>(Call.getCalledOperand());
  if (!Asm ||
      !llvm::StringRef(Asm->getAsmString()).starts_with(X86RepAsmMarker))
    return std::nullopt;
  llvm::StringRef Text(Asm->getAsmString());
  Text.consume_front(X86RepAsmMarker);
  if (!Text.contains(':'))
    throw std::invalid_argument("missing owned REP descriptor");
  const auto Name = Text.take_front(Text.find(':'));
  Text = Text.drop_front(Name.size() + 1);
  uint32_t Descriptor;
  if (Text.consumeInteger(10, Descriptor))
    throw std::invalid_argument("invalid owned REP descriptor");
  std::optional<X86RepAsm> P;
#define NEVERD_X86_REP_STATE(Id, Mnemonic, ...)                                \
  if (Name == Mnemonic)                                                        \
    P = x86RepAsm(Intrinsic::Id, A, Descriptor);
#include "neverd/ir/intrinsics/X86/X86RepState.def"
#undef NEVERD_X86_REP_STATE
  if (!P || P->Text != Asm->getAsmString() ||
      P->Constraints != Asm->getConstraintString() || !Asm->hasSideEffects() ||
      Asm->isAlignStack() || Asm->getDialect() != llvm::InlineAsm::AD_ATT ||
      Call.getNumOperandBundles() ||
      Call.getMemoryEffects() != llvm::MemoryEffects::unknown() ||
      Call.doesNotReturn())
    throw std::invalid_argument("invalid owned REP assembly contract");
  auto *Word = llvm::IntegerType::get(Call.getContext(), P->WordBytes * 8);
  const auto *Result = llvm::dyn_cast<llvm::StructType>(Call.getType());
  if (!Result || Result->isPacked() ||
      Result->getNumElements() != P->OutputConstraints.size() ||
      Call.arg_size() != P->InputConstraints.size())
    throw std::invalid_argument("invalid owned REP result shape");
  for (const auto *Type : Result->elements())
    if (Type != Word)
      throw std::invalid_argument("invalid owned REP result width");
  for (unsigned I = 0; I < Call.arg_size(); ++I) {
    const unsigned Bits = I == 3                       ? 32
                          : I == 2 && P->Contract.Port ? 16
                                                       : P->WordBytes * 8;
    if (!Call.getArgOperand(I)->getType()->isIntegerTy(Bits))
      throw std::invalid_argument("invalid owned REP input width");
  }
  return P;
}

std::string
renderX86RepExpression(Arch A, const HighExpr &E, const std::string &ResultType,
                       const std::function<std::string(const HighExpr &)> &Expr,
                       const std::function<std::string(const MedVar &)> &Var,
                       const std::function<bool(const MedVar &)> &Alive) {
  X86RepShape S;
  S.TargetArch = A;
  S.MemoryOrdering = E.MemoryOrdering;
  S.MemoryAddressSpace = E.MemoryAddressSpace;
  S.ValidId = E.Kind == ExprKind::Call && !E.IsIndirectCall &&
              !E.IndirectTarget && !E.SourceCallHint && !E.DoesNotReturn;
  S.WritableOutput = E.Type && (E.Type->Kind == NdTypeKind::Int ||
                                E.Type->Kind == NdTypeKind::Ptr);
  S.OutputSize = E.Type ? E.Type->Size : 0;
  S.Arguments = E.Operands.size();
  std::vector<std::string> Arguments;
  for (unsigned I = 0; I < S.Arguments && I < S.InputSizes.size(); ++I) {
    const auto &V = E.Operands[I];
    S.InputSizes[I] = V && V->Type ? V->Type->Size : 0;
    S.ScalarInputs[I] =
        V && V->Type &&
        (V->Type->Kind == NdTypeKind::Int || V->Type->Kind == NdTypeKind::Ptr);
    if (V)
      Arguments.push_back(Expr(*V));
  }
  if (S.Arguments == S.InputSizes.size() && E.Operands.back() &&
      E.Operands.back()->Kind == ExprKind::Const &&
      E.Operands.back()->ConstVal <= UINT32_MAX)
    S.Descriptor = static_cast<uint32_t>(E.Operands.back()->ConstVal);
  const auto C = x86RepContract(E.IntrinsicId);
  if (!C || E.IntrinsicOutputs.size() != C->auxiliaryOutputs() ||
      !x86RepShapeIsValid(E.IntrinsicId, S))
    throw std::invalid_argument("invalid HighC REP completion contract");
  const auto P = *x86RepAsm(E.IntrinsicId, A, *S.Descriptor);
  Arguments.pop_back();
  const auto StatusValue = Arguments.back();
  std::vector<std::string> Existing;
  for (const auto &V : E.IntrinsicOutputs)
    if (Alive(V))
      Existing.push_back(Var(V));
  const auto Prefix = localPrefix(Arguments, Existing);
  const auto Status = Prefix + "status";
  Arguments.back() =
      "((" + Status + " >> " + std::to_string(X86RepDirectionShift) + ") & 1)";
  if (C->Compare)
    Arguments.push_back("(" + Status + " & " + std::to_string(X86RepFlagsMask) +
                        ")");
  std::vector<std::string> Outputs;
  for (unsigned I = 0; I < C->WordResults + C->Compare; ++I)
    Outputs.push_back(Prefix + std::to_string(I));
  std::string Text = "({ uint32_t " + Status + " = (uint32_t)(" + StatusValue +
                     "); " + declarations(P, Outputs) +
                     dispatch(P, Outputs, Arguments);
  for (unsigned I = 0; I < E.IntrinsicOutputs.size(); ++I) {
    const auto &V = E.IntrinsicOutputs[I];
    if (V.Kind != MedVar::Temp || V.Id < 0 ||
        V.Size != (I + 1 < C->WordResults ? P.WordBytes : X86RepFlagBytes))
      throw std::invalid_argument("invalid HighC REP auxiliary result");
    for (unsigned J = 0; J < I; ++J)
      if (E.IntrinsicOutputs[J].Id == V.Id)
        throw std::invalid_argument("aliased HighC REP auxiliary results");
    if (Alive(V))
      Text += Var(V) + " = " + Outputs[I + 1] + "; ";
  }
  return Text + "(" + ResultType + ")(uintptr_t)" + Outputs[0] + "; })";
}

std::optional<std::string>
renderX86RepCall(Arch A, const llvm::CallInst &Call, const std::string &Result,
                 bool ResultLive, const std::vector<std::string> &Arguments) {
  const auto P = x86RepCallPlan(A, Call);
  if (!P)
    return std::nullopt;
  if (Arguments.size() != Call.arg_size() || (ResultLive && Result.empty()))
    throw std::invalid_argument("invalid REP source operands or result");
  const bool Native = Call.getCalledFunction();
  const unsigned Count = Native ? P->Contract.WordResults + P->Contract.Compare
                                : P->OutputConstraints.size();
  const auto Prefix = localPrefix(Arguments, {Result});
  std::vector<std::string> Outputs;
  for (unsigned I = 0; I < Count; ++I)
    Outputs.push_back(ResultLive ? Result + ".field_" + std::to_string(I)
                                 : Prefix + std::to_string(I));
  const unsigned Inputs = P->Contract.Compare ? 5 : 4;
  const std::vector<std::string> Values(Arguments.begin(),
                                        Arguments.begin() + Inputs);
  return "{ " + (ResultLive ? "" : declarations(*P, Outputs)) +
         dispatch(*P, Outputs, Values) + "}\n";
}

} // namespace neverd
