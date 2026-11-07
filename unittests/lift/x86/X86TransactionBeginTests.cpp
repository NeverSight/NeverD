//===- X86TransactionBeginTests.cpp - Signed abort continuations --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Decode targets and controlled status completions independently of physical
/// RTM availability. Compile the complete two-exit region through all routes.
//===----------------------------------------------------------------------===//

#include "../x86_64/X86FPStateAccuracyFixture.h"
#include "X86TransactionTestCases.h"

#include "neverd/decode/X86/X86TransactionEncoding.h"
#include "neverd/lift/X86/X86Lifter.h"

#include "llvm/Object/ObjectFile.h"

#include <array>
#include <functional>

namespace {
std::vector<uint8_t> beginBytes(std::vector<uint8_t> Prefix, unsigned Width,
                                uint32_t Relative) {
  Prefix.insert(Prefix.end(), {0xc7, 0xf8});
  for (unsigned I = 0; I < Width; ++I)
    Prefix.push_back(Relative >> (I * 8));
  return Prefix;
}

int decode(Decoder &D, unsigned Route, const std::vector<uint8_t> &Bytes,
           va_t Address, DecodedInsn &I) {
  return Route == 0 ? D.decodeOne(Bytes.data(), Bytes.size(), Address, I)
         : Route == 1
             ? D.decodeOneLight(Bytes.data(), Bytes.size(), Address, I)
             : D.decodeOneForLift(Bytes.data(), Bytes.size(), Address, I);
}

TEST(X86TransactionEncoding, ControlsShareExactPrefixAndImmediateOwnership) {
  unsigned Cases = 0, Refusals = 0;
  for (Arch A : {Arch::X86, Arch::X64}) {
    Decoder D;
    ASSERT_TRUE(D.init(A));
    auto Prefixes = x86TransactionTestPrefixes(A == Arch::X64);
    Prefixes.push_back({std::vector<uint8_t>(12, 0x66), 2});
    Prefixes.push_back({std::vector<uint8_t>(12, 0x67), 4});
    for (const auto &[Prefix, Width] : Prefixes)
      for (unsigned Code = 0; Code < 258; ++Code) {
        auto Bytes = Prefix;
        const unsigned ID = Code < 256    ? X86_INS_XABORT
                            : Code == 256 ? X86_INS_XEND
                                          : X86_INS_XTEST;
        if (Code < 256)
          Bytes.insert(Bytes.end(), {0xc6, 0xf8, uint8_t(Code)});
        else
          Bytes.insert(Bytes.end(),
                       {0x0f, 0x01, uint8_t(Code == 256 ? 0xd5 : 0xd6)});
        SCOPED_TRACE(testing::PrintToString(Bytes));
        const auto E = x86TransactionEncoding(Bytes, 0x1000, A == Arch::X64);
        const bool Valid = Code < 256 || std::none_of(
                                             Prefix.begin(), Prefix.end(),
                                             [](uint8_t Byte) {
                                               return Byte == 0x66 ||
                                                      Byte == 0xf2 ||
                                                      Byte == 0xf3;
                                             });
        ASSERT_EQ(E.has_value(), Valid);
        if (E)
          EXPECT_EQ(E->OpcodeOffset, Prefix.size());
        if (Code < 256)
          EXPECT_EQ(E->AbortCode, Code);
        for (unsigned Route = 0; Route < 3; ++Route) {
          DecodedInsn I{};
          ASSERT_EQ(decode(D, Route, Bytes, 0x1000, I), Bytes.size());
          ASSERT_EQ(I.Id, ID);
          if (E)
            ASSERT_TRUE(x86TransactionControlDetailMatches(I.Raw->detail->x86,
                                                           *E, A == Arch::X64));
          for (bool Strict : {false, true}) {
            D.setStrict(Strict);
            std::vector<LowOp> Ops;
            if (!Valid) {
              EXPECT_THROW(D.liftToLow(I, Ops), UnliftedInstruction);
              EXPECT_TRUE(Ops.empty());
              ++Refusals;
            } else {
              ASSERT_NO_THROW(D.liftToLow(I, Ops));
              ASSERT_FALSE(Ops.empty());
              if (Code < 256)
                EXPECT_EQ(Ops.front().Inputs[1].Offset, Code);
              ++Cases;
            }
          }
        }
        // None of these instructions has an optional/truncated final byte.
        auto Invalid = Bytes;
        Invalid.pop_back();
        EXPECT_FALSE(x86TransactionEncoding(Invalid, 0x1000, A == Arch::X64));
        Invalid = Bytes;
        Invalid.insert(Invalid.begin(), 0xf0);
        EXPECT_FALSE(x86TransactionEncoding(Invalid, 0x1000, A == Arch::X64));
        Invalid = Bytes;
        Invalid.push_back(0x90);
        EXPECT_FALSE(x86TransactionEncoding(Invalid, 0x1000, A == Arch::X64));
      }
  }
  RecordProperty("control_decoder_lift_checks", Cases);
  RecordProperty("invalid_np_prefix_refusals", Refusals);
}

TEST(X86TransactionBegin, EveryRel16FieldRetainsItsSignedFullWidthTarget) {
  unsigned Cases = 0;
  constexpr uint64_t Address = 0x12345000;
  for (Arch A : {Arch::X86, Arch::X64})
    for (bool AddressOverride : {false, true})
      for (bool Strict : {false, true}) {
        Decoder D;
        ASSERT_TRUE(D.init(A));
        D.setStrict(Strict);
        for (unsigned Bits = 0; Bits <= UINT16_MAX; ++Bits) {
          const auto Bytes =
              beginBytes(AddressOverride ? std::vector<uint8_t>{0x67, 0x66}
                                         : std::vector<uint8_t>{0x66},
                         2, Bits);
          const int64_t Signed = Bits < 0x8000 ? Bits : int64_t(Bits) - 0x10000;
          const uint64_t Expected = Address + Bytes.size() + Signed;
          for (unsigned Route = 0; Route < 3; ++Route) {
            DecodedInsn I{};
            ASSERT_EQ(decode(D, Route, Bytes, Address, I), Bytes.size());
            ASSERT_EQ(I.Id, X86_INS_XBEGIN);
            ASSERT_EQ(uint64_t(I.Raw->detail->x86.operands[0].imm), Expected);
            std::vector<LowOp> Ops;
            ASSERT_NO_THROW(D.liftToLow(I, Ops));
            ASSERT_FALSE(Ops.empty());
            ASSERT_EQ(Ops.back().Opcode, NdOp::COND_BR);
            ASSERT_EQ(Ops.back().Inputs[0].Offset, Expected);
            ++Cases;
          }
        }
      }
  EXPECT_EQ(Cases, 1572864u);
  RecordProperty("rel16_decoder_lift_checks", Cases);
}

TEST(X86TransactionBegin, PrefixOrderAndCodeWidthDetermineFallback) {
  unsigned Cases = 0;
  for (Arch A : {Arch::X86, Arch::X64}) {
    std::vector<std::pair<std::vector<uint8_t>, unsigned>> Prefixes = {
        {{}, 4},           {{0x66}, 2},       {{0x67}, 4},
        {{0x67, 0x66}, 2}, {{0xf2, 0x66}, 2}, {{0xf3, 0x66}, 2},
        {{0x2e, 0x66}, 2}, {{0x66, 0x66}, 2}, {{0x65, 0x66, 0x67}, 2}};
    if (A == Arch::X64)
      Prefixes.insert(Prefixes.end(), {{{0x48}, 4},
                                       {{0x66, 0x48}, 4},
                                       {{0x48, 0x66}, 2},
                                       {{0x66, 0x4f}, 4},
                                       {{0x66, 0x4f, 0x40}, 2}});
    Decoder D;
    ASSERT_TRUE(D.init(A));
    for (const auto &[Prefix, Width] : Prefixes)
      for (uint64_t Address : {UINT64_C(0x10000), UINT64_C(0xfffffffc),
                               UINT64_C(0xffffffff80001000)})
        for (uint32_t Bits : {0u, 1u, 0x7fffu, 0x8000u, 0xffffu, 0x7fffffffu,
                              0x80000000u, 0xffffffffu})
          for (unsigned Route = 0; Route < 3; ++Route) {
            const auto Bytes = beginBytes(Prefix, Width, Bits);
            const int64_t Relative =
                Width == 2 ? int64_t(int16_t(Bits)) : int64_t(int32_t(Bits));
            const uint64_t Expected =
                (Address + Bytes.size() + uint64_t(Relative)) &
                (A == Arch::X64 ? UINT64_MAX : UINT32_MAX);
            SCOPED_TRACE(testing::Message()
                         << unsigned(A) << " " << Address << " "
                         << testing::PrintToString(Bytes));
            DecodedInsn I{};
            ASSERT_EQ(decode(D, Route, Bytes, Address, I), Bytes.size());
            ASSERT_EQ(uint64_t(I.Raw->detail->x86.operands[0].imm), Expected);
            std::vector<LowOp> Ops;
            ASSERT_NO_THROW(D.liftToLow(I, Ops));
            ASSERT_EQ(Ops.back().Inputs[0].Offset, Expected);
            ++Cases;
          }
  }
  RecordProperty("prefix_wrap_decoder_lift_checks", Cases);
}

TEST(X86TransactionBegin, ContradictoryDetailCannotPublishAnExit) {
  using Mutation = std::function<void(cs_insn &)>;
  const std::vector<Mutation> Mutations = {
      [](auto &I) { I.bytes[I.size - 1] ^= 1; },
      [](auto &I) { I.detail->x86.operands[0].imm++; },
      [](auto &I) { I.detail->x86.operands[0].size = 1; },
      [](auto &I) { I.detail->x86.op_count = 0; },
      [](auto &I) { I.detail->x86.encoding.imm_size = 1; },
      [](auto &I) { I.detail->x86.encoding.imm_offset++; },
      [](auto &I) { I.detail->x86.encoding.modrm_offset++; },
      [](auto &I) { I.detail->x86.encoding.disp_size = 1; },
      [](auto &I) { I.detail->x86.opcode[2] = 1; },
      [](auto &I) { I.detail->x86.rex2 = 1; },
      [](auto &I) { I.detail->x86.eflags = 1; },
      [](auto &I) { I.bytes[0] = 0xf0; }};
  for (Arch A : {Arch::X86, Arch::X64})
    for (bool Short : {false, true})
      for (bool Strict : {false, true})
        for (const auto &Mutate : Mutations) {
          Decoder D;
          ASSERT_TRUE(D.init(A));
          D.setStrict(Strict);
          const auto Bytes = beginBytes(Short ? std::vector<uint8_t>{0x66}
                                              : std::vector<uint8_t>{},
                                        Short ? 2 : 4, UINT32_MAX);
          DecodedInsn I{};
          ASSERT_EQ(decode(D, 2, Bytes, 0x12345000, I), Bytes.size());
          Mutate(*I.Raw);
          std::vector<LowOp> Ops;
          EXPECT_THROW(D.liftToLow(I, Ops), UnliftedInstruction);
          EXPECT_TRUE(Ops.empty());
        }
}

TEST(X86TransactionBegin, ControlledStatusPreservesSuccessEaxAndFlags) {
  for (Arch A : {Arch::X86, Arch::X64})
    for (bool Short : {false, true}) {
      Decoder D;
      ASSERT_TRUE(D.init(A));
      const auto Bytes = beginBytes(Short ? std::vector<uint8_t>{0x66}
                                          : std::vector<uint8_t>{},
                                    Short ? 2 : 4, UINT32_MAX);
      DecodedInsn I{};
      ASSERT_EQ(decode(D, 2, Bytes, 0x12345000, I), Bytes.size());
      std::vector<LowOp> Ops;
      ASSERT_NO_THROW(D.liftToLow(I, Ops));
      ASSERT_EQ(Ops.front().Opcode, NdOp::INTRINSIC);
      for (uint32_t Status : {0u, 1u, 0x42000001u, UINT32_MAX}) {
        BinaryImage Image;
        Image.Arch = A;
        NdOpEmulator Emu(Image);
        constexpr uint64_t Incoming = UINT64_C(0x1234567887654321);
        const auto Initial = A == Arch::X64 ? Incoming : Incoming & UINT32_MAX;
        Emu.setRegister(x86reg::RAX, Initial);
        for (auto Flag : {x86reg::CF, x86reg::PF, x86reg::AF, x86reg::ZF,
                          x86reg::SF, x86reg::OF, x86reg::DF})
          Emu.setRegister(Flag, 1);
        // Supply an independently controlled completion, never execute RTM.
        Emu.setRegister(Ops.front().Output.Offset, Status);
        for (unsigned N = 1; N + 1 < Ops.size(); ++N)
          ASSERT_TRUE(Emu.step(Ops[N]));
        EXPECT_EQ(Emu.getRegister(x86reg::RAX),
                  Status == UINT32_MAX ? Initial : Status);
        EXPECT_EQ(Emu.getRegister(Ops.back().Inputs[1].Offset),
                  Status != UINT32_MAX);
        for (auto Flag : {x86reg::CF, x86reg::PF, x86reg::AF, x86reg::ZF,
                          x86reg::SF, x86reg::OF, x86reg::DF})
          EXPECT_EQ(Emu.getRegister(Flag), 1);
      }
    }
}

class X86TransactionBeginSource : public X86FPStateFixture {
protected:
  void checkObject(llvm::StringRef Data, Arch A) {
    auto Obj = llvm::object::ObjectFile::createObjectFile(
        llvm::MemoryBufferRef(Data, "transaction.o"));
    ASSERT_TRUE(bool(Obj)) << llvm::toString(Obj.takeError());
    Decoder D;
    ASSERT_TRUE(D.init(A));
    unsigned Begins = 0, Ends = 0;
    for (const auto &Section : (*Obj)->sections()) {
      if (!Section.isText())
        continue;
      auto Text = Section.getContents();
      ASSERT_TRUE(bool(Text)) << llvm::toString(Text.takeError());
      for (size_t Offset = 0; Offset < Text->size();) {
        DecodedInsn I{};
        const auto Size = D.decodeOneForLift(
            reinterpret_cast<const uint8_t *>(Text->data() + Offset),
            Text->size() - Offset, Entry + Offset, I);
        ASSERT_GT(Size, 0);
        Begins += I.Id == X86_INS_XBEGIN;
        Ends += I.Id == X86_INS_XEND;
        Offset += Size;
      }
    }
    EXPECT_EQ(Begins, 1u);
    EXPECT_EQ(Ends, 1u);
  }
};

TEST_F(X86TransactionBeginSource, NativeAndBothCRoutesRetainTheRegion) {
  unsigned Objects = 0;
  for (Arch A : {Arch::X86, Arch::X64})
    for (auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (bool Short : {false, true})
        for (bool NoOpt : {false, true}) {
          SCOPED_TRACE(testing::Message()
                       << unsigned(A) << " " << unsigned(Format) << " " << Short
                       << " " << NoOpt);
          // The rel16 fallback precedes XBEGIN and is reachable only through
          // its abort edge. A decoder target shifted by 64 KiB cannot quietly
          // turn into a different CFG while still passing source compilation.
          auto Bytes =
              Short ? std::vector<uint8_t>{0xeb, 1, 0xc3, 0xb8, 7, 0, 0, 0}
                    : std::vector<uint8_t>{0xb8, 7, 0, 0, 0};
          const auto Begin = beginBytes(
              Short ? std::vector<uint8_t>{0x66} : std::vector<uint8_t>{},
              Short ? 2 : 4, Short ? uint32_t(-11) : 7);
          Bytes.insert(Bytes.end(), Begin.begin(), Begin.end());
          Bytes.insert(Bytes.end(), {0x83, 0xc0, 4, 0x0f, 0x01, 0xd5, 0xc3});
          if (!Short)
            Bytes.push_back(0xc3);
          BinaryImage Image;
          Image.Arch = A;
          Image.Format = Format;
          Image.Bits = A == Arch::X64 ? Bitness::Bits64 : Bitness::Bits32;
          Image.Base = Image.Entry = Entry;
          Segment Text;
          Text.VA = Entry;
          Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
          Text.Data = Bytes;
          Text.Size = Text.FileSz = Bytes.size();
          Image.Segments.push_back(std::move(Text));
          auto FunctionSymbol = Symbol::makeFunc(Entry, Bytes.size());
          FunctionSymbol.Name = "transaction_begin";
          Image.Symbols.push_back(std::move(FunctionSymbol));
          for (unsigned Route = 0; Route < 3; ++Route) {
            llvm::LLVMContext Context;
            PipelineOptions Options;
            Options.LiftMode = Route != 1;
            Options.SourceProjection = Route == 2;
            Options.NoOpt = NoOpt;
            Options.EmitDumpOutput = false;
            Options.OnlyFunctionEntries = {Entry};
            auto Result = Pipeline().run(Image, Context, Options);
            ASSERT_TRUE(Result.Success) << Result.Error;
            if (Route == 0) {
              ASSERT_NE(Result.LlvmModule, nullptr);
              auto Object = Codegen().compile(*Result.LlvmModule, A, Format);
              ASSERT_TRUE(Object.Success);
              const llvm::StringRef Data(
                  reinterpret_cast<const char *>(Object.ObjectData.data()),
                  Object.ObjectData.size());
              checkObject(Data, A);
              retain("llvm.o", Data);
              ++Objects;
              continue;
            }
            CEmitterOptions Emit;
            Emit.TheArch = A;
            Emit.Format = Format;
            Emit.Image = &Image;
            Emit.PreserveLLVMFunctionTypes = true;
            std::string Source;
            llvm::raw_string_ostream OS(Source);
            ASSERT_TRUE(Route == 1
                            ? HighCEmitter().emit(Result.HighFuncs, OS, Emit)
                            : LLVMCEmitter().emit(*Result.LlvmModule, OS, Emit,
                                                  nullptr, &Image));
            EXPECT_NE(Source.find("_xbegin("), std::string::npos);
            const std::string Include = "#include <string.h>";
            if (auto At = Source.find(Include); At != std::string::npos)
              Source.replace(At, Include.size(),
                             "extern void *memcpy(void *, const void *, "
                             "__SIZE_TYPE__);\nextern void *memset(void *, "
                             "int, __SIZE_TYPE__);");
            const auto Stem = Route == 1 ? "high" : "llvm-c";
            const auto Input = file(std::string(Stem) + ".c");
            write(Input, Source);
            retain(std::string(Stem) + ".c", Source);
            const std::string Triple =
                std::string(A == Arch::X64 ? "x86_64" : "i686") +
                (Format == BinaryFormat::COFF    ? "-pc-windows-msvc"
                 : Format == BinaryFormat::MachO ? "-apple-darwin"
                                                 : "-linux-gnu");
            for (const auto *Opt : {"-O0", "-O2"}) {
              const auto Output = file(std::string(Stem) + Opt + ".o");
              const auto Built = command(
                  Compiler, {"--target=" + Triple, "-std=gnu23",
                             "-ffreestanding", Opt, "-c", Input, "-o", Output});
              ASSERT_EQ(Built.Status, 0) << Built.Error << Source;
              checkObject(contents(Output), A);
              retainFile(std::string(Stem) + Opt + ".o", Output);
              ++Objects;
            }
          }
          ++ProbeNumber;
        }
  EXPECT_EQ(Objects, 120u);
  RecordProperty("native_objects", Objects);
}
} // namespace
