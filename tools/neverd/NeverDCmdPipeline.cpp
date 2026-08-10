//===- NeverDCmdPipeline.cpp - Engine-driven commands --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Handlers that drive the decompiler engine through the C API: `plugins`,
/// `lift`, `decompile`, and `patch`.  All heavy pipeline work stays behind the
/// C API so the CLI and GUI share one runtime path and this TU never
/// instantiates LLVM PassManager templates.
///
//===----------------------------------------------------------------------===//

#include "NeverDCLI.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

// This TU uses LLVM_DEBUG (which expands DEBUG_TYPE); define a category so the
// macro resolves.  Placed after the includes so a header that defines and
// #undefs its own DEBUG_TYPE cannot clobber it.
#define DEBUG_TYPE "neverd-cli"

using namespace llvm;

namespace neverd::cli {

namespace {

const char *hardforkSpelling(evm::Hardfork Fork) {
  switch (Fork) {
#define EVM_HARDFORK(NAME, SPELLING)                                           \
  case evm::Hardfork::NAME:                                                    \
    return SPELLING;
#include "neverd/evm/EVMHardforks.def"
  }
  return "";
}

const char *sbfVersionSpelling(sbf::Version TheVersion) {
  switch (TheVersion) {
#define SBF_VERSION_AUTO(NAME, SPELLING, DISPLAY_NAME)                         \
  case sbf::Version::NAME:                                                     \
    return SPELLING;
#define SBF_VERSION(NAME, ELF_FLAGS, SPELLING, DISPLAY_NAME, FEATURES, STATUS) \
  case sbf::Version::NAME:                                                     \
    return SPELLING;
#include "neverd/sbf/SBFVersions.def"
  default:
    return "";
  }
}

bool configureEVM(neverd_session_t Sess) {
  neverd_evm_set_strict(Sess, EVMRelaxed ? 0 : 1);
  if (!neverd_evm_set_hardfork(Sess,
                               hardforkSpelling(EVMHardfork.getValue()))) {
    WithColor::error() << "invalid EVM hardfork: " << takeLastError(Sess)
                       << "\n";
    return false;
  }
  return true;
}

bool configureSBF(neverd_session_t Sess) {
  neverd_sbf_set_strict(Sess, SBFRelaxed ? 0 : 1);
  if (!neverd_sbf_set_version(Sess,
                              sbfVersionSpelling(SBFVersion.getValue()))) {
    WithColor::error() << "invalid SBF version: " << takeLastError(Sess)
                       << "\n";
    return false;
  }
  return true;
}

bool configureVirtualMachines(neverd_session_t Sess) {
  return configureEVM(Sess) && configureSBF(Sess);
}

} // namespace

int runPlugins(const char *Argv0) {
  neverd_session_t Sess = neverd_session_create();
  SessionGuard SessGuard(Sess);

  auto BinDir = std::filesystem::path(Argv0).parent_path().string();
  neverd_plugins_load_dir(Sess, (BinDir + "/plugins").c_str());
  if (const char *Home = getenv("HOME"))
    neverd_plugins_load_dir(Sess,
                            (std::string(Home) + "/.neverd/plugins").c_str());
  if (const char *Env = getenv("NEVERD_PLUGIN_PATH")) {
    std::string EnvStr(Env);
    size_t Pos = 0;
    while (Pos < EnvStr.size()) {
      size_t Sep = EnvStr.find(':', Pos);
      if (Sep == std::string::npos)
        Sep = EnvStr.size();
      neverd_plugins_load_dir(Sess, EnvStr.substr(Pos, Sep - Pos).c_str());
      Pos = Sep + 1;
    }
  }
  if (!PluginDir.empty())
    neverd_plugins_load_dir(Sess, PluginDir.getValue().c_str());

  if (PluginList || PluginRun.empty()) {
    const char *Json = neverd_plugins_list_json(Sess);
    if (JsonOutput) {
      outs() << (Json ? Json : "[]") << "\n";
    } else {
      auto Parsed = json::parse(Json ? Json : "[]");
      if (Parsed) {
        if (auto *Arr = Parsed->getAsArray()) {
          if (Arr->empty()) {
            outs() << "No plugins loaded.\n";
          } else {
            outs() << "Loaded plugins (" << Arr->size() << "):\n";
            for (const auto &V : *Arr) {
              auto *Obj = V.getAsObject();
              if (!Obj)
                continue;
              outs() << "  " << Obj->getString("name").value_or("?") << " v"
                     << Obj->getString("version").value_or("?") << " — "
                     << Obj->getString("description").value_or("") << "\n";
            }
          }
        }
      }
    }
    neverd_free_string(Json);
    return 0;
  }

  if (!PluginRun.empty()) {
    if (!PluginBinary.empty()) {
      if (!neverd_session_load(Sess, PluginBinary.getValue().c_str())) {
        WithColor::error() << "failed to load binary: "
                           << PluginBinary.getValue() << "\n";
        return 1;
      }
    }
    neverd_plugins_init(Sess);
    int Ret = neverd_plugins_run(Sess, PluginRun.getValue().c_str(), 0);
    if (!JsonOutput)
      outs() << "Plugin '" << PluginRun.getValue() << "' returned " << Ret
             << "\n";
    else {
      json::Object Result;
      Result["plugin"] = PluginRun.getValue();
      Result["result"] = Ret;
      outs() << json::Value(std::move(Result)) << "\n";
    }
    neverd_plugins_term(Sess);
    return (Ret == 0) ? 0 : 1;
  }

  return 0;
}

int runLift(neverd_session_t Sess) {
  const char *InPath = InputFile.getValue().c_str();
  if (!configureVirtualMachines(Sess))
    return 1;

  if (DumpLow || DumpMed || DumpHigh) {
    int Level = DumpLow ? 0 : (DumpMed ? 1 : 2);
    const char *Dump = neverd_lift_dump(Sess, InPath, Level, MaxFunc);
    if (!Dump) {
      WithColor::error() << "dump failed: " << takeLastError(Sess) << "\n";
      return 1;
    }
    outs() << Dump;
    neverd_free_string(Dump);
    return 0;
  }

  const char *IR = neverd_lift_module(Sess, InPath, NoOpt, MaxFunc);
  if (!IR) {
    WithColor::error() << "lift failed: " << takeLastError(Sess) << "\n";
    return 1;
  }
  if (!OutputFile.empty()) {
    std::error_code EC;
    raw_fd_ostream OS(OutputFile, EC);
    if (EC) {
      WithColor::error() << "cannot open: " << EC.message() << "\n";
      neverd_free_string(IR);
      return 1;
    }
    OS << IR;
    outs() << "IR written to " << OutputFile.getValue() << "\n";
  } else {
    outs() << IR;
  }
  neverd_free_string(IR);
  return 0;
}

int runDecompile(neverd_session_t Sess) {
  const char *InPath = InputFile.getValue().c_str();

  if (!configureVirtualMachines(Sess))
    return 1;

  const neverd_output_language_t Language = OutputLanguage.getValue();
  const bool DedicatedLanguage = Language != NEVERD_OUTPUT_C;
  if (DedicatedLanguage && LlvmRoute) {
    WithColor::error()
        << "--llvm cannot be combined with a dedicated source language\n";
    return 1;
  }
  const char *Source = DedicatedLanguage
                           ? neverd_decompile_all_ex(Sess, InPath, Language,
                                                    NoOpt, MaxFunc)
                           : neverd_decompile_all(Sess, InPath,
                                                 LlvmRoute ? 1 : 0, NoOpt,
                                                 MaxFunc);
  if (!Source) {
    WithColor::error() << "decompile failed: " << takeLastError(Sess) << "\n";
    return 1;
  }
  if (!OutputFile.empty()) {
    std::error_code EC;
    raw_fd_ostream OS(OutputFile, EC);
    if (EC) {
      WithColor::error() << "cannot open: " << EC.message() << "\n";
      neverd_free_string(Source);
      return 1;
    }
    OS << Source;
    outs() << outputLanguageDisplayName(Language) << " source written to "
           << OutputFile.getValue() << "\n";
  } else {
    outs() << Source;
  }
  neverd_free_string(Source);
  return 0;
}

int runPatch(neverd_session_t Sess) {
  const char *InPath = InputFile.getValue().c_str();

  std::string OutPath = OutputFile.getValue();
  if (OutPath.empty())
    OutPath = InputFile.getValue() + ".patched";

  int Strategy = PatchStrat == InplaceMode ? 1 : 0;
  neverd_set_inst_substitution(Sess, InstSubst ? 1 : 0, InstSubstRounds);
  neverd_set_constant_encryption(Sess, ConstEnc ? 1 : 0);
  neverd_set_opaque_predicate(Sess, OpaquePred ? 1 : 0);
  neverd_set_control_flow_flattening(Sess, Flatten ? 1 : 0);
  neverd_set_bogus_control_flow(Sess, BogusCF ? 1 : 0);
  neverd_set_indirect_branch(Sess, IndirectBr ? 1 : 0);
  neverd_set_indirect_call(Sess, IndirectCall ? 1 : 0);
  neverd_set_mba(Sess, Mba ? 1 : 0);
  neverd_set_indirect_global(Sess, IndirectGv ? 1 : 0);
  neverd_set_value_launder(Sess, ValueLaunder ? 1 : 0);
  neverd_set_constant_pooling(Sess, ConstPool ? 1 : 0);
  neverd_set_bit_masking(Sess, BitMask ? 1 : 0);
  neverd_set_text_section(Sess, TextSection.c_str());
  int Ret = 0;
  if (!PatchFromIR.empty()) {
    // Patch from external LLVM IR: skip the lift stage and feed the IR
    // straight into the rewrite pipeline (section or inplace per --mode).
    auto IRBuf = MemoryBuffer::getFile(PatchFromIR.getValue());
    if (!IRBuf) {
      WithColor::error() << "cannot read IR file: " << PatchFromIR.getValue()
                         << "\n";
      return 1;
    }
    Ret = neverd_patch_from_ir(Sess, (*IRBuf)->getBufferStart(), Strategy,
                               OutPath.c_str())
              ? 0
              : 1;
  } else if (!PatchFromC.empty()) {
    auto CBuf = MemoryBuffer::getFile(PatchFromC.getValue());
    if (!CBuf) {
      WithColor::error() << "cannot read C file: " << PatchFromC.getValue()
                         << "\n";
      return 1;
    }
    uint64_t FuncAddr = 0;
    if (!PatchFuncAddr.empty())
      StringRef(PatchFuncAddr.getValue()).getAsInteger(0, FuncAddr);
    Ret = neverd_patch_from_c(Sess, (*CBuf)->getBufferStart(), FuncAddr,
                              OutPath.c_str())
              ? 0
              : 1;
  } else {
    Ret = neverd_patch_full(Sess, InPath, OutPath.c_str(), Strategy, NoOpt,
                            InjectHello ? 1 : 0, RunNop ? 1 : 0, MaxFunc);
  }
  if (Ret != 0) {
    WithColor::error() << "patch failed: " << takeLastError(Sess) << "\n";
    return 1;
  }
  const char *PatchOut = neverd_patch_output_path(Sess);
  std::string FinalPath = PatchOut ? PatchOut : OutPath;
  outs() << "Patched binary written to " << FinalPath << " ("
         << neverd_patch_code_size(Sess) << " bytes code, "
         << neverd_patch_trampoline_count(Sess) << " trampolines)\n";
  if (InstSubst)
    outs() << "instruction substitution: "
           << neverd_patch_substitution_count(Sess) << " operator(s)\n";
  if (ConstEnc)
    outs() << "constant encryption: "
           << neverd_patch_constant_encryption_count(Sess) << " constant(s)\n";
  if (OpaquePred)
    outs() << "opaque predicate: " << neverd_patch_opaque_predicate_count(Sess)
           << " predicate(s)\n";
  if (Flatten)
    outs() << "control-flow flattening: "
           << neverd_patch_control_flow_flattening_count(Sess) << " block(s)\n";
  if (BogusCF)
    outs() << "bogus control flow: "
           << neverd_patch_bogus_control_flow_count(Sess) << " block(s)\n";
  if (IndirectBr)
    outs() << "indirect branch: " << neverd_patch_indirect_branch_count(Sess)
           << " branch(es)\n";
  if (IndirectCall)
    outs() << "indirect call: " << neverd_patch_indirect_call_count(Sess)
           << " call(s)\n";
  if (Mba)
    outs() << "mba: " << neverd_patch_mba_count(Sess) << " operator(s)\n";
  if (IndirectGv)
    outs() << "indirect global: " << neverd_patch_indirect_global_count(Sess)
           << " reference(s)\n";
  if (ValueLaunder)
    outs() << "value launder: " << neverd_patch_value_launder_count(Sess)
           << " value(s)\n";
  if (ConstPool)
    outs() << "constant pooling: " << neverd_patch_constant_pooling_count(Sess)
           << " constant(s)\n";
  if (BitMask)
    outs() << "bit masking: " << neverd_patch_bit_masking_count(Sess)
           << " value(s)\n";
  if (PatchOut)
    neverd_free_string(PatchOut);

#ifdef __APPLE__
  if (auto Codesign = sys::findProgramByName("codesign")) {
    std::vector<StringRef> Args = {*Codesign, "-f", "-s", "-", FinalPath};
    if (sys::ExecuteAndWait(*Codesign, Args) == 0)
      LLVM_DEBUG(llvm::dbgs()
                 << "patch: ad-hoc codesigned " << FinalPath << "\n");
  }
#endif
  return 0;
}

} // namespace neverd::cli
