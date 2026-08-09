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
#include "neverd/evm/Analyzer.h"
#include "neverd/evm/CEmitter.h"
#include "neverd/evm/SolidityEmitter.h"
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

  if (S->PipeResult.EVM) {
    auto Output = evm::emitC(*S->PipeResult.EVM);
    if (!Output) {
      S->setError(llvm::toString(Output.takeError()));
      return dupStr(std::string());
    }
    return dupStr(*Output);
  }

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

  if (S->PipeResult.EVM)
    return dupStr(evm::dumpLowIR(S->PipeResult.EVM->Low));

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

  if (S->PipeResult.EVM)
    return dupStr(evm::dumpMedIR(S->PipeResult.EVM->Med));

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

  if (S->PipeResult.EVM)
    return dupStr(evm::dumpHighIR(S->PipeResult.EVM->High));

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
    if (S->LastError.empty())
      S->setError("failed to generate LLVM module");
    return dupStr(std::string());
  }

  if (S->PipeResult.EVM)
    return dupStr(evm::emitLLVMText(*S->PipeResult.LlvmModule));

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
  if (S) {
    Opts.EVMFork = S->EVMFork;
    Opts.EVMStrict = S->EVMStrict;
  }
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
  if (S) {
    Opts.EVMFork = S->EVMFork;
    Opts.EVMStrict = S->EVMStrict;
  }
  if (Level == 0)
    Opts.DumpLow = true;
  else if (Level == 1)
    Opts.DumpMed = true;
  else
    Opts.DumpHigh = true;
  // The C API returns the dump as a string. Printing it from inside the shared
  // library as well gives Windows DLL and executable instances of llvm::outs()
  // independent buffers whose flush order is not deterministic.
  Opts.EmitDumpOutput = false;
  if (!R.run(Opts, Err)) {
    if (S)
      S->setError(Err);
    return nullptr;
  }

  std::string Buf;
  llvm::raw_string_ostream OS(Buf);
  if (R.Result.EVM) {
    if (Level == 0)
      OS << evm::dumpLowIR(R.Result.EVM->Low);
    else if (Level == 1)
      OS << evm::dumpMedIR(R.Result.EVM->Med);
    else
      OS << evm::dumpHighIR(R.Result.EVM->High);
  } else if (Level == 0)
    Pipeline::dumpLowIR(R.Result.LowFuncs, OS);
  else if (Level == 1)
    Pipeline::dumpMedIR(R.Result.MedFuncs, OS);
  else
    Pipeline::dumpHighIR(R.Result.HighFuncs, OS);
  return dupStr(Buf);
}

// ===--------------------------------------------------------------------===//
// High-level pipeline: decompile all
// ===--------------------------------------------------------------------===//

static const char *decompileAllImpl(neverd_session_t Sess,
                                    const char *InputPath, int UseLlvmRoute,
                                    neverd_output_language_t Language,
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
  Opts.NoOpt = NoOpt != 0;
  Opts.MaxFunctions =
      MaxFunctions > 0 ? static_cast<size_t>(MaxFunctions) : 0;
  if (S) {
    Opts.EVMFork = S->EVMFork;
    Opts.EVMStrict = S->EVMStrict;
  }
  if (UseLlvmRoute)
    Opts.LiftMode = true;
  if (!R.run(Opts, Err)) {
    if (S)
      S->setError(Err);
    return nullptr;
  }

  std::string Buf;
  llvm::raw_string_ostream OS(Buf);

  if (R.Result.EVM) {
    if (Language == NEVERD_OUTPUT_SOLIDITY) {
      auto Output = evm::emitSolidity(*R.Result.EVM);
      if (!Output) {
        if (S)
          S->setError(llvm::toString(Output.takeError()));
        return nullptr;
      }
      return dupStr(*Output);
    }
    auto Output = evm::emitC(*R.Result.EVM);
    if (!Output) {
      if (S)
        S->setError(llvm::toString(Output.takeError()));
      return nullptr;
    }
    return dupStr(*Output);
  }

  if (Language == NEVERD_OUTPUT_SOLIDITY) {
    if (S)
      S->setError("Solidity output is supported only for EVM bytecode");
    return nullptr;
  }

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

const char *neverd_decompile_all(neverd_session_t Sess, const char *InputPath,
                                 int UseLlvmRoute, int NoOpt,
                                 int MaxFunctions) {
  return decompileAllImpl(Sess, InputPath, UseLlvmRoute, NEVERD_OUTPUT_C,
                          NoOpt, MaxFunctions);
}

const char *neverd_decompile_all_ex(neverd_session_t Sess,
                                    const char *InputPath,
                                    neverd_output_language_t Language,
                                    int NoOpt, int MaxFunctions) {
  const bool KnownLanguage = [&] {
    switch (Language) {
#define NEVERD_OUTPUT_LANGUAGE(NAME, VALUE, SPELLING, DISPLAY_NAME)            \
    case NEVERD_OUTPUT_##NAME:                                                \
      return true;
#include "neverd/OutputLanguages.def"
    }
    return false;
  }();
  if (!KnownLanguage) {
    if (auto *S = static_cast<Session *>(Sess))
      S->setError("unknown output language");
    return nullptr;
  }
  return decompileAllImpl(Sess, InputPath, 0, Language, NoOpt, MaxFunctions);
}

void neverd_evm_set_strict(neverd_session_t Sess, int Strict) {
  auto *S = static_cast<Session *>(Sess);
  if (!S)
    return;
  S->clearError();
  S->EVMStrict = Strict != 0;
  S->PipeRan = false;
  S->PipeResult = {};
}

int neverd_evm_set_hardfork(neverd_session_t Sess, const char *Hardfork) {
  auto *S = static_cast<Session *>(Sess);
  if (!S || !Hardfork)
    return 0;
  auto Fork = evm::parseHardfork(Hardfork);
  if (!Fork) {
    S->setError("unknown EVM hardfork: " + std::string(Hardfork));
    return 0;
  }
  S->clearError();
  S->EVMFork = *Fork;
  S->PipeRan = false;
  S->PipeResult = {};
  return 1;
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
