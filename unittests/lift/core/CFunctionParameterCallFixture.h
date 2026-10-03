#ifndef NEVERD_TEST_CFUNCTIONPARAMETERCALLFIXTURE_H
#define NEVERD_TEST_CFUNCTIONPARAMETERCALLFIXTURE_H

#include "RuntimeFunctionAddressFixture.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/loader/MachO/CFunctionParameterCalls.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/Support/Endian.h"

#include <iterator>
#include <stdexcept>

namespace c_function_parameter_test {
using namespace neverd;
constexpr va_t Entry = 0x1000;
constexpr va_t Veneer = 0x1100;
constexpr va_t Call = Entry + 28;

struct Fixture {
  BinaryImage Image = runtime_function_address_test::image(Arch::AArch64);
  SourceFunctionTypeHint Signature;
  llvm::LLVMContext Context;
  PipelineResult Result;
  va_t CallAddress = Call;

  void word(unsigned Index, uint32_t Value) {
    llvm::support::endian::write32le(Image.Segments[0].Data.data() + Index * 4,
                                     Value);
  }
  Fixture(bool Diamond = false, bool SameCallback = true,
          bool SharedStoreTail = false) {
    Image.Entry = Entry;
    Segment Text;
    Text.VA = Entry;
    Text.Size = Text.FileSz = 0x200;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.resize(Text.Size);
    Image.Segments.insert(Image.Segments.begin(), std::move(Text));
    Section Code;
    Code.Name = "__text";
    Code.VA = Entry;
    Code.Size = Code.FileSz = 0x200;
    Code.Flags = Image.Segments[0].Flags;
    Code.Type = llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
    Image.Sections.push_back(Code);
    Image.Symbols = {{"_invoke_callback", Entry, Diamond ? 56u : 44u, true},
                     {"_swift_release_veneer", Veneer, 12, true}};
    // Save both arguments, cross one catalogued ordinary C runtime call,
    // invoke the unchanged callback, and restore the callee-saved registers.
    std::vector<uint32_t> Words{0xa9be53f3, 0xa9017bfd, 0x910043fd, 0xaa0103f3,
                                0xaa0003f4};
    if (Diamond) {
      Words.insert(Words.end(), {0xb4000062, 0x9400003a, 0x14000002,
                                 SameCallback ? 0xaa0103f3u : 0xaa0003f3u});
      CallAddress = Entry + 40;
    } else
      Words.push_back(0x9400003b);
    Words.insert(Words.end(),
                 {0xaa1403e0, 0xd63f0260, 0xa9417bfd, 0xa8c253f3, 0xd65f03c0});
    if (SharedStoreTail) {
      // Three paths write different first words and converge on one store,
      // callback and return. The callback proof belongs to this one BLR.
      Words = {0xa9be53f3, 0xa9017bf5, 0xaa0003f3, 0xaa0103f4, 0xb40000a2,
               0xb40000e3, 0xd2800168, 0xf9000268, 0x14000006, 0xd28002c8,
               0xf9000268, 0x14000003, 0xd2800428, 0xf9000268, 0xf900067f,
               0xaa1303e0, 0xd63f0280, 0xa9417bf5, 0xa8c253f3, 0xd65f03c0};
      Image.Symbols[0].Name = "_shared_tail_callback";
      Image.Symbols[0].Size = Words.size() * 4;
      CallAddress = Entry + 64;
    }
    for (unsigned I = 0; I < std::size(Words); ++I)
      word(I, Words[I]);
    word((Veneer - Entry) / 4, 0xb0000010);
    word((Veneer - Entry) / 4 + 1, 0xf9400210);
    word((Veneer - Entry) / 4 + 2, 0xd61f0200);
    Signature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    Signature.ReturnType = NdType::makeVoid();
    const auto Pointer = NdType::makePtr(NdType::makeVoid());
    Signature.Parameters = {{"value", Pointer},
                            {"callback", NdType::makePtr(NdType::makeFunc(
                                             NdType::makeVoid(), {Pointer}))}};
    if (SharedStoreTail) {
      Signature.Parameters.push_back({"first", NdType::makeInt(8, false)});
      Signature.Parameters.push_back({"second", NdType::makeInt(8, false)});
    } else if (Diamond)
      Signature.Parameters.push_back({"branch", NdType::makeInt(8, false)});
    std::string Error;
    if (!assignDarwinScalarSourceABI(Signature, Arch::AArch64, Error))
      throw std::runtime_error(Error);
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    Options.OnlyFunctionEntries = {Entry, Veneer};
    Options.SourceTypeHints.emplace(Entry, Signature);
    Result = Pipeline().run(Image, Context, Options);
  }
  const LowFunc *low() const {
    for (const auto &Function : Result.LowFuncs)
      if (Function.Entry == Entry)
        return &Function;
    return nullptr;
  }
  const HighFunc *high() const {
    for (const auto &Function : Result.HighFuncs)
      if (Function.Entry == Entry)
        return &Function;
    return nullptr;
  }
};
} // namespace c_function_parameter_test
#endif
