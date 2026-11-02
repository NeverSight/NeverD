//===- X86SkinitTests.cpp - Secure-loader launch contracts --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Validate physical EAX input and termination without executing SKINIT.
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/c/render/X86/X86SystemState.h"
#include "../x86_64/X86FPStateAccuracyFixture.h"

#include "neverd/ir/low/LowNoReturn.h"
#include "neverd/ir/med/MedNoReturn.h"
#include "neverd/lift/X86/X86Lifter.h"

#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/ModRef.h"

#include <algorithm>
#include <functional>
#include <stdexcept>

namespace {
std::vector<uint8_t> launchBytes(std::vector<uint8_t> Prefix = {}) {
  Prefix.insert(Prefix.end(), {0x0f, 0x01, 0xde});
  return Prefix;
}

TEST(X86Skinit, PrefixesRetainOnePhysicalEaxInputAndTerminateEveryRoute) {
  unsigned Checks = 0;
  for (Arch A : {Arch::X86, Arch::X64}) {
    std::vector<std::vector<uint8_t>> Prefixes = {
        {},           {0x66},       {0x67},       {0xf2},
        {0xf3},       {0x26},       {0x2e},       {0x36},
        {0x3e},       {0x64},       {0x65},       {0x66, 0x67},
        {0xf2, 0xf3}, {0x67, 0x66}, {0x65, 0x66}, {0x66, 0x66}};
    if (A == Arch::X64)
      Prefixes.insert(
          Prefixes.end(),
          {{0x40}, {0x48}, {0x4f}, {0x48, 0x66}, {0x66, 0x48}, {0x4f, 0x40}});
    Prefixes.push_back(std::vector<uint8_t>(12, 0x66));
    for (const auto &Prefix : Prefixes)
      for (bool Strict : {false, true}) {
        const auto Bytes = launchBytes(Prefix);
        Decoder D;
        ASSERT_TRUE(D.init(A));
        D.setStrict(Strict);
        for (unsigned Route = 0; Route < 3; ++Route) {
          SCOPED_TRACE(testing::PrintToString(Bytes));
          DecodedInsn I{};
          const auto Size =
              Route == 0 ? D.decodeOne(Bytes.data(), Bytes.size(), Entry, I)
              : Route == 1
                  ? D.decodeOneLight(Bytes.data(), Bytes.size(), Entry, I)
                  : D.decodeOneForLift(Bytes.data(), Bytes.size(), Entry, I);
          ASSERT_EQ(Size, Bytes.size());
          EXPECT_TRUE(X86Lifter::isFunctionTerminator(I.Raw));
          EXPECT_FALSE(X86Lifter::isReturn(I.Raw));
          std::vector<LowOp> Ops;
          ASSERT_NO_THROW(D.liftToLow(I, Ops));
          ASSERT_EQ(Ops.size(), 1u);
          const auto &Op = Ops.front();
          ASSERT_EQ(Op.Opcode, NdOp::INTRINSIC);
          ASSERT_EQ(Intrinsic(Op.Inputs[0].Offset), Intrinsic::X86Skinit);
          EXPECT_EQ(Op.NumInputs, 2u);
          EXPECT_EQ(Op.Output.Size, 0u);
          EXPECT_EQ(Op.Inputs[1], NdVar::reg(x86reg::RAX, 4));
          EXPECT_TRUE(isArchitecturalNoReturn(Op));
          EXPECT_TRUE(x86SystemStateShapeIsValid(
              Intrinsic::X86Skinit, x86SystemStateLowShape(Op, A)));
          EXPECT_TRUE(isSideeffectIntrinsic(Intrinsic::X86Skinit));
          ++Checks;
        }
      }
  }
  EXPECT_EQ(Checks, 240u);
  RecordProperty("decode_lift_termination_checks", Checks);
}

TEST(X86Skinit, ContradictoryMetadataCannotPublishALaunch) {
  using Mutation = std::function<void(cs_insn &)>;
  const std::vector<Mutation> Mutations = {
      [](auto &I) { I.bytes[I.size - 1] ^= 1; },
      [](auto &I) { I.bytes[0] = 0xf0; },
      [](auto &I) { I.detail->x86.op_count = 0; },
      [](auto &I) { I.detail->x86.op_count = 2; },
      [](auto &I) { I.detail->x86.operands[0].type = X86_OP_MEM; },
      [](auto &I) { I.detail->x86.operands[0].reg = X86_REG_RAX; },
      [](auto &I) { I.detail->x86.operands[0].size = 8; },
      [](auto &I) { I.detail->x86.operands[0].access = CS_AC_WRITE; },
      [](auto &I) { I.detail->x86.opcode[2] = 1; },
      [](auto &I) { I.detail->x86.modrm ^= 1; },
      [](auto &I) { I.detail->x86.encoding.modrm_offset++; },
      [](auto &I) { I.detail->x86.encoding.imm_size = 1; },
      [](auto &I) { I.detail->x86.encoding.imm_offset = 1; },
      [](auto &I) { I.detail->x86.encoding.disp_size = 1; },
      [](auto &I) { I.detail->x86.encoding.disp_offset = 1; },
      [](auto &I) { I.detail->x86.rex2 = 1; },
      [](auto &I) { I.detail->x86.addr_size = 1; },
      [](auto &I) { I.detail->x86.prefix[0] = 0xf0; },
      [](auto &I) { I.detail->x86.prefix[1] = 0x65; },
      [](auto &I) { I.detail->x86.eflags = 1; },
      [](auto &I) { I.detail->x86.sib = 1; },
      [](auto &I) { I.detail->x86.disp = 1; },
      [](auto &I) { I.detail->x86.avx_sae = true; },
      [](auto &I) { I.detail->regs_read_count = 0; },
      [](auto &I) { I.detail->regs_read[0] = X86_REG_RAX; },
      [](auto &I) { I.detail->regs_write_count = 1; },
      [](auto &I) { I.detail->groups_count = 1; },
      [](auto &I) { I.detail->groups[0] = X86_GRP_JUMP; }};
  unsigned Checks = 0;
  for (Arch A : {Arch::X86, Arch::X64})
    for (bool Strict : {false, true})
      for (const auto &Mutate : Mutations) {
        Decoder D;
        ASSERT_TRUE(D.init(A));
        D.setStrict(Strict);
        const auto Bytes = launchBytes();
        DecodedInsn I{};
        ASSERT_EQ(D.decodeOneForLift(Bytes.data(), Bytes.size(), Entry, I),
                  Bytes.size());
        Mutate(*I.Raw);
        std::vector<LowOp> Ops;
        EXPECT_THROW(D.liftToLow(I, Ops), UnliftedInstruction);
        EXPECT_TRUE(Ops.empty());
        ++Checks;
      }
  RecordProperty("malformed_metadata_refusals", Checks);
}

TEST(X86Skinit, ConcreteRefusalDoesNotReadTheSlbOrResetRegisters) {
  for (Arch A : {Arch::X86, Arch::X64}) {
    LowOp Op;
    Op.Opcode = NdOp::INTRINSIC;
    Op.NumInputs = 2;
    Op.Inputs[0] = NdVar::scalar(uint16_t(Intrinsic::X86Skinit), 2);
    Op.Inputs[1] = NdVar::reg(x86reg::RAX, 4);
    BinaryImage Image;
    Image.Arch = A;
    NdOpEmulator Emu(Image);
    Emu.setLoadCollect(true);
    for (auto R : {x86reg::RAX, x86reg::RBX, x86reg::RCX, x86reg::RDX,
                   x86reg::RSP, x86reg::RBP, x86reg::CF, x86reg::DF})
      Emu.setRegister(R, 0x8765cdef);
    EXPECT_FALSE(Emu.step(Op));
    EXPECT_TRUE(Emu.getLoadRecords().empty());
    for (auto R : {x86reg::RAX, x86reg::RBX, x86reg::RCX, x86reg::RDX,
                   x86reg::RSP, x86reg::RBP, x86reg::CF, x86reg::DF})
      EXPECT_EQ(Emu.getRegister(R), 0x8765cdefu);
    auto Bad = Op;
    Bad.Inputs[1].Size = 8;
    EXPECT_FALSE(x86SystemStateShapeIsValid(Intrinsic::X86Skinit,
                                            x86SystemStateLowShape(Bad, A)));
    Bad = Op;
    Bad.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
    EXPECT_FALSE(x86SystemStateShapeIsValid(Intrinsic::X86Skinit,
                                            x86SystemStateLowShape(Bad, A)));
  }
}

TEST(X86Skinit, CProjectionRequiresTheCompleteNonreturningMemoryContract) {
  unsigned Checks = 0;
  for (Arch A : {Arch::X86, Arch::X64})
    for (bool Native : {false, true}) {
      const auto IID = llvm::Intrinsic::lookupIntrinsicID("llvm.x86.skinit");
      if (Native && !IID)
        continue; // The pinned SDK uses the owned assembly contract.
      llvm::LLVMContext Context;
      llvm::Module Module("skinit-contract", Context);
      auto *Type =
          llvm::FunctionType::get(llvm::Type::getVoidTy(Context),
                                  {llvm::Type::getInt32Ty(Context)}, false);
      auto *Function = llvm::Function::Create(
          Type, llvm::Function::ExternalLinkage, "launch", Module);
      llvm::IRBuilder<> Builder(
          llvm::BasicBlock::Create(Context, "entry", Function));
      llvm::CallInst *Call;
      if (Native) {
        auto *Callee = llvm::Intrinsic::getOrInsertDeclaration(&Module, IID);
        Callee->setAttributes({});
        Call = Builder.CreateCall(Callee, {Function->getArg(0)});
      } else {
        const auto Plan = x86SystemStateAsm(Intrinsic::X86Skinit, A, 0,
                                            NdMemoryAddressSpace::Default);
        ASSERT_TRUE(Plan);
        auto *Asm =
            llvm::InlineAsm::get(Type, Plan->Text, Plan->Constraints, true);
        Call = Builder.CreateCall(Asm, {Function->getArg(0)});
      }
      const auto Render = [&]() {
        return Native ? renderX86SystemStateNativeCall(A, *Call, "", false,
                                                       {"physical"})
                      : renderX86SystemStateCall(A, *Call, "", false,
                                                 {"physical"});
      };
      for (const auto Attribute :
           {llvm::Attribute::NoReturn, llvm::Attribute::Convergent,
            llvm::Attribute::NoDuplicate})
        Call->addFnAttr(Attribute);
      ASSERT_TRUE(Render());
      for (const auto Attribute :
           {llvm::Attribute::NoReturn, llvm::Attribute::Convergent,
            llvm::Attribute::NoDuplicate}) {
        Call->removeFnAttr(Attribute);
        EXPECT_THROW((void)Render(), std::invalid_argument);
        Call->addFnAttr(Attribute);
        ++Checks;
      }
      for (const auto Attribute :
           {llvm::Attribute::WillReturn, llvm::Attribute::NoSync,
            llvm::Attribute::NoFree, llvm::Attribute::NoCallback,
            llvm::Attribute::Speculatable}) {
        Call->addFnAttr(Attribute);
        EXPECT_THROW((void)Render(), std::invalid_argument);
        Call->removeFnAttr(Attribute);
        ++Checks;
      }
      for (const auto Effects :
           {llvm::MemoryEffects::none(), llvm::MemoryEffects::readOnly(),
            llvm::MemoryEffects::writeOnly(),
            llvm::MemoryEffects::argMemOnly()}) {
        Call->setMemoryEffects(Effects);
        EXPECT_THROW((void)Render(), std::invalid_argument);
        ++Checks;
      }
      Call->setMemoryEffects(llvm::MemoryEffects::unknown());
      EXPECT_TRUE(Render());
      Builder.CreateUnreachable();
      EXPECT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
    }
  RecordProperty("invalid_C_memory_and_termination_contracts", Checks);
}

class X86SkinitSource : public X86FPStateFixture {
protected:
  unsigned RawParameterExecutions = 0;

  void checkRawParameterExecution(const std::string &Source, bool High) {
    if (!nativeX64() || hostFormat() != BinaryFormat::ELF)
      return;
    const auto Observed =
        High ? "(uint32_t)dword_4040" : "*(const uint32_t *)(unk_4000 + 64)";
    const std::string Probe =
        "#define _GNU_SOURCE 1\n#include <stdint.h>\n#include <stdlib.h>\n"
        "#include <sys/mman.h>\n#include <string.h>\n"
        "static _Noreturn void record_launch(uint32_t);\n"
        "#define NEVERD_X86_SYSTEM_STATE(native, ...) native\n"
        "#define __builtin_ia32_skinit record_launch\n" +
        Source +
        "\nstatic _Noreturn void record_launch(uint32_t physical) {\n"
        " _Exit(physical == UINT32_C(0x8765cdf0) && " +
        Observed +
        " == UINT32_C(0x2468ace0) ? 0 : 93);\n}\n"
        "int main(void) {\n"
        " void *p = mmap((void *)(uintptr_t)0x8765c000, 4096, "
        "PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|"
        "MAP_FIXED_NOREPLACE, -1, 0);\n"
        " if (p != (void *)(uintptr_t)0x8765c000) return 92;\n"
        " uint32_t value = UINT32_C(0x2468ace0);\n"
        " memcpy((char *)p + 0xdf0, &value, sizeof(value));\n"
        " secure_launch(UINT32_C(0x8765cdf0));\n return 94;\n}\n";
    const auto Input = file("physical-parameter.c");
    write(Input, Probe);
    retain(High ? "high-parameter-recorder.c" : "llvm-c-parameter-recorder.c",
           Probe);
    // Inspect the preprocessed source before any execution. The owned dispatch
    // and builtin are replaced by a nonreturning recorder; no launch remains.
    const auto Preprocessed = command(Compiler, {"-std=gnu23", "-E", Input});
    ASSERT_EQ(Preprocessed.Status, 0) << Preprocessed.Error;
    ASSERT_EQ(Preprocessed.Out.find("\"skinit\""), std::string::npos);
    ASSERT_EQ(Preprocessed.Out.find("__builtin_ia32_skinit("),
              std::string::npos);
    for (const auto *Opt : {"-O0", "-O2"}) {
      const auto Executable = file(std::string("physical-parameter") + Opt);
      const auto Built =
          command(Compiler,
                  {"-std=gnu23", "-fno-strict-aliasing", "-fno-pie", "-no-pie",
                   "-fsanitize=undefined", Opt, Input, "-o", Executable});
      ASSERT_EQ(Built.Status, 0) << Built.Error << Probe;
      const auto Actual = command(Executable, {});
      EXPECT_EQ(Actual.Status, 0) << Actual.Error << Probe;
      ++RawParameterExecutions;
    }
  }

  void checkObject(llvm::StringRef Data, Arch A) {
    auto Obj = llvm::object::ObjectFile::createObjectFile(
        llvm::MemoryBufferRef(Data, "skinit.o"));
    ASSERT_TRUE(bool(Obj)) << llvm::toString(Obj.takeError());
    Decoder D;
    ASSERT_TRUE(D.init(A));
    unsigned Launches = 0;
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
        Launches += I.Id == X86_INS_SKINIT;
        EXPECT_FALSE(X86Lifter::isReturn(I.Raw));
        if (I.Id == X86_INS_CALL) {
          // i386 PIC may use a local call/pop PC anchor for ordinary image
          // data. It must not introduce an external launch helper or return.
          EXPECT_EQ(A, Arch::X86);
          EXPECT_EQ(Size, 5);
          EXPECT_EQ(llvm::StringRef(
                        reinterpret_cast<const char *>(I.Raw->bytes), Size),
                    llvm::StringRef("\xe8\0\0\0\0", 5));
          ASSERT_LT(Offset + Size, Text->size());
          EXPECT_EQ(uint8_t((*Text)[Offset + Size]) & 0xf8u, 0x58u);
        }
        EXPECT_NE(I.Id, X86_INS_VMRUN);
        Offset += Size;
      }
    }
    EXPECT_EQ(Launches, 1u);
  }
};

TEST_F(X86SkinitSource, PhysicalInputAndTerminalCFGSurviveEveryOutputRoute) {
  unsigned Objects = 0;
  for (Arch A : {Arch::X86, Arch::X64})
    for (auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (bool NoOpt : {false, true})
        for (unsigned Kind = 0; Kind < 3; ++Kind) {
          SCOPED_TRACE(testing::Message()
                       << unsigned(A) << " " << unsigned(Format) << " " << NoOpt
                       << " " << Kind);
          std::vector<uint8_t> Bytes;
          if (Kind == 2)
            Bytes = A == Arch::X86 ? std::vector<uint8_t>{0x8b, 0x44, 0x24, 4}
                    : Format == BinaryFormat::COFF
                        ? std::vector<uint8_t>{0x89, 0xc8}
                        : std::vector<uint8_t>{0x89, 0xf8};
          else {
            const uint32_t Base = Kind == 1 ? 0x4000 : 0x8765cdef;
            Bytes = {0xb8, uint8_t(Base), uint8_t(Base >> 8),
                     uint8_t(Base >> 16), uint8_t(Base >> 24)};
          }
          if (Kind) {
            Bytes.insert(Bytes.end(), {0x8b, 0x10, 0x89});
            if (A == Arch::X64)
              Bytes.insert(Bytes.end(), {0x14, 0x25});
            else
              Bytes.push_back(0x15);
            Bytes.insert(Bytes.end(), {0x40, 0x40, 0, 0});
          }
          Bytes.insert(Bytes.end(), {0x0f, 0x01, 0xde, 0x0f, 0x01, 0xd8, 0xc3});
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
          Segment Data;
          Data.VA = 0x4000;
          Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
          Data.Data.resize(128, 0x31);
          std::fill(Data.Data.begin() + 64, Data.Data.end(), 0x42);
          Data.Size = Data.FileSz = Data.Data.size();
          Image.Segments.push_back(std::move(Data));
          auto Sym = Symbol::makeFunc(Entry, Bytes.size());
          Sym.Name = "secure_launch";
          Image.Symbols.push_back(std::move(Sym));
          retain(
              "profile.json",
              llvm::formatv(
                  "{{\"arch\":{0},\"format\":{1},\"kind\":{2},\"no_opt\":{3}}",
                  unsigned(A), unsigned(Format), Kind, NoOpt)
                  .str());
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
            ASSERT_EQ(Result.MedFuncs.size(), 1u);
            EXPECT_TRUE(Result.MedFuncs.front().DoesNotReturn);
            std::string MedDump;
            llvm::raw_string_ostream MedStream(MedDump);
            Pipeline::dumpMedIR(Result.MedFuncs, MedStream);
            retain(Route == 1 ? "high-med.txt" : "llvm-med.txt", MedDump);
            if (Route == 1 && Kind == 2) {
              ASSERT_EQ(Result.HighFuncs.size(), 1u);
              ASSERT_EQ(Result.HighFuncs.front().Params.size(), 1u);
              const auto &Type = Result.HighFuncs.front().Params.front().Type;
              ASSERT_NE(Type, nullptr);
              // Only the low 32 bits enter EAX. A process-pointer ABI would
              // rebase callers' image constants and discard that integer role.
              EXPECT_EQ(Type->Kind, NdTypeKind::Int);
              EXPECT_EQ(Type->Size, 4u);
            }
            if (Route == 0) {
              ASSERT_NE(Result.LlvmModule, nullptr);
              std::string IR;
              llvm::raw_string_ostream IRStream(IR);
              Result.LlvmModule->print(IRStream, nullptr);
              retain("llvm.ll", IR);
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
            EXPECT_NE(Source.find("skinit"), std::string::npos);
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
            if (A == Arch::X64 && Format == BinaryFormat::ELF && Kind == 2)
              checkRawParameterExecution(Source, Route == 1);
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
  EXPECT_EQ(Objects, 180u);
  RecordProperty("native_objects", Objects);
  if (nativeX64() && hostFormat() == BinaryFormat::ELF)
    EXPECT_EQ(RawParameterExecutions, 8u);
  RecordProperty("raw_parameter_ubsan_executions", RawParameterExecutions);
}
} // namespace
