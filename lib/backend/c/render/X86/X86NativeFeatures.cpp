//===- X86NativeFeatures.cpp - Native C feature ownership ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Repeated target attributes do not merge. Collect instruction requirements
/// and emit exactly one attribute for the compiler interfaces in use.
///
//===----------------------------------------------------------------------===//

#include "X86NativeFeatures.h"

#include "X86APXAtomic.h"
#include "X86AmxState.h"
#include "X86BoundedLeaf.h"
#include "X86CompressExpand.h"
#include "X86FourSource.h"
#include "X86RepState.h"

#include "neverd/backend/llvm/X86/LLVMX86SHA.h"
#include "neverd/backend/llvm/X86/X86SgxNative.h"
#include "neverd/backend/llvm/X86/X86SystemStateNative.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"

#include <functional>
#include <set>

namespace neverd {
namespace {
std::string featureNames(const std::set<std::string> &Features) {
  std::string Text;
  for (const auto &Name : Features) {
    if (!Text.empty())
      Text += ',';
    Text += Name;
  }
  return Text;
}
std::string preamble(const std::set<std::string> &Amx, bool Sgx, bool Rep,
                     bool Compare, bool Bounded, bool BoundedEnclu,
                     const std::set<std::string> &Required = {}) {
  if (Amx.empty() && !Sgx && !Rep && !Bounded && Required.empty())
    return {};
  std::string Text;
  if (!Amx.empty())
    Text += "#ifndef NEVERD_X86_AMX\n"
            "#if defined(__NEVERC_X86_AMX_CONTROLS__) && "
            "__NEVERC_X86_AMX_CONTROLS__ >= 1\n"
            "#define NEVERD_X86_NATIVE_AMX 1\n"
            "#endif\n"
            "#if !defined(NEVERD_X86_NATIVE_AMX) && defined(__has_builtin)\n"
            "#if __has_builtin(__builtin_ia32_ldtilecfg_address) && "
            "__has_builtin(__builtin_ia32_tilemovrow_reg)\n"
            "#define NEVERD_X86_NATIVE_AMX 1\n"
            "#endif\n#endif\n"
            "#if defined(NEVERD_X86_NATIVE_AMX)\n"
            "#define NEVERD_X86_AMX(native, ...) native\n"
            "#else\n"
            "#define NEVERD_X86_AMX(native, ...) __VA_ARGS__\n"
            "#endif\n#endif\n";
  if (Bounded)
    Text += "#ifndef NEVERD_X86_BOUNDED_LEAF\n"
            "#if defined(__NEVERC_X86_BOUNDED_LEAF_STATE__) && "
            "__NEVERC_X86_BOUNDED_LEAF_STATE__ >= 1\n"
            "#define NEVERD_X86_NATIVE_BOUNDED_LEAF 1\n"
            "#define NEVERD_X86_BOUNDED_LEAF(native, ...) native\n"
            "#else\n#define NEVERD_X86_BOUNDED_LEAF(native, ...) __VA_ARGS__\n"
            "#endif\n#endif\n";
  if (Sgx)
    Text += "#ifndef NEVERD_X86_SGX\n"
            "#if defined(__NEVERC_X86_SGX_COMPLETION__) && "
            "__NEVERC_X86_SGX_COMPLETION__ >= 1\n"
            "#define NEVERD_X86_NATIVE_SGX 1\n"
            "#define NEVERD_X86_SGX(native, ...) native\n"
            "#else\n"
            "#define NEVERD_X86_SGX(native, ...) __VA_ARGS__\n"
            "#endif\n#endif\n";
  if (Rep)
    Text += "#ifndef NEVERD_X86_REP\n"
            "#if defined(__NEVERC_X86_REP_STATE__) && __NEVERC_X86_REP_STATE__ "
            ">= 1\n"
            "#define NEVERD_X86_REP(native, ...) native\n"
            "#else\n"
            "#define NEVERD_X86_REP(native, ...) __VA_ARGS__\n"
            "#endif\n#endif\n";
  const auto AmxNames = featureNames(Amx);
  const auto Attribute = [&](const std::string &Names) {
    auto All = featureNames(Required);
    if (!Names.empty())
      All += (All.empty() ? "" : ",") + Names;
    if (Compare)
      All += All.empty() ? "sahf" : ",sahf";
    return All.empty() ? std::string()
                       : "__attribute__((target(\"" + All + "\")))\n";
  };
  const auto End = Compare || !Required.empty()
                       ? "#else\n" + Attribute("") + "#endif\n"
                       : "#endif\n";
  std::string SgxCondition;
  if (Sgx)
    SgxCondition = "defined(NEVERD_X86_NATIVE_SGX)";
  if (BoundedEnclu)
    SgxCondition += (SgxCondition.empty() ? "" : " || ") +
                    std::string("defined(NEVERD_X86_NATIVE_BOUNDED_LEAF)");
  if (!Amx.empty() && !SgxCondition.empty())
    Text += "#if defined(NEVERD_X86_NATIVE_AMX) && (" + SgxCondition + ")\n" +
            Attribute(AmxNames + ",sgx") +
            "#elif defined(NEVERD_X86_NATIVE_AMX)\n" + Attribute(AmxNames) +
            "#elif " + SgxCondition + "\n" + Attribute("sgx") + End;
  else if (!Amx.empty())
    Text += "#if defined(NEVERD_X86_NATIVE_AMX)\n" + Attribute(AmxNames) + End;
  else if (!SgxCondition.empty())
    Text += "#if " + SgxCondition + "\n" + Attribute("sgx") + End;
  else if (Compare || !Required.empty())
    Text += Attribute("");
  return Text;
}
} // namespace

std::string x86NativeFunctionPreamble(const HighFunc &Function) {
  bool Atomic = false, Compact = false, FourSource = false;
  bool Sgx = false, Rep = false, Compare = false, Bounded = false,
       BoundedEnclu = false;
  std::set<std::string> Required;
  std::function<void(const ExprPtr &)> Visit = [&](const ExprPtr &E) {
    if (!E)
      return;
    Atomic |= isX86AtomicIntrinsic(E->IntrinsicId);
    if (isX86CompactMemory(E->IntrinsicId)) {
      Compact = true;
      // Target-independent source scanning validates its shape; the writer
      // separately validates the actual target architecture.
      const auto P = x86CompactHighPlan(Arch::X64, *E);
      for (const auto &Name : x86CompactSourceFeatures(P))
        Required.insert(Name);
    }
    if (E->IntrinsicId == Intrinsic::X86FourSourceMemory) {
      (void)x86FourSourceHighPlan(Arch::X64, *E);
      FourSource = true;
      Required.insert("avx512f");
    }
    Sgx |= isX86SgxIntrinsic(E->IntrinsicId);
    if (isX86BoundedLeafIntrinsic(E->IntrinsicId)) {
      Bounded = true;
      BoundedEnclu |= isX86EncluLeaf(E->IntrinsicId);
      Compare = true;
    }
    if (const auto C = x86RepContract(E->IntrinsicId)) {
      Rep = true;
      Compare |= C->Compare;
    }
    for (const auto &Operand : E->Operands)
      Visit(Operand);
  };
  walkStmts(Function.Body, [&](const HighStmt &S) { forEachExpr(S, Visit); });
  return std::string(Atomic ? X86APXAtomicCompilerDispatch : "") +
         (Compact ? X86CompactCompilerDispatch : "") +
         (FourSource ? X86FourSourceCompilerDispatch : "") +
         preamble(x86AmxFunctionFeatures(Function), Sgx, Rep, Compare, Bounded,
                  BoundedEnclu, Required);
}
std::string x86NativeFunctionPreamble(const llvm::Function &Function) {
  bool Atomic = false, Compact = false, FourSource = false;
  bool Sgx = false, Rep = false, Compare = false, Bounded = false,
       BoundedEnclu = false;
  std::set<std::string> Required;
  for (const auto &Block : Function)
    for (const auto &Instruction : Block) {
      const auto *Call = llvm::dyn_cast<llvm::CallInst>(&Instruction);
      if (!Call)
        continue;
      if (const auto *F = Call->getCalledFunction())
        Atomic |= bool(x86APXAtomicName(F->getName().str()));
      if (const auto *Asm =
              llvm::dyn_cast<llvm::InlineAsm>(Call->getCalledOperand()))
        Atomic |= llvm::StringRef(Asm->getAsmString())
                      .starts_with(X86APXAtomicAsmMarker);
      if (const auto P = x86CompactCallPlan(Arch::X64, *Call)) {
        Compact = true;
        for (const auto &Name : x86CompactSourceFeatures(*P))
          Required.insert(Name);
      }
      if (x86FourSourceCallPlan(Arch::X64, *Call)) {
        FourSource = true;
        Required.insert("avx512f");
      }
      if (llvmX86SHA256CName(*Call))
        Required.insert("sha");
      const auto *Result = llvm::dyn_cast<llvm::StructType>(Call->getType());
      const Arch RepArch = Result && Result->getNumElements() &&
                                   Result->getElementType(0)->isIntegerTy(64)
                               ? Arch::X64
                               : Arch::X86;
      if (const auto Id = x86BoundedLeafCallId(RepArch, *Call)) {
        Bounded = true;
        BoundedEnclu |= isX86EncluLeaf(*Id);
        Compare = true;
      }
      if (const auto P = x86RepCallPlan(RepArch, *Call)) {
        Rep = true;
        Compare |= P->Contract.Compare;
      }
      if (const auto *Callee = Call->getCalledFunction()) {
        Sgx |= isX86SgxNativeName(Callee->getName().str());
        if (const auto *Feature =
                x86SystemNativeTargetFeature(Callee->getName().str()))
          Required.insert(Feature);
      }
      if (const auto *Asm =
              llvm::dyn_cast<llvm::InlineAsm>(Call->getCalledOperand())) {
        const auto Text = llvm::StringRef(Asm->getAsmString()).trim();
        const auto Mnemonic = Text.substr(0, Text.find_first_of(" \t\r\n"));
        Sgx |= Mnemonic == "encls" || Mnemonic == "enclv";
        // An older SDK's owned assembly can select a native C builtin later.
        // Its feature requirement must survive that assembly representation.
        if (const auto *Feature =
                x86SystemNativeTargetFeature("llvm.x86." + Mnemonic.str()))
          Required.insert(Feature);
      }
    }
  return std::string(Atomic ? X86APXAtomicCompilerDispatch : "") +
         (Compact ? X86CompactCompilerDispatch : "") +
         (FourSource ? X86FourSourceCompilerDispatch : "") +
         preamble(x86AmxFunctionFeatures(Function), Sgx, Rep, Compare, Bounded,
                  BoundedEnclu, Required);
}
} // namespace neverd
