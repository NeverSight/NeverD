//===- DarwinFileTestData.h - Explicit file observations --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_TESTS_DARWINFILETESTDATA_H
#define NEVERD_TESTS_DARWINFILETESTDATA_H

#include "neverd/emulation/DarwinFileOptions.h"

#include <string_view>

namespace neverd::emulation::darwin_test {
inline constexpr char KernelPathConfHex[] =
    "0100000000000000010000000000000001000000000000000000000000000000"
    "0010000000000000000001000000000000100000000000000010000000000000"
    "ff000000000000000000000000000000";
inline constexpr char KernelPathConfJSON[] =
    R"({"files":[{"path":"/data","bytes_hex":"30313233343536373839"}],"directories":[{"path":"/"},{"path":"/empty"}],"working_directory":"/empty","symbolic_links":[{"path":"/alias","target_hex":"64617461"},{"path":"/dangling","target_hex":"6d697373696e67"},{"path":"/cycle","target_hex":"6379636c65"}]})";
inline DarwinFileOptions kernelPathConfOptions() {
  DarwinFileOptions O;
  O.Files["/data"] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9'};
  O.Directories = {"/", "/empty"};
  O.WorkingDirectory = "/empty";
  O.SymbolicLinks["/alias"] = {'d', 'a', 't', 'a'};
  O.SymbolicLinks["/dangling"] = {'m', 'i', 's', 's', 'i', 'n', 'g'};
  O.SymbolicLinks["/cycle"] = {'c', 'y', 'c', 'l', 'e'};
  return O;
}
inline constexpr char AttributeNamesHex[] =
    "2c0000000900008000000000000000000000000000000000"
    "0c00000005000000010000006461746100000000";
inline constexpr char XattrMutationsHex[] =
    "00ff410080420a757365722e6e65766572642e6265746100";
inline DarwinFileOptions xattrMutationsOptions() {
  auto O = kernelPathConfOptions();
  O.WorkingDirectory = "/";
  O.Files["/readonly"] = {};
  O.MutableDirectories.insert("/");
  for (const auto &[Path, Target] : O.SymbolicLinks)
    O.MutableSymbolicLinks.insert(Path);
  for (const char *Path : {"/data", "/empty", "/alias", "/readonly"})
    O.ExtendedAttributes[Path] = {};
  O.MutableExtendedAttributes = {"/data", "/empty", "/alias"};
  return O;
}
inline constexpr char XattrMutationsJSON[] = R"({
  "files":[
    {"path":"/data","bytes_hex":"30313233343536373839",
     "extended_attributes":[],"mutable_extended_attributes":true},
    {"path":"/readonly","bytes_hex":"","extended_attributes":[]}],
  "directories":[{"path":"/","mutable":true},
    {"path":"/empty","extended_attributes":[],
     "mutable_extended_attributes":true}],
  "symbolic_links":[
    {"path":"/alias","target_hex":"64617461","mutable":true,
     "extended_attributes":[],"mutable_extended_attributes":true},
    {"path":"/dangling","target_hex":"6d697373696e67","mutable":true},
    {"path":"/cycle","target_hex":"6379636c65","mutable":true}],
  "working_directory":"/"})";
inline DarwinFileOptions attributeNamesOptions() {
  auto O = kernelPathConfOptions();
  O.WorkingDirectory = "/";
  O.MutableDirectories = {"/"};
  O.MovableDirectories = {"/empty"};
  O.RemovableDirectories = {"/empty"};
  for (const auto &[Path, Target] : O.SymbolicLinks)
    O.MutableSymbolicLinks.insert(Path);
  return O;
}
inline constexpr char AttributeNamesJSON[] =
    R"({"files":[{"path":"/data","bytes_hex":"30313233343536373839"}],"directories":[{"path":"/","mutable":true},{"path":"/empty","movable":true,"removable":true}],"working_directory":"/","symbolic_links":[{"path":"/alias","target_hex":"64617461","mutable":true},{"path":"/dangling","target_hex":"6d697373696e67","mutable":true},{"path":"/cycle","target_hex":"6379636c65","mutable":true}]})";
inline DarwinFileMetadata metadata(uint64_t Size = 10) {
  return {-123,
          0xfedcba9876543210ULL,
          0100644,
          3,
          0x89abcdef,
          0xfedcba98,
          Size,
          4096,
          8,
          0x1234,
          0x89abcdef,
          {INT64_MIN + 1, 1},
          {INT64_MAX, 999999999},
          {-3, 4},
          {-5, 6}};
}
// Independent raw common-attribute records, including full mode and signed
// times.
inline constexpr char ExtendedAttributesHex[] =
    "00ff410080420a757365722e6e65766572642e6265746100757365722e6e65766572642e61"
    "6c70686100757365722e6e65766572642e656d70747900";
inline DarwinFileOptions extendedAttributesOptions() {
  DarwinFileOptions O;
  O.Files["/data"] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9'};
  O.Files["/unknown"] = {};
  O.Files["/known-empty"] = {};
  O.ExtendedAttributes["/data"] = {
      {"user.neverd.beta", {0, 255, 'A', 0, 128, 'B', '\n'}},
      {"user.neverd.alpha", {'a', 'l', 'p', 'h', 'a'}},
      {"user.neverd.empty", {}}};
  O.ExtendedAttributes["/known-empty"] = {};
  O.Directories.insert("/");
  O.MutableDirectories.insert("/");
  O.WorkingDirectory = "/";
  for (const auto &[Name, Target] :
       {std::pair{"/alias", "data"}, std::pair{"/dangling", "missing"},
        std::pair{"/cycle", "cycle"}}) {
    O.SymbolicLinks[Name] =
        std::vector<uint8_t>(Target, Target + std::string_view(Target).size());
    O.MutableSymbolicLinks.insert(Name);
    O.ExtendedAttributes[Name] = {};
  }
  return O;
}
inline constexpr char ExtendedAttributesJSON[] = R"({
  "files": [
    {
      "path": "/data",
      "bytes_hex": "30313233343536373839",
      "extended_attributes": [
        {
          "name": "user.neverd.beta",
          "bytes_hex": "00ff410080420a"
        },
        {
          "name": "user.neverd.alpha",
          "bytes_hex": "616c706861"
        },
        {
          "name": "user.neverd.empty",
          "bytes_hex": ""
        }
      ]
    },
    {
      "path": "/unknown",
      "bytes_hex": ""
    },
    {
      "path": "/known-empty",
      "bytes_hex": "",
      "extended_attributes": []
    }
  ],
  "directories": [
    {
      "path": "/",
      "mutable": true
    }
  ],
  "symbolic_links": [
    {
      "path": "/alias",
      "target_hex": "64617461",
      "mutable": true,
      "extended_attributes": []
    },
    {
      "path": "/dangling",
      "target_hex": "6d697373696e67",
      "mutable": true,
      "extended_attributes": []
    },
    {
      "path": "/cycle",
      "target_hex": "6379636c65",
      "mutable": true,
      "extended_attributes": []
    }
  ],
  "working_directory": "/"
})";
inline constexpr char CommonAttributesHex[] =
    "780000000a9e07820000000000000000000000000000000085ffffff01000000"
    "fbffffffffffffff0600000000000000ffffffffffffff7fffc99a3b00000000"
    "fdffffffffffffff040000000000000001000000000000800100000000000000"
    "efcdab8998badcfea4810000341200001032547698badcfe";
inline constexpr char PlainCommonAttributesHex[] =
    "6400000085ffffff01000000fbffffffffffffff0600000000000000ffffffff"
    "ffffff7fffc99a3b00000000fdffffffffffffff040000000000000001000000"
    "000000800100000000000000efcdab8998badcfea48100003412000010325476"
    "98badcfe";
inline constexpr char CommonNameAttributesHex[] =
    "880000000b9e0782000000000000000000000000000000006800000005000000"
    "85ffffff01000000fbffffffffffffff0600000000000000ffffffffffffff7f"
    "ffc99a3b00000000fdffffffffffffff04000000000000000100000000000080"
    "0100000000000000efcdab8998badcfea4810000341200001032547698badcfe"
    "6461746100000000";
inline constexpr char PlainCommonNameAttributesHex[] =
    "74000000680000000500000085ffffff01000000fbffffffffffffff06000000"
    "00000000ffffffffffffff7fffc99a3b00000000fdffffffffffffff04000000"
    "0000000001000000000000800100000000000000efcdab8998badcfea4810000"
    "341200001032547698badcfe6461746100000000";
inline DarwinFileOptions commonAttributesOptions() {
  auto O = kernelPathConfOptions();
  O.Metadata["/data"] = metadata();
  auto Parent = metadata(0);
  Parent.Mode = 0040755;
  Parent.Inode = 41;
  O.Metadata["/"] = Parent;
  Parent.Inode = 42;
  O.Metadata["/empty"] = Parent;
  auto Link = metadata(4);
  Link.Mode = 0120777;
  Link.Inode = 123;
  O.Metadata["/alias"] = Link;
  return O;
}
inline constexpr char CommonAttributesJSON[] = R"({
  "files": [
    {
      "path": "/data",
      "bytes_hex": "30313233343536373839",
      "metadata": {
        "device": -123,
        "inode": "18364758544493064720",
        "mode": 33188,
        "link_count": 3,
        "uid": 2309737967,
        "gid": 4275878552,
        "size": 10,
        "block_size": 4096,
        "blocks": 8,
        "flags": 4660,
        "generation": 2309737967,
        "access_time": {
          "seconds": "-9223372036854775807",
          "nanoseconds": 1
        },
        "modification_time": {
          "seconds": "9223372036854775807",
          "nanoseconds": 999999999
        },
        "change_time": {
          "seconds": -3,
          "nanoseconds": 4
        },
        "birth_time": {
          "seconds": -5,
          "nanoseconds": 6
        }
      }
    }
  ],
  "directories": [
    {
      "path": "/",
      "metadata": {
        "device": -123,
        "inode": 41,
        "mode": 16877,
        "link_count": 3,
        "uid": 2309737967,
        "gid": 4275878552,
        "size": 0,
        "block_size": 4096,
        "blocks": 8,
        "flags": 4660,
        "generation": 2309737967,
        "access_time": {
          "seconds": "-9223372036854775807",
          "nanoseconds": 1
        },
        "modification_time": {
          "seconds": "9223372036854775807",
          "nanoseconds": 999999999
        },
        "change_time": {
          "seconds": -3,
          "nanoseconds": 4
        },
        "birth_time": {
          "seconds": -5,
          "nanoseconds": 6
        }
      }
    },
    {
      "path": "/empty",
      "metadata": {
        "device": -123,
        "inode": 42,
        "mode": 16877,
        "link_count": 3,
        "uid": 2309737967,
        "gid": 4275878552,
        "size": 0,
        "block_size": 4096,
        "blocks": 8,
        "flags": 4660,
        "generation": 2309737967,
        "access_time": {
          "seconds": "-9223372036854775807",
          "nanoseconds": 1
        },
        "modification_time": {
          "seconds": "9223372036854775807",
          "nanoseconds": 999999999
        },
        "change_time": {
          "seconds": -3,
          "nanoseconds": 4
        },
        "birth_time": {
          "seconds": -5,
          "nanoseconds": 6
        }
      }
    }
  ],
  "working_directory": "/empty",
  "symbolic_links": [
    {
      "path": "/alias",
      "target_hex": "64617461",
      "metadata": {
        "device": -123,
        "inode": 123,
        "mode": 41471,
        "link_count": 3,
        "uid": 2309737967,
        "gid": 4275878552,
        "size": 4,
        "block_size": 4096,
        "blocks": 8,
        "flags": 4660,
        "generation": 2309737967,
        "access_time": {
          "seconds": "-9223372036854775807",
          "nanoseconds": 1
        },
        "modification_time": {
          "seconds": "9223372036854775807",
          "nanoseconds": 999999999
        },
        "change_time": {
          "seconds": -3,
          "nanoseconds": 4
        },
        "birth_time": {
          "seconds": -5,
          "nanoseconds": 6
        }
      }
    },
    {
      "path": "/dangling",
      "target_hex": "6d697373696e67"
    },
    {
      "path": "/cycle",
      "target_hex": "6379636c65"
    }
  ]
})";
inline DarwinFileMetadata mutationMetadata(uint64_t Size = 10) {
  auto M = metadata(Size);
  M.LinkCount = 1;
  M.Flags = 0;
  M.Blocks = Size ? 8 : 0;
  return M;
}
inline constexpr DarwinFileMutationPolicy MutationPolicy{4096, {-7, 123456789}};
inline DarwinFileMetadata creationParentMetadata() {
  auto M = mutationMetadata(0);
  M.Mode = 0040755;
  M.Inode = 41;
  return M;
}
inline constexpr DarwinFileCreationPolicy CreationPolicy{
    0xfedcba9876543211ULL, 8192, 0x89abcdef, {-19, 987654321}, MutationPolicy};
inline constexpr char CreationPolicyJSON[] = R"({
  "first_inode":"18364758544493064721","block_size":8192,
  "generation":2309737967,
  "creation_time":{"seconds":-19,"nanoseconds":987654321},
  "mutation_policy":{"allocation_unit":4096,
    "mutation_time":{"seconds":-7,"nanoseconds":123456789}}})";
inline constexpr DarwinNamespaceCreationPolicy NamespacePolicy{512, 32, 7};
inline constexpr DarwinFileCreationPolicy NamespaceCreationPolicy{
    0xfedcba9876543211ULL, 8192,           0x89abcdef,
    {-19, 987654321},      MutationPolicy, NamespacePolicy};
inline constexpr char NamespaceCreationPolicyJSON[] = R"({
  "first_inode":"18364758544493064721","block_size":8192,
  "generation":2309737967,
  "creation_time":{"seconds":-19,"nanoseconds":987654321},
  "mutation_policy":{"allocation_unit":4096,
    "mutation_time":{"seconds":-7,"nanoseconds":123456789}},
  "namespace_policy":{"symbolic_link_allocation_unit":512,
    "directory_entry_size":32,"directory_blocks":7}})";
inline constexpr DarwinDirectoryMutationPolicy InitialDirectoryMutationPolicy{
    17, {-11, 321}};
inline DarwinFileMetadata initialSymbolicLinkMetadata(uint64_t Size = 6,
                                                      uint64_t Inode = 57) {
  auto M = mutationMetadata(Size);
  M.Mode = 0120777;
  M.Inode = Inode;
  return M;
}
inline constexpr DarwinSymbolicLinkMutationPolicy InitialSymbolicLinkPolicy{
    {-13, 456}};
inline constexpr char InitialSymbolicLinkPolicyJSON[] = R"({
  "mutation_time":{"seconds":-13,"nanoseconds":456}})";
inline constexpr char InitialSymbolicLinkMetadataHex[] =
    "85ffffffffa101003900000000000000efcdab8998badcfe0000000000000000"
    "01000000000000800100000000000000ffffffffffffff7fffc99a3b00000000"
    "f3ffffffffffffffc801000000000000fbffffffffffffff0600000000000000"
    "040000000000000008000000000000000010000000000000efcdab8900000000"
    "00000000000000000000000000000000";
inline constexpr char DirectoryLinkRootMetadataHex[] =
    "85ffffffffa101003900000000000000efcdab8998badcfe0000000000000000"
    "01000000000000800100000000000000ffffffffffffff7fffc99a3b00000000"
    "f3ffffffffffffffc801000000000000fbffffffffffffff0600000000000000"
    "060000000000000008000000000000000010000000000000efcdab8900000000"
    "00000000000000000000000000000000";
inline DarwinFileOptions mutableInitialSymbolicLinkOptions() {
  DarwinFileOptions O;
  O.Files["/data"] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9'};
  O.Metadata["/data"] = mutationMetadata();
  O.Metadata["/"] = creationParentMetadata();
  O.Metadata["/empty"] = creationParentMetadata();
  O.Metadata["/empty"].Inode = 42;
  O.Directories = {"/", "/empty"};
  O.MutableDirectories = O.SwapRenameDirectories = {"/"};
  O.InitialUmask = 0027;
  O.CreationPolicy = NamespaceCreationPolicy;
  O.SymbolicLinks = {{"/initial", {'d', 'a', 't', 'a'}},
                     {"/initial-dir", {'e', 'm', 'p', 't', 'y'}}};
  O.MutableSymbolicLinks = {"/initial", "/initial-dir"};
  O.Metadata["/initial"] = initialSymbolicLinkMetadata(4);
  O.SymbolicLinkMutationPolicies["/initial"] = InitialSymbolicLinkPolicy;
  return O;
}
inline DarwinFileOptions directoryLinkRootOptions() {
  DarwinFileOptions O;
  O.Directories = {"/", "/a", "/b", "/a/d", "/a/other", "/b/other"};
  O.MutableDirectories = {"/", "/a", "/b", "/a/d"};
  O.MovableDirectories = {"/a", "/b", "/a/d"};
  O.ExchangeableDirectories.insert("/a/d");
  O.SwapRenameDirectories = {"/a", "/b", "/a/d"};
  O.Files = {{"/a/target", {11}},
             {"/b/target", {22}},
             {"/a/d/c", {31}},
             {"/a/other/mark", {41}},
             {"/b/other/mark", {42}}};
  O.SymbolicLinks = {
      {"/b/l", {'t', 'a', 'r', 'g', 'e', 't'}},
      {"/b/dang", {'m', 'i', 's', 's', 'i', 'n', 'g'}},
      {"/b/dirlink", {'o', 't', 'h', 'e', 'r'}},
      {"/b/self", {'.', '.', '/', 'a', '/', 'd'}},
      {"/a/d/inside", {'.', '.', '/', 't', 'a', 'r', 'g', 'e', 't'}}};
  uint64_t Inode = 41;
  for (const auto &Name : O.Directories) {
    auto M = creationParentMetadata();
    M.Inode = Inode++;
    O.Metadata[Name] = M;
  }
  Inode = 101;
  for (const auto &[Name, Bytes] : O.Files) {
    auto M = mutationMetadata(Bytes.size());
    M.Inode = Inode++;
    O.Metadata[Name] = M;
  }
  Inode = 57;
  for (const char *Name :
       {"/b/l", "/b/dang", "/b/dirlink", "/b/self", "/a/d/inside"}) {
    O.MutableSymbolicLinks.insert(Name);
    O.Metadata[Name] =
        initialSymbolicLinkMetadata(O.SymbolicLinks.at(Name).size(), Inode++);
    O.SymbolicLinkMutationPolicies[Name] = InitialSymbolicLinkPolicy;
  }
  O.DirectoryMutationPolicies["/a/d"] = InitialDirectoryMutationPolicy;
  O.CreationPolicy = NamespaceCreationPolicy;
  O.InitialUmask = 0027;
  return O;
}
inline constexpr char InitialDirectoryMutationPolicyJSON[] = R"({
  "directory_entry_size":17,"mutation_time":{"seconds":-11,"nanoseconds":321}})";
inline constexpr char InitialDirectoryMetadataHex[] =
    "85ffffffed4105002900000000000000efcdab8998badcfe0000000000000000"
    "01000000000000800100000000000000f5ffffffffffffff4101000000000000"
    "f5ffffffffffffff4101000000000000fbffffffffffffff0600000000000000"
    "550000000000000000000000000000000010000000000000efcdab8900000000"
    "00000000000000000000000000000000";
inline constexpr DarwinDirectoryEnumerationPolicy EnumerationPolicy{1, 64, 0};
inline constexpr char EnumerationPolicyJSON[] = R"({
  "minimum_buffer_size":1,"initial_minimum_buffer_size":64,"seek_offset":0})";
inline constexpr char BulkAttributesHex[] =
    "3000000009000080000000000000000000000000000000000c00000006000000"
    "05000000616c6961730000000000000030000000090000800000000000000000"
    "00000000000000000c00000006000000050000006379636c6500000000000000"
    "3000000009000080000000000000000000000000000000000c00000009000000"
    "0500000064616e676c696e670000000030000000090000800000000000000000"
    "00000000000000000c0000000500000001000000646174610000000000000000"
    "3000000009000080000000000000000000000000000000000c00000006000000"
    "02000000656d70747900000000000000";
inline DarwinFileOptions bulkAttributesOptions() {
  auto O = kernelPathConfOptions();
  O.WorkingDirectory = "/";
  for (const char *Path : {"/", "/empty"}) {
    DarwinFileMetadata M;
    M.Inode = std::string_view(Path) == "/" ? 41 : 42;
    M.Mode = 0040755;
    M.LinkCount = 2;
    M.BlockSize = 4096;
    O.Metadata[Path] = M;
    O.DirectoryEnumerationPolicies[Path] = {1, 64, 0, true};
  }
  return O;
}
inline constexpr char BulkAttributesJSON[] = R"({
  "files": [
    {
      "path": "/data",
      "bytes_hex": "30313233343536373839"
    }
  ],
  "directories": [
    {
      "path": "/",
      "metadata": {
        "device": 0,
        "inode": 41,
        "mode": 16877,
        "link_count": 2,
        "uid": 0,
        "gid": 0,
        "size": 0,
        "block_size": 4096,
        "blocks": 0,
        "flags": 0,
        "generation": 0,
        "access_time": {
          "seconds": 0,
          "nanoseconds": 0
        },
        "modification_time": {
          "seconds": 0,
          "nanoseconds": 0
        },
        "change_time": {
          "seconds": 0,
          "nanoseconds": 0
        },
        "birth_time": {
          "seconds": 0,
          "nanoseconds": 0
        }
      },
      "enumeration_policy": {
        "minimum_buffer_size": 1,
        "initial_minimum_buffer_size": 64,
        "seek_offset": 0,
        "bulk_attributes": true
      }
    },
    {
      "path": "/empty",
      "metadata": {
        "device": 0,
        "inode": 42,
        "mode": 16877,
        "link_count": 2,
        "uid": 0,
        "gid": 0,
        "size": 0,
        "block_size": 4096,
        "blocks": 0,
        "flags": 0,
        "generation": 0,
        "access_time": {
          "seconds": 0,
          "nanoseconds": 0
        },
        "modification_time": {
          "seconds": 0,
          "nanoseconds": 0
        },
        "change_time": {
          "seconds": 0,
          "nanoseconds": 0
        },
        "birth_time": {
          "seconds": 0,
          "nanoseconds": 0
        }
      },
      "enumeration_policy": {
        "minimum_buffer_size": 1,
        "initial_minimum_buffer_size": 64,
        "seek_offset": 0,
        "bulk_attributes": true
      }
    }
  ],
  "working_directory": "/",
  "symbolic_links": [
    {
      "path": "/alias",
      "target_hex": "64617461"
    },
    {
      "path": "/dangling",
      "target_hex": "6d697373696e67"
    },
    {
      "path": "/cycle",
      "target_hex": "6379636c65"
    }
  ]
})";
// Literal LP64 records: a, its parent, d, f and l in virtual byte order.
inline constexpr char EnumerationMetadataHex[] =
    "1132547698badcfe000000000000000020000100042e00000000000000000000"
    "2900000000000000000000000000000020000200042e2e000000000000000000"
    "1332547698badcfe000000000000000020000100046400000000000000000000"
    "1432547698badcfe000000000000000020000100086600000000000000000000"
    "1532547698badcfe0000000000000000200001000a6c00000000000000000000";
inline constexpr char NamespaceMetadataHex[] =
    "85ffffffe84102001132547698badcfee803000098badcfe0000000000000000"
    "edffffffffffffffb168de3a00000000f9ffffffffffffff15cd5b0700000000"
    "f9ffffffffffffff15cd5b0700000000edffffffffffffffb168de3a00000000"
    "400000000000000007000000000000000020000000000000efcdab8900000000"
    "0000000000000000000000000000000085ffffffe8a101001232547698badcfe"
    "e803000098badcfe0000000000000000edffffffffffffffb168de3a00000000"
    "edffffffffffffffb168de3a00000000f9ffffffffffffff15cd5b0700000000"
    "edffffffffffffffb168de3a0000000004000000000000000100000000000000"
    "0020000000000000efcdab890000000000000000000000000000000000000000";
inline constexpr char CreationMetadataHex[] =
    "85ffffffe88100001132547698badcfee803000098badcfe0000000000000000"
    "edffffffffffffffb168de3a00000000f9ffffffffffffff15cd5b0700000000"
    "f9ffffffffffffff15cd5b0700000000edffffffffffffffb168de3a00000000"
    "082000000000000008000000000000000020000000000000efcdab8900000000"
    "00000000000000000000000000000000";
inline constexpr char MutationPolicyJSON[] = R"({"allocation_unit":4096,
  "mutation_time":{"seconds":-7,"nanoseconds":123456789}})";
inline constexpr char MutationMetadataHex[] =
    "85ffffffa48101001032547698badcfeefcdab8998badcfe0000000000000000"
    "01000000000000800100000000000000f9ffffffffffffff15cd5b0700000000"
    "f9ffffffffffffff15cd5b0700000000fbffffffffffffff0600000000000000"
    "012000000000000018000000000000000010000000000000efcdab8900000000"
    "00000000000000000000000000000000";
inline DarwinDirectoryContents directoryContents() {
  return {{{".", 41, 4, 11, 0, 64},
           {"..", 41, 4, 22, 0},
           {"empty", 42, 4, 7, 0},
           {"data", 0xfedcba9876543210ULL, 8, 99, 0}},
          1};
}
inline constexpr char DirectoryContentsJSON[] = R"({
  "minimum_buffer_size":1,"entries":[
    {"name":".","inode":41,"type":4,"next_offset":11,"seek_offset":0,"minimum_buffer_size":64},
    {"name":"..","inode":41,"type":4,"next_offset":22,"seek_offset":0},
    {"name":"empty","inode":42,"type":4,"next_offset":7,"seek_offset":0},
    {"name":"data","inode":"18364758544493064720","type":8,"next_offset":99,"seek_offset":0}]})";
inline constexpr char MetadataJSON[] = R"({
  "device":-123,"inode":"18364758544493064720","mode":33188,
  "link_count":3,"uid":2309737967,"gid":4275878552,"size":10,
  "block_size":4096,"blocks":8,"flags":4660,"generation":2309737967,
  "access_time":{"seconds":"-9223372036854775807","nanoseconds":1},
  "modification_time":{"seconds":"9223372036854775807","nanoseconds":999999999},
  "change_time":{"seconds":-3,"nanoseconds":4},
  "birth_time":{"seconds":-5,"nanoseconds":6}})";
inline constexpr char NonblockingDescriptorsJSON[] = R"({
  "files":[{"path":"/data","bytes_hex":"30313233343536373839","writable":true}],
  "directories":[{"path":"/"}],
  "symbolic_links":[{"path":"/fd-nonblock","target_hex":"64617461"}],
  "working_directory":"/"})";
inline DarwinFileOptions nonblockingDescriptorOptions() {
  DarwinFileOptions O;
  O.Files["/data"] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9'};
  O.WritableFiles.insert("/data");
  O.Directories.insert("/");
  O.SymbolicLinks["/fd-nonblock"] = {'d', 'a', 't', 'a'};
  O.WorkingDirectory = "/";
  return O;
}
inline DarwinFileOptions symbolicDescriptorOptions() {
  DarwinFileOptions O;
  O.Files["/data"] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9'};
  O.Directories.insert("/");
  O.MutableDirectories.insert("/");
  O.Metadata["/"] = creationParentMetadata();
  O.CreationPolicy = NamespaceCreationPolicy;
  O.InitialUmask = 0027;
  O.WorkingDirectory = "/";
  O.SymbolicLinks["/fd-attrs"] = {'d', 'a', 't', 'a'};
  O.MutableSymbolicLinks.insert("/fd-attrs");
  O.ExtendedAttributes["/fd-attrs"] = {};
  O.MutableExtendedAttributes.insert("/fd-attrs");
  return O;
}
// Independent LP64 literal: first runtime symbolic stat and OBJTYPE reply.
inline constexpr char SymbolicDescriptorHex[] =
    "85ffffffe8a101001132547698badcfee803000098badcfe0000000000000000"
    "edffffffffffffffb168de3a00000000edffffffffffffffb168de3a00000000"
    "edffffffffffffffb168de3a00000000edffffffffffffffb168de3a00000000"
    "040000000000000001000000000000000020000000000000efcdab8900000000"
    "000000000000000000000000000000000800000005000000";
inline constexpr char SymbolicDescriptorJSON[] = R"({
  "files": [
    {
      "path": "/data",
      "bytes_hex": "30313233343536373839"
    }
  ],
  "directories": [
    {
      "path": "/",
      "mutable": true,
      "metadata": {
        "device": -123,
        "inode": 41,
        "mode": 16877,
        "link_count": 1,
        "uid": 2309737967,
        "gid": 4275878552,
        "size": 0,
        "block_size": 4096,
        "blocks": 0,
        "flags": 0,
        "generation": 2309737967,
        "access_time": {
          "seconds": "-9223372036854775807",
          "nanoseconds": 1
        },
        "modification_time": {
          "seconds": "9223372036854775807",
          "nanoseconds": 999999999
        },
        "change_time": {
          "seconds": -3,
          "nanoseconds": 4
        },
        "birth_time": {
          "seconds": -5,
          "nanoseconds": 6
        }
      }
    }
  ],
  "creation_policy": {
    "first_inode": "18364758544493064721",
    "block_size": 8192,
    "generation": 2309737967,
    "creation_time": {
      "seconds": -19,
      "nanoseconds": 987654321
    },
    "mutation_policy": {
      "allocation_unit": 4096,
      "mutation_time": {
        "seconds": -7,
        "nanoseconds": 123456789
      }
    },
    "namespace_policy": {
      "symbolic_link_allocation_unit": 512,
      "directory_entry_size": 32,
      "directory_blocks": 7
    }
  },
  "umask": 23,
  "working_directory": "/",
  "symbolic_links": [
    {
      "path": "/fd-attrs",
      "target_hex": "64617461",
      "mutable": true,
      "extended_attributes": [],
      "mutable_extended_attributes": true
    }
  ]
})";
inline DarwinFileOptions hardLinksOptions() {
  auto O = kernelPathConfOptions();
  O.WorkingDirectory = "/";
  O.Directories.insert("/");
  O.MutableDirectories.insert("/");
  O.WritableFiles.insert("/data");
  O.Metadata["/data"] = mutationMetadata();
  O.MutationPolicies["/data"] = MutationPolicy;
  O.Metadata["/"] = creationParentMetadata();
  O.Files["/attributes"] = {'x'};
  O.Metadata["/alias"] = initialSymbolicLinkMetadata(4);
  O.SymbolicLinkMutationPolicies["/alias"] = InitialSymbolicLinkPolicy;
  O.CreationPolicy = NamespaceCreationPolicy;
  O.InitialUmask = 0027;
  for (const auto &[Path, Target] : O.SymbolicLinks)
    O.MutableSymbolicLinks.insert(Path);
  for (const char *Path : {"/data", "/alias", "/attributes"}) {
    O.ExtendedAttributes[Path] = {};
    O.MutableExtendedAttributes.insert(Path);
  }
  return O;
}
inline constexpr char HardLinksHex[] = "51313233343536373839";
inline constexpr char HardLinksJSON[] = R"({
  "files": [
    {
      "path": "/data",
      "bytes_hex": "30313233343536373839",
      "writable": true,
      "metadata": {
        "device": -123,
        "inode": "18364758544493064720",
        "mode": 33188,
        "link_count": 1,
        "uid": 2309737967,
        "gid": 4275878552,
        "size": 10,
        "block_size": 4096,
        "blocks": 8,
        "flags": 0,
        "generation": 2309737967,
        "access_time": {
          "seconds": "-9223372036854775807",
          "nanoseconds": 1
        },
        "modification_time": {
          "seconds": "9223372036854775807",
          "nanoseconds": 999999999
        },
        "change_time": {
          "seconds": -3,
          "nanoseconds": 4
        },
        "birth_time": {
          "seconds": -5,
          "nanoseconds": 6
        }
      },
      "mutation_policy": {
        "allocation_unit": 4096,
        "mutation_time": {
          "seconds": -7,
          "nanoseconds": 123456789
        }
      },
      "extended_attributes": [],
      "mutable_extended_attributes": true
    },
    {
      "path": "/attributes",
      "bytes_hex": "78",
      "extended_attributes": [],
      "mutable_extended_attributes": true
    }
  ],
  "directories": [
    {
      "path": "/",
      "mutable": true,
      "metadata": {
        "device": -123,
        "inode": 41,
        "mode": 16877,
        "link_count": 1,
        "uid": 2309737967,
        "gid": 4275878552,
        "size": 0,
        "block_size": 4096,
        "blocks": 0,
        "flags": 0,
        "generation": 2309737967,
        "access_time": {
          "seconds": "-9223372036854775807",
          "nanoseconds": 1
        },
        "modification_time": {
          "seconds": "9223372036854775807",
          "nanoseconds": 999999999
        },
        "change_time": {
          "seconds": -3,
          "nanoseconds": 4
        },
        "birth_time": {
          "seconds": -5,
          "nanoseconds": 6
        }
      }
    },
    {
      "path": "/empty"
    }
  ],
  "symbolic_links": [
    {
      "path": "/alias",
      "target_hex": "64617461",
      "mutable": true,
      "metadata": {
        "device": -123,
        "inode": 57,
        "mode": 41471,
        "link_count": 1,
        "uid": 2309737967,
        "gid": 4275878552,
        "size": 4,
        "block_size": 4096,
        "blocks": 8,
        "flags": 0,
        "generation": 2309737967,
        "access_time": {
          "seconds": "-9223372036854775807",
          "nanoseconds": 1
        },
        "modification_time": {
          "seconds": "9223372036854775807",
          "nanoseconds": 999999999
        },
        "change_time": {
          "seconds": -3,
          "nanoseconds": 4
        },
        "birth_time": {
          "seconds": -5,
          "nanoseconds": 6
        }
      },
      "mutation_policy": {
        "mutation_time": {
          "seconds": -13,
          "nanoseconds": 456
        }
      },
      "extended_attributes": [],
      "mutable_extended_attributes": true
    },
    {
      "path": "/dangling",
      "target_hex": "6d697373696e67",
      "mutable": true
    },
    {
      "path": "/cycle",
      "target_hex": "6379636c65",
      "mutable": true
    }
  ],
  "creation_policy": {
    "first_inode": "18364758544493064721",
    "block_size": 8192,
    "generation": 2309737967,
    "creation_time": {
      "seconds": -19,
      "nanoseconds": 987654321
    },
    "mutation_policy": {
      "allocation_unit": 4096,
      "mutation_time": {
        "seconds": -7,
        "nanoseconds": 123456789
      }
    },
    "namespace_policy": {
      "symbolic_link_allocation_unit": 512,
      "directory_entry_size": 32,
      "directory_blocks": 7
    }
  },
  "umask": 23,
  "working_directory": "/"
})";
inline DarwinFileOptions symbolicLinkOptions() {
  DarwinFileOptions O;
  O.Files["/data"] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9'};
  O.Metadata["/data"] = metadata();
  O.Directories.insert("/empty");
  O.WorkingDirectory = "/";
  O.SymbolicLinks = {{"/link", {'d', 'a', 't', 'a'}},
                     {"/chain", {'l', 'i', 'n', 'k'}},
                     {"/dangling", {'m', 'i', 's', 's', 'i', 'n', 'g'}},
                     {"/cycle", {'c', 'y', 'c', 'l', 'e'}},
                     {"/dirlink", {'e', 'm', 'p', 't', 'y'}}};
  auto &M = O.Metadata["/link"];
  M = metadata(4);
  M.Mode = 0120777;
  M.Inode = 123;
  return O;
}
// Public consumers construct this same explicit catalogue; snapshots are
// omitted because the five new names require their own complete observation.
inline constexpr char SymbolicLinksJSON[] = R"([
  {"path":"/link","target_hex":"64617461"},
  {"path":"/chain","target_hex":"6c696e6b"},
  {"path":"/dangling","target_hex":"6d697373696e67"},
  {"path":"/cycle","target_hex":"6379636c65"},
  {"path":"/dirlink","target_hex":"656d707479"}])";
inline DarwinFileOptions mixedSymbolicLinkOptions() {
  DarwinFileOptions O;
  O.Files["/work/data"] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9'};
  O.Metadata["/work/data"] = mutationMetadata();
  O.WritableFiles.insert("/work/data");
  O.MutationPolicies["/work/data"] = MutationPolicy;
  O.Directories = {"/static", "/work"};
  O.MutableDirectories.insert("/work");
  O.Metadata["/work"] = creationParentMetadata();
  O.CreationPolicy = CreationPolicy;
  O.InitialUmask = 0027;
  O.WorkingDirectory = "/";
  O.SymbolicLinks = {
      {"/static/alias", {'.', '.', '/', 'w', 'o', 'r', 'k'}},
      {"/static/data-link",
       {'.', '.', '/', 'w', 'o', 'r', 'k', '/', 'd', 'a', 't', 'a'}},
      {"/static/missing-link",
       {'.', '.', '/', 'w', 'o', 'r', 'k', '/', 'n', 'e', 'w'}}};
  auto &M = O.Metadata["/static/data-link"];
  M = mutationMetadata(12);
  M.Mode = 0120777;
  M.Inode = 123;
  return O;
}
inline constexpr char MixedSymbolicLinksJSON[] = R"([
  {"path":"/static/alias","target_hex":"2e2e2f776f726b"},
  {"path":"/static/data-link","target_hex":"2e2e2f776f726b2f64617461"},
  {"path":"/static/missing-link","target_hex":"2e2e2f776f726b2f6e6577"}])";
} // namespace neverd::emulation::darwin_test
#endif
