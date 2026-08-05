//===- NeverDCAPIDecompile.cpp - C API: decompilation and IR
//---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Single-function decompilation, multi-stage IR dump (Low/Med/High/LLVM),
/// and high-level pipeline operations for lift and decompile-all.
///
//===----------------------------------------------------------------------===//

#include "SessionImpl.h"

#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/ir/NdOps.h"

using namespace neverd;
using namespace neverd::sdk;

// ===--------------------------------------------------------------------===//
// Single-function decompilation
// ===--------------------------------------------------------------------===//

const char *neverd_decompile(neverd_session_t Sess, neverd_va_t FuncEntry) {
  auto *S = toSession(Sess);
  S->clearError();

  if (!S->ensurePipeline())
    return dupStr(std::string());

  const HighFunc *HF = S->findHighFunc(FuncEntry);
  if (!HF) {
    S->setError("function not found in HighIR");
    return dupStr(std::string());
  }

  std::vector<HighFunc> Single = {*HF};
  std::string Out;
  llvm::raw_string_ostream OS(Out);

  CEmitterOptions Opts;
  Opts.TheArch = S->Img.Arch;
  Opts.Format = S->Img.Format;
  HighCEmitter Emitter;
  Emitter.emit(Single, OS, Opts);

  return dupStr(Out);
}

// ===--------------------------------------------------------------------===//
// Multi-stage IR
// ===--------------------------------------------------------------------===//

const char *neverd_ir_low(neverd_session_t Sess, neverd_va_t FuncEntry) {
  auto *S = toSession(Sess);
  S->clearError();

  if (!S->ensurePipeline())
    return dupStr(std::string());

  const LowFunc *F = S->findLowFunc(FuncEntry);
  if (!F) {
    S->setError("function not found in LowIR");
    return dupStr(std::string());
  }

  std::string Out;
  llvm::raw_string_ostream OS(Out);

  OS << "; LowIR: " << F->Name << " @ " << vaHex(F->Entry) << "\n";
  for (const auto &B : F->Blocks) {
    OS << "block_" << B.Id << ":  ; [" << vaHex(B.StartAddr) << " - "
       << vaHex(B.EndAddr) << ")\n";
    for (const auto &Op : B.Ops) {
      OS << "  " << ndOpName(Op.Opcode);
      if (Op.Output.Size > 0)
        OS << " -> (" << static_cast<int>(Op.Output.Space) << ":"
           << Op.Output.Offset << ":" << Op.Output.Size << ")";
      for (int I = 0; I < Op.NumInputs; ++I)
        OS << " (" << static_cast<int>(Op.Inputs[I].Space) << ":"
           << Op.Inputs[I].Offset << ":" << Op.Inputs[I].Size << ")";
      OS << "\n";
    }
    OS << "  succs: [";
    for (size_t I = 0; I < B.Succs.size(); ++I) {
      if (I)
        OS << ", ";
      OS << B.Succs[I];
    }
    OS << "]\n";
  }

  return dupStr(Out);
}

const char *neverd_ir_med(neverd_session_t Sess, neverd_va_t FuncEntry) {
  auto *S = toSession(Sess);
  S->clearError();

  if (!S->ensurePipeline())
    return dupStr(std::string());

  for (const auto &F : S->PipeResult.MedFuncs) {
    if (F.Entry != FuncEntry)
      continue;

    std::string Out;
    llvm::raw_string_ostream OS(Out);
    OS << "; MedIR: " << F.Name << " @ " << vaHex(F.Entry) << "\n";
    OS << "; CC: " << static_cast<int>(F.CC) << " FrameSize: " << F.FrameSize
       << "\n";
    for (const auto &B : F.Blocks) {
      OS << "block_" << B.Id << ":\n";
      for (const auto &Phi : B.Phis) {
        OS << "  PHI " << Phi.Output.display() << " = [";
        for (size_t I = 0; I < Phi.Args.size(); ++I) {
          if (I)
            OS << ", ";
          OS << "b" << Phi.Args[I].first << ":" << Phi.Args[I].second.display();
        }
        OS << "]\n";
      }
      for (const auto &Op : B.Ops) {
        OS << "  " << ndOpName(Op.Opcode);
        if (Op.Output.Id >= 0)
          OS << " " << Op.Output.display() << " =";
        for (int I = 0; I < Op.NumInputs; ++I)
          OS << " " << Op.Inputs[I].display();
        OS << "\n";
      }
      OS << "  succs: [";
      for (size_t I = 0; I < B.Succs.size(); ++I) {
        if (I)
          OS << ", ";
        OS << B.Succs[I];
      }
      OS << "]\n";
    }
    return dupStr(Out);
  }

  S->setError("function not found in MedIR");
  return dupStr(std::string());
}

const char *neverd_ir_high(neverd_session_t Sess, neverd_va_t FuncEntry) {
  auto *S = toSession(Sess);
  S->clearError();

  if (!S->ensurePipeline())
    return dupStr(std::string());

  const HighFunc *HF = S->findHighFunc(FuncEntry);
  if (!HF) {
    S->setError("function not found in HighIR");
    return dupStr(std::string());
  }

  std::string Out;
  llvm::raw_string_ostream OS(Out);
  OS << "; HighIR: " << HF->Name << " @ " << vaHex(HF->Entry) << "\n";
  OS << "; Params: " << HF->Params.size() << ", Locals: " << HF->Locals.size()
     << "\n\n";
  for (const auto &Stmt : HF->Body)
    OS << Stmt.str(0) << "\n";

  return dupStr(Out);
}

const char *neverd_ir_llvm(neverd_session_t Sess, neverd_va_t FuncEntry) {
  auto *S = toSession(Sess);
  S->clearError();

  if (!S->ensureLlvmModule()) {
    S->setError("failed to generate LLVM module");
    return dupStr(std::string());
  }

  std::string FuncName = "sub_" + llvm::utohexstr(FuncEntry);

  llvm::Function *LF = S->PipeResult.LlvmModule->getFunction(FuncName);
  if (!LF) {
    for (auto &F : *S->PipeResult.LlvmModule) {
      if (F.getName().starts_with(FuncName)) {
        LF = &F;
        break;
      }
    }
  }

  if (!LF) {
    S->setError("LLVM function not found");
    return dupStr(std::string());
  }

  std::string Out;
  llvm::raw_string_ostream OS(Out);
  LF->print(OS);

  return dupStr(Out);
}

// ===--------------------------------------------------------------------===//
// High-level pipeline: lift module
// ===--------------------------------------------------------------------===//

const char *neverd_lift_module(neverd_session_t Sess, const char *InputPath,
                               int NoOpt, int MaxFunctions) {
  auto *S = static_cast<Session *>(Sess);
  PipelineRunner R;
  std::string Err;
  if (!R.load(InputPath, Err)) {
    if (S)
      S->setError(Err);
    return nullptr;
  }

  PipelineOptions Opts;
  Opts.LiftMode = true;
  Opts.NoOpt = NoOpt != 0;
  Opts.MaxFunctions =
      MaxFunctions > 0 ? static_cast<size_t>(MaxFunctions) : 0;
  if (!R.run(Opts, Err)) {
    if (S)
      S->setError(Err);
    return nullptr;
  }
  if (!R.Result.LlvmModule)
    return nullptr;

  std::string Buf;
  llvm::raw_string_ostream OS(Buf);
  R.Result.LlvmModule->print(OS, nullptr);
  return dupStr(Buf);
}

// ===--------------------------------------------------------------------===//
// High-level pipeline: lift dump (LowIR / MedIR / HighIR)
// ===--------------------------------------------------------------------===//

const char *neverd_lift_dump(neverd_session_t Sess, const char *InputPath,
                             int Level, int MaxFunctions) {
  auto *S = static_cast<Session *>(Sess);
  PipelineRunner R;
  std::string Err;
  if (!R.load(InputPath, Err)) {
    if (S)
      S->setError(Err);
    return nullptr;
  }

  PipelineOptions Opts;
  Opts.MaxFunctions =
      MaxFunctions > 0 ? static_cast<size_t>(MaxFunctions) : 0;
  if (Level == 0)
    Opts.DumpLow = true;
  else if (Level == 1)
    Opts.DumpMed = true;
  else
    Opts.DumpHigh = true;
  if (!R.run(Opts, Err)) {
    if (S)
      S->setError(Err);
    return nullptr;
  }

  std::string Buf;
  llvm::raw_string_ostream OS(Buf);
  if (Level == 0) {
    OS << "=== LowIR Dump ===\n";
    for (const auto &F : R.Result.LowFuncs) {
      OS << "func " << F.Name << " @ 0x" << llvm::utohexstr(F.Entry) << " ("
         << F.Blocks.size() << " blocks)\n";
      for (const auto &B : F.Blocks) {
        OS << "  bb" << B.Id << " [0x" << llvm::utohexstr(B.StartAddr)
           << " - 0x" << llvm::utohexstr(B.EndAddr) << "] (" << B.Ops.size()
           << " ops)\n";
      }
    }
  } else if (Level == 1) {
    OS << "=== MedIR Dump ===\n";
    for (const auto &F : R.Result.MedFuncs) {
      OS << "func " << F.Name << " @ 0x" << llvm::utohexstr(F.Entry) << " ("
         << F.Blocks.size() << " blocks)\n";
      for (const auto &B : F.Blocks) {
        OS << "  bb" << B.Id << " (" << B.Ops.size() << " ops)\n";
      }
    }
  } else {
    OS << "=== HighIR Dump ===\n";
    for (const auto &F : R.Result.HighFuncs) {
      OS << "func " << F.Name << " @ 0x" << llvm::utohexstr(F.Entry) << " ("
         << F.Body.size() << " stmts, " << F.Locals.size() << " locals)\n";
    }
  }
  return dupStr(Buf);
}

// ===--------------------------------------------------------------------===//
// High-level pipeline: decompile all
// ===--------------------------------------------------------------------===//

const char *neverd_decompile_all(neverd_session_t Sess, const char *InputPath,
                                 int UseLlvmRoute, int NoOpt,
                                 int MaxFunctions) {
  auto *S = static_cast<Session *>(Sess);
  PipelineRunner R;
  std::string Err;
  if (!R.load(InputPath, Err)) {
    if (S)
      S->setError(Err);
    return nullptr;
  }

  PipelineOptions Opts;
  Opts.NoOpt = NoOpt != 0;
  Opts.MaxFunctions =
      MaxFunctions > 0 ? static_cast<size_t>(MaxFunctions) : 0;
  if (UseLlvmRoute)
    Opts.LiftMode = true;
  if (!R.run(Opts, Err)) {
    if (S)
      S->setError(Err);
    return nullptr;
  }

  std::string Buf;
  llvm::raw_string_ostream OS(Buf);

  if (UseLlvmRoute) {
    if (!R.Result.LlvmModule)
      return nullptr;
    CEmitterOptions COpts;
    COpts.TheArch = R.Img.Arch;
    COpts.Format = R.Img.Format;
    LLVMCEmitter Emitter;
    Emitter.emit(*R.Result.LlvmModule, OS, COpts, R.Dbg.get(), &R.Img);
  } else {
    CEmitterOptions COpts;
    COpts.TheArch = R.Img.Arch;
    COpts.Format = R.Img.Format;
    HighCEmitter Emitter;
    Emitter.emit(R.Result.HighFuncs, OS, COpts, R.Dbg.get());
  }
  return dupStr(Buf);
}

// ===--------------------------------------------------------------------===//
// Inject hello world pass
// ===--------------------------------------------------------------------===//

int neverd_inject_hello(neverd_session_t Sess) {
  auto *S = static_cast<Session *>(Sess);
  if (!S || !S->PipeResult.LlvmModule) {
    if (S)
      S->setError("no LLVM module available");
    return 1;
  }
  Pipeline::runHelloWorldPass(*S->PipeResult.LlvmModule);
  return 0;
}
