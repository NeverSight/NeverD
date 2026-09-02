//===- AtomicOutput.cpp - Single-mutation output publication ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/support/AtomicOutput.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/Signals.h"

#ifdef _WIN32
#include "llvm/Support/Windows/WindowsSupport.h"
#include "llvm/Support/WindowsError.h"

#include <io.h>
#endif

#include <algorithm>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
namespace {

std::error_code clearTemporaryDeleteDispositionOnWindows(HANDLE Handle) {
  FILE_DISPOSITION_INFO Disposition{};
  Disposition.DeleteFile = FALSE;
  if (::SetFileInformationByHandle(Handle, FileDispositionInfo, &Disposition,
                                   sizeof(Disposition)))
    return {};
  return llvm::mapWindowsError(::GetLastError());
}

std::error_code
replaceTemporaryOutputHandleOnWindows(HANDLE TemporaryHandle,
                                      llvm::StringRef OutputPath) {
  llvm::SmallVector<wchar_t, 128> WideOutput;
  if (std::error_code Error =
          llvm::sys::windows::widenPath(OutputPath, WideOutput))
    return Error;

  if (WideOutput.size() > std::numeric_limits<size_t>::max() / sizeof(wchar_t))
    return std::make_error_code(std::errc::value_too_large);
  const size_t FileNameBytes = WideOutput.size() * sizeof(wchar_t);
  constexpr size_t RenameHeaderBytes =
      sizeof(FILE_RENAME_INFO) - sizeof(wchar_t);
  static_assert(RenameHeaderBytes <= std::numeric_limits<DWORD>::max());
  if (FileNameBytes > std::numeric_limits<DWORD>::max() ||
      FileNameBytes > std::numeric_limits<DWORD>::max() - RenameHeaderBytes)
    return std::make_error_code(std::errc::value_too_large);

  std::vector<char> RenameBuffer(RenameHeaderBytes + FileNameBytes);
  auto &Rename = *reinterpret_cast<FILE_RENAME_INFO *>(RenameBuffer.data());
  Rename.ReplaceIfExists = TRUE;
  Rename.RootDirectory = nullptr;
  Rename.FileNameLength = static_cast<DWORD>(FileNameBytes);
  std::copy(WideOutput.begin(), WideOutput.end(), Rename.FileName);

  // Submit exactly one namespace mutation.  In particular, an error is never
  // followed by moving the destination aside or by a second rename attempt.
  ::SetLastError(ERROR_SUCCESS);
  if (::SetFileInformationByHandle(TemporaryHandle, FileRenameInfo, &Rename,
                                   static_cast<DWORD>(RenameBuffer.size())))
    return {};
  unsigned Error = ::GetLastError();
  if (Error == ERROR_SUCCESS)
    Error = ERROR_CALL_NOT_IMPLEMENTED;
  return llvm::mapWindowsError(Error);
}

} // namespace
#endif

namespace neverd::support::atomic_output {

std::error_code
replaceTemporaryOutputWithoutMoveAside(llvm::StringRef TemporaryPath,
                                       llvm::StringRef OutputPath,
                                       const ReplaceOperations &Operations) {
  return Operations.Replace(TemporaryPath, OutputPath);
}

llvm::Error discardTemporaryOutput(llvm::sys::fs::TempFile &Temporary) {
  const std::string TemporaryPath = Temporary.TmpName;
  llvm::Error DiscardError = Temporary.discard();
  if (!DiscardError)
    return llvm::Error::success();

  const bool CloseFailed = Temporary.FD != -1;
  Temporary.FD = -1;
  Temporary.TmpName.clear();
  llvm::sys::DontRemoveFileOnSignal(TemporaryPath);

  const std::string Detail = llvm::toString(std::move(DiscardError));
  if (!CloseFailed)
    return llvm::createStringError(llvm::errc::io_error,
                                   "cannot remove temporary output %s: %s",
                                   TemporaryPath.c_str(), Detail.c_str());

  llvm::Error CloseError = llvm::createStringError(
      llvm::errc::io_error, "cannot close temporary output %s: %s",
      TemporaryPath.c_str(), Detail.c_str());
  std::error_code RemoveError = llvm::sys::fs::remove(TemporaryPath);
  if (!RemoveError || RemoveError == llvm::errc::no_such_file_or_directory)
    return CloseError;
  return llvm::joinErrors(
      std::move(CloseError),
      llvm::createStringError(RemoveError, "cannot remove temporary output %s",
                              TemporaryPath.c_str()));
}

llvm::Error commitTemporaryOutput(llvm::StringRef TemporaryPath,
                                  llvm::StringRef OutputPath,
                                  const CommitOperations &Operations) {
  if (llvm::Error RegistrationError =
          Operations.RegisterSignalCleanup(TemporaryPath))
    return llvm::joinErrors(std::move(RegistrationError),
                            Operations.Discard(TemporaryPath));

  llvm::scope_exit Unregister(
      [&] { Operations.UnregisterSignalCleanup(TemporaryPath); });
  if (llvm::Error CloseError = Operations.Close())
    return llvm::joinErrors(std::move(CloseError),
                            Operations.Discard(TemporaryPath));
  if (std::error_code Error = Operations.Rename(TemporaryPath, OutputPath)) {
    return llvm::joinErrors(
        llvm::createStringError(
            Error, "cannot atomically replace output %s with %s",
            OutputPath.str().c_str(), TemporaryPath.str().c_str()),
        Operations.Discard(TemporaryPath));
  }
  return llvm::Error::success();
}

llvm::Error closeAndCommitTemporaryOutput(llvm::sys::fs::TempFile &Temporary,
                                          llvm::StringRef OutputPath) {
  const std::string TemporaryPath = Temporary.TmpName;
#ifdef _WIN32
  llvm::ScopedFileHandle RenameHandle;
  std::error_code RenameHandleError;
  HANDLE NativeHandle =
      reinterpret_cast<HANDLE>(::_get_osfhandle(Temporary.FD));
  if (NativeHandle == INVALID_HANDLE_VALUE) {
    RenameHandleError = llvm::mapWindowsError(ERROR_INVALID_HANDLE);
  } else {
    HANDLE Duplicate = INVALID_HANDLE_VALUE;
    if (::DuplicateHandle(::GetCurrentProcess(), NativeHandle,
                          ::GetCurrentProcess(), &Duplicate, 0, FALSE,
                          DUPLICATE_SAME_ACCESS))
      RenameHandle = Duplicate;
    else
      RenameHandleError = llvm::mapWindowsError(::GetLastError());
  }
#endif
  auto Close = [&]() -> llvm::Error {
#ifdef _WIN32
    if (RenameHandleError) {
      RenameHandle = INVALID_HANDLE_VALUE;
      return llvm::createStringError(
          RenameHandleError,
          "cannot retain temporary output %s for atomic replacement",
          TemporaryPath.c_str());
    }
    if (!Temporary.RemoveOnClose) {
      if (std::error_code Error =
              clearTemporaryDeleteDispositionOnWindows(NativeHandle)) {
        RenameHandle = INVALID_HANDLE_VALUE;
        return llvm::createStringError(
            Error, "cannot retain temporary output %s for atomic replacement",
            TemporaryPath.c_str());
      }
      // TempFile::discard() must remove by path after the delete disposition
      // has been cleared and the descriptor has been closed.
      Temporary.RemoveOnClose = true;
    }
#endif
    const int Descriptor = std::exchange(Temporary.FD, -1);
    llvm::Error CloseError =
        Descriptor == -1
            ? llvm::Error::success()
            : llvm::errorCodeToError(
                  llvm::sys::Process::SafelyCloseFileDescriptor(Descriptor));

    // A close error is a data-flush failure, not permission to retry a numeric
    // descriptor that the operating system may already have recycled.
    if (CloseError) {
      const std::string Detail = llvm::toString(std::move(CloseError));
      CloseError = llvm::createStringError(
          llvm::errc::io_error, "cannot close temporary output %s: %s",
          TemporaryPath.c_str(), Detail.c_str());
    }
#ifdef _WIN32
    if (CloseError)
      RenameHandle = INVALID_HANDLE_VALUE;
#endif
    return CloseError;
  };
  auto Rename = [&](llvm::StringRef From,
                    llvm::StringRef To) -> std::error_code {
#ifdef _WIN32
    auto Replace = [&](llvm::StringRef, llvm::StringRef TargetPath) {
      std::error_code Error =
          replaceTemporaryOutputHandleOnWindows(RenameHandle, TargetPath);
      RenameHandle = INVALID_HANDLE_VALUE;
      return Error;
    };
    const ReplaceOperations ReplaceOps{Replace};
    return replaceTemporaryOutputWithoutMoveAside(From, To, ReplaceOps);
#else
    return llvm::sys::fs::rename(From, To);
#endif
  };
  auto Discard = [&](llvm::StringRef) -> llvm::Error {
#ifdef _WIN32
    RenameHandle = INVALID_HANDLE_VALUE;
#endif
    return discardTemporaryOutput(Temporary);
  };
  auto Register = [&](llvm::StringRef Path) -> llvm::Error {
#ifndef _WIN32
    // TempFile::create() already registered the path on POSIX.  Keeping that
    // registration in place closes the keep()/register gap.
    (void)Path;
    return llvm::Error::success();
#else
    // If delete-on-close was unavailable, TempFile::create() already installed
    // signal cleanup.  Otherwise add it before clearing delete disposition, so
    // one cleanup mechanism remains active throughout the handoff.
    if (Temporary.RemoveOnClose)
      return llvm::Error::success();
    std::string Detail;
    if (!llvm::sys::RemoveFileOnSignal(Path, &Detail))
      return llvm::Error::success();
    RenameHandle = INVALID_HANDLE_VALUE;
    if (Detail.empty())
      Detail = "registration was refused";
    return llvm::createStringError(
        llvm::errc::operation_not_permitted,
        "cannot register temporary output for signal cleanup: %s",
        Detail.c_str());
#endif
  };
  auto Unregister = [](llvm::StringRef Path) {
    llvm::sys::DontRemoveFileOnSignal(Path);
  };
  const CommitOperations Operations{Close, Rename, Discard, Register,
                                    Unregister};
  llvm::Error CommitError =
      commitTemporaryOutput(TemporaryPath, OutputPath, Operations);
  if (CommitError)
    return CommitError;

  // Rename consumed the only temporary pathname.  Mark TempFile complete
  // without asking it to unregister or remove anything a second time.
  Temporary.TmpName.clear();
  llvm::cantFail(Temporary.discard());
  return llvm::Error::success();
}

} // namespace neverd::support::atomic_output
