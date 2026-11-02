//===- X86AmxState.cpp - Preserve the native AMX bank in C -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Tile state stays architectural; only row results enter C value storage.
///
//===----------------------------------------------------------------------===//

#include "X86AmxState.h"

#include "neverd/backend/llvm/X86/X86AmxAsm.h"
#include "neverd/backend/llvm/X86/X86AmxNative.h"
#include "neverd/backend/llvm/X86/X86AmxStackLeaf.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"

#include <algorithm>
#include <functional>
#include <set>
#include <stdexcept>
namespace neverd {
namespace {
struct NativeCall {
  uint32_t Descriptor;
  NdMemoryAddressSpace Space = NdMemoryAddressSpace::Default;
  unsigned Prefix = 0;
  unsigned Base = 0;
  unsigned RowOrStride = 0;
};
std::optional<NativeCall> nativeCall(const llvm::CallInst &Call) {
  const auto *F = Call.getCalledFunction();
  if (!F)
    return {};
  const auto Descriptor = x86AmxNativeDescriptor(F->getName().str());
  if (!Descriptor)
    return {};
  const auto ID = llvm::Intrinsic::lookupIntrinsicID(F->getName());
  if (!ID || !F->isDeclaration() || F->getIntrinsicID() != ID ||
      F->getFunctionType() != llvm::Intrinsic::getType(Call.getContext(), ID) ||
      Call.getFunctionType() != F->getFunctionType() ||
      Call.arg_size() != F->arg_size() || Call.getNumOperandBundles() ||
      Call.onlyReadsMemory() || Call.onlyWritesMemory() || Call.doesNotReturn())
    throw std::invalid_argument("invalid AMX native type or effects");
  for (unsigned I = 0; I < Call.arg_size(); ++I)
    if (Call.getArgOperand(I)->getType() !=
        F->getFunctionType()->getParamType(I))
      throw std::invalid_argument("invalid AMX native operand type");
  auto Constant = [&](unsigned Index, uint64_t Maximum) -> unsigned {
    if (Index >= Call.arg_size())
      throw std::invalid_argument("missing AMX native control");
    const auto *C =
        llvm::dyn_cast<llvm::ConstantInt>(Call.getArgOperand(Index));
    if (!C || C->getValue().ugt(Maximum))
      throw std::invalid_argument("invalid AMX native constant control");
    return C->getZExtValue();
  };
  NativeCall P{*Descriptor};
  const auto K = x86AmxOperation(P.Descriptor);
  if (x86AmxIsTileMemory(K) || x86AmxIsRow(K) || x86AmxIsCompute(K) ||
      K == X86AmxOp::Zero)
    P.Descriptor = (P.Descriptor & ~(7u << 5)) | (Constant(0, 7) << 5);
  if (x86AmxIsMemory(K)) {
    P.Base = x86AmxIsTileMemory(K) ? 1 : 0;
    P.RowOrStride = 2;
    const unsigned WidthIndex = x86AmxIsTileMemory(K) ? 3 : 1;
    const unsigned Bits = Constant(WidthIndex, 64);
    P.Prefix = Constant(WidthIndex + 1, X86AmxNativeStackReference | UINT8_MAX);
    const unsigned Prefix = P.Prefix & UINT8_MAX;
    if ((Bits != 32 && Bits != 64) ||
        (Prefix && Prefix != 0x26 && Prefix != 0x2e && Prefix != 0x36 &&
         Prefix != 0x3e && Prefix != 0x64 && Prefix != 0x65))
      throw std::invalid_argument("invalid AMX native address");
    if (Bits == 32)
      P.Descriptor |= X86AmxAddress32;
    if (Constant(WidthIndex + 2, 1))
      P.Descriptor |= X86AmxEvexMemory;
    if (P.Prefix & X86AmxNativeStackReference)
      P.Descriptor |= X86AmxStackReference;
    if (Prefix == 0x36)
      P.Descriptor |= X86AmxStackSegment;
    if (Prefix == 0x64)
      P.Space = NdMemoryAddressSpace::X86FS;
    if (Prefix == 0x65)
      P.Space = NdMemoryAddressSpace::X86GS;
  } else if (x86AmxIsRow(K)) {
    P.RowOrStride = 1;
    if (P.Descriptor & X86AmxImmediateRow)
      (void)Constant(1, UINT8_MAX);
    else if (Constant(2, 1))
      P.Descriptor |= X86AmxExtendedRow;
  } else if (x86AmxIsCompute(K)) {
    P.Descriptor = (P.Descriptor & ~((7u << 8) | (7u << 11))) |
                   (Constant(1, 7) << 8) | (Constant(2, 7) << 11);
  }
  if (!x86AmxDescriptorIsValid(P.Descriptor) ||
      !x86AmxAddressIsValid(P.Descriptor, P.Space))
    throw std::invalid_argument("invalid AMX native bank descriptor");
  return P;
}
std::string builtin(uint32_t D, unsigned Prefix,
                    const std::vector<std::string> &Inputs) {
  const auto K = x86AmxOperation(D);
  std::string Name = x86AmxNativeSuffix(D);
  std::replace(Name.begin(), Name.end(), '.', '_');
  std::vector<std::string> Args;
  if (x86AmxIsTileMemory(K) || x86AmxIsRow(K) || x86AmxIsCompute(K) ||
      K == X86AmxOp::Zero)
    Args.push_back(std::to_string(x86AmxTile(D)));
  if (x86AmxIsMemory(K)) {
    Args.push_back("(uint64_t)(uintptr_t)(" + Inputs[0] + ")");
    if (x86AmxIsTileMemory(K))
      Args.push_back("(uint64_t)(" + Inputs[1] + ")");
    Args.push_back(D & X86AmxAddress32 ? "32" : "64");
    Args.push_back(std::to_string(Prefix));
    Args.push_back(D & X86AmxEvexMemory ? "1" : "0");
  } else if (x86AmxIsRow(K)) {
    Args.push_back("(uint32_t)(" + Inputs[1] + ")");
    if (!(D & X86AmxImmediateRow))
      Args.push_back(D & X86AmxExtendedRow ? "1" : "0");
  } else if (x86AmxIsCompute(K)) {
    Args.push_back(std::to_string(x86AmxTile(D, 1)));
    Args.push_back(std::to_string(x86AmxTile(D, 2)));
  }
  std::string Text = "__builtin_ia32_" + Name + "(";
  for (const auto &Arg : Args) {
    if (Text.back() != '(')
      Text += ", ";
    Text += Arg;
  }
  Text += ')';
  if (x86AmxIsRow(K)) {
    std::string Temporary = "neverd_amx_native_row";
    while (Text.find(Temporary) != std::string::npos ||
           Inputs[0].find(Temporary) != std::string::npos)
      Temporary += '_';
    Text = "({ __auto_type " + Temporary + " = " + Text +
           "; __builtin_memcpy((void *)(uintptr_t)(" + Inputs[0] + "), &" +
           Temporary + ", " + std::to_string(X86AmxRowBytes) + "); })";
  }
  return Text;
}
std::string dispatch(uint32_t D, const std::string &Native,
                     const std::string &Fallback) {
  if (x86AmxNeedsStackLeaf(D))
    return "NEVERD_X86_AMX_STACK_MEMORY(" + Native + ")";
  return std::string(D & X86AmxStackReference ? "NEVERD_X86_AMX_STACK("
                                              : "NEVERD_X86_AMX(") +
         Native + ", ({ " + Fallback + " }))";
}
void addFeatures(std::set<std::string> &Features, uint32_t D) {
  const auto Names = x86AmxNativeFeatures(D);
  llvm::SmallVector<llvm::StringRef, 8> Parts;
  llvm::StringRef(Names).split(Parts, ',');
  for (const auto Part : Parts)
    Features.insert(Part.str());
}
std::string gnuText(const std::string &S) {
  std::string R;
  for (size_t I = 0; I < S.size(); ++I) {
    if (S.compare(I, 2, "$$") == 0) {
      R += '$';
      ++I;
      continue;
    }
    if (S.compare(I, 2, "${") == 0 && I + 5 < S.size() && S[I + 3] == ':' &&
        S[I + 5] == '}') {
      R += '%';
      R += S[I + 4];
      R += S[I + 2];
      I += 5;
      continue;
    }
    switch (S[I]) {
    case '$':
      R += '%';
      break;
    case '%':
      R += "%%";
      break;
    case '{':
      R += "%{";
      break;
    case '}':
      R += "%}";
      break;
    case '\n':
      R += "\\n";
      break;
    case '\t':
      R += "\\t";
      break;
    default:
      R += S[I];
      break;
    }
  }
  return R;
}
std::string render(const X86AmxAsm &P, std::vector<std::string> Inputs) {
  if (Inputs.size() != P.InputBytes.size())
    throw std::invalid_argument("invalid AMX assembly argument count");
  // An expression cannot borrow the containing C function's frame pointer.
  // The native builtin owns its isolated leaf and both unwind formats.
  if (P.StackLeaf)
    return {};
  std::string Text;
  for (unsigned I = 0; I < Inputs.size(); ++I)
    if (P.InputConstraints[I] == "{r16}") {
      Text += "register uint32_t __neverd_amx_selector __asm__(\"r16\") = "
              "(uint32_t)(" +
              Inputs[I] + "); ";
      Inputs[I] = "__neverd_amx_selector";
    }
  Text += "__asm__ volatile(\"" + gnuText(P.Text) + "\" : : ";
  for (unsigned I = 0; I < Inputs.size(); ++I) {
    if (I)
      Text += ", ";
    const auto C = P.InputConstraints[I] == "{r16}"  ? "r"
                   : P.InputConstraints[I] == "{ax}" ? "a"
                   : P.InputConstraints[I] == "{cx}" ? "c"
                                                     : P.InputConstraints[I];
    Text += "\"" + C + "\"((uint" + std::to_string(P.InputBytes[I] * 8) +
            "_t)(" + Inputs[I] + "))";
  }
  Text += " : ";
  for (unsigned I = 0; I < P.Clobbers.size(); ++I) {
    if (I)
      Text += ", ";
    Text += "\"" + P.Clobbers[I] + "\"";
  }
  return Text + ");";
}
} // namespace
std::string renderX86AmxExpression(Arch A, const HighExpr &E,
                                   const std::vector<std::string> &Args,
                                   const std::string &Type) {
  X86AmxShape S;
  S.TargetArch = A;
  S.ValidId = E.IntrinsicId == Intrinsic::X86AMXState &&
              E.Kind == ExprKind::Call && E.IntrinsicOutputs.empty() &&
              !E.IsIndirectCall && !E.IndirectTarget && !E.SourceCallHint &&
              !E.DoesNotReturn && E.Operands.size() == Args.size();
  S.MemoryAddressSpace = E.MemoryAddressSpace;
  S.MemoryOrdering = E.MemoryOrdering;
  S.WritableOutput = E.Type && E.Type->Kind == NdTypeKind::Int;
  S.OutputSize = E.Type ? E.Type->Size : 0;
  S.Arguments = E.Operands.size();
  for (unsigned I = 0; I < S.Arguments && I < S.InputSizes.size(); ++I) {
    const auto *V = E.Operands[I].get();
    S.InputSizes[I] = V && V->Type ? V->Type->Size : 0;
    S.ScalarInputs[I] =
        V && V->Type &&
        (V->Type->Kind == NdTypeKind::Int || V->Type->Kind == NdTypeKind::Ptr);
    if (!V || V->Kind != ExprKind::Const || !V->Type ||
        V->Type->Kind != NdTypeKind::Int)
      continue;
    if (I == 0)
      S.EmptyBase = V->ConstVal == 0;
    if (I == 1 && V->Type->Size == 4 && V->ConstVal <= UINT32_MAX)
      S.Descriptor = V->ConstVal;
    if (I == 2)
      S.RowImmediate = V->ConstVal;
  }
  if (A != Arch::X64 || !x86AmxShapeIsValid(S))
    throw std::invalid_argument(
        "invalid HighC AMX architectural bank contract");
  const auto K = x86AmxOperation(*S.Descriptor);
  const auto P = x86AmxAsm(*S.Descriptor, S.MemoryAddressSpace);
  std::vector<std::string> Inputs;
  std::string Text = "({ ";
  std::string RowName = "neverd_amx_row";
  for (const auto &Arg : Args)
    while (Arg.find(RowName) != std::string::npos)
      RowName += '_';
  if (P->Memory) {
    Inputs.push_back(Args[0]);
    if (x86AmxIsTileMemory(K))
      Inputs.push_back(Args[2]);
  } else if (P->Row) {
    Text += Type + " " + RowName + "; ";
    Inputs = {"&" + RowName, Args[2]};
  }
  Text +=
      dispatch(*S.Descriptor,
               builtin(*S.Descriptor,
                       x86AmxNativePrefix(*S.Descriptor, S.MemoryAddressSpace),
                       Inputs),
               render(*P, Inputs)) +
      ";";
  if (P->Row)
    Text += " " + RowName + ";";
  return Text + " })";
}
std::optional<std::string> renderX86AmxCall(
    Arch A, const llvm::CallInst &Call, const std::vector<std::string> &Args,
    const std::string &Result, bool ResultLive, const std::string &Type) {
  if (const auto N = nativeCall(Call)) {
    if (A != Arch::X64 || Args.size() != Call.arg_size())
      throw std::invalid_argument("invalid AMX native C route");
    auto P = *x86AmxAsm(N->Descriptor, N->Space);
    const unsigned Prefix = N->Prefix & UINT8_MAX;
    if (P.Memory && (Prefix == 0 || Prefix == 0x26 || Prefix == 0x2e)) {
      const auto At = P.Text.find("%ds:");
      P.Text.replace(At, 4,
                     Prefix == 0      ? ""
                     : Prefix == 0x26 ? "%es:"
                                      : "%cs:");
    }
    std::vector<std::string> Inputs;
    std::string Local = Result;
    std::string Text = "{ ";
    if (P.Memory) {
      Inputs.push_back(Args[N->Base]);
      if (x86AmxIsTileMemory(x86AmxOperation(N->Descriptor)))
        Inputs.push_back(Args[N->RowOrStride]);
    } else if (P.Row) {
      if (!ResultLive) {
        Local = "neverd_amx_discard";
        for (const auto &Arg : Args)
          while (Arg.find(Local) != std::string::npos)
            Local += '_';
        Text += Type + " " + Local + "; ";
      }
      if (Local.empty())
        throw std::invalid_argument("missing AMX C row destination");
      Inputs = {"&" + Local, Args[N->RowOrStride]};
    }
    return Text +
           dispatch(N->Descriptor, builtin(N->Descriptor, N->Prefix, Inputs),
                    render(P, Inputs)) +
           "; }\n";
  }
  const auto *Asm = llvm::dyn_cast<llvm::InlineAsm>(Call.getCalledOperand());
  if (!Asm)
    return {};
  const bool Legacy =
      llvm::StringRef(Asm->getAsmString()).starts_with(X86AmxLegacyAsmMarker);
  const llvm::StringRef Marker =
      llvm::StringRef(Asm->getAsmString()).starts_with(X86AmxStackLeafAsmMarker)
          ? X86AmxStackLeafAsmMarker
      : Legacy ? X86AmxLegacyAsmMarker
               : X86AmxAsmMarker;
  if (!llvm::StringRef(Asm->getAsmString()).starts_with(Marker))
    return {};
  auto Header = llvm::StringRef(Asm->getAsmString())
                    .drop_front(Marker.size())
                    .split('\n')
                    .first;
  auto Fields = Header.split(' ');
  uint32_t Descriptor;
  unsigned Space;
  if (A != Arch::X64 || Fields.first.getAsInteger(10, Descriptor) ||
      Fields.second.getAsInteger(10, Space) || Space > UINT8_MAX)
    throw std::invalid_argument("invalid AMX native descriptor");
  const auto P =
      x86AmxAsm(Descriptor, static_cast<NdMemoryAddressSpace>(Space), Legacy);
  if (!P || P->Text != Asm->getAsmString() ||
      (P->StackLeaf && !x86AmxIsStackLeaf(Call)) ||
      P->Constraints != Asm->getConstraintString() || !Asm->hasSideEffects() ||
      Asm->isAlignStack() || Asm->canThrow() ||
      Asm->getDialect() != llvm::InlineAsm::AD_ATT ||
      !Call.getType()->isVoidTy() || Call.arg_size() != P->InputBytes.size() ||
      Args.size() != Call.arg_size() || Call.getNumOperandBundles() ||
      Call.onlyReadsMemory() || Call.onlyWritesMemory() ||
      Call.doesNotReturn() || Call.getFunctionType()->isVarArg())
    throw std::invalid_argument("invalid AMX native assembly effects");
  for (unsigned I = 0; I < Call.arg_size(); ++I) {
    const auto *T = Call.getArgOperand(I)->getType();
    if (P->Row && I == 0) {
      if (!T->isPointerTy() || T->getPointerAddressSpace())
        throw std::invalid_argument("invalid AMX row storage");
    } else if (!T->isIntegerTy(P->InputBytes[I] * 8))
      throw std::invalid_argument("invalid AMX scalar input");
  }
  if (Descriptor & X86AmxImmediateRow) {
    const auto *C = llvm::dyn_cast<llvm::ConstantInt>(Call.getArgOperand(1));
    if (!C || C->getZExtValue() > UINT8_MAX)
      throw std::invalid_argument("invalid AMX row immediate");
  }
  // Scope the fixed r16 binding if more than one row instruction is emitted.
  return "{ " +
         dispatch(
             Descriptor,
             builtin(Descriptor,
                     x86AmxNativePrefix(
                         Descriptor, static_cast<NdMemoryAddressSpace>(Space)),
                     Args),
             render(*x86AmxAsm(Descriptor,
                               static_cast<NdMemoryAddressSpace>(Space)),
                    Args)) +
         "; }\n";
}
bool isX86AmxNativeRowCall(const llvm::CallInst &Call, Arch A) {
  const auto Plan = nativeCall(Call);
  return A == Arch::X64 && Plan &&
         x86AmxIsRow(x86AmxOperation(Plan->Descriptor));
}
std::set<std::string> x86AmxFunctionFeatures(const HighFunc &Function) {
  std::set<std::string> Features;
  std::function<void(const ExprPtr &)> Visit = [&](const ExprPtr &E) {
    if (!E)
      return;
    if (E->IntrinsicId == Intrinsic::X86AMXState && E->Operands.size() >= 2 &&
        E->Operands[1] && E->Operands[1]->Kind == ExprKind::Const &&
        E->Operands[1]->ConstVal <= UINT32_MAX &&
        x86AmxDescriptorIsValid(E->Operands[1]->ConstVal))
      addFeatures(Features, E->Operands[1]->ConstVal);
    for (const auto &Operand : E->Operands)
      Visit(Operand);
  };
  walkStmts(Function.Body, [&](const HighStmt &S) { forEachExpr(S, Visit); });
  return Features;
}
std::set<std::string> x86AmxFunctionFeatures(const llvm::Function &Function) {
  std::set<std::string> Features;
  for (const auto &Block : Function)
    for (const auto &Instruction : Block) {
      const auto *Call = llvm::dyn_cast<llvm::CallInst>(&Instruction);
      if (!Call)
        continue;
      if (const auto N = nativeCall(*Call)) {
        addFeatures(Features, N->Descriptor);
        continue;
      }
      const auto *Asm =
          llvm::dyn_cast<llvm::InlineAsm>(Call->getCalledOperand());
      if (!Asm)
        continue;
      const llvm::StringRef Text = Asm->getAsmString();
      const llvm::StringRef Marker =
          Text.starts_with(X86AmxStackLeafAsmMarker) ? X86AmxStackLeafAsmMarker
          : Text.starts_with(X86AmxLegacyAsmMarker)  ? X86AmxLegacyAsmMarker
                                                     : X86AmxAsmMarker;
      if (!Text.starts_with(Marker))
        continue;
      uint32_t D;
      if (!Text.drop_front(Marker.size())
               .split(' ')
               .first.getAsInteger(10, D) &&
          x86AmxDescriptorIsValid(D))
        addFeatures(Features, D);
    }
  return Features;
}
} // namespace neverd
