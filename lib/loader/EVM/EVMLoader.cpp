//===- EVMLoader.cpp - Ethereum Virtual Machine bytecode loader ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/EVM/EVMLoader.h"

#include "neverd/Common.h"
#include "neverd/evm/Bytecode.h"
#include "neverd/evm/EVMConstants.h"

namespace neverd {

llvm::Expected<BinaryImage> EVMLoader::load(const std::filesystem::path &Path) {
  auto Loaded = evm::loadBytecodeFile(Path);
  if (!Loaded)
    return Loaded.takeError();

  BinaryImage Image;
  Image.Arch = Arch::EVM;
  Image.Format = BinaryFormat::EVM;
  Image.Bits = Bitness::Bits256;
  Image.Base = evm::kEntryPC;
  Image.Entry = evm::kEntryPC;
  Image.Raw = Loaded->Code;

  const auto Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Segment CodeSegment;
  CodeSegment.Name = kEVMCodeSegmentName.str();
  CodeSegment.VA = evm::kEntryPC;
  CodeSegment.Size = Loaded->Code.size();
  CodeSegment.FileSz = Loaded->Code.size();
  CodeSegment.Flags = Flags;
  CodeSegment.Data = Loaded->Code;
  Image.Segments.push_back(std::move(CodeSegment));

  Section CodeSection;
  CodeSection.Name = kEVMCodeSectionName.str();
  CodeSection.SegmentName = kEVMCodeSegmentName.str();
  CodeSection.VA = evm::kEntryPC;
  CodeSection.Size = Loaded->Code.size();
  CodeSection.FileSz = Loaded->Code.size();
  CodeSection.Flags = Flags;
  CodeSection.Alignment = evm::kCodeAlignment;
  CodeSection.Data = Loaded->Code;
  Image.Sections.push_back(std::move(CodeSection));

  Image.addSymbol(kEVMEntrySymbolName.str(), evm::kEntryPC, Loaded->Code.size(),
                  true);
  return Image;
}

} // namespace neverd
