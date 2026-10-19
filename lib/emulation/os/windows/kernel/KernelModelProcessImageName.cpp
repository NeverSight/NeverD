//===- KernelModelProcessImageName.cpp - Exact guest short-name storage
//----===//
// NeverD Decompiler
///
/// \file
/// Exact guest short-name storage.
///

#include "KernelModel.h"
#include "KernelReferenceImage.h"
#include "WindowsKernelLayout.h"

#include <algorithm>
namespace neverd::emulation {
llvm::Expected<uint64_t> KernelModel::processImageFileName(uint64_t Object) {
  auto Unsupported = [](const llvm::Twine &Text) -> llvm::Error {
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "PsGetProcessImageFileName: " + Text);
  };
  const auto Identity = ProcessObjectIDs.find(Object);
  if (Identity == ProcessObjectIDs.end())
    return Unsupported("requires a known process object");
  if (!Version || Version->Major != 10 || Version->Minor != 0 ||
      Version->Build != 19041 || !ProcessInventory)
    return Unsupported("requires Windows 10.0.19041 and process_inventory");
  const auto Facts = std::find_if(
      ProcessInventory->begin(), ProcessInventory->end(),
      [&](const auto &P) { return P.ProcessID == Identity->second; });
  if (Facts == ProcessInventory->end() || !Facts->ImageFileName)
    return Unsupported("requires explicit image_file_name_hex bytes");
  const auto At = Object + windows::ProcessImageFileName19041Offset;
  if (ProcessObjectSize <
      windows::ProcessImageFileName19041Offset + DriverProcessImageFileNameSize)
    return Unsupported("process layout does not contain the native name field");
  if (DebuggerReference) {
    const auto Getter =
        DebuggerReference->exports().find("PsGetProcessImageFileName");
    const uint8_t Body[] = {0x48, 0x8d, 0x81, 0xa8, 0x05, 0x00, 0x00, 0xc3};
    if (Getter == DebuggerReference->exports().end() || !Getter->second ||
        !DebuggerReference->matchesCode(Getter->second, Body))
      return Unsupported(
          "reference getter does not match the selected native layout");
  }
  if (!PublishedProcessImageNames.contains(Object)) {
    if (auto E = Memory.write(At, *Facts->ImageFileName))
      return E;
    PublishedProcessImageNames.insert(Object);
  }
  return At;
}
} // namespace neverd::emulation
