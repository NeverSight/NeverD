//===- X86SEHDisplayTests.cpp - x86 SEH as real code in the decompile view ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// An x86 `_except_handler3` frame compiles a `__finally` to one body inside
/// its function, entered by the runtime at the scope's handler and on the
/// normal path by a `call`, and its filters to code that runs in the function's
/// frame. These tests give the pipeline that machine code byte for byte and pin
/// what the decompile view prints: the clauses as C, with the frame's upkeep
/// gone.
///
//===----------------------------------------------------------------------===//

#include "../../../lib/ir/low/X86/RegistrationDisplayFrame.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace neverd;

namespace {

constexpr va_t kBase = 0x400000;
constexpr va_t kText = 0x401000;
constexpr va_t kRData = 0x402000;
constexpr va_t kPersonality = 0x401f00;
constexpr va_t kOther = 0x401e00;

void emit32(std::vector<uint8_t> &Out, uint32_t Value) {
  for (unsigned Byte = 0; Byte < 4; ++Byte)
    Out.push_back(static_cast<uint8_t>(Value >> (Byte * 8)));
}

class Frame {
public:
  std::vector<uint8_t> Text;
  std::vector<uint8_t> RData;

  va_t here() const { return kText + Text.size(); }

  void bytes(std::initializer_list<uint8_t> Bytes) {
    Text.insert(Text.end(), Bytes);
  }

  /// `push ebp; mov ebp, esp; push -1; push table; push handler;
  /// mov eax, fs:[0]; push eax; mov fs:[0], esp; sub esp, 0x10;
  /// mov [ebp - 0x18], esp`
  void prologue() {
    bytes({0x55, 0x8b, 0xec, 0x6a, 0xff, 0x68});
    emit32(Text, kRData);
    Text.push_back(0x68);
    emit32(Text, kPersonality);
    bytes({0x64, 0xa1, 0, 0, 0, 0, 0x50, 0x64, 0x89, 0x25, 0, 0, 0, 0});
    bytes({0x83, 0xec, 0x10, 0x89, 0x65, 0xe8});
  }

  /// clang-cl's frame, with its record in words below what it pushes after EBP:
  /// `push ebp; mov ebp, esp; push ebx; push edi; push esi; sub esp, 0x10;
  /// mov [ebp - 0x24], esp; mov dword [ebp - 0x10], -1;
  /// mov dword [ebp - 0x14], table; lea eax, [ebp - 0x1c];
  /// mov dword [ebp - 0x18], handler; mov ecx, fs:[0]; mov [ebp - 0x1c], ecx;
  /// mov fs:[0], eax`
  void slotPrologue() {
    bytes({0x55, 0x89, 0xe5, 0x53, 0x57, 0x56, 0x83, 0xec, 0x10});
    bytes({0x89, 0x65, 0xdc});
    bytes({0xc7, 0x45, 0xf0, 0xff, 0xff, 0xff, 0xff});
    bytes({0xc7, 0x45, 0xec});
    emit32(Text, kRData);
    bytes({0x8d, 0x45, 0xe4});
    bytes({0xc7, 0x45, 0xe8});
    emit32(Text, kPersonality);
    bytes({0x64, 0x8b, 0x0d, 0, 0, 0, 0});
    bytes({0x89, 0x4d, 0xe4});
    bytes({0x64, 0xa3, 0, 0, 0, 0});
  }

  /// `mov ecx, [ebp - 0x1c]; mov fs:[0], ecx; add esp, 0x10; pop esi; pop edi;
  /// pop ebx; pop ebp; ret`
  void slotEpilogue() {
    bytes({0x8b, 0x4d, 0xe4, 0x64, 0x89, 0x0d, 0, 0, 0, 0});
    bytes({0x83, 0xc4, 0x10, 0x5e, 0x5f, 0x5b, 0x5d, 0xc3});
  }

  /// `mov dword ptr [ebp - 0x10], Level`
  void slotLevel(int32_t Level) {
    bytes({0xc7, 0x45, 0xf0});
    emit32(Text, static_cast<uint32_t>(Level));
  }

  /// `mov ecx, [ebp - 0x10]; mov fs:[0], ecx; mov esp, ebp; pop ebp; ret`
  void epilogue() {
    bytes({0x8b, 0x4d, 0xf0, 0x64, 0x89, 0x0d, 0, 0, 0, 0, 0x8b, 0xe5, 0x5d,
           0xc3});
  }

  /// `mov dword ptr [ebp - 4], Level`
  void level(int32_t Level) {
    bytes({0xc7, 0x45, 0xfc});
    emit32(Text, static_cast<uint32_t>(Level));
  }

  /// `mov eax, [ebp + 8]; add dword ptr [eax], Amount`
  void addToArgument(uint8_t Amount) {
    bytes({0x8b, 0x45, 0x08, 0x83, 0x00, Amount});
  }

  /// `call Target`, patched by address.
  void call(va_t Target) {
    Text.push_back(0xe8);
    emit32(Text, static_cast<uint32_t>(Target - (here() + 4)));
  }

  /// `jmp rel8`, whose target comes later: returns where to patch it.
  size_t jumpForward() {
    bytes({0xeb, 0});
    return Text.size() - 1;
  }

  void landJump(size_t At) {
    Text[At] = static_cast<uint8_t>(Text.size() - (At + 1));
  }

  size_t callForward() {
    Text.push_back(0xe8);
    emit32(Text, 0);
    return Text.size() - 4;
  }

  void landCall(size_t At) {
    const uint32_t Offset = static_cast<uint32_t>(Text.size() - (At + 4));
    for (unsigned Byte = 0; Byte < 4; ++Byte)
      Text[At + Byte] = static_cast<uint8_t>(Offset >> (Byte * 8));
  }

  void scope(int32_t Enclosing, va_t Filter, va_t Handler) {
    emit32(RData, static_cast<uint32_t>(Enclosing));
    emit32(RData, static_cast<uint32_t>(Filter));
    emit32(RData, static_cast<uint32_t>(Handler));
  }

  BinaryImage build() {
    for (unsigned Word = 0; Word < 3; ++Word)
      emit32(RData, 0);
    // A plain function the frame calls, `int other(int) { return arg; }`, and
    // the runtime handler past them.
    Text.resize(kOther - kText, 0xcc);
    bytes({0x8b, 0x44, 0x24, 0x04, 0xc3});
    Text.resize(kPersonality - kText, 0xcc);
    Text.push_back(0xc3);

    BinaryImage Img;
    Img.Arch = Arch::X86;
    Img.Bits = Bitness::Bits32;
    Img.Format = BinaryFormat::COFF;
    Img.Base = kBase;
    Img.Entry = kText;
    Segment TextSeg;
    TextSeg.Name = ".text";
    TextSeg.VA = kText;
    TextSeg.Size = TextSeg.FileSz = Text.size();
    TextSeg.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    TextSeg.Data = Text;
    Section TextSection;
    TextSection.Name = TextSeg.Name;
    TextSection.VA = TextSeg.VA;
    TextSection.Size = TextSection.FileSz = TextSeg.Size;
    TextSection.Flags = TextSeg.Flags;
    Img.Sections.push_back(std::move(TextSection));
    Img.Segments.push_back(std::move(TextSeg));
    Segment RDataSeg;
    RDataSeg.Name = ".rdata";
    RDataSeg.VA = kRData;
    RDataSeg.Size = RDataSeg.FileSz = std::max<size_t>(RData.size(), 16);
    RDataSeg.Flags = SegmentFlags::Readable;
    RDataSeg.Data = RData;
    RDataSeg.Data.resize(RDataSeg.Size, 0);
    Section RDataSection;
    RDataSection.Name = RDataSeg.Name;
    RDataSection.VA = RDataSeg.VA;
    RDataSection.Size = RDataSection.FileSz = RDataSeg.Size;
    RDataSection.Flags = RDataSeg.Flags;
    Img.Sections.push_back(std::move(RDataSection));
    Img.Segments.push_back(std::move(RDataSeg));
    for (const auto &[Name, Addr] :
         {std::pair<const char *, va_t>{"guarded", kText},
          {"other", kOther},
          {"_except_handler3", kPersonality}}) {
      Symbol Sym;
      Sym.Name = Name;
      Sym.Addr = Addr;
      Sym.IsFunc = true;
      Img.Symbols.push_back(std::move(Sym));
    }
    coff_loader::parseX86RegistrationExceptions(Img);
    return Img;
  }
};

/// The text of the function named `guarded`.
std::string decompileGuarded(const BinaryImage &Img) {
  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  auto Result = Pipeline().run(Img, Ctx, Opts);
  EXPECT_TRUE(Result.Success) << Result.Error;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions COpts;
  COpts.TheArch = Arch::X86;
  COpts.Format = BinaryFormat::COFF;
  EXPECT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS, COpts));
  OS.flush();
  const size_t At = Source.find(" guarded(");
  if (At == std::string::npos)
    return Source;
  int Depth = 0;
  for (size_t I = Source.find('{', At); I < Source.size(); ++I) {
    if (Source[I] == '{')
      ++Depth;
    else if (Source[I] == '}' && --Depth == 0)
      return Source.substr(At, I - At + 1);
  }
  return Source.substr(At);
}

bool has(const std::string &Text, const std::string &Part) {
  return Text.find(Part) != std::string::npos;
}

size_t count(const std::string &Text, const std::string &Part) {
  size_t N = 0;
  for (size_t At = Text.find(Part); At != std::string::npos;
       At = Text.find(Part, At + 1))
    ++N;
  return N;
}

/// What the first `__finally` of \p Text holds.
std::string finallyClause(const std::string &Text) {
  const size_t At = Text.find("__finally {");
  if (At == std::string::npos)
    return {};
  int Depth = 0;
  for (size_t I = Text.find('{', At); I < Text.size(); ++I) {
    if (Text[I] == '{')
      ++Depth;
    else if (Text[I] == '}' && --Depth == 0)
      return Text.substr(At, I - At + 1);
  }
  return Text.substr(At);
}

// __try { *arg0 += 1; } __finally { *arg0 += 2; }
BinaryImage makeFinally(bool ClearsFlag = false) {
  Frame F;
  F.prologue();
  F.level(0);
  if (ClearsFlag)
    F.bytes({0xc7, 0x45, 0xe4, 1, 0, 0, 0}); // mov dword [ebp - 0x1c], 1
  F.addToArgument(1);
  F.level(-1);
  if (ClearsFlag)
    F.bytes({0xc7, 0x45, 0xe4, 0, 0, 0, 0}); // mov dword [ebp - 0x1c], 0
  const size_t ToBody = F.callForward();
  const size_t ToEnd = F.jumpForward();
  const va_t Body = F.here();
  F.landCall(ToBody);
  if (ClearsFlag)
    F.bytes({0x83, 0x7d, 0xe4, 0x00, 0x75, 0x06}); // cmp [ebp - 0x1c], 0; jne
  F.addToArgument(2);
  F.bytes({0xc3});
  F.landJump(ToEnd);
  F.epilogue();
  F.scope(-1, 0, Body);
  return F.build();
}

/// A minimal runtime-compatible frame for checking LowIR display ownership
/// independently of the decoder's choice of block boundaries.
LowFunc displayFrameForRanges() {
  LowFunc Func;
  Func.Entry = kText;
  auto &EH = Func.ExceptionMetadata.emplace();
  EH.CodeRange = {kText, kText + 16};
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::ExceptHandler3;
  EH.Registration.emplace().SeededTryLevel = -1;
  Func.RegistrationStates.emplace();
  LowBlock Entry;
  Entry.Id = 0;
  Entry.StartAddr = kText;
  Entry.EndAddr = kText + 16;
  const NdVar FP = NdVar::reg(x86reg::RBP, 4);
  LowOp Frame;
  Frame.Opcode = NdOp::COPY;
  Frame.Addr = kText;
  Frame.Output = FP;
  Frame.addInput(NdVar::reg(x86reg::RSP, 4));
  Entry.Ops.push_back(Frame);
  LowOp Record;
  Record.Opcode = NdOp::INT_SUB;
  Record.Addr = kText + 4;
  Record.Output = NdVar::tmp(0, 4);
  Record.addInput(FP);
  Record.addInput(NdVar::scalar(16, 4));
  Entry.Ops.push_back(Record);
  LowOp Install;
  Install.Opcode = NdOp::STORE;
  Install.Addr = kText + 8;
  Install.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
  Install.addInput(NdVar::scalar(0, 4));
  Install.addInput(Record.Output);
  Entry.Ops.push_back(Install);
  Func.Blocks.push_back(std::move(Entry));
  return Func;
}

TEST(X86SEHDisplay, FinallyCallMembershipPreservesRangeUnions) {
  constexpr va_t Root = kText + 0x100;
  // Nested, overlapping, adjacent and separated blocks in non-address ID
  // order. Empty and reversed extents contain no call target.
  const std::array<ExceptionAddressRange, 7> Ranges = {
      {{Root, Root + 40},
       {Root + 8, Root + 16},
       {Root + 64, Root + 80},
       {Root + 36, Root + 48},
       {Root + 48, Root + 56},
       {Root + 60, Root + 60},
       {Root + 90, Root + 85}}};
  for (bool Reverse : {false, true}) {
    SCOPED_TRACE(Reverse);
    LowFunc Func = displayFrameForRanges();
    Func.ExceptionMetadata->Registration->Scopes.push_back({-1, 0, Root, true});
    for (size_t I = 0; I < Ranges.size(); ++I) {
      LowBlock Body;
      Body.Id = static_cast<int>(I) + 1;
      Body.StartAddr = Ranges[I].Begin;
      Body.EndAddr = Ranges[I].End;
      if (!I)
        for (size_t J = 1; J < Ranges.size(); ++J)
          Body.Succs.push_back(static_cast<int>(J) + 1);
      else
        Body.Preds = {1};
      if (I == 2) {
        LowOp Return;
        Return.Opcode = NdOp::RETURN;
        Return.Addr = Body.StartAddr;
        Body.Ops.push_back(Return);
      }
      Func.Blocks.push_back(std::move(Body));
    }
    std::set<va_t> ExpectedCalls;
    for (unsigned Offset = 0; Offset < 100; ++Offset) {
      const va_t Target = Root - 1 + Offset;
      LowBlock Site;
      Site.Id = static_cast<int>(Ranges.size()) + 1 + Offset;
      Site.StartAddr = kText + 0x200 + Offset * 8;
      Site.EndAddr = Site.StartAddr + 8;
      Site.Preds = {0};
      Func.Blocks[0].Succs.push_back(Site.Id);
      LowOp Call;
      Call.Opcode = NdOp::CALL;
      Call.Addr = Site.StartAddr;
      Call.addInput(NdVar::scalar(Target, 4));
      Site.Ops.push_back(Call);
      if (std::any_of(Ranges.begin(), Ranges.end(), [&](const auto &Range) {
            return Target >= Range.Begin && Target < Range.End;
          }))
        ExpectedCalls.insert(Call.Addr);
      Func.Blocks.push_back(std::move(Site));
    }
    if (Reverse)
      std::reverse(Func.Blocks.begin(), Func.Blocks.end());
    x86_registration::prepareRegistrationDisplayFrame(Func, nullptr);
    ASSERT_TRUE(Func.RegistrationStates->Display);
    EXPECT_EQ(Func.RegistrationStates->Display->NoValueCalls, ExpectedCalls);
  }
}

TEST(X86SEHDisplay, SingleBlockFinallyKeepsHalfOpenMembership) {
  constexpr va_t Root = kText + 0x100;
  for (va_t End : {Root + 16, Root, Root - 8}) {
    SCOPED_TRACE(End);
    LowFunc Func = displayFrameForRanges();
    Func.ExceptionMetadata->Registration->Scopes.push_back({-1, 0, Root, true});
    LowBlock Body;
    Body.Id = 1;
    Body.StartAddr = Root;
    Body.EndAddr = End;
    LowOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.Addr = Root;
    Body.Ops.push_back(Return);
    Func.Blocks.push_back(std::move(Body));
    std::set<va_t> ExpectedCalls;
    for (unsigned Offset = 0; Offset < 19; ++Offset) {
      const va_t Target = Root - 1 + Offset;
      LowBlock Site;
      Site.Id = 2 + Offset;
      Site.StartAddr = kText + 0x200 + Offset * 8;
      Site.EndAddr = Site.StartAddr + 8;
      Site.Preds = {0};
      Func.Blocks[0].Succs.push_back(Site.Id);
      LowOp Call;
      Call.Opcode = NdOp::CALL;
      Call.Addr = Site.StartAddr;
      Call.addInput(NdVar::scalar(Target, 4));
      Site.Ops.push_back(Call);
      if (Target >= Root && Target < End)
        ExpectedCalls.insert(Call.Addr);
      Func.Blocks.push_back(std::move(Site));
    }
    x86_registration::prepareRegistrationDisplayFrame(Func, nullptr);
    ASSERT_TRUE(Func.RegistrationStates->Display);
    EXPECT_EQ(Func.RegistrationStates->Display->NoValueCalls, ExpectedCalls);
  }
}

TEST(X86SEHDisplay, OverlappingFinallyBodiesKeepRootOrder) {
  for (bool FirstReturns : {false, true}) {
    SCOPED_TRACE(FirstReturns);
    LowFunc Func = displayFrameForRanges();
    constexpr va_t Root = kText + 0x100;
    auto &Scopes = Func.ExceptionMetadata->Registration->Scopes;
    Scopes = {{-1, 0, Root + 16, true}, {-1, 0, Root, true}};
    for (int I = 0; I < 2; ++I) {
      LowBlock Body;
      Body.Id = I + 1;
      Body.StartAddr = Root + I * 16;
      Body.EndAddr = Root + 64;
      if ((I == 0) == FirstReturns) {
        LowOp Return;
        Return.Opcode = NdOp::RETURN;
        Return.Addr = Body.StartAddr;
        Body.Ops.push_back(Return);
      }
      Func.Blocks.push_back(std::move(Body));
    }
    LowBlock Site;
    Site.Id = 3;
    Site.StartAddr = kText + 0x200;
    Site.EndAddr = Site.StartAddr + 16;
    Site.Preds = {0};
    Func.Blocks[0].Succs = {3};
    LowOp Call;
    Call.Opcode = NdOp::CALL;
    Call.Addr = Site.StartAddr;
    Call.addInput(NdVar::scalar(Root + 24, 4));
    Site.Ops.push_back(Call);
    LowOp After;
    After.Opcode = NdOp::COPY;
    After.Addr = Site.StartAddr + 8;
    After.Output = NdVar::reg(x86reg::RAX, 4);
    After.addInput(NdVar::scalar(7, 4));
    Site.Ops.push_back(After);
    Func.Blocks.push_back(std::move(Site));
    x86_registration::prepareRegistrationDisplayFrame(Func, nullptr);
    ASSERT_TRUE(Func.RegistrationStates->Display);
    EXPECT_TRUE(
        Func.RegistrationStates->Display->NoValueCalls.count(Call.Addr));
    EXPECT_EQ(Func.Blocks.back().Ops.back().Opcode,
              FirstReturns ? NdOp::COPY : NdOp::NOP);
  }
}

TEST(X86SEHDisplay, FinallyHoldsItsBodyAndTheCallGoes) {
  const std::string Text = decompileGuarded(makeFinally());
  EXPECT_TRUE(has(Text, "__try {")) << Text;
  EXPECT_TRUE(has(Text, "} __finally {")) << Text;
  // The address the clause came from stays beside the code.
  EXPECT_TRUE(has(Text, "/* finally handler @ 0x")) << Text;
  EXPECT_TRUE(has(Text, "+ 2")) << Text;
  const size_t Finally = Text.find("__finally");
  ASSERT_NE(Finally, std::string::npos) << Text;
  EXPECT_GT(Text.find("+ 2"), Finally) << Text;
  EXPECT_LT(Text.find("+ 1"), Finally) << Text;
  // C runs the clause: nothing calls it, and the handler's `ret` is its end.
  EXPECT_FALSE(has(Text, "sub_")) << Text;
  EXPECT_FALSE(has(Text, "return ")) << Text;
  EXPECT_FALSE(has(Text, "unstructured SEH region")) << Text;
  // The frame's upkeep says nothing C does not.
  EXPECT_FALSE(has(Text, "stack_storage")) << Text;
  EXPECT_FALSE(has(Text, "unknown register")) << Text;
}

TEST(X86SEHDisplay, TheFlagClearedBeforeTheCallIsClearedInTheTry) {
  const std::string Text = decompileGuarded(makeFinally(/*ClearsFlag=*/true));
  const size_t Finally = Text.find("__finally");
  ASSERT_NE(Finally, std::string::npos) << Text;
  // The machine clears it before it runs the body: C's `__finally` runs at the
  // end of the `__try`, so the clearing belongs there.
  const size_t Cleared = Text.rfind(" = 0;", Finally);
  EXPECT_NE(Cleared, std::string::npos) << Text;
  EXPECT_EQ(count(Text, " = 0;"), 1u) << Text;
  EXPECT_FALSE(has(Text, "sub_")) << Text;
}

// __try { *arg0 += 1; } __except (<filter>) { *arg0 += 4; }
template <typename FilterFn> BinaryImage makeExcept(FilterFn EmitFilter) {
  Frame F;
  F.prologue();
  F.level(0);
  F.addToArgument(1);
  F.level(-1);
  const size_t ToEnd = F.jumpForward();
  const va_t Filter = F.here();
  EmitFilter(F);
  const va_t Handler = F.here();
  F.bytes({0x8b, 0x65, 0xe8}); // mov esp, [ebp - 0x18]
  F.addToArgument(4);
  F.level(-1);
  F.landJump(ToEnd);
  F.epilogue();
  F.scope(-1, Filter, Handler);
  return F.build();
}

TEST(X86SEHDisplay, ConstantFilterNamesItsDisposition) {
  const std::string Text = decompileGuarded(makeExcept([](Frame &F) {
    F.bytes({0xb8, 1, 0, 0, 0, 0xc3}); // mov eax, 1; ret
  }));
  EXPECT_TRUE(has(Text, "__except (EXCEPTION_EXECUTE_HANDLER)")) << Text;
  // The arm reads the frame through the stack pointer it restores.
  EXPECT_TRUE(has(Text, "+ 4")) << Text;
  EXPECT_FALSE(has(Text, "sub_")) << Text;
  EXPECT_FALSE(has(Text, "stack_storage")) << Text;
  EXPECT_FALSE(has(Text, "unknown register")) << Text;
}

TEST(X86SEHDisplay, FilterReadingTheCodeIsGetExceptionCode) {
  const std::string Text = decompileGuarded(makeExcept([](Frame &F) {
    // mov eax, [ebp - 0x14]; mov eax, [eax]; mov ecx, [eax]; xor eax, eax;
    // cmp ecx, [ebp + 8]; sete al; ret
    F.bytes({0x8b, 0x45, 0xec, 0x8b, 0x00, 0x8b, 0x08, 0x33, 0xc0, 0x3b, 0x4d,
             0x08, 0x0f, 0x94, 0xc0, 0xc3});
  }));
  EXPECT_TRUE(has(Text, "__except (")) << Text;
  EXPECT_TRUE(has(Text, "GetExceptionCode()")) << Text;
  EXPECT_FALSE(has(Text, "sub_")) << Text;
  EXPECT_FALSE(has(Text, "stack_storage")) << Text;
}

TEST(X86SEHDisplay, FilterThatCallsPassesGetExceptionInformation) {
  const std::string Text = decompileGuarded(makeExcept([](Frame &F) {
    // mov eax, [ebp - 0x14]; push eax; call other; add esp, 4; ret
    F.bytes({0x8b, 0x45, 0xec, 0x50});
    F.call(kOther);
    F.bytes({0x83, 0xc4, 0x04, 0xc3});
  }));
  EXPECT_TRUE(has(Text, "__except (other(")) << Text;
  EXPECT_TRUE(has(Text, "GetExceptionInformation()")) << Text;
  EXPECT_FALSE(has(Text, "sub_")) << Text;
}

TEST(X86SEHDisplay, FilterThatWritesPrintsTheWriteBeforeItsValue) {
  const std::string Text = decompileGuarded(makeExcept([](Frame &F) {
    // mov eax, [ebp + 8]; add dword ptr [eax], 8; mov eax, 1; ret
    F.addToArgument(8);
    F.bytes({0xb8, 1, 0, 0, 0, 0xc3});
  }));
  const size_t At = Text.find("__except (");
  ASSERT_NE(At, std::string::npos) << Text;
  const std::string Filter = Text.substr(At, Text.find('\n', At) - At);
  EXPECT_TRUE(has(Filter, " + 8")) << Filter;
  EXPECT_TRUE(has(Filter, ", EXCEPTION_EXECUTE_HANDLER)")) << Filter;
  EXPECT_FALSE(has(Text, "sub_")) << Text;
}

// clang-cl's frame, whose handler comes back to its own EBP and whose filter is
// a function reading what the runtime left in EBP:
// __try { other(); } __except (other(GetExceptionInformation())) { *arg0 += 2;
// }
TEST(X86SEHDisplay, AFrameWithItsRecordInSlotsPrintsItsTry) {
  Frame F;
  F.slotPrologue();
  F.slotLevel(0);
  F.bytes({0x6a, 0x01}); // push 1
  F.call(kOther);
  F.bytes({0x83, 0xc4, 0x04}); // add esp, 4
  F.slotLevel(-1);
  const size_t Past = F.jumpForward();
  const va_t Handler = F.here();
  F.bytes(
      {0x8b, 0x65, 0xe8, 0x83, 0xc5, 0x0c}); // mov esp, [ebp-0x18]; add ebp, 12
  F.slotLevel(-1);
  F.addToArgument(2);
  F.landJump(Past);
  F.slotEpilogue();
  const va_t Filter = F.here();
  // push ebp; mov ebp, esp; mov eax, [ebp]; mov eax, [eax - 0x14]; push eax;
  // call other; add esp, 4; pop ebp; ret
  F.bytes({0x55, 0x89, 0xe5, 0x8b, 0x45, 0x00, 0x8b, 0x40, 0xec, 0x50});
  F.call(kOther);
  F.bytes({0x83, 0xc4, 0x04, 0x5d, 0xc3});
  F.scope(-1, Filter, Handler);
  const std::string Text = decompileGuarded(F.build());
  EXPECT_TRUE(has(Text, "__try {")) << Text;
  EXPECT_TRUE(has(Text, "__except (other(")) << Text;
  EXPECT_TRUE(has(Text, "GetExceptionInformation()")) << Text;
  EXPECT_TRUE(has(Text, " + 2")) << Text;
  // None of the frame's upkeep is left, and nothing of the runtime's stack.
  EXPECT_FALSE(has(Text, "fs_i32")) << Text;
  EXPECT_FALSE(has(Text, "__readfsdword")) << Text;
  EXPECT_FALSE(has(Text, "unknown register")) << Text;
  EXPECT_FALSE(has(Text, "unstructured")) << Text;
  EXPECT_FALSE(has(Text, "sub_")) << Text;
}

// The body of a clang-cl `__finally` is a function of its own, which the
// handler calls saying the unwind and the normal path calls saying not:
// __try { other(); } __finally { other(AbnormalTermination(), frame); }
TEST(X86SEHDisplay, AnOutlinedFinallyIsOneCallInItsClause) {
  Frame F;
  F.slotPrologue();
  F.slotLevel(0);
  F.bytes({0x6a, 0x01}); // push 1
  F.call(kOther);
  F.bytes({0x83, 0xc4, 0x04}); // add esp, 4
  F.slotLevel(-1);
  F.bytes({0x31, 0xc9, 0x89, 0xea}); // xor ecx, ecx; mov edx, ebp
  const size_t Normal = F.callForward();
  F.slotEpilogue();
  const va_t Handler = F.here();
  // push ebp; sub esp, 0x10; add ebp, 12; mov ecx, 1; mov edx, ebp; call body;
  // add esp, 0x10; pop ebp; ret
  F.bytes(
      {0x55, 0x83, 0xec, 0x10, 0x83, 0xc5, 0x0c, 0xb9, 1, 0, 0, 0, 0x89, 0xea});
  const size_t Unwind = F.callForward();
  F.bytes({0x83, 0xc4, 0x10, 0x5d, 0xc3});
  // add dword ptr [edx - 0x28], ecx; ret
  F.landCall(Normal);
  F.landCall(Unwind);
  F.bytes({0x01, 0x4a, 0xd8, 0xc3});
  F.scope(-1, 0, Handler);
  const std::string Text = decompileGuarded(F.build());
  EXPECT_TRUE(has(Text, "__try {")) << Text;
  const std::string Clause = finallyClause(Text);
  EXPECT_TRUE(has(Clause, "/* finally handler @ 0x")) << Text;
  EXPECT_TRUE(has(Clause, "AbnormalTermination()")) << Text;
  // The try's own call stays, and the normal path's is what the clause says.
  EXPECT_EQ(count(Text, "other("), 1u) << Text;
  EXPECT_EQ(count(Text, "AbnormalTermination()"), 1u) << Text;
  EXPECT_EQ(count(Text, "sub_"), 1u) << Text;
  EXPECT_FALSE(has(Text, "unknown register")) << Text;
  EXPECT_FALSE(has(Clause, "return")) << Text;
}

// __try { other(); } __finally { }: the clause keeps its address, and says with
// an empty statement that it does nothing.
TEST(X86SEHDisplay, AFinallyThatDoesNothingKeepsItsAddress) {
  Frame F;
  F.prologue();
  F.level(0);
  F.bytes({0x6a, 0x01}); // push 1
  F.call(kOther);
  F.bytes({0x83, 0xc4, 0x04}); // add esp, 4
  F.level(-1);
  const size_t ToBody = F.callForward();
  const size_t ToEnd = F.jumpForward();
  const va_t Body = F.here();
  F.landCall(ToBody);
  F.bytes({0xc3});
  F.landJump(ToEnd);
  F.epilogue();
  F.scope(-1, 0, Body);
  const std::string Text = decompileGuarded(F.build());
  const std::string Clause = finallyClause(Text);
  EXPECT_TRUE(has(Clause, "/* finally handler @ 0x")) << Text;
  EXPECT_TRUE(has(Clause, ";\n")) << Text;
  // Its `call` went with it.
  EXPECT_FALSE(has(Text, "sub_")) << Text;
}

// A `push ecx` that makes room for a local is no parameter, so a filter's call
// of such a function passes what it pushes and nothing else.
TEST(X86SEHDisplay, APushThatMakesRoomIsNoArgument) {
  Frame F;
  F.prologue();
  F.level(0);
  F.addToArgument(1);
  F.level(-1);
  const size_t Past = F.jumpForward();
  // int callee(void *p) { int local = p != 0; return local; }
  const va_t Callee = F.here();
  // push ebp; mov ebp, esp; push ecx; xor eax, eax; cmp [ebp + 8], eax;
  // setne al; mov [ebp - 4], eax; mov eax, [ebp - 4]; mov esp, ebp; pop ebp;
  // ret
  F.bytes({0x55, 0x8b, 0xec, 0x51, 0x33, 0xc0, 0x39, 0x45, 0x08, 0x0f, 0x95,
           0xc0, 0x89, 0x45, 0xfc, 0x8b, 0x45, 0xfc, 0x8b, 0xe5, 0x5d, 0xc3});
  const va_t Filter = F.here();
  // The register is set before the call, as a caller's code leaves it:
  // mov ecx, [ebp - 0x14]; push ecx; call callee; add esp, 4; ret
  F.bytes({0x8b, 0x4d, 0xec, 0x51});
  F.call(Callee);
  F.bytes({0x83, 0xc4, 0x04, 0xc3});
  const va_t Handler = F.here();
  F.bytes({0x8b, 0x65, 0xe8});
  F.level(-1);
  F.landJump(Past);
  F.epilogue();
  F.scope(-1, Filter, Handler);
  const std::string Text = decompileGuarded(F.build());
  const size_t At = Text.find("__except (");
  ASSERT_NE(At, std::string::npos) << Text;
  const std::string Value = Text.substr(At, Text.find('\n', At) - At);
  EXPECT_TRUE(has(Value, "(GetExceptionInformation()))")) << Text;
  EXPECT_EQ(count(Value, "GetExceptionInformation()"), 1u) << Text;
}

// __except (other(1), EXCEPTION_CONTINUE_SEARCH)
TEST(X86SEHDisplay, FilterKeepsACallWhoseValueNothingReads) {
  const std::string Text = decompileGuarded(makeExcept([](Frame &F) {
    // push 1; call other; add esp, 4; xor eax, eax; ret
    F.bytes({0x6a, 0x01});
    F.call(kOther);
    F.bytes({0x83, 0xc4, 0x04, 0x33, 0xc0, 0xc3});
  }));
  const size_t At = Text.find("__except (");
  ASSERT_NE(At, std::string::npos) << Text;
  const std::string Filter = Text.substr(At, Text.find('\n', At) - At);
  EXPECT_TRUE(has(Filter, "other(")) << Text;
  EXPECT_TRUE(has(Filter, "EXCEPTION_CONTINUE_SEARCH")) << Text;
  EXPECT_FALSE(has(Text, "sub_")) << Text;
}

// __except (other(1), other(2), EXCEPTION_EXECUTE_HANDLER)
TEST(X86SEHDisplay, FilterRunsItsCallsInOrder) {
  const std::string Text = decompileGuarded(makeExcept([](Frame &F) {
    // push 1; call other; add esp, 4; push 2; call other; add esp, 4;
    // mov eax, 1; ret
    F.bytes({0x6a, 0x01});
    F.call(kOther);
    F.bytes({0x83, 0xc4, 0x04, 0x6a, 0x02});
    F.call(kOther);
    F.bytes({0x83, 0xc4, 0x04, 0xb8, 1, 0, 0, 0, 0xc3});
  }));
  const size_t At = Text.find("__except (");
  ASSERT_NE(At, std::string::npos) << Text;
  const std::string Filter = Text.substr(At, Text.find('\n', At) - At);
  EXPECT_EQ(count(Filter, "other("), 2u) << Text;
  EXPECT_LT(Filter.find("other(1"), Filter.find("other(2")) << Text;
  EXPECT_TRUE(has(Filter, "EXCEPTION_EXECUTE_HANDLER")) << Text;
}

// A call's value read after another call: one expression would run them the
// other way round, so the filter is no expression.
TEST(X86SEHDisplay, FilterCannotReorderCalls) {
  const std::string Text = decompileGuarded(makeExcept([](Frame &F) {
    // push 1; call other; add esp, 4; mov esi, eax; push 2; call other;
    // add esp, 4; mov eax, esi; ret
    F.bytes({0x6a, 0x01});
    F.call(kOther);
    F.bytes({0x83, 0xc4, 0x04, 0x8b, 0xf0, 0x6a, 0x02});
    F.call(kOther);
    F.bytes({0x83, 0xc4, 0x04, 0x8b, 0xc6, 0xc3});
  }));
  const size_t At = Text.find("__except (");
  ASSERT_NE(At, std::string::npos) << Text;
  const std::string Filter = Text.substr(At, Text.find('\n', At) - At);
  EXPECT_LE(count(Filter, "other("), 1u) << Text;
  if (has(Filter, "other(2"))
    EXPECT_LT(Filter.find("other(1"), Filter.find("other(2")) << Text;
}

// __try { __try { *arg0 += 1; } __finally { *arg0 += 2; } }
// __finally { *arg0 += 3; }
TEST(X86SEHDisplay, NestedLevelsNestTheirTries) {
  Frame F;
  F.prologue();
  F.level(0);
  F.level(1);
  F.addToArgument(1);
  F.level(0);
  const size_t ToInner = F.callForward();
  const size_t ToOuterEnd = F.jumpForward();
  const va_t Inner = F.here();
  F.landCall(ToInner);
  F.addToArgument(2);
  F.bytes({0xc3});
  F.landJump(ToOuterEnd);
  F.level(-1);
  const size_t ToOuter = F.callForward();
  const size_t ToEnd = F.jumpForward();
  const va_t Outer = F.here();
  F.landCall(ToOuter);
  F.addToArgument(3);
  F.bytes({0xc3});
  F.landJump(ToEnd);
  F.epilogue();
  F.scope(-1, 0, Outer);
  F.scope(0, 0, Inner);
  const std::string Text = decompileGuarded(F.build());
  EXPECT_EQ(count(Text, "__try {"), 2u) << Text;
  EXPECT_EQ(count(Text, "} __finally {"), 2u) << Text;
  const size_t One = Text.find("+ 1");
  const size_t Two = Text.find("+ 2");
  const size_t Three = Text.find("+ 3");
  ASSERT_NE(One, std::string::npos) << Text;
  EXPECT_LT(One, Two) << Text;
  EXPECT_LT(Two, Three) << Text;
  EXPECT_LT(Two, Text.rfind("__finally")) << Text;
  EXPECT_GT(Three, Text.rfind("__finally")) << Text;
  EXPECT_FALSE(has(Text, "sub_")) << Text;
}

// A `return` in a `__finally`: the frame comes down in the body, so it never
// returns to its `call` and what the compiler left after that runs nowhere.
TEST(X86SEHDisplay, AFinallyThatLeavesTheFunctionEndsThePathOfItsCall) {
  Frame F;
  F.prologue();
  F.level(0);
  F.addToArgument(1);
  F.level(-1);
  const size_t ToBody = F.callForward();
  const size_t Dead = F.jumpForward();
  const va_t Body = F.here();
  F.landCall(ToBody);
  F.addToArgument(2);
  F.bytes({0x8b, 0x65, 0xe8}); // mov esp, [ebp - 0x18]
  F.epilogue();
  F.landJump(Dead);
  F.addToArgument(0x20);
  F.epilogue();
  F.scope(-1, 0, Body);
  const std::string Text = decompileGuarded(F.build());
  const size_t Finally = Text.find("__finally");
  ASSERT_NE(Finally, std::string::npos) << Text;
  EXPECT_GT(Text.find("+ 2"), Finally) << Text;
  EXPECT_EQ(count(Text, "+ 2"), 1u) << Text;
  EXPECT_FALSE(has(Text, "+ 32")) << Text;
  EXPECT_FALSE(has(Text, "0x20")) << Text;
  EXPECT_FALSE(has(Text, "sub_")) << Text;
}

// `__leave`-free scope reached by a local whose slot happens to hold what a
// level does: only the frame's own slot is the try level.
TEST(X86SEHDisplay, AnOrdinaryLocalIsNoTryLevel) {
  Frame F;
  F.prologue();
  F.level(0);
  F.addToArgument(1);
  F.bytes({0xc7, 0x45, 0xd8, 0xff, 0xff, 0xff, 0xff}); // [ebp - 0x28] = -1
  F.addToArgument(5);
  F.level(-1);
  const size_t ToBody = F.callForward();
  const size_t ToEnd = F.jumpForward();
  const va_t Body = F.here();
  F.landCall(ToBody);
  F.addToArgument(2);
  F.bytes({0xc3});
  F.landJump(ToEnd);
  F.epilogue();
  F.scope(-1, 0, Body);
  const std::string Text = decompileGuarded(F.build());
  const size_t Finally = Text.find("__finally");
  ASSERT_NE(Finally, std::string::npos) << Text;
  EXPECT_LT(Text.find("+ 5"), Finally) << Text;
}

// /O2 lays a scope's code out around its handlers:
// __try { *arg0 += 1; *arg0 += 8; } __except (1) { *arg0 += 4; }
TEST(X86SEHDisplay, AScopeInPiecesIsOneTry) {
  Frame F;
  F.prologue();
  F.level(0);
  F.addToArgument(1);
  const size_t ToRest = F.jumpForward();
  const va_t Filter = F.here();
  F.bytes({0xb8, 1, 0, 0, 0, 0xc3}); // mov eax, 1; ret
  const va_t Handler = F.here();
  F.bytes({0x8b, 0x65, 0xe8}); // mov esp, [ebp - 0x18]
  F.addToArgument(4);
  F.level(-1);
  const size_t ToEnd = F.jumpForward();
  F.landJump(ToRest);
  F.addToArgument(8);
  F.level(-1);
  F.landJump(ToEnd);
  F.epilogue();
  F.scope(-1, Filter, Handler);
  const std::string Text = decompileGuarded(F.build());
  EXPECT_EQ(count(Text, "__try {"), 1u) << Text;
  const size_t Except = Text.find("__except");
  ASSERT_NE(Except, std::string::npos) << Text;
  EXPECT_LT(Text.find("+ 1"), Except) << Text;
  EXPECT_LT(Text.find("+ 8"), Except) << Text;
  EXPECT_GT(Text.find("+ 4"), Except) << Text;
}

// The scope's entry lies past code it jumps back to.
// __try { *arg0 += 1; *arg0 += 8; } __except (1) { }
TEST(X86SEHDisplay, ATryStartsWhereItIsEntered) {
  Frame F;
  F.prologue();
  const size_t ToStart = F.jumpForward();
  const va_t Tail = F.here();
  F.addToArgument(8);
  F.level(-1);
  const size_t ToEnd = F.jumpForward();
  const va_t Filter = F.here();
  F.bytes({0xb8, 1, 0, 0, 0, 0xc3}); // mov eax, 1; ret
  const va_t Handler = F.here();
  F.bytes({0x8b, 0x65, 0xe8}); // mov esp, [ebp - 0x18]
  F.level(-1);
  const size_t FromArm = F.jumpForward();
  F.landJump(ToStart);
  F.level(0);
  F.addToArgument(1);
  F.bytes({0xeb, static_cast<uint8_t>(Tail - (F.here() + 2))}); // jmp Tail
  F.landJump(ToEnd);
  F.landJump(FromArm);
  F.epilogue();
  F.scope(-1, Filter, Handler);
  const std::string Text = decompileGuarded(F.build());
  EXPECT_EQ(count(Text, "__try {"), 1u) << Text;
  EXPECT_FALSE(has(Text, "unstructured SEH region")) << Text;
  const size_t Try = Text.find("__try {");
  const size_t First = Text.find("+ 1");
  ASSERT_NE(First, std::string::npos) << Text;
  EXPECT_LT(Try, First) << Text;
  EXPECT_LT(First, Text.find("+ 8")) << Text;
  EXPECT_LT(Text.find("+ 8"), Text.find("__except")) << Text;
}

// The entry of the scope lies in a loop that runs under it, where no layout can
// bring it to the front of the `__try`.
TEST(X86SEHDisplay, AnEntryInsideALoopIsAJumpInTheTry) {
  Frame F;
  F.prologue();
  F.bytes({0x8b, 0x4d, 0x0c}); // mov ecx, [ebp + 0xc]
  const size_t ToEntry = F.jumpForward();
  const va_t Head = F.here();
  F.addToArgument(1);
  F.landJump(ToEntry);
  F.level(0);
  F.addToArgument(8);
  F.bytes({0x49, 0x85, 0xc9});                                  // dec; test
  F.bytes({0x75, static_cast<uint8_t>(Head - (F.here() + 2))}); // jne Head
  F.level(-1);
  const size_t ToEnd = F.jumpForward();
  const va_t Filter = F.here();
  F.bytes({0xb8, 1, 0, 0, 0, 0xc3}); // mov eax, 1; ret
  const va_t Handler = F.here();
  F.bytes({0x8b, 0x65, 0xe8}); // mov esp, [ebp - 0x18]
  F.level(-1);
  F.landJump(ToEnd);
  F.epilogue();
  F.scope(-1, Filter, Handler);
  const std::string Text = decompileGuarded(F.build());
  EXPECT_EQ(count(Text, "__try {"), 1u) << Text;
  EXPECT_FALSE(has(Text, "unstructured SEH region")) << Text;
  EXPECT_LT(Text.find("__try {"), Text.find("+ 8")) << Text;
  EXPECT_LT(Text.find("+ 8"), Text.find("__except")) << Text;
}

// __try { __try { *arg0 += 1; } __except (1) { } } __finally { *arg0 += 2; }
TEST(X86SEHDisplay, AnArmThatDoesNothingJumpsNowhere) {
  Frame F;
  F.prologue();
  F.level(0);
  F.level(1);
  F.addToArgument(1);
  F.level(0);
  const size_t ToEnd = F.jumpForward();
  const va_t Filter = F.here();
  F.bytes({0xb8, 1, 0, 0, 0, 0xc3}); // mov eax, 1; ret
  const va_t Handler = F.here();
  F.bytes({0x8b, 0x65, 0xe8}); // mov esp, [ebp - 0x18]
  F.level(0);
  F.landJump(ToEnd);
  F.level(-1);
  const size_t ToBody = F.callForward();
  const size_t ToReturn = F.jumpForward();
  const va_t Body = F.here();
  F.landCall(ToBody);
  F.addToArgument(2);
  F.bytes({0xc3});
  F.landJump(ToReturn);
  F.epilogue();
  F.scope(-1, 0, Body);
  F.scope(0, Filter, Handler);
  const std::string Text = decompileGuarded(F.build());
  EXPECT_TRUE(has(Text, "__except (EXCEPTION_EXECUTE_HANDLER)")) << Text;
  EXPECT_FALSE(has(Text, "goto")) << Text;
}

// __try { *arg0 += 1; } __finally { *arg0 += 2; goto t; } t: *arg0 += 16;
TEST(X86SEHDisplay, AJumpOutOfAFinallyStaysAJump) {
  Frame F;
  F.prologue();
  F.level(0);
  F.addToArgument(1);
  F.level(-1);
  const size_t ToBody = F.callForward();
  const size_t Dead = F.jumpForward();
  const va_t Body = F.here();
  F.landCall(ToBody);
  F.addToArgument(2);
  F.bytes({0x8b, 0x65, 0xe8}); // mov esp, [ebp - 0x18]
  F.level(-1);
  const size_t Out = F.jumpForward();
  F.bytes({0xc3});
  F.landJump(Out);
  F.addToArgument(0x10);
  F.landJump(Dead);
  F.epilogue();
  F.scope(-1, 0, Body);
  const std::string Text = decompileGuarded(F.build());
  const size_t Finally = Text.find("__finally");
  ASSERT_NE(Finally, std::string::npos) << Text;
  const size_t Added = Text.find("+ 2");
  ASSERT_NE(Added, std::string::npos) << Text;
  EXPECT_GT(Added, Finally) << Text;
  // A jump out of a `__finally` stops an unwind: it is what the source says
  // even where the code it lands on comes next.
  const size_t Jump = Text.find("goto ", Added);
  ASSERT_NE(Jump, std::string::npos) << Text;
  const size_t After = Text.find("+ 0x10");
  ASSERT_NE(After, std::string::npos) << Text;
  EXPECT_LT(Jump, After) << Text;
  EXPECT_NE(Text.find("}", Jump), std::string::npos) << Text;
  EXPECT_LT(Text.find("}", Jump), After) << Text;
  EXPECT_FALSE(has(Text, "sub_")) << Text;
}

// __try { other(); } __except (1) { if (other() == 0) { *arg0 += 2; goto out; }
// *arg0 += 4; } *arg0 += 8; out: ;
TEST(X86SEHDisplay, AnArmStartsAtItsHandler) {
  Frame F;
  F.prologue();
  F.level(0);
  F.call(kOther);
  F.level(-1);
  const size_t Past = F.jumpForward();
  const va_t Filter = F.here();
  F.bytes({0xb8, 1, 0, 0, 0, 0xc3});
  const va_t Handler = F.here();
  F.bytes({0x8b, 0x65, 0xe8}); // mov esp, [ebp - 0x18]
  F.level(-1);
  F.call(kOther);
  F.bytes({0x85, 0xc0, 0x74, 0}); // test eax, eax; je Small
  const size_t Small = F.Text.size() - 1;
  F.addToArgument(4);
  F.landJump(Past);
  F.addToArgument(8);
  const size_t Out = F.jumpForward();
  F.landJump(Small);
  F.addToArgument(2);
  F.landJump(Out);
  F.epilogue();
  F.scope(-1, Filter, Handler);
  const std::string Text = decompileGuarded(F.build());
  const size_t Arm = Text.find("__except (");
  ASSERT_NE(Arm, std::string::npos) << Text;
  // The arm's first statement is the handler's call: what the branch jumps to
  // cannot run before it.
  const size_t Call = Text.find("other(", Arm);
  const size_t Two = Text.find("+ 2", Arm);
  ASSERT_NE(Call, std::string::npos) << Text;
  ASSERT_NE(Two, std::string::npos) << Text;
  EXPECT_LT(Call, Two) << Text;
  EXPECT_TRUE(has(Text, "+ 4")) << Text;
  EXPECT_TRUE(has(Text, "+ 8")) << Text;
  EXPECT_FALSE(has(Text, "while (1)")) << Text;
}

// The body of a call that never returns: nothing calls the clause, and the
// handler follows it directly.
// __try { *arg0 += 1; other(); } __finally { *arg0 += 2; }
TEST(X86SEHDisplay, RunningOnIntoAFinallyEndsTheTry) {
  Frame F;
  F.prologue();
  F.level(0);
  F.addToArgument(1);
  F.call(kOther);
  const va_t Body = F.here();
  F.addToArgument(2);
  F.bytes({0xc3});
  F.epilogue();
  F.scope(-1, 0, Body);
  const std::string Text = decompileGuarded(F.build());
  const size_t Finally = Text.find("__finally");
  ASSERT_NE(Finally, std::string::npos) << Text;
  EXPECT_TRUE(has(finallyClause(Text), "+ 2")) << Text;
  EXPECT_LT(Text.find("other("), Finally) << Text;
  EXPECT_FALSE(has(Text, "goto")) << Text;
}

// A loop whose test reads a register runs under the scope it is in, wherever
// the code it leaves for is, and leaving the try each time round would run the
// clause each time:
// __try { while (n) { *arg0 += 1; --n; } } __finally { *arg0 += 2; }
TEST(X86SEHDisplay, ALoopOnARegisterStaysInItsTry) {
  Frame F;
  F.prologue();
  F.bytes({0x8b, 0x4d, 0x0c}); // mov ecx, [ebp + 0xc]
  F.level(0);
  const va_t Head = F.here();
  F.bytes({0x85, 0xc9, 0x74, 0}); // test ecx, ecx; je Out
  const size_t ToOut = F.Text.size() - 1;
  F.addToArgument(1);
  F.bytes({0x49});                                              // dec ecx
  F.bytes({0xeb, static_cast<uint8_t>(Head - (F.here() + 2))}); // jmp Head
  const va_t Body = F.here();
  F.addToArgument(2);
  F.bytes({0xc3});
  F.landJump(ToOut);
  F.level(-1);
  F.call(Body);
  F.epilogue();
  F.scope(-1, 0, Body);
  const std::string Text = decompileGuarded(F.build());
  const size_t Try = Text.find("__try {");
  ASSERT_NE(Try, std::string::npos) << Text;
  const size_t Loop = Text.find("while (");
  ASSERT_NE(Loop, std::string::npos) << Text;
  EXPECT_LT(Try, Loop) << Text;
  EXPECT_EQ(count(Text, "__try {"), 1u) << Text;
  EXPECT_FALSE(has(Text, "sub_")) << Text;
  EXPECT_TRUE(has(finallyClause(Text), "+ 2")) << Text;
}

} // namespace
