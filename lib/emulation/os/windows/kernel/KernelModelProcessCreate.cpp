//===- KernelModelProcessCreate.cpp - Borrowed Win32 creation information
//--===//
// NeverD Decompiler
///
/// \file
/// Borrowed Win32 creation information.
///

#include "KernelModel.h"
#include "KernelProcessCreate.h"
#include "WindowsKernelLayout.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation {
namespace {
llvm::Error invalid(const llvm::Twine &T) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "process create info: " + T);
}
bool overlap(uint64_t A, uint64_t N, uint64_t B, uint64_t M) {
  return N && M && A < B + M && B < A + N;
}
} // namespace
llvm::Error KernelModel::prepareProcessCreateInfo(const DriverProcessEvent &E) {
  if (!ProcessNotificationState || !E.CreateInfo || CurrentIRQL)
    return invalid("requires an explicit event at PASSIVE_LEVEL");
  auto &N = *ProcessNotificationState;
  auto Info = allocate(process_create::Size);
  if (!Info)
    return Info.takeError();
  N.CreateInfo = *Info;
  N.Storage.emplace_back(*Info, process_create::Size);
  auto File = allocate(windows::FileSize);
  if (!File)
    return File.takeError();
  N.ImageFileObject = *File;
  N.Storage.emplace_back(*File, windows::FileSize);
  auto String = [&](const std::string &S) -> llvm::Expected<uint64_t> {
    auto A = makeUnicodeString(S);
    if (A)
      N.Storage.emplace_back(*A, ArenaAllocations.at(*A));
    return A;
  };
  auto Image = String(E.CreateInfo->ImageFileName);
  if (!Image)
    return Image.takeError();
  uint64_t Command = 0;
  if (E.CreateInfo->CommandLine) {
    auto A = String(*E.CreateInfo->CommandLine);
    if (!A)
      return A.takeError();
    Command = *A;
  }
  auto Thread = currentThreadID();
  if (!Thread)
    return Thread.takeError();
  struct Field {
    unsigned Offset, Size;
    uint64_t Value;
  };
  for (const auto &F :
       {Field{0, 8, process_create::Size},
        {process_create::Flags, 4,
         E.CreateInfo->FileOpenNameAvailable ? 1u : 0u},
        {process_create::ParentID, 8, E.ParentID},
        {process_create::CreatorProcessID, 8, E.CreatorProcessID},
        {process_create::CreatorThreadID, 8, *Thread},
        {process_create::FileObject, 8, *File},
        {process_create::ImageName, 8, *Image},
        {process_create::CommandLine, 8, Command},
        {process_create::Status, 4, 0}})
    if (auto Err = Memory.writeInteger(*Info + F.Offset, F.Value, F.Size))
      return Err;
  Result.ProcessEvents.at(N.ReportIndex).CreationStatus = 0;
  return llvm::Error::success();
}
llvm::Error KernelModel::validateProcessNotifyAccess(uint64_t Address,
                                                     uint32_t Size,
                                                     bool Write) const {
  if (!ProcessNotificationState)
    return llvm::Error::success();
  const auto &N = *ProcessNotificationState;
  for (const auto &[Base, Length] : N.Storage) {
    if (!overlap(Address, Size, Base, Length))
      continue;
    const auto R = ProcessNotifiers.find(N.Registration);
    if (!N.Active || N.Thread != CurrentThreadKey ||
        R == ProcessNotifiers.end() || !R->second.Extended || Address < Base ||
        Size > Length || Address - Base > Length - Size)
      return invalid(
          "storage is borrowed by the active Ex callback thread only");
    if (Base == N.ImageFileObject)
      return invalid(
          "executable FILE_OBJECT fields require an image-file owner");
    for (uint64_t I = Address - Base; I < Address - Base + Size; ++I) {
      const bool Allowed =
          Base == N.CreateInfo
              ? (Write ? I >= process_create::Status &&
                             I < process_create::Status + 4
                       : I < 12 || (I >= process_create::ParentID &&
                                    I < process_create::Status + 4))
              : !Write && (I < 4 || I >= 8);
      if (!Allowed)
        return invalid("access to padding, read-only input or unsupported file "
                       "replacement");
    }
  }
  return llvm::Error::success();
}
} // namespace neverd::emulation
