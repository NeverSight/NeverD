//===- NeverDCAPIPatch.cpp - C API: binary patching -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Patch operations: from LLVM IR text, from C source, and the full
/// high-level patch pipeline.
///
//===----------------------------------------------------------------------===//

#include "SessionImpl.h"

#include "neverd/ArchSupport.h"
#include "neverd/backend/codegen/CodeGen.h"

#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/WithColor.h"

using namespace neverd;
using namespace neverd::sdk;

namespace {

#ifdef __APPLE__
// Resolve the active macOS SDK path via $SDKROOT or `xcrun --show-sdk-path`.
// Returns empty if it cannot be determined.  Needed so `--from-c` can find
// system headers (<stdio.h>, …) when clang is invoked with an explicit
// --target= (which otherwise suppresses the default SDK header search paths).
std::string detectMacOSSDKPath() {
  if (const char *Env = std::getenv("SDKROOT"))
    if (*Env)
      return Env;

  auto Xcrun = llvm::sys::findProgramByName("xcrun");
  if (!Xcrun)
    return {};

  llvm::SmallString<128> OutFile;
  if (llvm::sys::fs::createTemporaryFile("neverd-sdk", "txt", OutFile))
    return {};

  llvm::StringRef XcrunArgs[] = {*Xcrun, "--show-sdk-path"};
  std::optional<llvm::StringRef> Redirects[] = {std::nullopt, OutFile.str(),
                                                std::nullopt};
  std::string Result;
  if (llvm::sys::ExecuteAndWait(*Xcrun, XcrunArgs, std::nullopt, Redirects) ==
      0)
    if (auto Buf = llvm::MemoryBuffer::getFile(OutFile))
      Result = (*Buf)->getBuffer().trim().str();

  auto EC = llvm::sys::fs::remove(OutFile);
  (void)EC;
  return Result;
}
#endif

// Apply the session's enabled obfuscation passes to \p Mod in the canonical
// patch order (Pipeline::runObfuscationPasses) and record the per-pass counts
// back on the session.  Shared by all three patch entry points so the toggle
// handling lives in one place; a null session applies nothing.
void applySessionObfuscation(Session *S, llvm::Module &Mod) {
  if (!S)
    return;
  Pipeline::ObfuscationConfig Cfg;
  Cfg.InstSubstitution = S->InstSubstitution;
  Cfg.InstSubstitutionRounds = S->InstSubstitutionRounds;
  Cfg.ConstantEncryption = S->ConstantEncryption;
  Cfg.OpaquePredicate = S->OpaquePredicate;
  Cfg.BogusControlFlow = S->BogusControlFlow;
  Cfg.ControlFlowFlattening = S->ControlFlowFlattening;
  Cfg.IndirectBranch = S->IndirectBranch;
  Cfg.IndirectCall = S->IndirectCall;
  Cfg.MBA = S->MBA;
  Cfg.IndirectGlobal = S->IndirectGlobal;
  Cfg.ValueLaunder = S->ValueLaunder;
  Cfg.ConstantPooling = S->ConstantPooling;
  Cfg.BitMasking = S->BitMasking;

  auto C = Pipeline::runObfuscationPasses(Mod, Cfg);

  S->LastSubstitutionCount = C.Substitution;
  S->LastConstEncCount = C.ConstEnc;
  S->LastOpaquePredCount = C.OpaquePred;
  S->LastBogusCount = C.Bogus;
  S->LastFlattenCount = C.Flatten;
  S->LastIndirectBranchCount = C.IndirectBranch;
  S->LastIndirectCallCount = C.IndirectCall;
  S->LastMBACount = C.MBA;
  S->LastIndirectGlobalCount = C.IndirectGlobal;
  S->LastValueLaunderCount = C.ValueLaunder;
  S->LastConstPoolCount = C.ConstPool;
  S->LastBitMaskCount = C.BitMask;
}

} // namespace

// ===--------------------------------------------------------------------===//
// Patch from LLVM IR text
// ===--------------------------------------------------------------------===//

int neverd_patch_from_ir(neverd_session_t Sess, const char *IRText,
                         int Strategy, const char *OutputPath) {
  auto *S = toSession(Sess);
  if (!S)
    return 0;
  S->clearError();

  if (!S->Loaded) {
    S->setError("no binary loaded");
    return 0;
  }
  if (S->Img.Arch == Arch::EVM || S->Img.Arch == Arch::SBF) {
    S->setError("binary patching is not supported for virtual-machine inputs");
    return 0;
  }
  if (!IRText) {
    S->setError("IR text is null");
    return 0;
  }

  llvm::LLVMContext IRCtx;
  llvm::SMDiagnostic Diag;
  auto MemBuf = llvm::MemoryBuffer::getMemBuffer(IRText, "patch-ir");
  auto Mod = llvm::parseIR(*MemBuf, Diag, IRCtx);
  if (!Mod) {
    std::string ErrMsg;
    llvm::raw_string_ostream OS(ErrMsg);
    Diag.print("neverd_patch_from_ir", OS);
    S->setError(ErrMsg);
    return 0;
  }

  std::filesystem::path OutPath;
  if (OutputPath && *OutputPath)
    OutPath = OutputPath;
  else
    OutPath = S->FilePath.string() + ".patched";

  // PIE/ASLR safety: symbolize absolute in-image pointer constants so
  // codegen emits PC-relative addressing (see symbolizeImageAbsolutePointers).
  symbolizeImageAbsolutePointers(*Mod, S->Img);

  // Obfuscation: cosmetic IR transforms applied after the correctness-critical
  // symbolization, before codegen (canonical order, shared by all entry
  // points).
  applySessionObfuscation(S, *Mod);

  if (Strategy == 1) {
    auto Rewriter = InplaceRewriter::create(S->Img.Format);
    if (!Rewriter) {
      S->setError("inplace rewrite not supported for this format");
      return 0;
    }
    if (!S->TextSectionOverride.empty())
      Rewriter->setTextSectionOverride(S->TextSectionOverride);
    S->LastPatch =
        Rewriter->rewrite(S->FilePath, OutPath, *Mod, S->Img, S->Img.Arch);
  } else {
    auto Patcher = BinaryPatcher::create(S->Img.Format);
    if (!Patcher) {
      S->setError("section patch not supported for this format");
      return 0;
    }
    Patcher->setImageContext(&S->Img);
    if (!S->TextSectionOverride.empty())
      Patcher->setTextSectionOverride(S->TextSectionOverride);
    S->LastPatch = Patcher->patch(S->FilePath, OutPath, *Mod, S->Img.Arch);
  }

  if (!S->LastPatch.Success) {
    S->setError("patch failed");
    return 0;
  }
  return 1;
}

// ===--------------------------------------------------------------------===//
// Patch from C source (invokes clang)
// ===--------------------------------------------------------------------===//

int neverd_patch_from_c(neverd_session_t Sess, const char *CText,
                        neverd_va_t /*FuncAddr*/, const char *OutputPath) {
  auto *S = toSession(Sess);
  if (!S)
    return 0;
  S->clearError();

  if (!S->Loaded) {
    S->setError("no binary loaded");
    return 0;
  }
  if (S->Img.Arch == Arch::EVM || S->Img.Arch == Arch::SBF) {
    S->setError("binary patching is not supported for virtual-machine inputs");
    return 0;
  }
  if (S->Img.Arch == Arch::ARM && S->Img.Format == BinaryFormat::MachO) {
    S->setError("C patching for ARM Mach-O requires an authenticated watchOS "
                "frontend ABI and is not published yet");
    return 0;
  }
  if (!CText) {
    S->setError("C source is null");
    return 0;
  }

  auto ClangPath = llvm::sys::findProgramByName("clang");
  if (!ClangPath) {
    S->setError("clang not found in PATH — required for C compilation");
    return 0;
  }

  llvm::SmallString<128> CFile, IRFile;
  if (auto EC =
          llvm::sys::fs::createTemporaryFile("neverd-patch", "c", CFile)) {
    S->setError("failed to create temp .c file: " + EC.message());
    return 0;
  }
  if (auto EC =
          llvm::sys::fs::createTemporaryFile("neverd-patch", "ll", IRFile)) {
    auto RemoveEC = llvm::sys::fs::remove(CFile);
    (void)RemoveEC;
    S->setError("failed to create temp .ll file: " + EC.message());
    return 0;
  }

  {
    std::error_code EC;
    llvm::raw_fd_ostream OS(CFile, EC);
    if (EC) {
      auto RemoveCEC = llvm::sys::fs::remove(CFile);
      auto RemoveIREC = llvm::sys::fs::remove(IRFile);
      (void)RemoveCEC;
      (void)RemoveIREC;
      S->setError("failed to write temp .c file: " + EC.message());
      return 0;
    }
    OS << CText;
  }

  // Use the loaded image's full target triple (e.g. arm64-apple-macos14.0),
  // not the bare ISA name.  An explicit `--target=aarch64` makes clang treat
  // the OS as unknown and skip the platform's header search paths, so any C
  // using system headers (<stdio.h>, …) fails to compile.  The IR is later
  // recompiled by compileForRewrite with this same triple, so matching here
  // also keeps the ABI / data layout consistent.
  std::string TargetArg =
      std::string("--target=") + llvmCodegenTriple(S->Img.Arch, S->Img.Format);

  std::vector<std::string> ArgStore = {
      *ClangPath, "-emit-llvm", "-S", "-O2", "-fno-discard-value-names",
      TargetArg};

#ifdef __APPLE__
  // For Darwin (Mach-O) targets, pin the SDK sysroot explicitly so header
  // resolution does not depend on the active developer dir or on whichever
  // clang (Apple vs. Homebrew) happens to be first in PATH.
  if (S->Img.Format == BinaryFormat::MachO) {
    std::string SDK = detectMacOSSDKPath();
    if (!SDK.empty()) {
      ArgStore.push_back("-isysroot");
      ArgStore.push_back(SDK);
    }
  }
#endif

  ArgStore.push_back("-o");
  ArgStore.push_back(IRFile.str().str());
  ArgStore.push_back(CFile.str().str());

  std::vector<llvm::StringRef> Args(ArgStore.begin(), ArgStore.end());
  std::string ErrMsg;
  int Ret = llvm::sys::ExecuteAndWait(*ClangPath, Args, std::nullopt, {}, 30, 0,
                                      &ErrMsg);

  {
    auto EC = llvm::sys::fs::remove(CFile);
    (void)EC;
  }

  if (Ret != 0) {
    {
      auto EC = llvm::sys::fs::remove(IRFile);
      (void)EC;
    }
    S->setError("clang compilation failed" +
                (ErrMsg.empty() ? std::string() : ": " + ErrMsg));
    return 0;
  }

  auto IRBufOrErr = llvm::MemoryBuffer::getFile(IRFile);
  {
    auto EC = llvm::sys::fs::remove(IRFile);
    (void)EC;
  }

  if (!IRBufOrErr) {
    S->setError("failed to read compiled IR");
    return 0;
  }

  llvm::LLVMContext IRCtx;
  llvm::SMDiagnostic Diag;
  auto Mod = llvm::parseIR(*(IRBufOrErr.get()), Diag, IRCtx);
  if (!Mod) {
    std::string ParseErr;
    llvm::raw_string_ostream OS(ParseErr);
    Diag.print("neverd_patch_from_c", OS);
    S->setError(ParseErr);
    return 0;
  }

  std::filesystem::path OutPath;
  if (OutputPath && *OutputPath)
    OutPath = OutputPath;
  else
    OutPath = S->FilePath.string() + ".patched";

  // PIE/ASLR safety: symbolize absolute in-image pointer constants so
  // codegen emits PC-relative addressing (see symbolizeImageAbsolutePointers).
  symbolizeImageAbsolutePointers(*Mod, S->Img);

  // Obfuscation: cosmetic IR transforms applied after the correctness-critical
  // symbolization, before codegen (canonical order, shared by all entry
  // points).
  applySessionObfuscation(S, *Mod);

  auto Patcher = BinaryPatcher::create(S->Img.Format);
  if (!Patcher) {
    S->setError("section patch not supported for this format");
    return 0;
  }
  Patcher->setImageContext(&S->Img);
  if (!S->TextSectionOverride.empty())
    Patcher->setTextSectionOverride(S->TextSectionOverride);
  S->LastPatch = Patcher->patch(S->FilePath, OutPath, *Mod, S->Img.Arch);

  if (!S->LastPatch.Success) {
    S->setError("patch failed");
    return 0;
  }
  return 1;
}

// ===--------------------------------------------------------------------===//
// Full pipeline patch (high-level)
// ===--------------------------------------------------------------------===//

int neverd_patch_full(neverd_session_t Sess, const char *InputPath,
                      const char *OutputPath, int Strategy, int NoOpt,
                      int InjectHello, int /*RunNop*/, int MaxFunctions) {
  auto *S = static_cast<Session *>(Sess);
  PipelineRunner R;
  std::string Err;
  if (!R.load(InputPath, Err)) {
    if (S)
      S->setError(Err);
    return 1;
  }
  if (R.Img.Arch == Arch::EVM || R.Img.Arch == Arch::SBF) {
    if (S)
      S->setError(
          "binary patching is not supported for virtual-machine inputs");
    return 1;
  }

  PipelineOptions Opts;
  Opts.PatchMode = true;
  Opts.NoOpt = NoOpt != 0;
  Opts.MaxFunctions = MaxFunctions > 0 ? static_cast<size_t>(MaxFunctions) : 0;
  if (!R.run(Opts, Err)) {
    if (S)
      S->setError(Err);
    return 1;
  }
  if (!R.Result.LlvmModule) {
    if (S)
      S->setError("no LLVM module produced");
    return 1;
  }

  auto Path = std::filesystem::path(InputPath);
  std::string OutPath =
      OutputPath ? std::string(OutputPath) : Path.string() + ".patched";

  if (InjectHello)
    Pipeline::runHelloWorldPass(*R.Result.LlvmModule);

  // PIE/ASLR safety: symbolize absolute in-image pointer constants so
  // codegen emits PC-relative addressing (see symbolizeImageAbsolutePointers).
  symbolizeImageAbsolutePointers(*R.Result.LlvmModule, R.Img);

  // Obfuscation: cosmetic IR transforms applied after the correctness-critical
  // symbolization, before codegen (canonical order, shared by all entry
  // points).
  applySessionObfuscation(S, *R.Result.LlvmModule);

  PatchResult PR;

  if (Strategy == 0) {
    auto Patcher = BinaryPatcher::create(R.Img.Format);
    if (!Patcher) {
      if (S)
        S->setError("unsupported binary format for patching");
      return 1;
    }
    Patcher->setImageContext(&R.Img);
    if (S && !S->TextSectionOverride.empty())
      Patcher->setTextSectionOverride(S->TextSectionOverride);
    PR = Patcher->patch(Path, OutPath, *R.Result.LlvmModule, R.Img.Arch);
  } else {
    auto Rewriter = InplaceRewriter::create(R.Img.Format);
    if (!Rewriter) {
      if (S)
        S->setError("unsupported format for inplace rewrite");
      return 1;
    }
    if (S && !S->TextSectionOverride.empty())
      Rewriter->setTextSectionOverride(S->TextSectionOverride);
    PR = Rewriter->rewrite(Path, OutPath, *R.Result.LlvmModule, R.Img,
                           R.Img.Arch);
  }

  if (!PR.Success) {
    if (S)
      S->setError("patch failed");
    return 1;
  }

  if (S) {
    S->LastPatch = PR;
    S->clearError();
  }
  return 0;
}

// ===--------------------------------------------------------------------===//
// Patch result queries
// ===--------------------------------------------------------------------===//

const char *neverd_patch_output_path(neverd_session_t Sess) {
  return dupStr(toSession(Sess)->LastPatch.OutputPath);
}

unsigned long long neverd_patch_code_size(neverd_session_t Sess) {
  return toSession(Sess)->LastPatch.CodeSize;
}

int neverd_patch_trampoline_count(neverd_session_t Sess) {
  return static_cast<int>(toSession(Sess)->LastPatch.TrampolineCount);
}

void neverd_set_inst_substitution(neverd_session_t Sess, int Enable,
                                  int Rounds) {
  auto *S = toSession(Sess);
  S->InstSubstitution = Enable != 0;
  S->InstSubstitutionRounds = Rounds > 0 ? static_cast<unsigned>(Rounds) : 1;
}

int neverd_patch_substitution_count(neverd_session_t Sess) {
  return static_cast<int>(toSession(Sess)->LastSubstitutionCount);
}

void neverd_set_constant_encryption(neverd_session_t Sess, int Enable) {
  toSession(Sess)->ConstantEncryption = Enable != 0;
}

int neverd_patch_constant_encryption_count(neverd_session_t Sess) {
  return static_cast<int>(toSession(Sess)->LastConstEncCount);
}

void neverd_set_opaque_predicate(neverd_session_t Sess, int Enable) {
  toSession(Sess)->OpaquePredicate = Enable != 0;
}

int neverd_patch_opaque_predicate_count(neverd_session_t Sess) {
  return static_cast<int>(toSession(Sess)->LastOpaquePredCount);
}

void neverd_set_control_flow_flattening(neverd_session_t Sess, int Enable) {
  toSession(Sess)->ControlFlowFlattening = Enable != 0;
}

int neverd_patch_control_flow_flattening_count(neverd_session_t Sess) {
  return static_cast<int>(toSession(Sess)->LastFlattenCount);
}

void neverd_set_bogus_control_flow(neverd_session_t Sess, int Enable) {
  toSession(Sess)->BogusControlFlow = Enable != 0;
}

int neverd_patch_bogus_control_flow_count(neverd_session_t Sess) {
  return static_cast<int>(toSession(Sess)->LastBogusCount);
}

void neverd_set_indirect_branch(neverd_session_t Sess, int Enable) {
  toSession(Sess)->IndirectBranch = Enable != 0;
}

int neverd_patch_indirect_branch_count(neverd_session_t Sess) {
  return static_cast<int>(toSession(Sess)->LastIndirectBranchCount);
}

void neverd_set_indirect_call(neverd_session_t Sess, int Enable) {
  toSession(Sess)->IndirectCall = Enable != 0;
}

int neverd_patch_indirect_call_count(neverd_session_t Sess) {
  return static_cast<int>(toSession(Sess)->LastIndirectCallCount);
}

void neverd_set_mba(neverd_session_t Sess, int Enable) {
  toSession(Sess)->MBA = Enable != 0;
}

int neverd_patch_mba_count(neverd_session_t Sess) {
  return static_cast<int>(toSession(Sess)->LastMBACount);
}

void neverd_set_indirect_global(neverd_session_t Sess, int Enable) {
  toSession(Sess)->IndirectGlobal = Enable != 0;
}

int neverd_patch_indirect_global_count(neverd_session_t Sess) {
  return static_cast<int>(toSession(Sess)->LastIndirectGlobalCount);
}

void neverd_set_value_launder(neverd_session_t Sess, int Enable) {
  toSession(Sess)->ValueLaunder = Enable != 0;
}

int neverd_patch_value_launder_count(neverd_session_t Sess) {
  return static_cast<int>(toSession(Sess)->LastValueLaunderCount);
}

void neverd_set_constant_pooling(neverd_session_t Sess, int Enable) {
  toSession(Sess)->ConstantPooling = Enable != 0;
}

int neverd_patch_constant_pooling_count(neverd_session_t Sess) {
  return static_cast<int>(toSession(Sess)->LastConstPoolCount);
}

void neverd_set_bit_masking(neverd_session_t Sess, int Enable) {
  toSession(Sess)->BitMasking = Enable != 0;
}

int neverd_patch_bit_masking_count(neverd_session_t Sess) {
  return static_cast<int>(toSession(Sess)->LastBitMaskCount);
}

void neverd_set_text_section(neverd_session_t Sess, const char *Name) {
  toSession(Sess)->TextSectionOverride = (Name && *Name) ? Name : "";
}
