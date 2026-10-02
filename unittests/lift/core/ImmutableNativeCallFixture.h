#ifndef NEVERD_TEST_IMMUTABLENATIVECALLFIXTURE_H
#define NEVERD_TEST_IMMUTABLENATIVECALLFIXTURE_H

#include "RuntimeFunctionAddressFixture.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/loader/MachO/ImmutableNativeCalls.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/Support/Endian.h"

#include <iterator>
#include <stdexcept>

namespace immutable_native_call_test {
using namespace neverd;
constexpr va_t Entry = 0x1000, Veneer = 0x1100, Target = 0x1200;
constexpr va_t Table = 0x3000, Slot = Table + 0x28, Call = Entry + 28;
struct Fixture {
  BinaryImage Image = runtime_function_address_test::image(Arch::AArch64);
  SourceFunctionTypeHint Signature, EntrySignature;
  llvm::LLVMContext Context;
  PipelineResult Result;

  void word(unsigned Index, uint32_t Value) {
    llvm::support::endian::write32le(Image.Segments[0].Data.data() + Index * 4,
                                     Value);
  }
  Fixture() {
    Image.Entry = Entry;
    Image.MachOHasChainedFixups = true;
    Segment Text;
    Text.VA = Entry;
    Text.Size = Text.FileSz = 0x300;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.resize(Text.Size);
    Image.Segments.insert(Image.Segments.begin(), std::move(Text));
    Section Code;
    Code.Name = "__text";
    Code.VA = Entry;
    Code.Size = Code.FileSz = 0x300;
    Code.Flags = Image.Segments[0].Flags;
    Code.Type = llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
    Image.Sections.push_back(Code);
    Segment Data;
    Data.VA = Table;
    Data.FileOff = 0x400;
    Data.Size = Data.FileSz = 0x100;
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Data.ReadOnlyAfterRelocations = true;
    Data.Data.resize(Data.Size);
    llvm::support::endian::write64le(Data.Data.data() + 0x28, Target);
    Image.Segments.push_back(std::move(Data));
    Section Pointers;
    Pointers.Name = "__const";
    Pointers.VA = Table;
    Pointers.FileOff = 0x400;
    Pointers.Size = Pointers.FileSz = 0x100;
    Pointers.Flags = Image.Segments.back().Flags;
    Image.Sections.push_back(Pointers);
    Image.CodePtrRelocSlots.insert(Slot);
    Image.MachOResolvedChainedPointerSlots.insert(Slot);
    Image.Symbols = {{"_indirect_native", Entry, 44, true},
                     {"_swift_release_veneer", Veneer, 12, true},
                     {"_constant_result", Target, 8, true}};
    // Save registers, materialize a read-only table, load a complete code
    // pointer, cross the real swift_release import, then invoke that pointer.
    const uint32_t Words[] = {0xa9be53f3, 0xa9017bfd, 0x910043fd, 0xd0000013,
                              0x91008273, 0xf9400674, 0x9400003a, 0xd63f0280,
                              0xa9417bfd, 0xa8c253f3, 0xd65f03c0};
    for (unsigned I = 0; I < std::size(Words); ++I)
      word(I, Words[I]);
    word((Veneer - Entry) / 4, 0xb0000010);
    word((Veneer - Entry) / 4 + 1, 0xf9400210);
    word((Veneer - Entry) / 4 + 2, 0xd61f0200);
    word((Target - Entry) / 4, 0xd2800540);
    word((Target - Entry) / 4 + 1, 0xd65f03c0);
    Signature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    Signature.ReturnType = NdType::makeInt(8, false);
    std::string Error;
    if (!assignDarwinScalarSourceABI(Signature, Arch::AArch64, Error))
      throw std::runtime_error(Error);
    run();
  }
  void run() {
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    Options.OnlyFunctionEntries = {Entry, Veneer, Target};
    Options.SourceTypeHints.emplace(Target, Signature);
    if (EntrySignature.ReturnType)
      Options.SourceTypeHints.emplace(Entry, EntrySignature);
    Result = Pipeline().run(Image, Context, Options);
  }
  void bindCaller() {
    EntrySignature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    EntrySignature.ReturnType = NdType::makeInt(8, false);
    EntrySignature.Parameters = {
        {"object", NdType::makePtr(NdType::makeVoid())}};
    std::string Error;
    if (!assignDarwinScalarSourceABI(EntrySignature, Arch::AArch64, Error))
      throw std::runtime_error(Error);
    run();
  }
  HighFunc *high() {
    for (auto &Function : Result.HighFuncs)
      if (Function.Entry == Entry)
        return &Function;
    return nullptr;
  }
  MedFunc *med() {
    for (auto &Function : Result.MedFuncs)
      if (Function.Entry == Entry)
        return &Function;
    return nullptr;
  }
  const LowFunc *low() const {
    for (const auto &Function : Result.LowFuncs)
      if (Function.Entry == Entry)
        return &Function;
    return nullptr;
  }
};
} // namespace immutable_native_call_test
#endif
