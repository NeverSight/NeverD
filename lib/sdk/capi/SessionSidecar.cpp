//===- SessionSidecar.cpp - Atomic private Session sidecar output ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Shared atomic write owner for Session metadata adapters.
#include "SessionImpl.h"
#include "SessionProjectInput.h"

#include "neverd/support/AtomicOutput.h"
#include "neverd/support/FilePath.h"
#include "neverd/support/ProjectFiles.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/raw_ostream.h"

using namespace neverd;
using namespace neverd::sdk;

bool neverd::sdk::saveSidecar(Session *S, const std::filesystem::path &Path,
                              llvm::json::Value Values, llvm::StringRef Kind) {
  // A row's address in lowercase hexadecimal, as the workbench writes it:
  // one spelling, whichever tool wrote the file last.
  if (llvm::json::Array *Rows = Values.getAsArray())
    for (llvm::json::Value &Row : *Rows)
      if (llvm::json::Object *Object = Row.getAsObject())
        if (std::optional<llvm::StringRef> Address = Object->getString("addr"))
          (*Object)["addr"] = Address->lower();
  // The project's files live in its folder beside the input.
  if (std::error_code Folder = project_files::prepare(S->FilePath)) {
    S->setError("cannot create the project folder: " + Folder.message());
    return false;
  }
  const auto UTF8Path = pathToUTF8(Path);
  auto Temporary = llvm::sys::fs::TempFile::create(
      UTF8Path + ".tmp-%%%%%%",
      llvm::sys::fs::owner_read | llvm::sys::fs::owner_write);
  if (!Temporary) {
    S->setError("cannot create " + Kind.str() +
                " temporary file: " + llvm::toString(Temporary.takeError()));
    return false;
  }
  std::error_code WriteError;
  {
    // TempFile owns the descriptor. Clear a handled stream error before its
    // destructor so ENOSPC/EFBIG becomes a C ABI error rather than LLVM abort.
    llvm::raw_fd_ostream OS(Temporary->FD, false);
    // Two-space indentation, sorted keys and a final newline, byte for byte
    // as the workbench writes the same files (the worker's sidecarText), so
    // a project diffs and merges line by line under version control.
    OS << llvm::formatv("{0:2}", Values) << '\n';
    OS.flush();
    WriteError = OS.error();
    OS.clear_error();
  }
  if (WriteError) {
    llvm::consumeError(
        support::atomic_output::discardTemporaryOutput(*Temporary));
    S->setError("cannot write " + Kind.str() + ": " + WriteError.message());
    return false;
  }
  if (auto Error = support::atomic_output::closeAndCommitTemporaryOutput(
          *Temporary, UTF8Path)) {
    S->setError("cannot save " + Kind.str() + ": " +
                llvm::toString(std::move(Error)));
    return false;
  }
  // A file the project now has describes this build of the input.
  return Path ==
             project_files::path(S->FilePath, project_files::File::Manifest) ||
         recordProjectInput(S);
}

bool neverd::sdk::recordProjectInput(Session *S) {
  if (S->MemoryInputBytes)
    return true;
  llvm::json::Object Input = projectInput(*S);
  if (Input.getString("sha256").value_or("").empty())
    return true;
  if (auto Recorded = recordedInput(*S);
      Recorded && Recorded->getString("sha256") == Input.getString("sha256"))
    return true;
  return saveSidecar(
      S, project_files::path(S->FilePath, project_files::File::Manifest),
      llvm::json::Object{{"neverd_project", 1}, {"input", std::move(Input)}},
      "project");
}
