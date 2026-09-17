//===- LLVMCWriter.h - Internal LLVM IR C writer class ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Internal class declaration for the LLVM-IR-to-C source writer.
/// This header is used only within the backend/c library; do not
/// install it in include/.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_C_LLVMC_LLVMCWRITER_H
#define NEVERD_LIB_BACKEND_C_LLVMC_LLVMCWRITER_H

#include "../CIdentifier.h"

#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/c/pass/LLVMC/LLVMCPasses.h"
#include "neverd/backend/c/render/CTypeFormat.h"
#include "neverd/backend/c/render/LLVMC/LLVMCIntrinsicRender.h"
#include "neverd/debug/DebugContext.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/raw_ostream.h"

#include <map>
#include <optional>
#include <set>
#include <string>

namespace neverd {

/// Internal writer that converts LLVM IR modules to goto-style C source.
/// Split across LLVMCEmitter.cpp (module-level orchestration),
/// LLVMCFuncWriter.cpp (function rendering), LLVMCStmtWriter.cpp (instruction
/// rendering), and LLVMCExprWriter.cpp (value/expression rendering).
class LLVMCWriter {
public:
  LLVMCWriter(llvm::raw_ostream &OS, const CEmitterOptions &Opts,
              DebugContext *Dbg, const BinaryImage *Img = nullptr,
              bool GuardAnalysisOnlyFunctions = true)
      : OS(OS), Opts(Opts), Dbg(Dbg), Img(Img),
        GuardAnalysisOnlyFunctions(GuardAnalysisOnlyFunctions) {}

  //--- Module-level (LLVMCEmitter.cpp) ---
  void writeModule(llvm::Module &Mod, const llvm::Function *Only = nullptr);
  void prepareFunctionIdentifiers(llvm::Module &Mod);
  std::string functionIdentifier(const llvm::Function &Fn) const;
  void writeIncludes(llvm::Module &Mod);
  void writeStructDefs(llvm::Module &Mod);
  void writeGlobals(llvm::Module &Mod);
  void writeReferencedImageObjects(const llvm::Function &Fn);
  void writeForwardDecls(llvm::Module &Mod);

  //--- Function rendering (LLVMCFuncWriter.cpp) ---
  bool isAnalysisOnlyFunction(const llvm::Function &Fn) const;
  bool isReferencedByExecutableProjection(const llvm::Function &Fn) const;
  void writeFunction(llvm::Function &Fn);
  void writeAnalysisOnlyFunction(llvm::Function &Fn);
  void writeFunctionProjection(llvm::Function &Fn);
  void setupFunction(llvm::Function &Fn);
  void emitFunctionDecls(llvm::Function &Fn);
  void scanReferencedBlocks(llvm::Function &Fn);
  void markInlinable(llvm::Function &Fn);
  void writeExceptionAnnotation(const llvm::Function &Fn);
  bool functionHasWindowsEHPads(const llvm::Function &Fn) const;
  bool functionIsCxxEH(const llvm::Function &Fn) const;

  bool isSimpleEntry(const llvm::BasicBlock *BB, const llvm::Function &Fn) {
    return BB == &Fn.getEntryBlock() && !ReferencedBlocks.count(BB);
  }

  //--- Instruction rendering (LLVMCStmtWriter.cpp) ---
  void writeInstruction(llvm::Instruction &Inst, int Indent);
  void writeCall(llvm::CallInst &Call, const std::string &Name, int Indent);
  void writeCallLike(llvm::CallBase &Call, const std::string &Name, int Indent);
  void writePhiCopies(const llvm::BasicBlock *From, const llvm::BasicBlock *To,
                      int Indent);
  std::string resolveImportCalleeName(const llvm::Value *Callee) const;
  bool isImportCalleeOnlyLoad(const llvm::LoadInst *LI) const;
  bool writeIntrinsicCall(llvm::CallBase &Call, int Indent);
  bool writeInlineAsmCall(llvm::CallInst &Call, const std::string &Name,
                          int Indent);
  bool callDoesNotReturn(const llvm::CallBase &Call) const;
  void writeGEP(llvm::GetElementPtrInst &GEP, const std::string &Name,
                int Indent);
  void writeReturn(llvm::ReturnInst &Ret, int Indent);
  void writeInvoke(llvm::InvokeInst &Invoke, const std::string &Name,
                   int Indent);
  void writeCatchSwitch(llvm::CatchSwitchInst &CS, int Indent);
  void writeCleanupRet(llvm::CleanupReturnInst &CR, int Indent);
  std::string windowsEHFilterExpr(const llvm::CatchSwitchInst &CS);
  std::string windowsCxxCatchType(const llvm::CatchPadInst &Pad);
  void emitIndent(int N);
  const llvm::AllocaInst *asAllocaPointer(const llvm::Value *V) const;

  //--- Expression rendering (LLVMCExprWriter.cpp) ---
  std::string resolveNdDataName(llvm::StringRef Name) const;
  std::optional<va_t> imageDataVA(const llvm::Value *V) const;
  std::string imageDataCName(const llvm::Value *V) const;
  std::string getName(const llvm::Value *V);
  std::string freshVar(const std::string &Hint = "v");
  std::string valueStr(const llvm::Value *V);
  std::string constStr(const llvm::Constant *C);
  std::string blockLabel(const llvm::BasicBlock *BB);
  std::string binopStr(unsigned Opcode, const std::string &LHS,
                       const std::string &RHS, llvm::Type *Ty);
  std::string castStr(unsigned Opcode, const std::string &Src,
                      llvm::Type *SrcTy, llvm::Type *DstTy);
  std::string cmpStr(llvm::CmpInst::Predicate Pred, const std::string &LHS,
                     const std::string &RHS, bool IsFP);
  std::string renderInline(const llvm::Instruction &Inst);

  //--- State ---
  llvm::raw_ostream &OS;
  CEmitterOptions Opts;
  DebugContext *Dbg;
  const BinaryImage *Img;
  const llvm::Module *CurMod = nullptr;
  bool GuardAnalysisOnlyFunctions;
  const llvm::Function *OnlyFunction = nullptr;
  /// When false, emit recovered statements without a C wrapper so analysis-only
  /// functions can nest the listing inside `#if 0` of the trap stub.
  bool EmitFunctionWrapper = true;
  CProjectionIdentifierAllocator GlobalIdentifierAllocator;
  std::map<const llvm::Function *, std::string> FunctionIdentifiers;

  int NextVar = 0;
  std::map<const llvm::Value *, std::string> ValNames;
  std::set<std::string> UsedNames;
  std::map<const llvm::BasicBlock *, std::string> BlockLabels;
  std::set<const llvm::BasicBlock *> ReferencedBlocks;
  bool HasCIntrinsics = false;
  std::set<std::string> IntrinsicMappedNames;
  LLVMCAnalysisState Analysis;
  std::map<const llvm::Value *, std::string> InlineCache;
  bool InferredVoid = false;
  int EHTryDepth = 0;
  bool EHWrapIsCxx = false;
  /// True after a noreturn call (`throw`, `__fastfail`, libc abort/exit/…).
  /// The next `return` / `unreachable` / debugtrap is the compiler's
  /// fall-through, not source.
  bool AfterCxxThrow = false;
};

} // namespace neverd

#endif // NEVERD_LIB_BACKEND_C_LLVMC_LLVMCWRITER_H
