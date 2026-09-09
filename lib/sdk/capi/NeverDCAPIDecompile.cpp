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
#include "neverd/evm/analysis/EVMAnalyzer.h"
#include "neverd/evm/emit/EVMCEmitter.h"
#include "neverd/evm/emit/EVMSolidityEmitter.h"
#include "neverd/ir/NdOps.h"
#include "neverd/sbf/analysis/SBFAnalyzer.h"
#include "neverd/sbf/emit/SBFCEmitter.h"
#include "neverd/sbf/emit/SBFRustEmitter.h"

#include <limits>
#include <set>
#include <stdexcept>

using namespace neverd;
using namespace neverd::sdk;

namespace {
struct IRRowOrigin {
  std::string ObjectId;
  const char *Kind = "decoration";
  const char *Status = "unmapped";
  std::optional<va_t> Address;
  int Sequence = -1;
};
using IRRowSink = std::function<void(llvm::StringRef, const IRRowOrigin &)>;

std::set<std::pair<va_t, int>> instructionOrigins(const LowFunc &F) {
  std::set<std::pair<va_t, int>> Origins;
  for (const auto &B : F.Blocks)
    for (const auto &Boundary : B.InstructionBoundaries) {
      if (Boundary.FirstOp > B.Ops.size() ||
          Boundary.OpCount > B.Ops.size() - Boundary.FirstOp)
        continue;
      for (uint64_t I = Boundary.FirstOp;
           I < Boundary.FirstOp + Boundary.OpCount; ++I) {
        const auto &Op = B.Ops[I];
        if (Op.Seq >= 0 && Op.Addr == Boundary.Address && Boundary.Size > 0)
          Origins.emplace(Op.Addr, Op.Seq);
      }
    }
  return Origins;
}

std::string rowId(llvm::StringRef Stage, va_t Entry, int Block,
                  llvm::StringRef Kind, size_t Index = 0) {
  return (Stage + ":" + vaHex(Entry) + ":block:" + std::to_string(Block) + ":" +
          Kind + ":" + std::to_string(Index))
      .str();
}

// Both legacy text and mapped pages use these exact emission points. A name or
// operand containing a newline is split into physical rows by the page sink,
// rather than guessed from a parallel line-count model.
void emitLowView(const LowFunc &F, const IRRowSink &Sink,
                 bool CollectAnchors = false) {
  const auto Origins =
      CollectAnchors ? instructionOrigins(F) : std::set<std::pair<va_t, int>>();
  auto Emit = [&](const IRRowOrigin &Origin, auto Write) {
    std::string Text;
    llvm::raw_string_ostream OS(Text);
    Write(OS);
    Sink(Text, Origin);
  };
  Emit({"low:" + vaHex(F.Entry) + ":header", "header"}, [&](auto &OS) {
    OS << "; LowIR: " << F.Name << " @ " << vaHex(F.Entry) << "\n";
  });
  for (const auto &B : F.Blocks) {
    Emit({rowId("low", F.Entry, B.Id, "block"), "block"}, [&](auto &OS) {
      OS << "block_" << B.Id << ":  ; [" << vaHex(B.StartAddr) << " - "
         << vaHex(B.EndAddr) << ")\n";
    });
    for (size_t Index = 0; Index < B.Ops.size(); ++Index) {
      const auto &Op = B.Ops[Index];
      IRRowOrigin Origin{rowId("low", F.Entry, B.Id, "op", Index) + ":" +
                             vaHex(Op.Addr) + ":seq:" + std::to_string(Op.Seq),
                         "operation"};
      if (Origins.count({Op.Addr, Op.Seq})) {
        Origin.Address = Op.Addr;
        Origin.Sequence = Op.Seq;
        Origin.Status = "instruction_anchor";
      }
      Emit(Origin, [&](auto &OS) {
        OS << "  " << ndOpName(Op.Opcode);
        if (Op.Output.Size > 0)
          OS << " -> (" << static_cast<int>(Op.Output.Space) << ":"
             << Op.Output.Offset << ":" << Op.Output.Size << ")";
        for (int I = 0; I < Op.NumInputs; ++I)
          OS << " (" << static_cast<int>(Op.Inputs[I].Space) << ":"
             << Op.Inputs[I].Offset << ":" << Op.Inputs[I].Size << ")";
        OS << "\n";
      });
    }
    Emit({rowId("low", F.Entry, B.Id, "successors"), "successors"},
         [&](auto &OS) {
           OS << "  succs: [";
           for (size_t I = 0; I < B.Succs.size(); ++I) {
             if (I)
               OS << ", ";
             OS << B.Succs[I];
           }
           OS << "]\n";
         });
  }
}

void emitMedView(const MedFunc &F, const LowFunc *Low, const IRRowSink &Sink) {
  const auto LowOrigins =
      Low ? instructionOrigins(*Low) : std::set<std::pair<va_t, int>>();
  auto Emit = [&](const IRRowOrigin &Origin, auto Write) {
    std::string Text;
    llvm::raw_string_ostream OS(Text);
    Write(OS);
    Sink(Text, Origin);
  };
  Emit({"med:" + vaHex(F.Entry) + ":header", "header"}, [&](auto &OS) {
    OS << "; MedIR: " << F.Name << " @ " << vaHex(F.Entry) << "\n";
  });
  Emit({"med:" + vaHex(F.Entry) + ":frame", "header"}, [&](auto &OS) {
    OS << "; CC: " << static_cast<int>(F.CC) << " FrameSize: " << F.FrameSize
       << "\n";
  });
  for (const auto &B : F.Blocks) {
    Emit({rowId("med", F.Entry, B.Id, "block"), "block"},
         [&](auto &OS) { OS << "block_" << B.Id << ":\n"; });
    for (size_t Index = 0; Index < B.Phis.size(); ++Index) {
      const auto &Phi = B.Phis[Index];
      Emit({rowId("med", F.Entry, B.Id, "phi", Index), "phi", "synthetic"},
           [&](auto &OS) {
             OS << "  PHI " << Phi.Output.display() << " = [";
             for (size_t I = 0; I < Phi.Args.size(); ++I) {
               if (I)
                 OS << ", ";
               OS << "b" << Phi.Args[I].first << ":"
                  << Phi.Args[I].second.display();
             }
             OS << "]\n";
           });
    }
    for (size_t Index = 0; Index < B.Ops.size(); ++Index) {
      const auto &Op = B.Ops[Index];
      IRRowOrigin Origin{
          rowId("med", F.Entry, B.Id, "op", Index) + ":" + vaHex(Op.Addr) +
              ":seq:" + std::to_string(Op.OriginSeq),
          "operation", Op.OriginSeq < 0 ? "synthetic" : "unmapped"};
      if (Op.OriginSeq >= 0 && LowOrigins.count({Op.Addr, Op.OriginSeq})) {
        Origin.Address = Op.Addr;
        Origin.Sequence = Op.OriginSeq;
        Origin.Status = "instruction_anchor";
      }
      Emit(Origin, [&](auto &OS) {
        OS << "  " << ndOpName(Op.Opcode);
        if (Op.Output.Id >= 0)
          OS << " " << Op.Output.display() << " =";
        for (int I = 0; I < Op.NumInputs; ++I)
          OS << " " << Op.Inputs[I].display();
        OS << "\n";
      });
    }
    Emit({rowId("med", F.Entry, B.Id, "successors"), "successors"},
         [&](auto &OS) {
           OS << "  succs: [";
           for (size_t I = 0; I < B.Succs.size(); ++I) {
             if (I)
               OS << ", ";
             OS << B.Succs[I];
           }
           OS << "]\n";
         });
  }
}
} // namespace

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

  if (S->PipeResult.SBF) {
    auto Output = sbf::emitC(*S->PipeResult.SBF);
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
  if (S->PipeResult.SBF)
    return dupStr(sbf::dumpLowIR(S->PipeResult.SBF->Low));

  const LowFunc *F = S->findLowFunc(FuncEntry);
  if (!F) {
    S->setError("function not found in LowIR");
    return dupStr(std::string());
  }

  std::string Out;
  emitLowView(*F, [&](llvm::StringRef Text, const IRRowOrigin &) {
    Out.append(Text.data(), Text.size());
  });

  return dupStr(Out);
}

const char *neverd_ir_med(neverd_session_t Sess, neverd_va_t FuncEntry) {
  auto *S = toSession(Sess);
  S->clearError();

  if (!S->ensurePipeline())
    return dupStr(std::string());

  if (S->PipeResult.EVM)
    return dupStr(evm::dumpMedIR(S->PipeResult.EVM->Med));
  if (S->PipeResult.SBF)
    return dupStr(sbf::dumpMedIR(S->PipeResult.SBF->Med));

  for (const auto &F : S->PipeResult.MedFuncs) {
    if (F.Entry != FuncEntry)
      continue;

    std::string Out;
    emitMedView(F, nullptr, [&](llvm::StringRef Text, const IRRowOrigin &) {
      Out.append(Text.data(), Text.size());
    });
    return dupStr(Out);
  }

  S->setError("function not found in MedIR");
  return dupStr(std::string());
}

const char *neverd_ir_view_json(neverd_session_t Sess, neverd_va_t FuncEntry,
                                const char *Representation, size_t Offset,
                                size_t Limit) {
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  S->clearError();
  try {
    if (!Representation || Limit == 0 || Limit > 2048 ||
        Offset > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
      S->setError("invalid IR view representation or paging bounds");
      return nullptr;
    }
    const llvm::StringRef Stage(Representation);
    if (!llvm::json::isUTF8(Stage)) {
      S->setError("IR view representation is not valid UTF-8");
      return nullptr;
    }
    llvm::json::Object Result;
    Result["schema_version"] = 1;
    Result["address"] = vaHex(FuncEntry);
    Result["representation"] = Stage;
    Result["rows"] = llvm::json::Array();
    if (Stage != "low" && Stage != "med") {
      Result["mapping_status"] = "unsupported_representation";
      return dupStr(jsonToString(llvm::json::Value(std::move(Result))));
    }
    if (!S->ensurePipeline())
      return nullptr;
    if (S->PipeResult.EVM || S->PipeResult.SBF) {
      Result["mapping_status"] = "unsupported_architecture";
      return dupStr(jsonToString(llvm::json::Value(std::move(Result))));
    }
    const LowFunc *Low = S->findLowFunc(FuncEntry);
    const MedFunc *Med = nullptr;
    if (Stage == "med")
      for (const auto &F : S->PipeResult.MedFuncs)
        if (F.Entry == FuncEntry) {
          Med = &F;
          break;
        }
    if ((Stage == "low" && !Low) || (Stage == "med" && !Med)) {
      S->setError("function not found in requested IR stage");
      return nullptr;
    }
    size_t Total = 0;
    std::string Text;
    llvm::json::Array Rows;
    const IRRowSink Sink = [&](llvm::StringRef Chunk,
                               const IRRowOrigin &Origin) {
      size_t Start = 0;
      while (Start < Chunk.size()) {
        const auto Newline = Chunk.find('\n', Start);
        const auto End =
            Newline == llvm::StringRef::npos ? Chunk.size() : Newline + 1;
        if (Total >= Offset && Total - Offset < Limit) {
          if (End - Start > 2 * 1024 * 1024 - Text.size())
            throw std::length_error(
                "IR view page exceeds the 2 MiB budget; request fewer lines");
          Text.append(Chunk.data() + Start, End - Start);
          llvm::json::Object Row;
          Row["line"] = static_cast<int64_t>(Total);
          Row["object_id"] = Origin.ObjectId;
          Row["kind"] = Origin.Kind;
          Row["mapping_status"] = Origin.Status;
          llvm::json::Array Addresses;
          if (Origin.Address)
            Addresses.push_back(vaHex(*Origin.Address));
          Row["addresses"] = std::move(Addresses);
          if (Origin.Sequence >= 0)
            Row["origin_seq"] = Origin.Sequence;
          Rows.push_back(std::move(Row));
        }
        ++Total;
        Start = End;
      }
    };
    if (Stage == "low")
      emitLowView(*Low, Sink, true);
    else
      emitMedView(*Med, Low, Sink);
    if (!llvm::json::isUTF8(Text)) {
      S->setError("IR view page text is not valid UTF-8");
      return nullptr;
    }
    const size_t End = std::min(Offset, Total) + Rows.size();
    Result["mapping_status"] = "instruction_anchors";
    Result["provenance_complete"] = false;
    Result["text"] = std::move(Text);
    Result["rows"] = std::move(Rows);
    Result["offset"] = static_cast<int64_t>(Offset);
    Result["total_lines"] = static_cast<int64_t>(Total);
    Result["complete"] = End == Total;
    if (End == Total)
      Result["next_offset"] = nullptr;
    else
      Result["next_offset"] = static_cast<int64_t>(End);
    return dupStr(jsonToString(llvm::json::Value(std::move(Result))));
  } catch (const std::exception &Error) {
    S->setError(Error.what());
    return nullptr;
  } catch (...) {
    S->setError("unexpected IR view failure");
    return nullptr;
  }
}

const char *neverd_ir_high(neverd_session_t Sess, neverd_va_t FuncEntry) {
  auto *S = toSession(Sess);
  S->clearError();

  if (!S->ensurePipeline())
    return dupStr(std::string());

  if (S->PipeResult.EVM)
    return dupStr(evm::dumpHighIR(S->PipeResult.EVM->High));
  if (S->PipeResult.SBF)
    return dupStr(sbf::dumpHighIR(S->PipeResult.SBF->High));

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
  if (S->PipeResult.SBF)
    return dupStr(sbf::emitLLVMText(*S->PipeResult.LlvmModule));

  const llvm::Function *LF = S->findNativeLlvmFunction(FuncEntry);
  if (!LF)
    return dupStr(std::string());

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
  Opts.MaxFunctions = MaxFunctions > 0 ? static_cast<size_t>(MaxFunctions) : 0;
  if (S) {
    S->applyAnalysisOptions(Opts);
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
  Opts.MaxFunctions = MaxFunctions > 0 ? static_cast<size_t>(MaxFunctions) : 0;
  if (S) {
    S->applyAnalysisOptions(Opts);
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
  } else if (R.Result.SBF) {
    if (Level == 0)
      OS << sbf::dumpLowIR(R.Result.SBF->Low);
    else if (Level == 1)
      OS << sbf::dumpMedIR(R.Result.SBF->Med);
    else
      OS << sbf::dumpHighIR(R.Result.SBF->High);
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
  Opts.MaxFunctions = MaxFunctions > 0 ? static_cast<size_t>(MaxFunctions) : 0;
  if (S) {
    S->applyAnalysisOptions(Opts);
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
    if (UseLlvmRoute) {
      if (S)
        S->setError("LLVM-to-C route is not supported for EVM; use the "
                    "dedicated C backend");
      return nullptr;
    }
    if (Language == NEVERD_OUTPUT_SOLIDITY) {
      auto Output = evm::emitSolidity(*R.Result.EVM);
      if (!Output) {
        if (S)
          S->setError(llvm::toString(Output.takeError()));
        return nullptr;
      }
      return dupStr(*Output);
    }
    if (Language == NEVERD_OUTPUT_RUST) {
      if (S)
        S->setError("Rust output is supported only for Solana SBF programs");
      return nullptr;
    }
    auto Output = evm::emitC(*R.Result.EVM);
    if (!Output) {
      if (S)
        S->setError(llvm::toString(Output.takeError()));
      return nullptr;
    }
    return dupStr(*Output);
  }

  if (R.Result.SBF) {
    if (UseLlvmRoute) {
      if (S)
        S->setError("LLVM-to-C route is not supported for SBF; use the "
                    "dedicated C or Rust backend");
      return nullptr;
    }
    if (Language == NEVERD_OUTPUT_SOLIDITY) {
      if (S)
        S->setError("Solidity output is supported only for EVM bytecode");
      return nullptr;
    }
    if (Language == NEVERD_OUTPUT_RUST) {
      auto Output = sbf::emitRust(*R.Result.SBF);
      if (!Output) {
        if (S)
          S->setError(llvm::toString(Output.takeError()));
        return nullptr;
      }
      return dupStr(*Output);
    }
    auto Output = sbf::emitC(*R.Result.SBF);
    if (!Output) {
      if (S)
        S->setError(llvm::toString(Output.takeError()));
      return nullptr;
    }
    return dupStr(*Output);
  }

  if (Language == NEVERD_OUTPUT_SOLIDITY || Language == NEVERD_OUTPUT_RUST) {
    if (S)
      S->setError(
          Language == NEVERD_OUTPUT_SOLIDITY
              ? "Solidity output is supported only for EVM bytecode"
              : "Rust output is supported only for Solana SBF programs");
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
  return decompileAllImpl(Sess, InputPath, UseLlvmRoute, NEVERD_OUTPUT_C, NoOpt,
                          MaxFunctions);
}

const char *neverd_decompile_all_ex(neverd_session_t Sess,
                                    const char *InputPath,
                                    neverd_output_language_t Language,
                                    int NoOpt, int MaxFunctions) {
  const bool KnownLanguage = [&] {
    switch (Language) {
#define NEVERD_OUTPUT_LANGUAGE(NAME, VALUE, SPELLING, DISPLAY_NAME)            \
  case NEVERD_OUTPUT_##NAME:                                                   \
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
