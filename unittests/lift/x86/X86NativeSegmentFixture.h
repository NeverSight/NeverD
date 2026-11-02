//===- X86NativeSegmentFixture.h - i386 native segment pipelines -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Qualify raw segment offsets and original instruction prefixes in both C
/// routes.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_UNITTESTS_LIFT_X86_X86NATIVESEGMENTFIXTURE_H
#define NEVERD_UNITTESTS_LIFT_X86_X86NATIVESEGMENTFIXTURE_H

#include "../x86_64/X86FPStateAccuracyFixture.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/codegen/CodeGen.h"
#include "neverd/decode/Decoder.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {
class X86NativeSegmentFixture : public X86FPStateFixture {
protected:
  unsigned Objects = 0;

  BinaryImage segmentImage(const std::vector<uint8_t> &Bytes,
                           BinaryFormat Format, const char *Name) {
    BinaryImage Image;
    Image.Arch = Arch::X86;
    Image.Bits = Bitness::Bits32;
    Image.Format = Format;
    Image.Entry = Image.Base = Entry;
    Segment Text;
    Text.VA = Entry;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data = Bytes;
    Text.Size = Text.FileSz = Bytes.size();
    Image.Segments.push_back(std::move(Text));
    // An absolute segment offset aliases this ordinary DS object numerically.
    // Native segment transport must keep 0x4000, not its relocated C pointer.
    Segment Data;
    Data.VA = 0x4000;
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Data.Data.resize(64, 0x31);
    Data.Size = Data.FileSz = 64;
    Image.Segments.push_back(std::move(Data));
    auto Symbol = Symbol::makeFunc(Entry, Bytes.size());
    Symbol.Name = Name;
    Image.Symbols.push_back(std::move(Symbol));
    return Image;
  }

  void checkObject(llvm::StringRef Data, unsigned Prefix, unsigned Id) {
    auto Obj = llvm::object::ObjectFile::createObjectFile(
        llvm::MemoryBufferRef(Data, "segment.o"));
    ASSERT_TRUE(bool(Obj)) << llvm::toString(Obj.takeError());
    Decoder D;
    ASSERT_TRUE(D.init(Arch::X86));
    unsigned Count = 0;
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
        if (I.Id == Id) {
          ++Count;
          EXPECT_EQ(I.Raw->detail->x86.prefix[1], Prefix);
          bool Memory = false;
          for (unsigned N = 0; N < I.Raw->detail->x86.op_count; ++N)
            Memory |= I.Raw->detail->x86.operands[N].type == X86_OP_MEM;
          EXPECT_TRUE(Memory);
        }
        Offset += Size;
      }
    }
    EXPECT_EQ(Count, 1u);
  }

  template <typename RecognizeCall>
  void checkPipelines(BinaryImage &Image, bool NoOpt, unsigned Prefix,
                      unsigned Id, bool AbsoluteOffset,
                      RecognizeCall Recognize) {
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
        unsigned Calls = 0;
        for (const auto &F : *Result.LlvmModule)
          for (const auto &Block : F)
            for (const auto &I : Block)
              if (const auto *Call = llvm::dyn_cast<llvm::CallInst>(&I))
                if (const auto ActualPrefix = Recognize(Arch::X86, *Call)) {
                  ++Calls;
                  EXPECT_EQ(*ActualPrefix, Prefix);
                  if (AbsoluteOffset) {
                    const llvm::Value *Value = Call->getArgOperand(0);
                    if (Value->getType()->isPointerTy()) {
                      const auto *Pointer =
                          llvm::dyn_cast<llvm::ConstantExpr>(Value);
                      ASSERT_NE(Pointer, nullptr);
                      ASSERT_EQ(Pointer->getOpcode(),
                                llvm::Instruction::IntToPtr);
                      Value = Pointer->getOperand(0);
                    }
                    const auto *Offset =
                        llvm::dyn_cast<llvm::ConstantInt>(Value);
                    ASSERT_NE(Offset, nullptr);
                    EXPECT_EQ(Offset->getZExtValue(), 0x4000u);
                  }
                }
        EXPECT_EQ(Calls, 1u);
        auto Object =
            Codegen().compile(*Result.LlvmModule, Arch::X86, Image.Format);
        ASSERT_TRUE(Object.Success);
        const llvm::StringRef Raw(
            reinterpret_cast<const char *>(Object.ObjectData.data()),
            Object.ObjectData.size());
        ASSERT_NO_FATAL_FAILURE(checkObject(Raw, Prefix, Id));
        retain("native.o", Raw);
        std::string IR;
        llvm::raw_string_ostream OS(IR);
        Result.LlvmModule->print(OS, nullptr);
        retain("native.ll", IR);
        ++Objects;
        continue;
      }
      CEmitterOptions Emit;
      Emit.TheArch = Arch::X86;
      Emit.Format = Image.Format;
      Emit.Image = &Image;
      Emit.PreserveLLVMFunctionTypes = true;
      std::string Source;
      llvm::raw_string_ostream OS(Source);
      ASSERT_TRUE(Route == 1 ? HighCEmitter().emit(Result.HighFuncs, OS, Emit)
                             : LLVMCEmitter().emit(*Result.LlvmModule, OS, Emit,
                                                   nullptr, &Image));
      const std::string Include = "#include <string.h>";
      if (auto At = Source.find(Include); At != std::string::npos)
        Source.replace(
            At, Include.size(),
            "extern void *memcpy(void *, const void *, __SIZE_TYPE__);\n"
            "extern void *memset(void *, int, __SIZE_TYPE__);\n");
      const auto Stem = Route == 1 ? "high" : "llvm-c";
      const auto Input = file(std::string(Stem) + ".c");
      write(Input, Source);
      retain(std::string(Stem) + ".c", Source);
      const auto Triple = Image.Format == BinaryFormat::ELF ? "i686-linux-gnu"
                          : Image.Format == BinaryFormat::COFF
                              ? "i686-pc-windows-msvc"
                              : "i686-apple-darwin";
      for (const auto *Opt : {"-O0", "-O2"}) {
        const auto Output = file(std::string(Stem) + Opt + ".o");
        auto Built = command(Compiler, {std::string("--target=") + Triple,
                                        "-std=gnu23", "-ffreestanding", Opt,
                                        "-c", Input, "-o", Output});
        ASSERT_EQ(Built.Status, 0) << Built.Error << Source;
        ASSERT_NO_FATAL_FAILURE(checkObject(contents(Output), Prefix, Id));
        retainFile(std::string(Stem) + Opt + ".o", Output);
        ++Objects;
      }
    }
    ++ProbeNumber;
  }
};
} // namespace
#endif // NEVERD_UNITTESTS_LIFT_X86_X86NATIVESEGMENTFIXTURE_H
