//===- KernelModelFileInformation.cpp - File namespace and mutations
//-------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// File namespace and mutations.
///

#include "../WindowsUnicode.h"
#include "KernelFiniteFileSystemValues.h"
#include "KernelModel.h"
#include "KernelSectionNames.h"
#include "WindowsKernelLayout.h"

#include "llvm/Support/Endian.h"

#include <algorithm>

namespace neverd::emulation {
using namespace windows;
using namespace finite_file_system;

llvm::Expected<KernelModel::FilePath>
KernelModel::resolveFilePath(uint64_t OA) {
  if (auto E = objectBuffer(OA, ObjectAttributesSize, false))
    return E;
  auto Size = Memory.readInteger(OA, 4),
       Root = Memory.readInteger(OA + ObjectRootOffset, 8),
       Name = Memory.readInteger(OA + ObjectNameOffset, 8),
       Flags = Memory.readInteger(OA + ObjectFlagsOffset, 4),
       Security = Memory.readInteger(OA + ObjectSecurityOffset, 8),
       QoS = Memory.readInteger(OA + ObjectQualityOffset, 8);
  if (!Size)
    return Size.takeError();
  if (!Root)
    return Root.takeError();
  if (!Name)
    return Name.takeError();
  if (!Flags)
    return Flags.takeError();
  if (!Security)
    return Security.takeError();
  if (!QoS)
    return QoS.takeError();
  FilePath P;
  P.Attributes = uint32_t(*Flags);
  P.CaseInsensitive = bool(*Flags & 0x40);
  if (*Size != ObjectAttributesSize) {
    P.Status = StatusInvalidParameter;
    return P;
  }
  if (*Flags & ~uint64_t(0x240) || *Security || *QoS)
    return fileError("unsupported object attributes/security descriptor");
  auto Text = readNativeObjectName(*Name, true);
  if (!Text)
    return Text.takeError();
  std::string Path = *Text;
  if (*Root) {
    auto H = FileHandles.find(*Root);
    if (H == FileHandles.end() || !canAccessFileHandle(H->second)) {
      P.Status = StatusInvalidHandle;
      return P;
    }
    const auto &F = NativeFiles.at(H->second.Object);
    if (!F.Node || !F.Node->Directory) {
      P.Status = NotDirectory;
      return P;
    }
    if (F.Node->DeletePending) {
      P.Status = StatusDeletePending;
      return P;
    }
    if (!Path.empty() && Path.front() == '\\') {
      P.Status = StatusInvalidParameter;
      return P;
    }
    std::string Prefix = F.Name;
    if (!Path.empty() && Prefix.back() != '\\')
      Prefix += '\\';
    Path = Prefix + Path;
  }
  if (Path.empty() || Path.front() != '\\') {
    P.Status = 0xc000003b;
    return P;
  }
  auto Expanded = resolveObjectPath(Path, P.CaseInsensitive, true);
  if (!Expanded)
    return Expanded.takeError();
  Path = *Expanded;
  if (llvm::StringRef(Path).equals_insensitive(section::PhysicalName) ||
      NamedSections.contains(llvm::StringRef(Path).lower())) {
    P.Status = StatusObjectTypeMismatch;
    return P;
  }
  if (!llvm::StringRef(Path).starts_with_insensitive("\\Device\\")) {
    size_t Start =
        llvm::StringRef(Path).starts_with_insensitive("\\DosDevices\\") ? 12
        : llvm::StringRef(Path).starts_with_insensitive("\\??\\")       ? 4
                                                                        : 1;
    P.Status = Path.find('\\', Start) == std::string::npos
                   ? StatusObjectNameNotFound
                   : PathNotFound;
    return P;
  }
  size_t End = Path.find('\\', 8);
  if (End == std::string::npos)
    End = Path.size();
  auto Device = resolveDeviceName(Path.substr(0, End), P.CaseInsensitive);
  if (!Device)
    return Device.takeError();
  if (!*Device) {
    P.Status = End == Path.size() ? StatusObjectNameNotFound : PathNotFound;
    return P;
  }
  bool Found = false;
  for (auto [I, D] : VolumeDevices)
    if (D == *Device) {
      P.Volume = I;
      Found = true;
    }
  if (!Found)
    return fileError("guest device opens require an IRP_MJ_CREATE dispatch");
  if (Devices.at(*Device).Upper)
    return fileError("attached filesystem filters require guest IRP dispatch");
  P.Device = *Device;
  P.Raw = End == Path.size();
  P.Relative = Path.substr(End);
  if (!P.Raw) {
    if (P.Relative.size() > 1 && P.Relative.back() == '\\')
      P.Relative.pop_back();
    if (!validDriverFiniteFileSystemPath(P.Relative, true))
      return fileError("unsupported volume-relative path syntax");
    auto Key = windows_unicode::key(P.Relative,
                                    (*FiniteFileSystem)[P.Volume].UpcaseTable);
    if (!Key)
      return Key.takeError();
    P.Key = std::move(*Key);
  }
  return P;
}

std::shared_ptr<KernelModel::FileNode>
KernelModel::findFileNode(FilePath &P) const {
  if (P.Raw || P.Status)
    return {};
  const auto &Nodes = FileNodes.at(P.Volume);
  auto It = Nodes.find(P.Key);
  if (It != Nodes.end() &&
      (P.CaseInsensitive || It->second->Path == P.Relative)) {
    if (It->second->DeletePending)
      P.Status = StatusDeletePending;
    return It->second;
  }
  auto Parent =
      llvm::StringRef(P.Relative).take_front(P.Relative.rfind('\\')).str();
  if (Parent.empty())
    Parent = "\\";
  std::string ParentKey =
      llvm::StringRef(P.Key).take_front(P.Key.rfind('\\')).str();
  if (ParentKey.empty())
    ParentKey = "\\";
  auto Dir = Nodes.find(ParentKey);
  if (Dir == Nodes.end() || !Dir->second->Directory ||
      (!P.CaseInsensitive && Dir->second->Path != Parent))
    P.Status = PathNotFound;
  else if (Dir->second->DeletePending)
    P.Status = StatusDeletePending;
  else
    P.Status = StatusObjectNameNotFound;
  return {};
}

bool KernelModel::fileDirectoryEmpty(size_t Volume,
                                     const FileNode &Node) const {
  const std::string Prefix = Node.Key == "\\" ? "\\" : Node.Key + "\\";
  for (const auto &[Key, N] : FileNodes.at(Volume))
    if (Key != Node.Key && llvm::StringRef(Key).starts_with(Prefix))
      return false;
  return true;
}

std::pair<size_t, uint64_t> KernelModel::finiteFileSystemUsage() const {
  std::set<const FileNode *> Seen;
  uint64_t Bytes = 0;
  size_t Count = 0;
  auto Add = [&](const std::shared_ptr<FileNode> &N) {
    if (N && Seen.insert(N.get()).second && N->Path != "\\") {
      ++Count;
      Bytes += N->Data.size();
    }
  };
  for (const auto &[V, Nodes] : FileNodes)
    for (const auto &[K, N] : Nodes)
      Add(N);
  for (const auto &[O, F] : NativeFiles)
    Add(F.Node);
  return {Count, Bytes};
}

llvm::Error KernelModel::snapshotFiniteFileSystem() {
  Result.FiniteFileSystem = FiniteFileSystem;
  if (!Result.FiniteFileSystem)
    return llvm::Error::success();
  for (size_t I = 0; I < Result.FiniteFileSystem->size(); ++I) {
    auto &V = (*Result.FiniteFileSystem)[I];
    V.Entries.clear();
    auto It = FileNodes.find(I);
    if (It == FileNodes.end())
      continue;
    for (const auto &[Key, N] : It->second)
      if (N->Path != "\\")
        V.Entries.push_back({N->Path, N->Directory, N->Data});
  }
  return llvm::Error::success();
}

llvm::Error KernelModel::setFileDeletePending(
    size_t Volume, const std::shared_ptr<FileNode> &N, bool Pending) {
  N->DeletePending = Pending;
  for (const auto &[Object, F] : NativeFiles)
    if (F.Volume == Volume && F.Node == N)
      if (auto E =
              Memory.writeInteger(Object + FileDeletePendingOffset, Pending, 1))
        return E;
  return llvm::Error::success();
}

llvm::Error
KernelModel::unlinkFileIfClosed(size_t Volume,
                                const std::shared_ptr<FileNode> &N) {
  if (!N || !N->DeletePending)
    return llvm::Error::success();
  for (const auto &[O, F] : NativeFiles)
    if (F.Node == N && NativeOpenFiles.at(F.ID).State == NativeFileState::Open)
      return llvm::Error::success();
  if (N->Directory && !fileDirectoryEmpty(Volume, *N))
    return setFileDeletePending(Volume, N, false);
  auto &Nodes = FileNodes.at(Volume);
  auto It = Nodes.find(N->Key);
  if (It != Nodes.end() && It->second == N)
    Nodes.erase(It);
  return llvm::Error::success();
}

llvm::Expected<uint64_t> KernelModel::deleteFile(uint64_t OA) {
  auto P = resolveFilePath(OA);
  if (!P)
    return P.takeError();
  if (P->Status)
    return P->Status;
  if (P->Raw || P->Relative == "\\")
    return AccessDenied;
  auto N = findFileNode(*P);
  if (P->Status)
    return P->Status;
  if ((*FiniteFileSystem)[P->Volume].ReadOnly)
    return MediaWriteProtected;
  for (const auto &[O, F] : NativeFiles)
    if (F.Node == N &&
        NativeOpenFiles.at(F.ID).State == NativeFileState::Open &&
        !(F.Share & 4))
      return SharingViolation;
  if (!N->Image.expired())
    return SharingViolation;
  // NtDeleteFile's delete-on-close path reports its successful open even when
  // cleanup cannot remove a nonempty directory. The binding remains intact.
  if (N->Directory && !fileDirectoryEmpty(P->Volume, *N))
    return StatusSuccess;
  if (auto E = setFileDeletePending(P->Volume, N, true))
    return E;
  if (auto E = unlinkFileIfClosed(P->Volume, N))
    return E;
  return StatusSuccess;
}

llvm::Expected<uint64_t>
KernelModel::fileInformation(llvm::ArrayRef<uint64_t> A, bool Set) {
  auto H = FileHandles.find(A[0]);
  if (H == FileHandles.end() || !canAccessFileHandle(H->second))
    return StatusInvalidHandle;
  auto &F = NativeFiles.at(H->second.Object);
  if (!F.Node)
    return fileError("raw volume information is outside this profile");
  const auto N = F.Node;
  const uint32_t Class = uint32_t(A[4]), Size = uint32_t(A[3]);
  auto Complete = [&](uint32_t Status,
                      uint64_t Count) -> llvm::Expected<uint64_t> {
    if (auto E = Memory.writeInteger(A[1], Status, 4))
      return E;
    if (auto E = Memory.writeInteger(A[1] + 8, Count, 8))
      return E;
    return Status;
  };
  if (Set) {
    const uint32_t Need = Class == 13 ? 1 : 8;
    if (Class != 13 && Class != 14 && Class != 20)
      return fileError("unsupported file information class");
    if (Size < Need)
      return StatusInfoLengthMismatch;
    if (auto E = objectBuffer(A[2], Need, false))
      return E;
    auto Value = Memory.readInteger(A[2], Need);
    if (!Value)
      return Value.takeError();
    if (auto E = objectBuffer(A[1], 16, true))
      return E;
    if (Class == 14) {
      if (!(H->second.Access & (ReadData | WriteData)))
        return AccessDenied;
      if (*Value > INT64_MAX)
        return StatusInvalidParameter;
      if (auto E = Memory.writeInteger(H->second.Object + FileCurrentByteOffset,
                                       *Value, 8))
        return E;
    } else {
      if ((*FiniteFileSystem)[F.Volume].ReadOnly)
        return Complete(MediaWriteProtected, 0);
      if (Class == 20) {
        if (!(H->second.Access & WriteData))
          return AccessDenied;
        if (N->Directory)
          return Complete(IsDirectory, 0);
        if (*Value > INT64_MAX)
          return StatusInvalidParameter;
        if (!N->Image.expired() && *Value < N->Data.size())
          return uint64_t(
              0xc0000243); // STATUS_USER_MAPPED_FILE; IOSB untouched.
        const auto Usage = finiteFileSystemUsage();
        if (*Value > MaxDriverFiniteFileSystemBytes ||
            (*Value > N->Data.size() &&
             *Value - N->Data.size() >
                 MaxDriverFiniteFileSystemBytes - Usage.second))
          return Complete(DiskFull, 0);
        N->Data.resize(size_t(*Value), 0);
      } else {
        if (!(H->second.Access & Delete))
          return AccessDenied;
        if (N->Path == "\\")
          return Complete(AccessDenied, 0);
        if (*Value && !N->Image.expired())
          return uint64_t(0xc0000121); // STATUS_CANNOT_DELETE; IOSB untouched.
        if (*Value && N->Directory && !fileDirectoryEmpty(F.Volume, *N))
          return Complete(DirectoryNotEmpty, 0);
        if (auto E = setFileDeletePending(F.Volume, N, *Value != 0))
          return E;
      }
    }
    return Complete(0, 0);
  }
  std::vector<uint8_t> B;
  uint32_t Status = 0;
  if (Class == 14) {
    if (!(H->second.Access & (ReadData | WriteData)))
      return fileError("position information requires data access");
    if (Size < 8)
      return StatusInfoLengthMismatch;
    auto Pos = Memory.readInteger(H->second.Object + FileCurrentByteOffset, 8);
    if (!Pos)
      return Pos.takeError();
    B.resize(8);
    llvm::support::endian::write64le(B.data(), *Pos);
  } else if (Class == 5) {
    if (Size < 24)
      return StatusInfoLengthMismatch;
    B.resize(24);
    llvm::support::endian::write64le(B.data(),
                                     (N->Data.size() + 7) & ~uint64_t(7));
    llvm::support::endian::write64le(B.data() + 8, N->Data.size());
    llvm::support::endian::write32le(B.data() + 16, N->DeletePending ? 0 : 1);
    B[20] = N->DeletePending;
    B[21] = N->Directory;
  } else if (Class == 9) {
    // I/O manager minimum is sizeof(FILE_NAME_INFORMATION), including its
    // first WCHAR and alignment. Native short copies round down to WCHARs.
    if (Size < 8)
      return StatusInfoLengthMismatch;
    auto Encoded = windows_unicode::bytes(N->Path);
    if (!Encoded)
      return Encoded.takeError();
    auto &Text = *Encoded;
    B.resize(4 + std::min<size_t>(Text.size(), (Size - 4) & ~uint32_t(1)));
    llvm::support::endian::write32le(B.data(), uint32_t(Text.size()));
    std::copy_n(Text.begin(), B.size() - 4, B.begin() + 4);
    if (B.size() < 4 + Text.size())
      Status = BufferOverflow;
  } else
    return fileError("unsupported file information class");
  if (auto E = objectBuffer(A[1], 16, true))
    return E;
  if (auto E = objectBuffer(A[2], uint32_t(B.size()), true))
    return E;
  if (auto E = Memory.write(A[2], B))
    return E;
  return Complete(Status, B.size());
}
} // namespace neverd::emulation
