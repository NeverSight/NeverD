//===- NeverDCmdExport.cpp - Export and diff commands --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Handlers for `export` (write decompiled C, IR, or a JSON table to a file)
/// and `diff` (compare two binaries function-by-function).  `diff` owns its own
/// pair of sessions because it takes -a/-b instead of the shared positional
/// input.
///
//===----------------------------------------------------------------------===//

#include "../NeverDCLI.h"

#include "neverd/Common.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <cstring>
#include <string>

using namespace llvm;

namespace neverd::cli {

int runExport(neverd_session_t Sess) {
  int FuncIdx = -1;
  if (ExportFmt == FmtDecompile || ExportFmt == FmtIR) {
    if (!ExportFunc.empty()) {
      uint64_t Addr = 0;
      StringRef FuncRef(ExportFunc.getValue());
      if (FuncRef.consume_front("0x") || FuncRef.consume_front("0X")) {
        if (!FuncRef.empty() && !FuncRef.getAsInteger(16, Addr))
          FuncIdx = neverd_func_find_by_addr(Sess, Addr);
      } else {
        FuncIdx = neverd_func_find_by_name(Sess, ExportFunc.c_str());
        if (FuncIdx < 0 && !FuncRef.empty() &&
            !FuncRef.getAsInteger(16, Addr))
          FuncIdx = neverd_func_find_by_addr(Sess, Addr);
      }
    } else {
      FuncIdx = 0;
    }

    if (FuncIdx < 0 || FuncIdx >= neverd_func_count(Sess)) {
      WithColor::error() << "function not found: " << ExportFunc.getValue()
                         << "\n";
      return 1;
    }
  }

  std::error_code EC;
  raw_fd_ostream OS(ExportOutput.getValue(), EC);
  if (EC) {
    WithColor::error() << "cannot open output: " << EC.message() << "\n";
    return 1;
  }

  if (ExportFmt == FmtDecompile || ExportFmt == FmtIR) {
    uint64_t Entry = neverd_func_entry(Sess, FuncIdx);
    const char *Text = nullptr;
    if (ExportFmt == FmtDecompile)
      Text = neverd_decompile(Sess, Entry);
    else
      Text = neverd_ir_llvm(Sess, Entry);

    if (Text) {
      OS << Text;
      neverd_free_string(Text);
    } else {
      WithColor::error() << "export failed: " << takeLastError(Sess) << "\n";
      return 1;
    }
  } else {
    const char *Json = nullptr;
    if (ExportFmt == FmtFuncs) {
      json::Array Funcs;
      for (int I = 0; I < neverd_func_count(Sess); ++I) {
        const char *N = neverd_func_name(Sess, I);
        json::Object Func;
        Func["name"] = N ? N : "";
        Func["addr"] = "0x" + utohexstr(neverd_func_entry(Sess, I));
        Func["size"] = static_cast<int64_t>(neverd_func_size(Sess, I));
        Funcs.push_back(std::move(Func));
        neverd_free_string(N);
      }
      OS << json::Value(std::move(Funcs)) << "\n";
    } else if (ExportFmt == FmtImports) {
      Json = neverd_imports_json(Sess);
    } else if (ExportFmt == FmtExports) {
      Json = neverd_exports_json(Sess);
    } else if (ExportFmt == FmtStrings) {
      Json = neverd_strings_json(Sess, 4);
    }

    if (Json) {
      OS << Json;
      neverd_free_string(Json);
    }
  }

  OS.flush();
  if (!JsonOutput)
    outs() << "Exported to " << ExportOutput.getValue() << "\n";
  return 0;
}

int runDiff() {
  if (!JsonOutput && !DiffJson)
    outs() << "=== " << ProjectName << " v" << VersionString << " (diff) ===\n";

  neverd_session_t SA = neverd_session_create();
  neverd_session_t SB = neverd_session_create();
  if (!neverd_session_load(SA, DiffFileA.getValue().c_str())) {
    WithColor::error() << "failed to load A: " << takeLastError(SA) << "\n";
    neverd_session_destroy(SA);
    neverd_session_destroy(SB);
    return 1;
  }
  if (!neverd_session_load(SB, DiffFileB.getValue().c_str())) {
    WithColor::error() << "failed to load B: " << takeLastError(SB) << "\n";
    neverd_session_destroy(SA);
    neverd_session_destroy(SB);
    return 1;
  }

  if (!DiffFunc.empty()) {
    int IdxA = neverd_func_find_by_name(SA, DiffFunc.getValue().c_str());
    int IdxB = neverd_func_find_by_name(SB, DiffFunc.getValue().c_str());
    if (IdxA < 0 || IdxB < 0) {
      WithColor::error() << "function '" << DiffFunc.getValue()
                         << "' not found in both binaries\n";
      neverd_session_destroy(SA);
      neverd_session_destroy(SB);
      return 1;
    }
    neverd_va_t EntryA = neverd_func_entry(SA, IdxA);
    neverd_va_t EntryB = neverd_func_entry(SB, IdxB);
    const char *Json = neverd_diff_decompile(SA, EntryA, SB, EntryB);
    if (!Json) {
      WithColor::error() << "diff failed\n";
      neverd_session_destroy(SA);
      neverd_session_destroy(SB);
      return 1;
    }
    if (DiffJson) {
      outs() << Json << "\n";
    } else {
      outs() << "Comparing function: " << DiffFunc.getValue() << "\n\n";
      outs() << "--- A: " << DiffFileA.getValue() << "\n";
      outs() << "+++ B: " << DiffFileB.getValue() << "\n\n";
      const char *CodeA = neverd_decompile(SA, EntryA);
      const char *CodeB = neverd_decompile(SB, EntryB);
      if (CodeA && CodeB && std::strcmp(CodeA, CodeB) == 0) {
        outs() << "(identical)\n";
      } else {
        outs() << "=== A ===\n" << (CodeA ? CodeA : "(empty)") << "\n\n";
        outs() << "=== B ===\n" << (CodeB ? CodeB : "(empty)") << "\n";
      }
      if (CodeA)
        neverd_free_string(CodeA);
      if (CodeB)
        neverd_free_string(CodeB);
    }
    neverd_free_string(Json);
  } else {
    const char *Json = neverd_diff_functions(SA, SB);
    if (!Json) {
      WithColor::error() << "diff failed\n";
      neverd_session_destroy(SA);
      neverd_session_destroy(SB);
      return 1;
    }
    if (DiffJson) {
      outs() << Json << "\n";
    } else {
      outs() << "A: " << DiffFileA.getValue() << " (" << neverd_func_count(SA)
             << " functions)\n";
      outs() << "B: " << DiffFileB.getValue() << " (" << neverd_func_count(SB)
             << " functions)\n\n";

      size_t MatchedCount = 0, AddedCount = 0, RemovedCount = 0;
      auto Parsed = json::parse(Json ? Json : "{}");
      if (!Parsed) {
        consumeError(Parsed.takeError());
      } else if (const json::Object *Obj = Parsed->getAsObject()) {
        auto arraySize = [&](StringRef Key) -> size_t {
          const json::Array *Arr = Obj->getArray(Key);
          return Arr ? Arr->size() : 0;
        };
        MatchedCount = arraySize("matched");
        AddedCount = arraySize("added");
        RemovedCount = arraySize("removed");
      }

      outs() << "Summary:\n";
      outs() << "  Matched functions: " << MatchedCount << "\n";
      outs() << "  Added in B:        " << AddedCount << "\n";
      outs() << "  Removed from A:    " << RemovedCount << "\n";
    }
    neverd_free_string(Json);
  }

  neverd_session_destroy(SA);
  neverd_session_destroy(SB);
  return 0;
}

} // namespace neverd::cli
