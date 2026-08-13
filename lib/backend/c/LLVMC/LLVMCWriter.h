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
              DebugContext *Dbg, const BinaryImage *Img = nullptr)
      : OS(OS), Opts(Opts), Dbg(Dbg), Img(Img) {}

  //--- Module-level (LLVMCEmitter.cpp) ---
  void writeModule(llvm::Module &Mod);
  void writeIncludes(llvm::Module &Mod);
  void writeStructDefs(llvm::Module &Mod);
  void writeGlobals(llvm::Module &Mod);
  void writeForwardDecls(llvm::Module &Mod);

  //--- Function rendering (LLVMCFuncWriter.cpp) ---
  void writeFunction(llvm::Function &Fn);
  void setupFunction(llvm::Function &Fn);
  void emitFunctionDecls(llvm::Function &Fn);
  void scanReferencedBlocks(llvm::Function &Fn);
  void markInlinable(llvm::Function &Fn);

  bool isSimpleEntry(const llvm::BasicBlock *BB, const llvm::Function &Fn) {
    return BB == &Fn.getEntryBlock() && !ReferencedBlocks.count(BB);
  }

  //--- Instruction rendering (LLVMCStmtWriter.cpp) ---
  void writeInstruction(llvm::Instruction &Inst, int Indent);
  void writeCall(llvm::CallInst &Call, const std::string &Name, int Indent);
  bool writeIntrinsicCall(llvm::CallInst &Call, int Indent);
  bool writeInlineAsmCall(llvm::CallInst &Call, const std::string &Name,
                          int Indent);
  void writeGEP(llvm::GetElementPtrInst &GEP, const std::string &Name,
                int Indent);
  void writeReturn(llvm::ReturnInst &Ret, int Indent);
  void emitIndent(int N);

  //--- Expression rendering (LLVMCExprWriter.cpp) ---
  std::string resolveNdDataName(llvm::StringRef Name) const;
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
};

} // namespace neverd

#endif // NEVERD_LIB_BACKEND_C_LLVMC_LLVMCWRITER_H
