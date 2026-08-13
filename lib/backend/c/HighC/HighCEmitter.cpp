//===- HighCEmitter.cpp - High IR to C emitter ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Top-level orchestration for the HighIR C emitter: include generation,
/// forward declarations, and per-function dispatch.  Function and statement
/// rendering live in HighCFuncWriter.cpp and HighCStmtWriter.cpp; expression
/// rendering lives in HighCExprWriter.cpp and HighCExprBinOp.cpp.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/c/HighC/HighCEmitter.h"

#include "HighCWriter.h"

#define DEBUG_TYPE "neverd-highc-emitter"
#include "neverd/libc/LibCNames.h"

#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <functional>
#include <map>
#include <set>

namespace neverd {

std::string HighCWriter::memoryTypeName(const TypeRef &Ty) const {
  std::string Name = typeToC(Ty);
  return Name == "void" ? "uint32_t" : Name;
}

void HighCWriter::collectMemoryTypes(const std::vector<HighFunc> &Funcs) {
  std::set<std::string> Names;
  std::set<const HighExpr *> Seen;
  std::function<void(const HighExpr &)> Visit = [&](const HighExpr &E) {
    if (!Seen.insert(&E).second)
      return;
    if (E.Kind == ExprKind::Load)
      Names.insert(memoryTypeName(E.Type));
    if (E.Kind == ExprKind::Store && E.Operands.size() >= 2)
      Names.insert(memoryTypeName(E.Operands[1]->Type));
    for (const ExprPtr &Operand : E.Operands)
      if (Operand)
        Visit(*Operand);
  };

  for (const HighFunc &Func : Funcs) {
    walkStmts(Func.Body, [&](const HighStmt &Stmt) {
      if (Stmt.Kind == StmtKind::Store && Stmt.StoreVal)
        Names.insert(memoryTypeName(Stmt.StoreVal->Type));
      forEachExpr(Stmt, [&](const ExprPtr &E) {
        if (E)
          Visit(*E);
      });
    });
  }

  MemoryTypes.clear();
  unsigned Index = 0;
  for (const std::string &Name : Names)
    MemoryTypes.emplace(Name, Index++);
}

void HighCWriter::writeMemoryHelpers() {
  for (const auto &[Type, Index] : MemoryTypes) {
    OS << "static inline " << Type << " neverd_mem_load_" << Index
       << "(uintptr_t address) {\n"
       << "    " << Type << " value;\n"
       << "    memcpy(&value, (const void *)address, sizeof(value));\n"
       << "    return value;\n"
       << "}\n\n"
       << "static inline " << Type << " neverd_mem_store_" << Index
       << "(uintptr_t address, " << Type << " value) {\n"
       << "    memcpy((void *)address, &value, sizeof(value));\n"
       << "    return value;\n"
       << "}\n\n";
  }
}

std::string HighCWriter::memoryLoadExpr(const TypeRef &Ty,
                                        llvm::StringRef Addr) const {
  std::string Type = memoryTypeName(Ty);
  auto It = MemoryTypes.find(Type);
  if (It == MemoryTypes.end())
    llvm::report_fatal_error("HighC memory load type was not collected");
  unsigned Index = It->second;
  return "neverd_mem_load_" + std::to_string(Index) + "((uintptr_t)(" +
         Addr.str() + "))";
}

std::string HighCWriter::memoryStoreExpr(const TypeRef &Ty,
                                         llvm::StringRef Addr,
                                         llvm::StringRef Val) const {
  std::string Type = memoryTypeName(Ty);
  auto It = MemoryTypes.find(Type);
  if (It == MemoryTypes.end())
    llvm::report_fatal_error("HighC memory store type was not collected");
  unsigned Index = It->second;
  return "neverd_mem_store_" + std::to_string(Index) + "((uintptr_t)(" +
         Addr.str() + "), " + Val.str() + ")";
}

void HighCWriter::collectCallTargetsExpr(const HighExpr &Expr,
                                         std::set<std::string> &Targets) {
  std::set<const HighExpr *> Seen;
  std::function<void(const HighExpr &)> Visit = [&](const HighExpr &Ex) {
    if (!Seen.insert(&Ex).second)
      return;
    if (Ex.Kind == ExprKind::Call) {
      std::string Name = Ex.CallTarget;
      if (!Name.empty()) {
        if (Ex.IntrinsicId != Intrinsic::None) {
          if (intrinsicCName(Ex.IntrinsicId))
            HasCIntrinsics = true;
          CIntrinsicNames.insert(Name);
        }
        if (Ex.IntrinsicId == Intrinsic::None && Name[0] == '_')
          Name = Name.substr(1);
        Targets.insert(Name);
      }
    }
    for (auto &Op : Ex.Operands)
      if (Op)
        Visit(*Op);
  };
  Visit(Expr);
}

void HighCWriter::collectCallTargets(const std::vector<HighStmt> &Stmts,
                                     std::set<std::string> &Targets) {
  for (auto &S : Stmts) {
    forEachExpr(S, [&](const ExprPtr &Ex) {
      if (Ex)
        collectCallTargetsExpr(*Ex, Targets);
    });
    collectCallTargets(S.Body, Targets);
    collectCallTargets(S.ElseBody, Targets);
    for (auto &C : S.Cases)
      collectCallTargets(C.Body, Targets);
    collectCallTargets(S.DefaultBody, Targets);
    for (auto &ClauseBody : S.EHClauseBodies)
      collectCallTargets(ClauseBody, Targets);
  }
}

void HighCWriter::writeIncludes(const std::vector<HighFunc> &Funcs) {
  if (!Opts.EmitIncludes)
    return;

  std::set<std::string> Headers;
  Headers.insert("stdint.h");
  if (!MemoryTypes.empty())
    Headers.insert("string.h");

  std::set<std::string> CallTargets;
  for (auto &F : Funcs)
    collectCallTargets(F.Body, CallTargets);

  for (auto &Name : CallTargets) {
    if (const char *Hdr = libc::headerFor(Name))
      Headers.insert(Hdr);
  }

  if (HasCIntrinsics)
    for (const char *Hdr : getArchIntrinsicHeaders(Opts.TheArch))
      Headers.insert(Hdr);

  for (auto &H : Headers)
    OS << "#include <" << H << ">\n";
  OS << "\n";
}

void HighCWriter::writeForwardDecls(const std::vector<HighFunc> &Funcs) {
  for (auto &F : Funcs)
    DefinedFuncs[F.Name] = &F;

  std::set<std::string> CallTargets;
  for (auto &F : Funcs)
    collectCallTargets(F.Body, CallTargets);

  for (auto &Name : CallTargets) {
    if (DefinedFuncs.count(Name))
      continue;
    if (libc::isKnownFunction(Name))
      continue;
    if (CIntrinsicNames.count(Name))
      continue;

    std::string CleanName = Name;
    if (!CleanName.empty() && CleanName[0] == '_')
      CleanName = CleanName.substr(1);
    if (libc::isKnownFunction(CleanName))
      continue;

    ExternFuncs.insert(Name);
  }

  for (auto &Name : ExternFuncs)
    OS << "extern int " << Name << "();\n";

  if (!ExternFuncs.empty())
    OS << "\n";
}

void HighCWriter::writeAll(const std::vector<HighFunc> &Funcs) {
  collectMemoryTypes(Funcs);
  writeIncludes(Funcs);
  writeMemoryHelpers();
  writeForwardDecls(Funcs);

  for (size_t I = 0; I < Funcs.size(); ++I) {
    if (Funcs[I].Name.empty())
      continue;
    writeFunction(Funcs[I]);
    if (I + 1 < Funcs.size())
      OS << "\n";
  }
}

//===----------------------------------------------------------------------===//
// Public API
//===----------------------------------------------------------------------===//

bool HighCEmitter::emit(const std::vector<HighFunc> &Funcs,
                        llvm::raw_ostream &Out, const CEmitterOptions &Opts,
                        DebugContext *Dbg) {
  HighCWriter W(Out, Opts, Dbg);
  W.writeAll(Funcs);
  return true;
}

bool HighCEmitter::emitToFile(const std::vector<HighFunc> &Funcs,
                              const std::string &Path,
                              const CEmitterOptions &Opts, DebugContext *Dbg) {
  std::error_code EC;
  llvm::raw_fd_ostream OS(Path, EC);
  if (EC) {
    llvm::WithColor::error() << "high_c_emitter: cannot open " << Path << ": "
                             << EC.message() << "\n";
    return false;
  }
  bool Ok = emit(Funcs, OS, Opts, Dbg);
  LLVM_DEBUG(llvm::dbgs() << "high_c_emitter: written to " << Path << "\n");
  return Ok;
}

} // namespace neverd
