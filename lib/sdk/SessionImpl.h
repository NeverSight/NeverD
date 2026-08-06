//===- SessionImpl.h - Internal session state for C API -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Internal session data structure shared across C API implementation files.
/// This header is NOT part of the public API.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_SESSION_IMPL_H
#define NEVERD_SDK_SESSION_IMPL_H

#include "PluginManager.h"

#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/backend/codegen/CodeGen.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/sdk/NeverDCAPI.h"
#include "neverd/sigs/SignatureDB.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace neverd {
namespace sdk {

struct FuncInfo {
  va_t Entry;
  uint64_t Size;
  std::string Name;
};

struct Session {
  std::filesystem::path FilePath;
  BinaryImage Img;
  bool Loaded = false;

  std::unique_ptr<llvm::LLVMContext> LLVMCtx;
  PipelineResult PipeResult;
  bool PipeRan = false;

  Decoder Dec;

  std::vector<FuncInfo> Functions;

  PatchResult LastPatch;

  // Instruction-substitution toggle, consulted by every patch
  // entry point.  LastSubstitutionCount records how many operators the most
  // recent patch substituted (0 when the toggle is off).
  bool InstSubstitution = false;
  unsigned InstSubstitutionRounds = 1;
  unsigned LastSubstitutionCount = 0;

  // Constant-encryption toggle, consulted by every patch entry point.
  // LastConstEncCount records how many constant operands the most recent patch
  // encrypted (0 when the toggle is off).
  bool ConstantEncryption = false;
  unsigned LastConstEncCount = 0;

  // Opaque-predicate toggle, consulted by every patch entry point.
  // LastOpaquePredCount records how many predicates the most recent patch
  // inserted (0 when the toggle is off).
  bool OpaquePredicate = false;
  unsigned LastOpaquePredCount = 0;

  // Control-flow flattening toggle, consulted by every patch entry point.
  // LastFlattenCount records how many basic blocks the most recent patch
  // flattened (0 when the toggle is off).
  bool ControlFlowFlattening = false;
  unsigned LastFlattenCount = 0;

  // Bogus-control-flow toggle, consulted by every patch entry point.
  // LastBogusCount records how many basic blocks the most recent patch gave a
  // bogus sub-graph (0 when the toggle is off).
  bool BogusControlFlow = false;
  unsigned LastBogusCount = 0;

  // Indirect-branch toggle, consulted by every patch entry point.
  // LastIndirectBranchCount records how many conditional branches the most
  // recent patch converted to indirect branches (0 when the toggle is off).
  bool IndirectBranch = false;
  unsigned LastIndirectBranchCount = 0;

  // Indirect-call toggle, consulted by every patch entry point.
  // LastIndirectCallCount records how many direct calls the most recent patch
  // converted to indirect calls (0 when the toggle is off).
  bool IndirectCall = false;
  unsigned LastIndirectCallCount = 0;

  // Mixed-boolean-arithmetic toggle, consulted by every patch entry point.
  // LastMBACount records how many operators the most recent patch wrapped with
  // an MBA term (0 when the toggle is off).
  bool MBA = false;
  unsigned LastMBACount = 0;

  // Indirect global-variable toggle, consulted by every patch entry point.
  // LastIndirectGlobalCount records how many global references the most recent
  // patch made indirect (0 when the toggle is off).
  bool IndirectGlobal = false;
  unsigned LastIndirectGlobalCount = 0;

  // Value-laundering toggle, consulted by every patch entry point.
  // LastValueLaunderCount records how many values the most recent patch routed
  // through a volatile stack slot (0 when the toggle is off).
  bool ValueLaunder = false;
  unsigned LastValueLaunderCount = 0;

  // Constant-pooling toggle, consulted by every patch entry point.
  // LastConstPoolCount records how many constant operands the most recent patch
  // moved into a read-only global pool (0 when the toggle is off).
  bool ConstantPooling = false;
  unsigned LastConstPoolCount = 0;

  // Bit-masking toggle, consulted by every patch entry point.
  // LastBitMaskCount records how many values the most recent patch wrapped with
  // the `(x & m) | (x & ~m)` bitwise identity (0 when the toggle is off).
  bool BitMasking = false;
  unsigned LastBitMaskCount = 0;

  // Forced original code-section name (empty = format default).  Lets the user
  // point the patcher at a code section renamed by a packer/protector
  // (e.g. ".vmp0"); consulted by every patch entry point.
  std::string TextSectionOverride;

  struct RoundTripResult {
    std::string IR;
    CodegenResult CG;
    std::vector<int> ParamCounts;
    bool Valid = false;
  } RoundTrip;

  std::map<va_t, std::string> Annotations;

  std::map<va_t, std::string> Renames;
  std::map<va_t, std::string> OriginalNames;

  sigs::SignatureDB SigDB;

  PluginManager PM;

  std::string LastError;

  void setError(const std::string &Msg) { LastError = Msg; }
  void clearError() { LastError.clear(); }

  bool ensurePipeline() {
    if (PipeRan)
      return PipeResult.Success;
    if (!Loaded) {
      setError("no binary loaded");
      return false;
    }
    LLVMCtx = std::make_unique<llvm::LLVMContext>();
    PipelineOptions Opts;
    Pipeline ThePipeline;
    PipeResult = ThePipeline.run(Img, *LLVMCtx, Opts);
    PipeRan = true;
    if (!PipeResult.Success)
      setError("pipeline failed");
    return PipeResult.Success;
  }

  const LowFunc *findLowFunc(va_t Addr) const {
    for (const auto &F : PipeResult.LowFuncs)
      if (F.Entry == Addr)
        return &F;
    return nullptr;
  }

  const HighFunc *findHighFunc(va_t Addr) const {
    for (const auto &F : PipeResult.HighFuncs)
      if (F.Entry == Addr)
        return &F;
    return nullptr;
  }

  bool ensureLlvmModule() {
    if (PipeResult.LlvmModule)
      return true;
    if (!ensurePipeline())
      return false;
    if (PipeResult.MedFuncs.empty())
      return false;
    std::vector<std::pair<va_t, std::string>> ImportMap;
    for (const auto &[Addr, Name] : Img.getImportAddressNames())
      ImportMap.emplace_back(Addr, Name);
    MedLLVMEmitter Emitter;
    PipeResult.LlvmModule =
        Emitter.emit(PipeResult.MedFuncs, *LLVMCtx, "neverd_output", Img.Arch,
                     ImportMap, &Img, Img.Format);
    return PipeResult.LlvmModule != nullptr;
  }
};

inline Session *toSession(neverd_session_t Sess) {
  return static_cast<Session *>(Sess);
}

inline char *dupStr(const std::string &S) { return strdup(S.c_str()); }

inline std::string vaHex(va_t Addr) { return "0x" + llvm::utohexstr(Addr); }

inline std::string jsonToString(const llvm::json::Value &V) {
  std::string Buf;
  llvm::raw_string_ostream OS(Buf);
  OS << V;
  return Buf;
}

/// PipelineRunner — self-contained binary-load-and-run for high-level
/// C API functions that take an input path instead of a session.
struct PipelineRunner {
  BinaryImage Img;
  std::unique_ptr<DebugContext> Dbg;
  llvm::LLVMContext LLVMCtx;
  PipelineResult Result;

  bool load(const char *InputPath, std::string &Err);
  bool run(PipelineOptions Opts, std::string &Err);
};

} // namespace sdk
} // namespace neverd

#endif // NEVERD_SDK_SESSION_IMPL_H
