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

namespace {

const char *memoryOrderingName(NdMemoryOrdering Ordering) {
  switch (Ordering) {
  case NdMemoryOrdering::None:
    return "plain";
  case NdMemoryOrdering::Relaxed:
    return "relaxed";
  case NdMemoryOrdering::Acquire:
    return "acquire";
  case NdMemoryOrdering::Release:
    return "release";
  case NdMemoryOrdering::AcquireRelease:
    return "acq_rel";
  case NdMemoryOrdering::SequentiallyConsistent:
    return "seq_cst";
  }
  llvm_unreachable("unknown NeverD memory ordering");
}

const char *atomicOrderingToken(NdMemoryOrdering Ordering) {
  switch (Ordering) {
  case NdMemoryOrdering::Relaxed:
    return "__ATOMIC_RELAXED";
  case NdMemoryOrdering::Acquire:
    return "__ATOMIC_ACQUIRE";
  case NdMemoryOrdering::Release:
    return "__ATOMIC_RELEASE";
  case NdMemoryOrdering::AcquireRelease:
    return "__ATOMIC_ACQ_REL";
  case NdMemoryOrdering::SequentiallyConsistent:
    return "__ATOMIC_SEQ_CST";
  case NdMemoryOrdering::None:
    break;
  }
  llvm::report_fatal_error("plain memory access has no atomic order token");
}

void validateAtomicLoadOrdering(NdMemoryOrdering Ordering) {
  if (Ordering == NdMemoryOrdering::Release ||
      Ordering == NdMemoryOrdering::AcquireRelease)
    llvm::report_fatal_error("release ordering is invalid on a load");
}

void validateAtomicStoreOrdering(NdMemoryOrdering Ordering) {
  if (Ordering == NdMemoryOrdering::Acquire ||
      Ordering == NdMemoryOrdering::AcquireRelease)
    llvm::report_fatal_error("acquire ordering is invalid on a store");
}

} // anonymous namespace

std::string HighCWriter::memoryTypeName(const TypeRef &Ty) const {
  std::string Name = typeToC(Ty);
  return Name == "void" ? "uint32_t" : Name;
}

void HighCWriter::collectMemoryTypes(const std::vector<HighFunc> &Funcs) {
  std::set<std::string> Names;
  AtomicLoadTypes.clear();
  AtomicStoreTypes.clear();
  std::set<const HighExpr *> Seen;
  std::function<void(const HighExpr &)> Visit = [&](const HighExpr &E) {
    if (!Seen.insert(&E).second)
      return;
    if (E.Kind == ExprKind::Load) {
      std::string Type = memoryTypeName(E.Type);
      Names.insert(Type);
      if (E.MemoryOrdering != NdMemoryOrdering::None)
        AtomicLoadTypes.insert({Type, E.MemoryOrdering});
    }
    if (E.Kind == ExprKind::Store && E.Operands.size() >= 2) {
      std::string Type = memoryTypeName(E.Operands[1]->Type);
      Names.insert(Type);
      if (E.MemoryOrdering != NdMemoryOrdering::None)
        AtomicStoreTypes.insert({Type, E.MemoryOrdering});
    }
    for (const ExprPtr &Operand : E.Operands)
      if (Operand)
        Visit(*Operand);
  };

  for (const HighFunc &Func : Funcs) {
    walkStmts(Func.Body, [&](const HighStmt &Stmt) {
      if (Stmt.Kind == StmtKind::Store && Stmt.StoreVal) {
        std::string Type = memoryTypeName(Stmt.StoreVal->Type);
        Names.insert(Type);
        if (Stmt.MemoryOrdering != NdMemoryOrdering::None)
          AtomicStoreTypes.insert({Type, Stmt.MemoryOrdering});
      }
      if (Stmt.Kind == StmtKind::Assign && Stmt.Dst &&
          Stmt.Dst->Kind == ExprKind::Load &&
          Stmt.Dst->MemoryOrdering != NdMemoryOrdering::None)
        AtomicStoreTypes.insert(
            {memoryTypeName(Stmt.Dst->Type), Stmt.Dst->MemoryOrdering});
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

  for (const auto &[Type, Ordering] : AtomicLoadTypes) {
    validateAtomicLoadOrdering(Ordering);
    unsigned Index = MemoryTypes.at(Type);
    OS << "static inline " << Type << " neverd_mem_load_"
       << memoryOrderingName(Ordering) << "_" << Index
       << "(uintptr_t address) {\n"
       << "    return __atomic_load_n((const " << Type << " *)address, "
       << atomicOrderingToken(Ordering) << ");\n"
       << "}\n\n";
  }

  for (const auto &[Type, Ordering] : AtomicStoreTypes) {
    validateAtomicStoreOrdering(Ordering);
    unsigned Index = MemoryTypes.at(Type);
    OS << "static inline " << Type << " neverd_mem_store_"
       << memoryOrderingName(Ordering) << "_" << Index << "(uintptr_t address, "
       << Type << " value) {\n"
       << "    __atomic_store_n((" << Type << " *)address, value, "
       << atomicOrderingToken(Ordering) << ");\n"
       << "    return value;\n"
       << "}\n\n";
  }
}

std::string HighCWriter::memoryLoadExpr(const TypeRef &Ty, llvm::StringRef Addr,
                                        NdMemoryOrdering Ordering) const {
  std::string Type = memoryTypeName(Ty);
  auto It = MemoryTypes.find(Type);
  if (It == MemoryTypes.end())
    llvm::report_fatal_error("HighC memory load type was not collected");
  unsigned Index = It->second;
  std::string Name = "neverd_mem_load_";
  if (Ordering != NdMemoryOrdering::None) {
    validateAtomicLoadOrdering(Ordering);
    Name += std::string(memoryOrderingName(Ordering)) + "_";
  }
  return Name + std::to_string(Index) + "((uintptr_t)(" + Addr.str() + "))";
}

std::string HighCWriter::memoryStoreExpr(const TypeRef &Ty,
                                         llvm::StringRef Addr,
                                         llvm::StringRef Val,
                                         NdMemoryOrdering Ordering) const {
  std::string Type = memoryTypeName(Ty);
  auto It = MemoryTypes.find(Type);
  if (It == MemoryTypes.end())
    llvm::report_fatal_error("HighC memory store type was not collected");
  unsigned Index = It->second;
  std::string Name = "neverd_mem_store_";
  if (Ordering != NdMemoryOrdering::None) {
    validateAtomicStoreOrdering(Ordering);
    Name += std::string(memoryOrderingName(Ordering)) + "_";
  }
  return Name + std::to_string(Index) + "((uintptr_t)(" + Addr.str() + "), " +
         Val.str() + ")";
}

std::string HighCWriter::atomicExchangeExpr(
    const TypeRef &Ty, llvm::StringRef Addr, llvm::StringRef Val,
    NdMemoryOrdering Ordering) const {
  if (Ordering == NdMemoryOrdering::None)
    llvm::report_fatal_error("atomic exchange requires memory ordering");
  std::string Type = memoryTypeName(Ty);
  return "__atomic_exchange_n((" + Type + " *)(uintptr_t)(" + Addr.str() +
         "), (" + Type + ")(" + Val.str() + "), " +
         atomicOrderingToken(Ordering) + ")";
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
