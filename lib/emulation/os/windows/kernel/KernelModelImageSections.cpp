//===- KernelModelImageSections.cpp - File-backed image section ownership
//---===//
// NeverD Decompiler
///
/// \file
/// File-backed image section ownership.
///

#include "../driver/DriverProcessImages.h"
#include "KernelModel.h"
#include "KernelSectionNames.h"
#include "WindowsKernelLayout.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
using namespace windows;
llvm::Error imageError(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "image section: " + Text);
}
} // namespace

llvm::Expected<std::shared_ptr<KernelModel::ImageSection>>
KernelModel::imageSection(const DriverProcessImageMapping &Input,
                          std::shared_ptr<FileNode> File) {
  if (File)
    if (auto Existing = File->Image.lock())
      return Existing;
  auto Plan = planDriverProcessImage(Input);
  if (!Plan)
    return Plan.takeError();
  auto Backing = allocatePhysicalBuffer(Plan->Bytes.size(), profile::PageSize,
                                        0, Plan->Bytes.size());
  if (!Backing)
    return Backing.takeError();
  if (auto E = Memory.write(*Backing, Plan->Bytes))
    return E;
  if (auto E = Memory.protect(*Backing, Plan->Bytes.size(), 0))
    return E;
  auto Section = std::make_shared<ImageSection>(
      ImageSection{std::make_shared<DriverProcessImagePlan>(std::move(*Plan)),
                   *Backing, std::move(File)});
  if (Section->File)
    Section->File->Image = Section;
  return Section;
}

llvm::Expected<std::vector<KernelModel::ProcessMemoryView>>
KernelModel::imagePages(const std::shared_ptr<ImageSection> &Section,
                        uint64_t Size) {
  std::vector<ProcessMemoryView> Pages;
  for (size_t I = 0; I < Size / profile::PageSize; ++I) {
    const uint8_t Protect = Section->Plan->PageProtection[I];
    unsigned Permissions = Protect == 1 ? 0 : Protect == 0x10 ? Execute : Read;
    if (Protect == 8 || Protect == 0x80)
      Permissions |= Write;
    if (Protect >= 0x10)
      Permissions |= Execute;
    const uint64_t Backing = Section->Backing + I * profile::PageSize;
    auto Storage = Memory.pinBacking(Backing, profile::PageSize);
    if (!Storage)
      return Storage.takeError();
    auto Page = std::make_shared<ProcessMemoryView::ImagePage>();
    Page->Source = Section->Plan;
    Page->Section = Section;
    Page->Protection = Protect;
    if (Permissions & Write) {
      // COW storage is reserved before publishing any alias. CPU hooks never
      // reenter the allocator. Locked MDLs retain their old physical pages.
      auto Private = allocatePhysicalBuffer(
          profile::PageSize, profile::PageSize, 0, profile::PageSize);
      if (!Private)
        return Private.takeError();
      auto Storage = Memory.pinBacking(*Private, profile::PageSize);
      if (!Storage)
        return Storage.takeError();
      Page->PrivateBacking = *Private;
      Page->PrivateStorage = std::move(*Storage);
      if (auto E = Memory.protect(*Private, profile::PageSize, 0))
        return E;
    }
    Pages.push_back({Section->Plan->Base + I * profile::PageSize,
                     profile::PageSize, Backing, Permissions,
                     std::move(*Storage), std::move(Page)});
  }
  return Pages;
}

llvm::Expected<uint64_t>
KernelModel::createImageSection(llvm::ArrayRef<uint64_t> A) {
  if (!Version || Version->Major != 10 || Version->Minor != 0 ||
      Version->Build != 19041)
    return imageError("requires Windows 10.0.19041");
  if (uint32_t(A[5]) != 0x1000000)
    return imageError("section creation currently requires SEC_IMAGE");
  const uint32_t Protect = uint32_t(A[4]);
  if (Protect != 2 && Protect != 4 && Protect != 8 && Protect != 0x10 &&
      Protect != 0x20 && Protect != 0x40 && Protect != 0x80)
    return uint64_t(0xc0000045);
  if (A[3])
    return imageError("explicit MaximumSize is unmodeled for SEC_IMAGE");
  uint32_t Attributes = 0;
  if (A[2]) {
    if (auto E = objectBuffer(A[2], ObjectAttributesSize, false))
      return E;
    auto Length = Memory.readInteger(A[2], 4);
    if (!Length)
      return Length.takeError();
    if (*Length != ObjectAttributesSize)
      return StatusInvalidParameter;
    for (unsigned Offset : {8u, 16u, 32u, 40u}) {
      auto V = Memory.readInteger(A[2] + Offset, 8);
      if (!V)
        return V.takeError();
      if (*V)
        return imageError(
            "named image sections and security descriptors are unmodeled");
    }
    auto Flags = Memory.readInteger(A[2] + 24, 4);
    if (!Flags)
      return Flags.takeError();
    if (*Flags & ~uint64_t(ObjectKernelHandle))
      return imageError("unsupported unnamed section object attributes");
    Attributes = uint32_t(*Flags);
  }
  const uint32_t PID = handleProcessID();
  if (PID != SystemProcessID && !(Attributes & ObjectKernelHandle))
    return imageError(
        "driver creation outside System requires OBJ_KERNEL_HANDLE");
  auto H = FileHandles.find(A[6]);
  if (H == FileHandles.end()) {
    if (SectionHandles.contains(A[6]) || ObjectHandles.contains(A[6]) ||
        Registry.ownsHandle(A[6]))
      return StatusObjectTypeMismatch;
    return StatusInvalidHandle;
  }
  if (!canAccessFileHandle(H->second))
    return StatusInvalidHandle;
  const auto &F = NativeFiles.at(H->second.Object);
  if (!F.Node || F.Node->Directory || F.RawVolume)
    return imageError("requires an ordinary file source");
  if (F.Node->DeletePending)
    return StatusDeletePending;
  const uint32_t Required = Protect == 4                         ? 3
                            : Protect == 0x10 || Protect == 0x20 ? 0x21
                            : Protect == 0x40                    ? 0x23
                            : Protect == 0x80                    ? 0x21
                                                                 : 1;
  if ((H->second.Access & Required) != Required)
    return uint64_t(0xc0000022);
  const uint32_t Access = section::accessMask(uint32_t(A[1]));
  if (Access & 0x01000000)
    return imageError(
        "ACCESS_SYSTEM_SECURITY requires an explicit privilege policy");
  if (Access & ~uint32_t(0x1f001f))
    return imageError("unsupported section access mask");
  if (auto E = objectBuffer(A[0], 8, true))
    return E;
  if (SectionHandles.size() >= 256 || NextSectionHandle > 0x520ffffc)
    return StatusInsufficientResources;
  DriverProcessImageMapping Input;
  Input.MappedFile = (*FiniteFileSystem)[F.Volume].DeviceName + F.Node->Path;
  Input.DeviceName = (*FiniteFileSystem)[F.Volume].DeviceName;
  Input.FileBytes = F.Node->Data;
  auto Section = imageSection(Input, F.Node);
  if (!Section)
    return Section.takeError();
  if (auto E = Memory.writeInteger(A[0], NextSectionHandle, 8))
    return E;
  SectionHandles.emplace(NextSectionHandle,
                         SectionHandle{Access, Attributes, PID, {}, *Section});
  NextSectionHandle += 4;
  return StatusSuccess;
}

llvm::Expected<uint64_t>
KernelModel::mapImageSection(llvm::ArrayRef<uint64_t> A) {
  if (A[1] != UINT64_MAX)
    return imageError(
        "image mapping requires the current-process pseudo handle");
  const uint32_t PID = handleProcessID();
  const auto Context = executionProcessContext();
  if (!Context || Context->ProcessID != PID || !isLiveProcess(PID) ||
      PID != (UserRequestContext ? CurrentUserProcessID
                                 : uint32_t(SystemProcessID)))
    return imageError("mapping requires its live active process context");
  if (A[3] || A[4] || uint32_t(A[7]) != 2 || uint32_t(A[8]))
    return imageError("image mapping requires ZeroBits=0, CommitSize=0, "
                      "ViewUnmap and AllocationType=0");
  const uint32_t Protect = uint32_t(A[9]);
  if (Protect != 1 && Protect != 2 && Protect != 4 && Protect != 8 &&
      Protect != 0x10 && Protect != 0x20 && Protect != 0x40 && Protect != 0x80)
    return uint64_t(0xc0000045);
  const auto &Handle = SectionHandles.at(A[0]);
  const uint32_t Required = Protect == 4 || Protect == 8         ? 2
                            : Protect == 0x10                    ? 8
                            : Protect == 0x20                    ? 12
                            : Protect == 0x40 || Protect == 0x80 ? 10
                                                                 : 4;
  if ((Handle.Access & Required) != Required)
    return uint64_t(0xc0000022);
  for (uint64_t Output : {A[2], A[6]})
    if (auto E = objectBuffer(Output, 8, true))
      return E;
  if (A[5]) {
    if (auto E = objectBuffer(A[5], 8, true))
      return E;
    auto Offset = Memory.readInteger(A[5], 8);
    if (!Offset)
      return Offset.takeError();
    if (*Offset % 0x10000)
      return uint64_t(0xc0000220);
    if (*Offset)
      return imageError("nonzero image SectionOffset is unmodeled");
  }
  auto Base = Memory.readInteger(A[2], 8), Size = Memory.readInteger(A[6], 8);
  if (!Base)
    return Base.takeError();
  if (!Size)
    return Size.takeError();
  const auto &Section = Handle.Image;
  const uint64_t Address = Section->Plan->Base,
                 Limit = Section->Plan->Bytes.size();
  if (*Base && *Base != Address)
    return imageError("rebased image views require relocation support");
  if (*Size > Limit)
    return uint64_t(0xc000001f);
  const uint64_t Bytes = *Size ? (*Size + 4095) & ~uint64_t(4095) : Limit;
  if (ImageMappings.size() >= 256)
    return StatusInsufficientResources;
  if (auto It = ProcessMemoryViews.find(PID); It != ProcessMemoryViews.end())
    for (const auto &V : It->second)
      if (V.Base < Address + Bytes && Address < V.Base + V.Size) {
        if (*Base)
          return uint64_t(0xc0000018);
        return imageError("preferred image address is occupied; alternate "
                          "placement is unmodeled");
      }
  auto Pages = imagePages(Section, Bytes);
  if (!Pages)
    return Pages.takeError();
  std::vector<GuestAliasMapping> Adding;
  for (const auto &V : *Pages)
    Adding.push_back({V.Base, V.Backing, V.Size, processMemoryPermissions(V)});
  if (auto E = Memory.replaceAliases({}, Adding))
    return E;
  auto &Views = ProcessMemoryViews[PID];
  Views.insert(Views.end(), std::make_move_iterator(Pages->begin()),
               std::make_move_iterator(Pages->end()));
  ImageMappings.emplace(std::pair{PID, Address},
                        ImageMapping{Section, Bytes, true});
  // The matched native call publishes these three outputs in this order.
  if (auto E = Memory.writeInteger(A[6], Bytes, 8))
    return E;
  if (auto E = Memory.writeInteger(A[2], Address, 8))
    return E;
  if (A[5])
    if (auto E = Memory.writeInteger(A[5], 0, 8))
      return E;
  return StatusSuccess;
}

llvm::Error KernelModel::retireImageMapping(uint32_t PID, uint64_t Base) {
  auto Mapping = ImageMappings.find({PID, Base});
  if (Mapping == ImageMappings.end())
    return imageError("retirement lost image mapping identity");
  const auto Context = executionProcessContext();
  if (!Context || Context->ProcessID != PID ||
      PID != (UserRequestContext ? CurrentUserProcessID
                                 : uint32_t(SystemProcessID)))
    return imageError("unmapping requires its active process context");
  const uint64_t End = Base + Mapping->second.Size;
  auto &Views = ProcessMemoryViews.at(PID);
  std::vector<GuestAliasRange> Removing;
  for (const auto &V : Views)
    if (V.Base >= Base && V.Base < End) {
      if (!V.Image || V.Image->Section != Mapping->second.Section)
        return imageError("retirement encountered unrelated process memory");
      if (auto E = canRevokeVirtualRange(V.Base, V.Size))
        return E;
      Removing.push_back({V.Base, V.Size});
    }
  if (auto E = Memory.replaceAliases(Removing, {}))
    return E;
  if (auto E = prepareRevokeVirtualRange(Base, Mapping->second.Size))
    return E;
  std::erase_if(Views,
                [&](const auto &V) { return V.Base >= Base && V.Base < End; });
  ImageMappings.erase(Mapping);
  return llvm::Error::success();
}

llvm::Expected<uint64_t> KernelModel::querySection(llvm::ArrayRef<uint64_t> A) {
  if (uint32_t(A[1]) != 0)
    return imageError("unmodeled section information class");
  if (uint32_t(A[3]) < 24)
    return StatusInfoLengthMismatch;
  auto H = SectionHandles.find(A[0]);
  if (H == SectionHandles.end()) {
    if (FileHandles.contains(A[0]) || ObjectHandles.contains(A[0]) ||
        Registry.ownsHandle(A[0]))
      return StatusObjectTypeMismatch;
    return StatusInvalidHandle;
  }
  if (!canAccessHandle(H->second.Attributes, H->second.ProcessID))
    return StatusInvalidHandle;
  if (!(H->second.Access & 1))
    return uint64_t(0xc0000022);
  if (!H->second.Image)
    return imageError("section basic information requires an image provider");
  if (auto E = objectBuffer(A[2], 24, true))
    return E;
  if (A[4])
    if (auto E = objectBuffer(A[4], 4, true))
      return E;
  // The native implementation preserves the four padding bytes at +12.
  if (auto E = Memory.writeInteger(A[2], 0, 8))
    return E;
  if (auto E = Memory.writeInteger(A[2] + 8, 0x1800000, 4))
    return E;
  if (auto E = Memory.writeInteger(A[2] + 16,
                                   H->second.Image->Plan->Bytes.size(), 8))
    return E;
  if (A[4])
    if (auto E = Memory.writeInteger(A[4], 24, 4))
      return E;
  return StatusSuccess;
}
} // namespace neverd::emulation
