//===- COFFOrdinalImportTests.cpp - Exact ordinal import names -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Checks the module and target scope of the SDK's D3D12 ordinal contract.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFLoaderUtils.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/Object/COFF.h"

#include <cstring>
#include <initializer_list>
#include <string>
#include <utility>

namespace {
using namespace neverd;

TEST(COFFOrdinalImports, DelayImportsKeepTheModuleOrdinalAndExactSlot) {
  for (Arch Architecture : {Arch::X86, Arch::X64, Arch::AArch64, Arch::ARM}) {
    for (unsigned Mode = 0; Mode != 5; ++Mode) {
      SCOPED_TRACE(static_cast<unsigned>(Architecture));
      SCOPED_TRACE(Mode);
      const bool Wide =
          Architecture == Arch::X64 || Architecture == Arch::AArch64;
      BinaryImage Image;
      Image.Arch = Architecture;
      Image.Format = BinaryFormat::COFF;
      Image.Bits = Wide ? Bitness::Bits64 : Bitness::Bits32;
      Image.Base = 0x400000;
      Segment Data;
      Data.VA = Image.Base + 0x1000;
      Data.Size = 0x300;
      Data.FileSz = Data.Size;
      Data.Flags = SegmentFlags::Readable;
      Data.Data.resize(Data.Size);
      const char *Module = Mode == 1   ? "D3D12.DLL"
                           : Mode == 2 ? "example.dll"
                                       : "d3d12.dll";
      std::memcpy(Data.Data.data(), Module, std::strlen(Module) + 1);
      const uint16_t Ordinal = Mode == 3 ? 100 : 101;
      writePtr(Data.Data.data() + 0x200,
               (uint64_t{1} << (Wide ? 63 : 31)) | Ordinal |
                   (Mode == 4 ? 0x10000 : 0),
               Wide);
      Image.Segments.push_back(std::move(Data));
      llvm::object::delay_import_directory_table_entry Descriptor{};
      Descriptor.Attributes = 1;
      Descriptor.Name = 0x1000;
      Descriptor.DelayImportAddressTable = 0x1100;
      Descriptor.DelayImportNameTable = 0x1200;
      if (Mode == 4) {
        EXPECT_EQ(coff_loader::parseDelayImportDescriptor(Descriptor, Image),
                  0u);
        EXPECT_TRUE(Image.Imports.empty());
        EXPECT_TRUE(Image.ImportStorageSlots.empty());
        continue;
      }
      ASSERT_EQ(coff_loader::parseDelayImportDescriptor(Descriptor, Image), 1u);
      ASSERT_EQ(Image.Imports.size(), 1u);
      const Import &Imported = Image.Imports.front();
      const bool Known = Architecture != Arch::ARM && Mode < 2;
      EXPECT_EQ(Imported.Name, Known       ? "D3D12CreateDevice"
                               : Mode == 3 ? "ord_100"
                                           : "ord_101");
      EXPECT_EQ(Imported.Ordinal, Ordinal);
      EXPECT_EQ(Imported.Module, std::string(Module) + " [delay]");
      EXPECT_TRUE(Imported.IsDelayImport);
      EXPECT_EQ(Imported.IATAddr, 0x401100u);
      EXPECT_EQ(Image.ImportStorageSlots.at(0x401100).Name, Imported.Name);
    }
  }
}
} // namespace
