//===- LinuxFileTestMetadata.h - Independent status observations -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_UNITTESTS_EMULATION_LINUXFILETESTMETADATA_H
#define NEVERD_UNITTESTS_EMULATION_LINUXFILETESTMETADATA_H

#include "neverd/emulation/LinuxFileOptions.h"

namespace neverd::emulation {
inline constexpr char FileTestMetadataJSON[] = R"({
  "device":4262645044,"inode":"18364758544493064720","mode":33188,
  "link_count":2309737967,"uid":2271560481,"gid":4275878552,
  "size":0,"block_size":16384,"blocks":78187493520,
  "access_time":{"seconds":"-9223372036854775807","nanoseconds":123456789},
  "modification_time":{"seconds":4294967297,"nanoseconds":987654321},
  "change_time":{"seconds":"9223372036854775807","nanoseconds":999999999}})";
inline LinuxFileMetadata fileTestMetadata() {
  return {0xfe12cd34,
          0xfedcba9876543210ULL,
          0100644,
          0x89abcdef,
          0x87654321,
          0xfedcba98,
          0,
          16384,
          0x1234567890ULL,
          {-0x7fffffffffffffffLL, 123456789},
          {4294967297, 987654321},
          {0x7fffffffffffffffLL, 999999999}};
}
inline LinuxFileOptions fileRenameTestInput() {
  LinuxFileOptions F;
  F.Files["/fixture/data"] = {0, 0xff, 0x41, 0x0a, 0x80, 0x5a};
  F.WriteLimits["/fixture/data"] = 8192;
  F.TrackWriteMetadata.insert("/fixture/data");
  LinuxFileMetadata M;
  M.Device = 7;
  M.Inode = 42;
  M.Mode = 0100644;
  M.UID = M.GID = 1000;
  M.LinkCount = 1;
  M.Size = 6;
  M.BlockSize = 4096;
  M.Blocks = 8;
  M.AccessTime = {3, 4};
  M.ModificationTime = {5, 6};
  M.ChangeTime = {7, 8};
  F.Metadata["/fixture/data"] = M;
  M.Inode = 91;
  M.Mode = 0040755;
  M.LinkCount = 2;
  M.Size = 60;
  M.Blocks = 0;
  F.DirectoryCreation.emplace().Parents["/fixture"] = M;
  F.Directories.push_back("/fixture");
  F.FileRenames.emplace().Parents.insert("/fixture");
  F.FileRenames->Limit = 2;
  F.DynamicProcFDs = true;
  return F;
}
inline LinuxFileOptions fileUnlinkTestInput() {
  auto F = fileRenameTestInput();
  F.FileUnlinks.emplace().Parents.insert("/fixture");
  F.FileUnlinks->Limit = 1;
  return F;
}
inline LinuxFileOptions fileCreationTestInput() {
  auto F = fileUnlinkTestInput();
  F.FileCreations.emplace().Files["/fixture/data"] = {8192};
  return F;
}

inline LinuxFileOptions fileCreationCountTestInput() {
  auto F = fileCreationTestInput();
  F.FileCreations->Files.at("/fixture/data") = {3, 2};
  F.FileUnlinks->Limit = 2;
  return F;
}
} // namespace neverd::emulation
#endif
