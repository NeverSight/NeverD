//===- X87PortableTestImage.h - Owned x64 portable fixtures ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authored machine bytes through normal decoding and ABI-recovered MedIR.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_TRANSLATE_X86_X87PORTABLETESTIMAGE_H
#define NEVERD_UNITTESTS_TRANSLATE_X86_X87PORTABLETESTIMAGE_H

#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/Error.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <utility>
#include <vector>

namespace neverd::test {

inline constexpr uint64_t kPortableX87FixtureEntry = 0x1000;

inline std::vector<uint8_t> portableX87LeafBytes() {
  // mov rax,0; fld80 [rdi]; fld80 [rsi]; faddp; fstp80 [rdx]; fnstsw ax; ret
  return {0x48, 0xc7, 0xc0, 0,    0,    0,    0,    0xdb, 0x2f,
          0xdb, 0x2e, 0xde, 0xc1, 0xdb, 0x3a, 0xdf, 0xe0, 0xc3};
}

inline std::vector<uint8_t> portableX87BranchBytes() {
  auto Bytes = portableX87LeafBytes();
  Bytes.pop_back();
  // test rcx,rcx; jz second_return; add rax,1; ret; second_return: ret
  const uint8_t Branch[] = {0x48, 0x85, 0xc9, 0x74, 0x05, 0x48,
                            0x83, 0xc0, 0x01, 0xc3, 0xc3};
  Bytes.insert(Bytes.end(), std::begin(Branch), std::end(Branch));
  return Bytes;
}

inline std::vector<uint8_t> portableX87LoopBytes() {
  auto Bytes = portableX87LeafBytes();
  // After faddp: loop: fchs; dec rcx; jnz loop. The x87 context persists.
  const uint8_t Loop[] = {0xd9, 0xe0, 0x48, 0xff, 0xc9, 0x75, 0xf9};
  Bytes.insert(Bytes.begin() + 13, std::begin(Loop), std::end(Loop));
  return Bytes;
}

inline llvm::Expected<MedFunc>
liftPortableX87Fixture(llvm::ArrayRef<uint8_t> Bytes, bool NoOpt = false) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  Image.Base = Image.Entry = kPortableX87FixtureEntry;
  Image.X87ContextOverride = true;
  Segment Text;
  Text.Name = ".text";
  Text.VA = Image.Entry;
  Text.Size = Text.FileSz = Bytes.size();
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Bytes.begin(), Bytes.end());
  Image.Segments.push_back(Text);
  Section Code;
  Code.Name = Text.Name;
  Code.VA = Text.VA;
  Code.Size = Code.FileSz = Text.Size;
  Code.Flags = Text.Flags;
  Code.Data = Text.Data;
  Image.Sections.push_back(std::move(Code));
  auto Symbol = Symbol::makeFunc(Image.Entry, Bytes.size());
  Symbol.Name = "portable_x87_source";
  Image.Symbols.push_back(std::move(Symbol));
  llvm::LLVMContext Context;
  PipelineOptions Options;
  Options.MedAnalysisOnly = true;
  Options.NoOpt = NoOpt;
  Options.EmitDumpOutput = false;
  Options.OnlyFunctionEntries = {Image.Entry};
  auto Result = Pipeline().run(Image, Context, Options);
  if (!Result.Success)
    return llvm::createStringError(llvm::errc::invalid_argument, Result.Error);
  auto Found = std::find_if(
      Result.MedFuncs.begin(), Result.MedFuncs.end(),
      [&](const MedFunc &Function) { return Function.Entry == Image.Entry; });
  if (Found == Result.MedFuncs.end())
    return llvm::createStringError(llvm::errc::invalid_argument,
                                   "fixture has no recovered MedIR entry");
  return std::move(*Found);
}

} // namespace neverd::test

#endif // NEVERD_UNITTESTS_TRANSLATE_X86_X87PORTABLETESTIMAGE_H
