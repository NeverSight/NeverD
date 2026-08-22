//===- HuntEngineTests.cpp - Overflow verdicts and witnesses -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/loader/BinaryImageModel.h"
#include "neverd/safety/ArgSlicer.h"
#include "neverd/safety/HuntEngine.h"
#include "neverd/safety/SinkScanner.h"

using namespace neverd;
using namespace neverd::safety;

namespace {

constexpr uint64_t kSP = 0x1000;

MedVar temp(int Id, uint16_t Size = 8) {
  MedVar V;
  V.Kind = MedVar::Temp;
  V.Id = Id;
  V.Size = Size;
  return V;
}
MedVar param(int Id, uint16_t Size = 8) {
  MedVar V;
  V.Kind = MedVar::Param;
  V.Id = Id;
  V.Size = Size;
  return V;
}
MedVar mkReg(uint64_t Off, int Ver, uint16_t Size = 8) {
  MedVar V;
  V.Kind = MedVar::Reg;
  V.Id = static_cast<int>(Off);
  V.RegOff = Off;
  V.SSAVer = Ver;
  V.Size = Size;
  return V;
}

va_t addCString(BinaryImage &Img, llvm::StringRef Value,
                va_t Address = 0x1000) {
  Segment Seg;
  Seg.Name = "data";
  Seg.VA = Address;
  Seg.Flags = SegmentFlags::Readable;
  Seg.Data.assign(Value.bytes_begin(), Value.bytes_end());
  Seg.Data.push_back(0);
  Seg.Size = Seg.Data.size();
  Seg.FileSz = Seg.Data.size();
  Img.Segments.push_back(std::move(Seg));
  return Address;
}

struct Builder {
  MedFunc F;
  explicit Builder(const std::string &Name = "f") {
    F.Entry = 0x100;
    F.Name = Name;
    MedBlock B;
    B.Id = 0;
    F.Blocks.push_back(std::move(B));
  }
  void op(NdOp Op, MedVar Out, std::vector<MedVar> Ins) {
    MedOp O;
    O.Opcode = Op;
    O.Output = Out;
    for (auto &I : Ins)
      O.addInput(I);
    F.Blocks[0].Ops.push_back(O);
  }
  void call(const std::string &Name, MedVar Ret, std::vector<MedVar> Args) {
    int Idx = static_cast<int>(F.Blocks[0].Ops.size());
    MedOp O;
    O.Opcode = NdOp::CALL;
    O.Output = Ret;
    O.addInput(MedVar::makeConst(0x9000, 8));
    F.Blocks[0].Ops.push_back(O);
    MedCallInfo CI;
    CI.BlockId = 0;
    CI.OpIdx = Idx;
    CI.TargetName = Name;
    CI.Args = std::move(Args);
    F.CallInfos.push_back(CI);
  }
};

// Run the hunt over one function and return its first finding, if any.
std::optional<Finding> hunt(MedFunc &F, bool StackRegs = false,
                            LowFunc *LF = nullptr, Arch A = Arch::Unknown,
                            const SafetyBudgets &Budgets = {},
                            BinaryFormat Format = BinaryFormat::Unknown,
                            const BinaryImage *Image = nullptr,
                            const SinkCatalog *Catalog = nullptr) {
  BinaryImage Img = Image ? *Image : BinaryImage{};
  Img.Arch = A;
  Img.Format = Format;
  std::vector<MedFunc> Funcs{F};
  std::vector<LowFunc> Lows;
  if (LF) {
    LF->Entry = F.Entry;
    Lows.push_back(*LF);
  }
  AnalysisInput In;
  In.Img = &Img;
  In.MedFuncs = &Funcs;
  if (!Lows.empty())
    In.LowFuncs = &Lows;
  In.StackRegsKnown = StackRegs;
  In.StackPointerReg = kSP;
  SinkCatalog DefaultCatalog = SinkCatalog::defaults();
  const SinkCatalog &Cat = Catalog ? *Catalog : DefaultCatalog;
  for (const SinkSite &S : scanSinks(In, Cat))
    if (auto Fnd = huntSink(In, Cat, Budgets, Funcs[0], S))
      return Fnd;
  return std::nullopt;
}

LowOp lop(NdOp Opcode, NdVar Output, std::vector<NdVar> Inputs, va_t Addr = 0) {
  LowOp O;
  O.Opcode = Opcode;
  O.Output = Output;
  O.Addr = Addr;
  for (const NdVar &I : Inputs)
    O.addInput(I);
  return O;
}

LowFunc guardedMemcpyLow(va_t MemcpyVA, bool OverflowGuard) {
  constexpr uint64_t kRdx = 16;
  constexpr uint64_t kFlag = 200;
  constexpr va_t kEntry = 0x400000;
  LowFunc LF;
  LF.Entry = kEntry;
  LowBlock B0, B1, B2;
  B0.Id = 0;
  B0.StartAddr = kEntry;
  B0.EndAddr = kEntry + 0x10;
  B1.Id = 1;
  B1.StartAddr = MemcpyVA;
  B1.EndAddr = MemcpyVA + 0x10;
  B2.Id = 2;
  B2.StartAddr = MemcpyVA + 0x10;
  B2.EndAddr = MemcpyVA + 0x20;
  if (OverflowGuard)
    B0.Ops.push_back(lop(NdOp::INT_LESS, NdVar::reg(kFlag, 1),
                         {NdVar::cst(16, 8), NdVar::reg(kRdx, 8)}));
  else
    B0.Ops.push_back(lop(NdOp::INT_LESSEQUAL, NdVar::reg(kFlag, 1),
                         {NdVar::reg(kRdx, 8), NdVar::cst(8, 8)}));
  B0.Ops.push_back(lop(NdOp::COND_BR, NdVar{},
                       {NdVar::cst(MemcpyVA, 8), NdVar::reg(kFlag, 1)}));
  B0.Succs = {1, 2};
  B1.Ops.push_back(lop(NdOp::CALL, NdVar{}, {NdVar::cst(0x9000, 8)}, MemcpyVA));
  B1.Succs = {2};
  B1.Preds = {0};
  B2.Ops.push_back(lop(NdOp::RETURN, NdVar{}, {}));
  B2.Preds = {0, 1};
  LF.Blocks.push_back(std::move(B0));
  LF.Blocks.push_back(std::move(B1));
  LF.Blocks.push_back(std::move(B2));
  return LF;
}

LowFunc solverHeavyGuardedMemcpyLow(va_t MemcpyVA) {
  constexpr uint64_t kRdx = 16;
  constexpr uint64_t kAValue = 201;
  constexpr uint64_t kA = 202;
  constexpr uint64_t kBValue = 203;
  constexpr uint64_t kB = 204;
  constexpr uint64_t kNotB = 205;
  constexpr uint64_t kClause1 = 206;
  constexpr uint64_t kClause2 = 207;
  constexpr uint64_t kFlag = 208;
  constexpr va_t kEntry = 0x400000;
  LowFunc LF;
  LF.Entry = kEntry;
  LowBlock Entry, Sink, Exit;
  Entry.Id = 0;
  Entry.StartAddr = kEntry;
  Entry.EndAddr = kEntry + 0x10;
  Entry.Ops.push_back(lop(NdOp::INT_AND, NdVar::reg(kAValue, 8),
                          {NdVar::reg(kRdx, 8), NdVar::cst(1, 8)}));
  Entry.Ops.push_back(lop(NdOp::INT_NOTEQUAL, NdVar::reg(kA, 1),
                          {NdVar::reg(kAValue, 8), NdVar::cst(0, 8)}));
  Entry.Ops.push_back(lop(NdOp::INT_AND, NdVar::reg(kBValue, 8),
                          {NdVar::reg(24, 8), NdVar::cst(1, 8)}));
  Entry.Ops.push_back(lop(NdOp::INT_NOTEQUAL, NdVar::reg(kB, 1),
                          {NdVar::reg(kBValue, 8), NdVar::cst(0, 8)}));
  Entry.Ops.push_back(
      lop(NdOp::INT_NOT, NdVar::reg(kNotB, 1), {NdVar::reg(kB, 1)}));
  Entry.Ops.push_back(lop(NdOp::INT_OR, NdVar::reg(kClause1, 1),
                          {NdVar::reg(kA, 1), NdVar::reg(kB, 1)}));
  Entry.Ops.push_back(lop(NdOp::INT_OR, NdVar::reg(kClause2, 1),
                          {NdVar::reg(kA, 1), NdVar::reg(kNotB, 1)}));
  Entry.Ops.push_back(lop(NdOp::INT_AND, NdVar::reg(kFlag, 1),
                          {NdVar::reg(kClause1, 1), NdVar::reg(kClause2, 1)}));
  Entry.Ops.push_back(lop(NdOp::COND_BR, NdVar{},
                          {NdVar::cst(MemcpyVA, 8), NdVar::reg(kFlag, 1)}));
  Entry.Succs = {1, 2};
  Sink.Id = 1;
  Sink.StartAddr = MemcpyVA;
  Sink.EndAddr = MemcpyVA + 8;
  Sink.Ops.push_back(
      lop(NdOp::CALL, NdVar{}, {NdVar::cst(0x9000, 8)}, MemcpyVA));
  Sink.Preds = {0};
  Exit.Id = 2;
  Exit.StartAddr = MemcpyVA + 8;
  Exit.EndAddr = MemcpyVA + 16;
  Exit.Ops.push_back(lop(NdOp::RETURN, NdVar{}, {}));
  Exit.Preds = {0};
  LF.Blocks.push_back(std::move(Entry));
  LF.Blocks.push_back(std::move(Sink));
  LF.Blocks.push_back(std::move(Exit));
  return LF;
}

LowFunc fortifiedMemcpyLow(va_t MemcpyVA) {
  constexpr uint64_t kRuntimeCap = 24;
  constexpr uint64_t kFlag = 200;
  constexpr va_t kEntry = 0x400000;
  LowFunc LF;
  LF.Entry = kEntry;
  LowBlock B0, B1, B2;
  B0.Id = 0;
  B0.StartAddr = kEntry;
  B0.EndAddr = kEntry + 0x10;
  B1.Id = 1;
  B1.StartAddr = MemcpyVA;
  B1.EndAddr = MemcpyVA + 0x10;
  B2.Id = 2;
  B2.StartAddr = MemcpyVA + 0x10;
  B2.EndAddr = MemcpyVA + 0x20;
  B0.Ops.push_back(lop(NdOp::INT_LESSEQUAL, NdVar::reg(kFlag, 1),
                       {NdVar::reg(kRuntimeCap, 8), NdVar::cst(8, 8)}));
  B0.Ops.push_back(lop(NdOp::COND_BR, NdVar{},
                       {NdVar::cst(MemcpyVA, 8), NdVar::reg(kFlag, 1)}));
  B0.Succs = {1, 2};
  B1.Ops.push_back(lop(NdOp::CALL, NdVar{}, {NdVar::cst(0x9000, 8)}, MemcpyVA));
  B1.Succs = {2};
  B1.Preds = {0};
  B2.Ops.push_back(lop(NdOp::RETURN, NdVar{}, {}));
  B2.Preds = {0, 1};
  LF.Blocks.push_back(std::move(B0));
  LF.Blocks.push_back(std::move(B1));
  LF.Blocks.push_back(std::move(B2));
  return LF;
}

LowFunc strlenGuardedStrcpyLow(va_t StrlenVA, va_t StrcpyVA,
                               bool OverflowGuard) {
  constexpr uint64_t kRax = 0;
  constexpr uint64_t kFlag = 200;
  constexpr va_t kEntry = 0x400000;
  LowFunc LF;
  LF.Entry = kEntry;
  LowBlock B0, B1, B2;
  B0.Id = 0;
  B0.StartAddr = kEntry;
  B0.EndAddr = StrlenVA + 8;
  B1.Id = 1;
  B1.StartAddr = StrcpyVA;
  B1.EndAddr = StrcpyVA + 0x10;
  B2.Id = 2;
  B2.StartAddr = StrcpyVA + 0x10;
  B2.EndAddr = StrcpyVA + 0x20;
  B0.Ops.push_back(lop(NdOp::CALL, NdVar{}, {NdVar::cst(0x9100, 8)}, StrlenVA));
  if (OverflowGuard)
    B0.Ops.push_back(lop(NdOp::INT_LESS, NdVar::reg(kFlag, 1),
                         {NdVar::cst(16, 8), NdVar::reg(kRax, 8)}));
  else
    B0.Ops.push_back(lop(NdOp::INT_LESS, NdVar::reg(kFlag, 1),
                         {NdVar::reg(kRax, 8), NdVar::cst(16, 8)}));
  B0.Ops.push_back(lop(NdOp::COND_BR, NdVar{},
                       {NdVar::cst(StrcpyVA, 8), NdVar::reg(kFlag, 1)}));
  B0.Succs = {1, 2};
  B1.Ops.push_back(lop(NdOp::CALL, NdVar{}, {NdVar::cst(0x9000, 8)}, StrcpyVA));
  B1.Succs = {2};
  B1.Preds = {0};
  B2.Ops.push_back(lop(NdOp::RETURN, NdVar{}, {}));
  B2.Preds = {0, 1};
  LF.Blocks.push_back(std::move(B0));
  LF.Blocks.push_back(std::move(B1));
  LF.Blocks.push_back(std::move(B2));
  return LF;
}

LowFunc strlenThenStoreGuardedStrcpyLow(va_t StrlenVA, va_t StrcpyVA) {
  LowFunc LF = strlenGuardedStrcpyLow(StrlenVA, StrcpyVA,
                                      /*OverflowGuard=*/true);
  LF.Blocks[0].Ops.insert(std::next(LF.Blocks[0].Ops.begin()),
                          lop(NdOp::STORE, NdVar{},
                              {NdVar::cst(0x7000, 8), NdVar::cst(1, 1)},
                              StrlenVA + 1));
  return LF;
}

LowFunc reachableSinkLow(va_t SinkVA) {
  LowFunc LF;
  LowBlock B;
  B.Id = 0;
  B.StartAddr = SinkVA;
  B.EndAddr = SinkVA + 8;
  B.Ops.push_back(lop(NdOp::CALL, NdVar{}, {NdVar::cst(0x9000, 8)}, SinkVA));
  B.Ops.push_back(lop(NdOp::RETURN, NdVar{}, {}));
  LF.Blocks.push_back(std::move(B));
  return LF;
}

LowFunc mutuallyExclusiveSourceAndSinkLow(va_t MallocVA, va_t SourceVA,
                                          va_t SinkVA) {
  constexpr uint64_t kConditionInput = 24;
  constexpr uint64_t kFlag = 96;
  constexpr va_t kEntry = 0x400000;
  constexpr va_t kBypass = 0x400020;
  constexpr va_t kJoin = 0x400030;
  constexpr va_t kExit = 0x400040;

  LowFunc LF;
  LF.Entry = kEntry;
  LF.Blocks.resize(6);
  for (int I = 0; I < 6; ++I)
    LF.Blocks[I].Id = I;

  LowBlock &Entry = LF.Blocks[0];
  Entry.StartAddr = kEntry;
  Entry.EndAddr = SourceVA;
  Entry.Ops.push_back(
      lop(NdOp::CALL, NdVar::reg(0, 8), {NdVar::cst(0x9000, 8)}, MallocVA));
  Entry.Ops.push_back(lop(NdOp::INT_NOTEQUAL, NdVar::reg(kFlag, 1),
                          {NdVar::reg(kConditionInput, 8), NdVar::cst(0, 8)},
                          kEntry + 4));
  Entry.Ops.push_back(lop(NdOp::COND_BR, NdVar{},
                          {NdVar::cst(SourceVA, 8), NdVar::reg(kFlag, 1)},
                          kEntry + 8));
  Entry.Succs = {1, 2};

  LowBlock &Source = LF.Blocks[1];
  Source.StartAddr = SourceVA;
  Source.EndAddr = kBypass;
  Source.Preds = {0};
  Source.Succs = {3};
  Source.Ops.push_back(
      lop(NdOp::CALL, NdVar::reg(0, 8), {NdVar::cst(0x9000, 8)}, SourceVA));
  Source.Ops.push_back(
      lop(NdOp::BRANCH, NdVar{}, {NdVar::cst(kJoin, 8)}, SourceVA + 4));

  LowBlock &Bypass = LF.Blocks[2];
  Bypass.StartAddr = kBypass;
  Bypass.EndAddr = kJoin;
  Bypass.Preds = {0};
  Bypass.Succs = {3};
  Bypass.Ops.push_back(
      lop(NdOp::BRANCH, NdVar{}, {NdVar::cst(kJoin, 8)}, kBypass));

  LowBlock &Join = LF.Blocks[3];
  Join.StartAddr = kJoin;
  Join.EndAddr = kExit;
  Join.Preds = {1, 2};
  Join.Succs = {4, 5};
  Join.Ops.push_back(lop(NdOp::COND_BR, NdVar{},
                         {NdVar::cst(kExit, 8), NdVar::reg(kFlag, 1)}, kJoin));

  LowBlock &Exit = LF.Blocks[4];
  Exit.StartAddr = kExit;
  Exit.EndAddr = SinkVA;
  Exit.Preds = {3};
  Exit.Ops.push_back(lop(NdOp::RETURN, NdVar{}, {}, kExit));

  LowBlock &Sink = LF.Blocks[5];
  Sink.StartAddr = SinkVA;
  Sink.EndAddr = SinkVA + 8;
  Sink.Preds = {3};
  Sink.Ops.push_back(lop(NdOp::CALL, NdVar{}, {NdVar::cst(0x9000, 8)}, SinkVA));
  Sink.Ops.push_back(lop(NdOp::RETURN, NdVar{}, {}, SinkVA + 4));
  return LF;
}

LowFunc sourceReturnThenMemcpyLow(va_t SourceVA, va_t MemcpyVA,
                                  bool RequireNonnegative,
                                  uint16_t ReturnBytes = 8,
                                  uint64_t CopyLengthReg = 16,
                                  uint16_t CallOutputBytes = 8) {
  constexpr uint64_t kRax = 0;
  constexpr uint64_t kFlag = 200;
  constexpr va_t kEntry = 0x400000;
  LowFunc LF;
  LF.Entry = kEntry;
  LowBlock Entry, Sink, Exit;
  Entry.Id = 0;
  Entry.StartAddr = kEntry;
  Entry.EndAddr = MemcpyVA;
  Entry.Ops.push_back(lop(NdOp::CALL, NdVar::reg(kRax, CallOutputBytes),
                          {NdVar::cst(0x9000, 8)}, SourceVA));
  NdVar ComparedReturn = NdVar::reg(kRax, CallOutputBytes);
  if (ReturnBytes == 4) {
    Entry.Ops.push_back(lop(
        NdOp::SUBBYTES, NdVar::reg(kRax, 4),
        {NdVar::reg(kRax, CallOutputBytes), NdVar::cst(0, 4)}, SourceVA + 1));
    Entry.Ops.push_back(lop(NdOp::INT_SEXT, NdVar::reg(CopyLengthReg, 8),
                            {NdVar::reg(kRax, 4)}, SourceVA + 2));
    ComparedReturn = NdVar::reg(kRax, 4);
  } else {
    Entry.Ops.push_back(lop(NdOp::COPY, NdVar::reg(CopyLengthReg, 8),
                            {NdVar::reg(kRax, 8)}, SourceVA + 1));
  }
  if (RequireNonnegative) {
    Entry.Ops.push_back(lop(NdOp::INT_SLESSEQUAL, NdVar::reg(kFlag, 1),
                            {NdVar::cst(0, ReturnBytes), ComparedReturn},
                            SourceVA + 3));
    Entry.Ops.push_back(lop(NdOp::COND_BR, NdVar{},
                            {NdVar::cst(MemcpyVA, 8), NdVar::reg(kFlag, 1)},
                            SourceVA + 4));
    Entry.Succs = {1, 2};
  } else {
    Entry.Ops.push_back(
        lop(NdOp::BRANCH, NdVar{}, {NdVar::cst(MemcpyVA, 8)}, SourceVA + 4));
    Entry.Succs = {1};
  }
  Sink.Id = 1;
  Sink.StartAddr = MemcpyVA;
  Sink.EndAddr = MemcpyVA + 8;
  Sink.Ops.push_back(
      lop(NdOp::CALL, NdVar{}, {NdVar::cst(0x9000, 8)}, MemcpyVA));
  Sink.Succs = {2};
  Sink.Preds = {0};
  Exit.Id = 2;
  Exit.StartAddr = MemcpyVA + 8;
  Exit.EndAddr = MemcpyVA + 16;
  Exit.Ops.push_back(lop(NdOp::RETURN, NdVar{}, {}));
  Exit.Preds =
      RequireNonnegative ? std::vector<int>{0, 1} : std::vector<int>{1};
  LF.Blocks.push_back(std::move(Entry));
  LF.Blocks.push_back(std::move(Sink));
  LF.Blocks.push_back(std::move(Exit));
  return LF;
}

LowFunc sourceBufferScalarThenMemcpyLow(va_t SourceVA, va_t MemcpyVA,
                                        va_t OutputBufferVA,
                                        uint64_t RequiredWrittenBytes,
                                        uint16_t ScalarBytes = 8,
                                        uint64_t LoadOffset = 0) {
  constexpr uint64_t kRax = 0;
  constexpr uint64_t kCopyLengthReg = 16;
  constexpr uint64_t kFlag = 200;
  constexpr va_t kEntry = 0x400000;
  LowFunc LF;
  LF.Entry = kEntry;
  LowBlock Entry, Sink, Exit;
  Entry.Id = 0;
  Entry.StartAddr = kEntry;
  Entry.EndAddr = MemcpyVA;
  Entry.Ops.push_back(
      lop(NdOp::CALL, NdVar::reg(kRax, 8), {NdVar::cst(0x9000, 8)}, SourceVA));
  Entry.Ops.push_back(
      lop(NdOp::INT_EQUAL, NdVar::reg(kFlag, 1),
          {NdVar::reg(kRax, 8), NdVar::cst(RequiredWrittenBytes, 8)},
          SourceVA + 1));
  Entry.Ops.push_back(lop(NdOp::COND_BR, NdVar{},
                          {NdVar::cst(MemcpyVA, 8), NdVar::reg(kFlag, 1)},
                          SourceVA + 2));
  Entry.Succs = {1, 2};

  Sink.Id = 1;
  Sink.StartAddr = MemcpyVA;
  Sink.EndAddr = MemcpyVA + 8;
  Sink.Preds = {0};
  Sink.Succs = {2};
  Sink.Ops.push_back(lop(NdOp::LOAD, NdVar::reg(kCopyLengthReg, ScalarBytes),
                         {NdVar::cst(OutputBufferVA + LoadOffset, 8)},
                         SourceVA + 3));
  if (ScalarBytes < 8)
    Sink.Ops.push_back(lop(NdOp::INT_ZEXT, NdVar::reg(kCopyLengthReg, 8),
                           {NdVar::reg(kCopyLengthReg, ScalarBytes)},
                           SourceVA + 4));
  Sink.Ops.push_back(
      lop(NdOp::CALL, NdVar{}, {NdVar::cst(0x9000, 8)}, MemcpyVA));

  Exit.Id = 2;
  Exit.StartAddr = MemcpyVA + 8;
  Exit.EndAddr = MemcpyVA + 16;
  Exit.Preds = {0, 1};
  Exit.Ops.push_back(lop(NdOp::RETURN, NdVar{}, {}, MemcpyVA + 8));
  LF.Blocks.push_back(std::move(Entry));
  LF.Blocks.push_back(std::move(Sink));
  LF.Blocks.push_back(std::move(Exit));
  return LF;
}

LowFunc overlappingReadsThenBufferScalarLow(va_t FirstReadVA, va_t SecondReadVA,
                                            va_t MemcpyVA, va_t BufferVA) {
  constexpr uint64_t kRax = 0;
  constexpr uint64_t kFirstMatches = 200;
  constexpr uint64_t kSecondMatches = 201;
  constexpr uint64_t kPath = 202;
  constexpr uint64_t kCopyLengthReg = 16;
  constexpr va_t kEntry = 0x400000;
  LowFunc LF;
  LF.Entry = kEntry;
  LowBlock Entry, Sink, Exit;
  Entry.Id = 0;
  Entry.StartAddr = kEntry;
  Entry.EndAddr = MemcpyVA;
  Entry.Ops.push_back(lop(NdOp::CALL, NdVar::reg(kRax, 8),
                          {NdVar::cst(0x9000, 8)}, FirstReadVA));
  Entry.Ops.push_back(lop(NdOp::INT_EQUAL, NdVar::reg(kFirstMatches, 1),
                          {NdVar::reg(kRax, 8), NdVar::cst(8, 8)},
                          FirstReadVA + 1));
  Entry.Ops.push_back(lop(NdOp::CALL, NdVar::reg(kRax, 8),
                          {NdVar::cst(0x9000, 8)}, SecondReadVA));
  Entry.Ops.push_back(lop(NdOp::INT_EQUAL, NdVar::reg(kSecondMatches, 1),
                          {NdVar::reg(kRax, 8), NdVar::cst(1, 8)},
                          SecondReadVA + 1));
  Entry.Ops.push_back(
      lop(NdOp::INT_AND, NdVar::reg(kPath, 1),
          {NdVar::reg(kFirstMatches, 1), NdVar::reg(kSecondMatches, 1)},
          SecondReadVA + 2));
  Entry.Ops.push_back(lop(NdOp::COND_BR, NdVar{},
                          {NdVar::cst(MemcpyVA, 8), NdVar::reg(kPath, 1)},
                          SecondReadVA + 3));
  Entry.Succs = {1, 2};
  Sink.Id = 1;
  Sink.StartAddr = MemcpyVA;
  Sink.EndAddr = MemcpyVA + 8;
  Sink.Preds = {0};
  Sink.Succs = {2};
  Sink.Ops.push_back(lop(NdOp::LOAD, NdVar::reg(kCopyLengthReg, 8),
                         {NdVar::cst(BufferVA, 8)}, SecondReadVA + 4));
  Sink.Ops.push_back(
      lop(NdOp::CALL, NdVar{}, {NdVar::cst(0x9000, 8)}, MemcpyVA));
  Exit.Id = 2;
  Exit.StartAddr = MemcpyVA + 8;
  Exit.EndAddr = MemcpyVA + 16;
  Exit.Preds = {0, 1};
  Exit.Ops.push_back(lop(NdOp::RETURN, NdVar{}, {}, MemcpyVA + 8));
  LF.Blocks.push_back(std::move(Entry));
  LF.Blocks.push_back(std::move(Sink));
  LF.Blocks.push_back(std::move(Exit));
  return LF;
}

LowFunc loadBeforeSourceInLoopLow(va_t MallocVA, va_t SourceVA, va_t MemcpyVA,
                                  va_t OutputBufferVA) {
  constexpr uint64_t kRax = 0;
  constexpr uint64_t kSavedLengthReg = 24;
  constexpr uint64_t kCopyLengthReg = 16;
  LowFunc LF;
  LF.Entry = MallocVA;
  LowBlock Entry, Sink;
  Entry.Id = 0;
  Entry.StartAddr = MallocVA;
  Entry.EndAddr = SourceVA;
  Entry.Preds = {1};
  Entry.Succs = {1};
  Entry.Ops.push_back(
      lop(NdOp::CALL, NdVar::reg(kRax, 8), {NdVar::cst(0x9000, 8)}, MallocVA));
  Entry.Ops.push_back(lop(NdOp::LOAD, NdVar::reg(kSavedLengthReg, 8),
                          {NdVar::cst(OutputBufferVA, 8)}, SourceVA - 1));
  Entry.Ops.push_back(
      lop(NdOp::BRANCH, NdVar{}, {NdVar::cst(SourceVA, 8)}, SourceVA - 1));

  Sink.Id = 1;
  Sink.StartAddr = SourceVA;
  Sink.EndAddr = MemcpyVA + 8;
  Sink.Preds = {0};
  Sink.Succs = {0};
  Sink.Ops.push_back(
      lop(NdOp::CALL, NdVar::reg(kRax, 8), {NdVar::cst(0x9000, 8)}, SourceVA));
  Sink.Ops.push_back(lop(NdOp::COPY, NdVar::reg(kCopyLengthReg, 8),
                         {NdVar::reg(kSavedLengthReg, 8)}, SourceVA + 1));
  Sink.Ops.push_back(
      lop(NdOp::CALL, NdVar{}, {NdVar::cst(0x9000, 8)}, MemcpyVA));
  LF.Blocks.push_back(std::move(Entry));
  LF.Blocks.push_back(std::move(Sink));
  return LF;
}

LowFunc sourceAfterSinkLoopLow(va_t MallocVA, va_t SinkVA, va_t SourceVA,
                               va_t SourceBufferVA) {
  constexpr uint64_t kSourceBufferReg = 24;
  LowFunc LF;
  LF.Entry = MallocVA;

  LowBlock Entry;
  Entry.Id = 0;
  Entry.StartAddr = MallocVA;
  Entry.EndAddr = SinkVA;
  Entry.Succs = {1};
  Entry.Ops.push_back(
      lop(NdOp::CALL, NdVar::reg(0, 8), {NdVar::cst(0x9000, 8)}, MallocVA));
  Entry.Ops.push_back(
      lop(NdOp::BRANCH, NdVar{}, {NdVar::cst(SinkVA, 8)}, MallocVA + 4));

  LowBlock Loop;
  Loop.Id = 1;
  Loop.StartAddr = SinkVA;
  Loop.EndAddr = SourceVA + 8;
  Loop.Preds = {0, 1};
  Loop.Succs = {1};
  Loop.Ops.push_back(lop(NdOp::COPY, NdVar::reg(kSourceBufferReg, 8),
                         {NdVar::cst(SourceBufferVA, 8)}, SinkVA));
  Loop.Ops.push_back(lop(NdOp::CALL, NdVar{}, {NdVar::cst(0x9000, 8)}, SinkVA));
  Loop.Ops.push_back(lop(NdOp::COPY, NdVar::reg(kSourceBufferReg, 8),
                         {NdVar::cst(SourceBufferVA, 8)}, SourceVA));
  Loop.Ops.push_back(
      lop(NdOp::CALL, NdVar::reg(0, 8), {NdVar::cst(0x9000, 8)}, SourceVA));
  Loop.Ops.push_back(
      lop(NdOp::BRANCH, NdVar{}, {NdVar::cst(SinkVA, 8)}, SourceVA + 4));

  LF.Blocks.push_back(std::move(Entry));
  LF.Blocks.push_back(std::move(Loop));
  return LF;
}

LowFunc readFileThenMemcpyLow(va_t SourceVA, va_t MemcpyVA, va_t BytesReadVA,
                              uint64_t CopyLengthReg = 24) {
  constexpr uint64_t kRax = 0;
  constexpr va_t kEntry = 0x400000;
  LowFunc LF;
  LF.Entry = kEntry;
  LowBlock Block;
  Block.Id = 0;
  Block.StartAddr = kEntry;
  Block.EndAddr = MemcpyVA + 8;
  Block.Ops.push_back(
      lop(NdOp::CALL, NdVar::reg(kRax, 8), {NdVar::cst(0x9000, 8)}, SourceVA));
  Block.Ops.push_back(lop(NdOp::LOAD, NdVar::reg(CopyLengthReg, 4),
                          {NdVar::cst(BytesReadVA, 8)}, SourceVA + 4));
  Block.Ops.push_back(
      lop(NdOp::CALL, NdVar{}, {NdVar::cst(0x9000, 8)}, MemcpyVA));
  Block.Ops.push_back(lop(NdOp::RETURN, NdVar{}, {}, MemcpyVA + 4));
  LF.Blocks.push_back(std::move(Block));
  return LF;
}

LowFunc readFileBufferScalarThenMemcpyLow(va_t SourceVA, va_t MemcpyVA,
                                          va_t OutputBufferVA, va_t BytesReadVA,
                                          uint32_t RequiredWrittenBytes) {
  constexpr uint64_t kRax = 0;
  constexpr uint64_t kCount = 24;
  const uint64_t kCopyLengthReg = getTargetRegInfo(Arch::X64).Win64ParamRegs[2];
  constexpr uint64_t kReturnMatches = 200;
  constexpr uint64_t kCountMatches = 201;
  constexpr uint64_t kSuccess = 202;
  constexpr va_t kEntry = 0x400000;
  LowFunc LF;
  LF.Entry = kEntry;
  LowBlock Entry, Sink, Exit;
  Entry.Id = 0;
  Entry.StartAddr = kEntry;
  Entry.EndAddr = MemcpyVA;
  Entry.Ops.push_back(
      lop(NdOp::CALL, NdVar::reg(kRax, 8), {NdVar::cst(0x9000, 8)}, SourceVA));
  Entry.Ops.push_back(lop(NdOp::SUBBYTES, NdVar::reg(kRax, 4),
                          {NdVar::reg(kRax, 8), NdVar::cst(0, 4)},
                          SourceVA + 1));
  Entry.Ops.push_back(lop(NdOp::LOAD, NdVar::reg(kCount, 4),
                          {NdVar::cst(BytesReadVA, 8)}, SourceVA + 2));
  Entry.Ops.push_back(lop(NdOp::INT_EQUAL, NdVar::reg(kReturnMatches, 1),
                          {NdVar::reg(kRax, 4), NdVar::cst(1, 4)},
                          SourceVA + 3));
  Entry.Ops.push_back(
      lop(NdOp::INT_EQUAL, NdVar::reg(kCountMatches, 1),
          {NdVar::reg(kCount, 4), NdVar::cst(RequiredWrittenBytes, 4)},
          SourceVA + 4));
  Entry.Ops.push_back(
      lop(NdOp::INT_AND, NdVar::reg(kSuccess, 1),
          {NdVar::reg(kReturnMatches, 1), NdVar::reg(kCountMatches, 1)},
          SourceVA + 5));
  Entry.Ops.push_back(lop(NdOp::COND_BR, NdVar{},
                          {NdVar::cst(MemcpyVA, 8), NdVar::reg(kSuccess, 1)},
                          SourceVA + 6));
  Entry.Succs = {1, 2};

  Sink.Id = 1;
  Sink.StartAddr = MemcpyVA;
  Sink.EndAddr = MemcpyVA + 8;
  Sink.Preds = {0};
  Sink.Succs = {2};
  Sink.Ops.push_back(lop(NdOp::LOAD, NdVar::reg(kCopyLengthReg, 8),
                         {NdVar::cst(OutputBufferVA, 8)}, SourceVA + 7));
  Sink.Ops.push_back(
      lop(NdOp::CALL, NdVar{}, {NdVar::cst(0x9000, 8)}, MemcpyVA));

  Exit.Id = 2;
  Exit.StartAddr = MemcpyVA + 8;
  Exit.EndAddr = MemcpyVA + 16;
  Exit.Preds = {0, 1};
  Exit.Ops.push_back(lop(NdOp::RETURN, NdVar{}, {}, MemcpyVA + 8));
  LF.Blocks.push_back(std::move(Entry));
  LF.Blocks.push_back(std::move(Sink));
  LF.Blocks.push_back(std::move(Exit));
  return LF;
}

LowFunc readFileFailureThenSinkLow(va_t SourceVA, va_t SinkVA, va_t BytesReadVA,
                                   bool LoadCount,
                                   uint64_t CopyLengthReg = 24) {
  constexpr uint64_t kRax = 0;
  constexpr uint64_t kFlag = 200;
  constexpr va_t kEntry = 0x400000;
  LowFunc LF;
  LF.Entry = kEntry;
  LowBlock Entry, Sink, Exit;
  Entry.Id = 0;
  Entry.StartAddr = kEntry;
  Entry.EndAddr = SinkVA;
  Entry.Ops.push_back(
      lop(NdOp::CALL, NdVar::reg(kRax, 8), {NdVar::cst(0x9000, 8)}, SourceVA));
  Entry.Ops.push_back(lop(NdOp::SUBBYTES, NdVar::reg(kRax, 4),
                          {NdVar::reg(kRax, 8), NdVar::cst(0, 4)},
                          SourceVA + 2));
  Entry.Ops.push_back(lop(NdOp::INT_EQUAL, NdVar::reg(kFlag, 1),
                          {NdVar::reg(kRax, 4), NdVar::cst(0, 4)},
                          SourceVA + 4));
  Entry.Ops.push_back(lop(NdOp::COND_BR, NdVar{},
                          {NdVar::cst(SinkVA, 8), NdVar::reg(kFlag, 1)},
                          SourceVA + 8));
  Entry.Succs = {1, 2};

  Sink.Id = 1;
  Sink.StartAddr = SinkVA;
  Sink.EndAddr = SinkVA + 8;
  Sink.Preds = {0};
  Sink.Succs = {2};
  if (LoadCount)
    Sink.Ops.push_back(lop(NdOp::LOAD, NdVar::reg(CopyLengthReg, 4),
                           {NdVar::cst(BytesReadVA, 8)}, SinkVA - 4));
  Sink.Ops.push_back(lop(NdOp::CALL, NdVar{}, {NdVar::cst(0x9000, 8)}, SinkVA));

  Exit.Id = 2;
  Exit.StartAddr = SinkVA + 8;
  Exit.EndAddr = SinkVA + 16;
  Exit.Preds = {0, 1};
  Exit.Ops.push_back(lop(NdOp::RETURN, NdVar{}, {}, SinkVA + 8));
  LF.Blocks.push_back(std::move(Entry));
  LF.Blocks.push_back(std::move(Sink));
  LF.Blocks.push_back(std::move(Exit));
  return LF;
}

LowFunc readFileResultThenFormatLow(va_t SourceVA, va_t SinkVA,
                                    va_t BytesReadVA, uint32_t ReturnValue,
                                    uint32_t BytesRead) {
  constexpr uint64_t kRax = 0;
  constexpr uint64_t kCount = 24;
  constexpr uint64_t kReturnMatches = 200;
  constexpr uint64_t kCountMatches = 201;
  constexpr uint64_t kSuccess = 202;
  constexpr va_t kEntry = 0x400000;
  LowFunc LF;
  LF.Entry = kEntry;
  LowBlock Entry, Sink, Exit;
  Entry.Id = 0;
  Entry.StartAddr = kEntry;
  Entry.EndAddr = SinkVA;
  Entry.Ops.push_back(
      lop(NdOp::CALL, NdVar::reg(kRax, 8), {NdVar::cst(0x9000, 8)}, SourceVA));
  Entry.Ops.push_back(lop(NdOp::SUBBYTES, NdVar::reg(kRax, 4),
                          {NdVar::reg(kRax, 8), NdVar::cst(0, 4)},
                          SourceVA + 1));
  Entry.Ops.push_back(lop(NdOp::LOAD, NdVar::reg(kCount, 4),
                          {NdVar::cst(BytesReadVA, 8)}, SourceVA + 2));
  Entry.Ops.push_back(lop(NdOp::INT_EQUAL, NdVar::reg(kReturnMatches, 1),
                          {NdVar::reg(kRax, 4), NdVar::cst(ReturnValue, 4)},
                          SourceVA + 3));
  Entry.Ops.push_back(lop(NdOp::INT_EQUAL, NdVar::reg(kCountMatches, 1),
                          {NdVar::reg(kCount, 4), NdVar::cst(BytesRead, 4)},
                          SourceVA + 4));
  Entry.Ops.push_back(
      lop(NdOp::INT_AND, NdVar::reg(kSuccess, 1),
          {NdVar::reg(kReturnMatches, 1), NdVar::reg(kCountMatches, 1)},
          SourceVA + 5));
  Entry.Ops.push_back(lop(NdOp::COND_BR, NdVar{},
                          {NdVar::cst(SinkVA, 8), NdVar::reg(kSuccess, 1)},
                          SourceVA + 6));
  Entry.Succs = {1, 2};
  Sink.Id = 1;
  Sink.StartAddr = SinkVA;
  Sink.EndAddr = SinkVA + 8;
  Sink.Ops.push_back(lop(NdOp::CALL, NdVar{}, {NdVar::cst(0x9000, 8)}, SinkVA));
  Sink.Succs = {2};
  Sink.Preds = {0};
  Exit.Id = 2;
  Exit.StartAddr = SinkVA + 8;
  Exit.EndAddr = SinkVA + 16;
  Exit.Ops.push_back(lop(NdOp::RETURN, NdVar{}, {}));
  Exit.Preds = {0, 1};
  LF.Blocks.push_back(std::move(Entry));
  LF.Blocks.push_back(std::move(Sink));
  LF.Blocks.push_back(std::move(Exit));
  return LF;
}

LowFunc repeatedReadFileThenFormatLow(va_t FirstSourceVA, va_t SecondSourceVA,
                                      va_t SinkVA, va_t FirstBytesReadVA,
                                      va_t SecondBytesReadVA) {
  constexpr uint64_t kRax = 0;
  constexpr uint64_t kFirstCount = 24;
  constexpr uint64_t kSecondCount = 28;
  constexpr uint64_t kFirstReturnMatches = 200;
  constexpr uint64_t kFirstCountMatches = 201;
  constexpr uint64_t kSecondReturnMatches = 202;
  constexpr uint64_t kSecondCountMatches = 203;
  constexpr uint64_t kFirstProducedInput = 204;
  constexpr uint64_t kSecondProducedNoInput = 205;
  constexpr uint64_t kPath = 206;
  constexpr va_t kEntry = 0x400000;
  LowFunc LF;
  LF.Entry = kEntry;
  LowBlock Entry, Sink, Exit;
  Entry.Id = 0;
  Entry.StartAddr = kEntry;
  Entry.EndAddr = SinkVA;
  Entry.Ops.push_back(lop(NdOp::CALL, NdVar::reg(kRax, 8),
                          {NdVar::cst(0x9000, 8)}, FirstSourceVA));
  Entry.Ops.push_back(lop(NdOp::SUBBYTES, NdVar::reg(kRax, 4),
                          {NdVar::reg(kRax, 8), NdVar::cst(0, 4)},
                          FirstSourceVA + 1));
  Entry.Ops.push_back(lop(NdOp::LOAD, NdVar::reg(kFirstCount, 4),
                          {NdVar::cst(FirstBytesReadVA, 8)},
                          FirstSourceVA + 2));
  Entry.Ops.push_back(lop(NdOp::INT_EQUAL, NdVar::reg(kFirstReturnMatches, 1),
                          {NdVar::reg(kRax, 4), NdVar::cst(1, 4)},
                          FirstSourceVA + 3));
  Entry.Ops.push_back(lop(NdOp::INT_EQUAL, NdVar::reg(kFirstCountMatches, 1),
                          {NdVar::reg(kFirstCount, 4), NdVar::cst(1, 4)},
                          FirstSourceVA + 4));
  Entry.Ops.push_back(lop(
      NdOp::INT_AND, NdVar::reg(kFirstProducedInput, 1),
      {NdVar::reg(kFirstReturnMatches, 1), NdVar::reg(kFirstCountMatches, 1)},
      FirstSourceVA + 5));
  Entry.Ops.push_back(lop(NdOp::CALL, NdVar::reg(kRax, 8),
                          {NdVar::cst(0x9000, 8)}, SecondSourceVA));
  Entry.Ops.push_back(lop(NdOp::SUBBYTES, NdVar::reg(kRax, 4),
                          {NdVar::reg(kRax, 8), NdVar::cst(0, 4)},
                          SecondSourceVA + 1));
  Entry.Ops.push_back(lop(NdOp::LOAD, NdVar::reg(kSecondCount, 4),
                          {NdVar::cst(SecondBytesReadVA, 8)},
                          SecondSourceVA + 2));
  Entry.Ops.push_back(lop(NdOp::INT_EQUAL, NdVar::reg(kSecondReturnMatches, 1),
                          {NdVar::reg(kRax, 4), NdVar::cst(1, 4)},
                          SecondSourceVA + 3));
  Entry.Ops.push_back(lop(NdOp::INT_EQUAL, NdVar::reg(kSecondCountMatches, 1),
                          {NdVar::reg(kSecondCount, 4), NdVar::cst(0, 4)},
                          SecondSourceVA + 4));
  Entry.Ops.push_back(lop(
      NdOp::INT_AND, NdVar::reg(kSecondProducedNoInput, 1),
      {NdVar::reg(kSecondReturnMatches, 1), NdVar::reg(kSecondCountMatches, 1)},
      SecondSourceVA + 5));
  Entry.Ops.push_back(lop(NdOp::INT_AND, NdVar::reg(kPath, 1),
                          {NdVar::reg(kFirstProducedInput, 1),
                           NdVar::reg(kSecondProducedNoInput, 1)},
                          SecondSourceVA + 6));
  Entry.Ops.push_back(lop(NdOp::COND_BR, NdVar{},
                          {NdVar::cst(SinkVA, 8), NdVar::reg(kPath, 1)},
                          SecondSourceVA + 7));
  Entry.Succs = {1, 2};
  Sink.Id = 1;
  Sink.StartAddr = SinkVA;
  Sink.EndAddr = SinkVA + 8;
  Sink.Ops.push_back(lop(NdOp::CALL, NdVar{}, {NdVar::cst(0x9000, 8)}, SinkVA));
  Sink.Succs = {2};
  Sink.Preds = {0};
  Exit.Id = 2;
  Exit.StartAddr = SinkVA + 8;
  Exit.EndAddr = SinkVA + 16;
  Exit.Ops.push_back(lop(NdOp::RETURN, NdVar{}, {}));
  Exit.Preds = {0, 1};
  LF.Blocks.push_back(std::move(Entry));
  LF.Blocks.push_back(std::move(Sink));
  LF.Blocks.push_back(std::move(Exit));
  return LF;
}

LowFunc returnSourceThenFormatLow(va_t SourceVA, va_t SinkVA,
                                  std::optional<uint64_t> RequiredReturn,
                                  uint16_t ReturnBytes = 8) {
  constexpr uint64_t kRax = 0;
  constexpr uint64_t kFlag = 200;
  constexpr va_t kEntry = 0x400000;
  LowFunc LF;
  LF.Entry = kEntry;
  LowBlock Entry, Sink, Exit;
  Entry.Id = 0;
  Entry.StartAddr = kEntry;
  Entry.EndAddr = SinkVA;
  Entry.Ops.push_back(
      lop(NdOp::CALL, NdVar::reg(kRax, 8), {NdVar::cst(0x9000, 8)}, SourceVA));
  if (RequiredReturn) {
    if (ReturnBytes < 8)
      Entry.Ops.push_back(lop(NdOp::SUBBYTES, NdVar::reg(kRax, ReturnBytes),
                              {NdVar::reg(kRax, 8), NdVar::cst(0, ReturnBytes)},
                              SourceVA + 2));
    Entry.Ops.push_back(lop(NdOp::INT_EQUAL, NdVar::reg(kFlag, 1),
                            {NdVar::reg(kRax, ReturnBytes),
                             NdVar::cst(*RequiredReturn, ReturnBytes)},
                            SourceVA + 4));
    Entry.Ops.push_back(lop(NdOp::COND_BR, NdVar{},
                            {NdVar::cst(SinkVA, 8), NdVar::reg(kFlag, 1)},
                            SourceVA + 8));
    Entry.Succs = {1, 2};
  } else {
    Entry.Ops.push_back(
        lop(NdOp::BRANCH, NdVar{}, {NdVar::cst(SinkVA, 8)}, SourceVA + 4));
    Entry.Succs = {1};
  }

  Sink.Id = 1;
  Sink.StartAddr = SinkVA;
  Sink.EndAddr = SinkVA + 8;
  Sink.Preds = {0};
  Sink.Succs = {2};
  Sink.Ops.push_back(lop(NdOp::CALL, NdVar{}, {NdVar::cst(0x9000, 8)}, SinkVA));

  Exit.Id = 2;
  Exit.StartAddr = SinkVA + 8;
  Exit.EndAddr = SinkVA + 16;
  Exit.Preds = RequiredReturn ? std::vector<int>{0, 1} : std::vector<int>{1};
  Exit.Ops.push_back(lop(NdOp::RETURN, NdVar{}, {}, SinkVA + 8));
  LF.Blocks.push_back(std::move(Entry));
  LF.Blocks.push_back(std::move(Sink));
  LF.Blocks.push_back(std::move(Exit));
  return LF;
}

LowFunc formattedScalarThenMemcpyLow(
    va_t SourceVA, va_t MemcpyVA, va_t OutputSlot,
    std::optional<uint64_t> RequiredAssignments = std::nullopt,
    uint16_t ScalarBytes = 8) {
  constexpr uint64_t kCopyLengthReg = 16;
  constexpr uint64_t kReturnReg = 0;
  constexpr uint64_t kFlag = 200;
  constexpr va_t kEntry = 0x400000;
  LowFunc LF;
  LF.Entry = kEntry;
  LowBlock Entry, Sink, Exit;
  Entry.Id = 0;
  Entry.StartAddr = kEntry;
  Entry.EndAddr = MemcpyVA;
  Entry.Ops.push_back(lop(NdOp::CALL, NdVar::reg(kReturnReg, 8),
                          {NdVar::cst(0x9000, 8)}, SourceVA));
  if (RequiredAssignments) {
    Entry.Ops.push_back(lop(NdOp::SUBBYTES, NdVar::reg(kReturnReg, 4),
                            {NdVar::reg(kReturnReg, 8), NdVar::cst(0, 4)},
                            SourceVA + 1));
    Entry.Ops.push_back(
        lop(NdOp::INT_EQUAL, NdVar::reg(kFlag, 1),
            {NdVar::reg(kReturnReg, 4), NdVar::cst(*RequiredAssignments, 4)},
            SourceVA + 2));
    Entry.Ops.push_back(lop(NdOp::COND_BR, NdVar{},
                            {NdVar::cst(MemcpyVA, 8), NdVar::reg(kFlag, 1)},
                            SourceVA + 3));
    Entry.Succs = {1, 2};
  } else {
    Entry.Ops.push_back(
        lop(NdOp::BRANCH, NdVar{}, {NdVar::cst(MemcpyVA, 8)}, SourceVA + 4));
    Entry.Succs = {1};
  }

  Sink.Id = 1;
  Sink.StartAddr = MemcpyVA;
  Sink.EndAddr = MemcpyVA + 8;
  Sink.Preds = {0};
  Sink.Succs = {2};
  Sink.Ops.push_back(lop(NdOp::LOAD, NdVar::reg(kCopyLengthReg, ScalarBytes),
                         {NdVar::cst(OutputSlot, 8)}, SourceVA + 4));
  if (ScalarBytes < 8)
    Sink.Ops.push_back(lop(NdOp::INT_ZEXT, NdVar::reg(kCopyLengthReg, 8),
                           {NdVar::reg(kCopyLengthReg, ScalarBytes)},
                           SourceVA + 5));
  Sink.Ops.push_back(
      lop(NdOp::CALL, NdVar{}, {NdVar::cst(0x9000, 8)}, MemcpyVA));

  Exit.Id = 2;
  Exit.StartAddr = MemcpyVA + 8;
  Exit.EndAddr = MemcpyVA + 16;
  Exit.Preds =
      RequiredAssignments ? std::vector<int>{0, 1} : std::vector<int>{1};
  Exit.Ops.push_back(lop(NdOp::RETURN, NdVar{}, {}, MemcpyVA + 8));
  LF.Blocks.push_back(std::move(Entry));
  LF.Blocks.push_back(std::move(Sink));
  LF.Blocks.push_back(std::move(Exit));
  return LF;
}

LowFunc fgetsThenStrcpyLow(va_t FgetsVA, va_t StrcpyVA, bool RequireSuccess) {
  constexpr uint64_t kRax = 0;
  constexpr uint64_t kFlag = 200;
  constexpr va_t kEntry = 0x400000;
  LowFunc LF;
  LF.Entry = kEntry;
  LowBlock Entry, Sink, Exit;
  Entry.Id = 0;
  Entry.StartAddr = kEntry;
  Entry.EndAddr = StrcpyVA;
  Entry.Ops.push_back(
      lop(NdOp::CALL, NdVar::reg(kRax, 8), {NdVar::cst(0x9100, 8)}, FgetsVA));
  if (RequireSuccess) {
    Entry.Ops.push_back(lop(NdOp::INT_NOTEQUAL, NdVar::reg(kFlag, 1),
                            {NdVar::reg(kRax, 8), NdVar::cst(0, 8)},
                            FgetsVA + 4));
    Entry.Ops.push_back(lop(NdOp::COND_BR, NdVar{},
                            {NdVar::cst(StrcpyVA, 8), NdVar::reg(kFlag, 1)},
                            FgetsVA + 8));
    Entry.Succs = {1, 2};
  } else {
    Entry.Ops.push_back(
        lop(NdOp::BRANCH, NdVar{}, {NdVar::cst(StrcpyVA, 8)}, FgetsVA + 4));
    Entry.Succs = {1};
  }
  Sink.Id = 1;
  Sink.StartAddr = StrcpyVA;
  Sink.EndAddr = StrcpyVA + 8;
  Sink.Ops.push_back(
      lop(NdOp::CALL, NdVar{}, {NdVar::cst(0x9000, 8)}, StrcpyVA));
  Sink.Succs = {2};
  Sink.Preds = {0};
  Exit.Id = 2;
  Exit.StartAddr = StrcpyVA + 8;
  Exit.EndAddr = StrcpyVA + 16;
  Exit.Ops.push_back(lop(NdOp::RETURN, NdVar{}, {}));
  Exit.Preds = RequireSuccess ? std::vector<int>{0, 1} : std::vector<int>{1};
  LF.Blocks.push_back(std::move(Entry));
  LF.Blocks.push_back(std::move(Sink));
  LF.Blocks.push_back(std::move(Exit));
  return LF;
}

LowFunc overwrittenPointerSpillThenStrcpyLow(va_t SourceVA, va_t StrcpyVA,
                                             va_t FirstBuffer,
                                             va_t SecondBuffer) {
  const uint64_t StackPointer = getTargetRegInfo(Arch::X64).StackPointer;
  constexpr uint64_t kBuffer = 64;
  constexpr uint64_t kReturn = 0;
  constexpr uint64_t kSlotEnd = 200;
  constexpr uint64_t kSlotDoesNotWrap = 216;
  constexpr uint64_t kSlotBeforeBuffer = 224;
  constexpr uint64_t kSourceSucceeded = 232;
  constexpr uint64_t kSlotIsBeforeBuffer = 240;
  constexpr va_t kEntry = 0x400000;
  constexpr va_t kOverwrite = 0x400030;

  LowFunc LF;
  LF.Entry = kEntry;
  LowBlock Entry, Source, Sink, Exit;

  Entry.Id = 0;
  Entry.StartAddr = kEntry;
  Entry.EndAddr = SourceVA;
  Entry.Ops.push_back(
      lop(NdOp::STORE, NdVar{},
          {NdVar::reg(StackPointer, 8), NdVar::cst(FirstBuffer, 8)}, kEntry));
  Entry.Ops.push_back(lop(NdOp::LOAD, NdVar::reg(kBuffer, 8),
                          {NdVar::reg(StackPointer, 8)}, kEntry + 4));
  Entry.Ops.push_back(lop(NdOp::INT_ADD, NdVar::reg(kSlotEnd, 8),
                          {NdVar::reg(StackPointer, 8), NdVar::cst(8, 8)},
                          kEntry + 8));
  Entry.Ops.push_back(
      lop(NdOp::INT_LESSEQUAL, NdVar::reg(kSlotDoesNotWrap, 1),
          {NdVar::reg(StackPointer, 8), NdVar::cst(UINT64_MAX - 8, 8)},
          kEntry + 12));
  Entry.Ops.push_back(lop(NdOp::INT_LESSEQUAL, NdVar::reg(kSlotBeforeBuffer, 1),
                          {NdVar::reg(kSlotEnd, 8), NdVar::cst(FirstBuffer, 8)},
                          kEntry + 16));
  Entry.Ops.push_back(
      lop(NdOp::BOOL_AND, NdVar::reg(kSlotIsBeforeBuffer, 1),
          {NdVar::reg(kSlotDoesNotWrap, 1), NdVar::reg(kSlotBeforeBuffer, 1)},
          kEntry + 20));
  Entry.Ops.push_back(
      lop(NdOp::COND_BR, NdVar{},
          {NdVar::cst(SourceVA, 8), NdVar::reg(kSlotIsBeforeBuffer, 1)},
          kEntry + 24));
  Entry.Succs = {1, 3};

  Source.Id = 1;
  Source.StartAddr = SourceVA;
  Source.EndAddr = kOverwrite;
  Source.Preds = {0};
  Source.Succs = {2, 3};
  Source.Ops.push_back(lop(NdOp::CALL, NdVar::reg(kReturn, 4),
                           {NdVar::cst(0x9100, 8)}, SourceVA));
  Source.Ops.push_back(lop(NdOp::INT_SLESS, NdVar::reg(kSourceSucceeded, 1),
                           {NdVar::cst(0, 4), NdVar::reg(kReturn, 4)},
                           SourceVA + 4));
  Source.Ops.push_back(
      lop(NdOp::COND_BR, NdVar{},
          {NdVar::cst(kOverwrite, 8), NdVar::reg(kSourceSucceeded, 1)},
          SourceVA + 8));

  Sink.Id = 2;
  Sink.StartAddr = kOverwrite;
  Sink.EndAddr = StrcpyVA + 8;
  Sink.Preds = {1};
  Sink.Succs = {3};
  Sink.Ops.push_back(lop(
      NdOp::STORE, NdVar{},
      {NdVar::reg(StackPointer, 8), NdVar::cst(SecondBuffer, 8)}, kOverwrite));
  Sink.Ops.push_back(lop(NdOp::LOAD, NdVar::reg(kBuffer, 8),
                         {NdVar::reg(StackPointer, 8)}, kOverwrite + 4));
  Sink.Ops.push_back(
      lop(NdOp::CALL, NdVar{}, {NdVar::cst(0x9000, 8)}, StrcpyVA));

  Exit.Id = 3;
  Exit.StartAddr = StrcpyVA + 8;
  Exit.EndAddr = StrcpyVA + 16;
  Exit.Preds = {0, 1, 2};
  Exit.Ops.push_back(lop(NdOp::RETURN, NdVar{}, {}, StrcpyVA + 8));

  LF.Blocks.push_back(std::move(Entry));
  LF.Blocks.push_back(std::move(Source));
  LF.Blocks.push_back(std::move(Sink));
  LF.Blocks.push_back(std::move(Exit));
  return LF;
}

LowFunc scanfThenStrcpyLow(va_t ScanfVA, va_t StrcpyVA) {
  constexpr uint64_t kRax = 0;
  constexpr uint64_t kFlag = 200;
  constexpr va_t kEntry = 0x400000;
  LowFunc LF;
  LF.Entry = kEntry;
  LowBlock Entry, Sink, Exit;
  Entry.Id = 0;
  Entry.StartAddr = kEntry;
  Entry.EndAddr = StrcpyVA;
  Entry.Ops.push_back(
      lop(NdOp::CALL, NdVar::reg(kRax, 8), {NdVar::cst(0x9100, 8)}, ScanfVA));
  Entry.Ops.push_back(lop(NdOp::SUBBYTES, NdVar::reg(kRax, 4),
                          {NdVar::reg(kRax, 8), NdVar::cst(0, 4)},
                          ScanfVA + 1));
  Entry.Ops.push_back(lop(NdOp::INT_SLESS, NdVar::reg(kFlag, 1),
                          {NdVar::cst(0, 4), NdVar::reg(kRax, 4)},
                          ScanfVA + 2));
  Entry.Ops.push_back(lop(NdOp::COND_BR, NdVar{},
                          {NdVar::cst(StrcpyVA, 8), NdVar::reg(kFlag, 1)},
                          ScanfVA + 3));
  Entry.Succs = {1, 2};
  Sink.Id = 1;
  Sink.StartAddr = StrcpyVA;
  Sink.EndAddr = StrcpyVA + 8;
  Sink.Ops.push_back(
      lop(NdOp::CALL, NdVar{}, {NdVar::cst(0x9000, 8)}, StrcpyVA));
  Sink.Succs = {2};
  Sink.Preds = {0};
  Exit.Id = 2;
  Exit.StartAddr = StrcpyVA + 8;
  Exit.EndAddr = StrcpyVA + 16;
  Exit.Ops.push_back(lop(NdOp::RETURN, NdVar{}, {}));
  Exit.Preds = {0, 1};
  LF.Blocks.push_back(std::move(Entry));
  LF.Blocks.push_back(std::move(Sink));
  LF.Blocks.push_back(std::move(Exit));
  return LF;
}

LowFunc scanfExactCountThenStrcpyLow(va_t ScanfVA, va_t StrcpyVA,
                                     uint32_t AssignmentCount) {
  constexpr uint64_t kRax = 0;
  constexpr uint64_t kFlag = 200;
  constexpr va_t kEntry = 0x400000;
  LowFunc LF;
  LF.Entry = kEntry;
  LowBlock Entry, Sink, Exit;
  Entry.Id = 0;
  Entry.StartAddr = kEntry;
  Entry.EndAddr = StrcpyVA;
  Entry.Ops.push_back(
      lop(NdOp::CALL, NdVar::reg(kRax, 8), {NdVar::cst(0x9100, 8)}, ScanfVA));
  Entry.Ops.push_back(lop(NdOp::SUBBYTES, NdVar::reg(kRax, 4),
                          {NdVar::reg(kRax, 8), NdVar::cst(0, 4)},
                          ScanfVA + 1));
  Entry.Ops.push_back(lop(NdOp::INT_EQUAL, NdVar::reg(kFlag, 1),
                          {NdVar::reg(kRax, 4), NdVar::cst(AssignmentCount, 4)},
                          ScanfVA + 2));
  Entry.Ops.push_back(lop(NdOp::COND_BR, NdVar{},
                          {NdVar::cst(StrcpyVA, 8), NdVar::reg(kFlag, 1)},
                          ScanfVA + 3));
  Entry.Succs = {1, 2};
  Sink.Id = 1;
  Sink.StartAddr = StrcpyVA;
  Sink.EndAddr = StrcpyVA + 8;
  Sink.Ops.push_back(
      lop(NdOp::CALL, NdVar{}, {NdVar::cst(0x9000, 8)}, StrcpyVA));
  Sink.Succs = {2};
  Sink.Preds = {0};
  Exit.Id = 2;
  Exit.StartAddr = StrcpyVA + 8;
  Exit.EndAddr = StrcpyVA + 16;
  Exit.Ops.push_back(lop(NdOp::RETURN, NdVar{}, {}));
  Exit.Preds = {0, 1};
  LF.Blocks.push_back(std::move(Entry));
  LF.Blocks.push_back(std::move(Sink));
  LF.Blocks.push_back(std::move(Exit));
  return LF;
}

LowFunc twoScanfThenStrcpyLow(va_t FirstScanfVA, va_t SecondScanfVA,
                              va_t StrcpyVA) {
  constexpr uint64_t kRax = 0;
  constexpr uint64_t kFlag = 200;
  constexpr va_t kEntry = 0x400000;
  LowFunc LF;
  LF.Entry = kEntry;
  LowBlock Entry, Sink, Exit;
  Entry.Id = 0;
  Entry.StartAddr = kEntry;
  Entry.EndAddr = StrcpyVA;
  Entry.Ops.push_back(lop(NdOp::CALL, NdVar::reg(kRax, 8),
                          {NdVar::cst(0x9100, 8)}, FirstScanfVA));
  Entry.Ops.push_back(lop(NdOp::CALL, NdVar::reg(kRax, 8),
                          {NdVar::cst(0x9100, 8)}, SecondScanfVA));
  Entry.Ops.push_back(lop(NdOp::SUBBYTES, NdVar::reg(kRax, 4),
                          {NdVar::reg(kRax, 8), NdVar::cst(0, 4)},
                          SecondScanfVA + 1));
  Entry.Ops.push_back(lop(NdOp::INT_SLESS, NdVar::reg(kFlag, 1),
                          {NdVar::cst(0, 4), NdVar::reg(kRax, 4)},
                          SecondScanfVA + 2));
  Entry.Ops.push_back(lop(NdOp::COND_BR, NdVar{},
                          {NdVar::cst(StrcpyVA, 8), NdVar::reg(kFlag, 1)},
                          SecondScanfVA + 3));
  Entry.Succs = {1, 2};
  Sink.Id = 1;
  Sink.StartAddr = StrcpyVA;
  Sink.EndAddr = StrcpyVA + 8;
  Sink.Ops.push_back(
      lop(NdOp::CALL, NdVar{}, {NdVar::cst(0x9000, 8)}, StrcpyVA));
  Sink.Succs = {2};
  Sink.Preds = {0};
  Exit.Id = 2;
  Exit.StartAddr = StrcpyVA + 8;
  Exit.EndAddr = StrcpyVA + 16;
  Exit.Ops.push_back(lop(NdOp::RETURN, NdVar{}, {}));
  Exit.Preds = {0, 1};
  LF.Blocks.push_back(std::move(Entry));
  LF.Blocks.push_back(std::move(Sink));
  LF.Blocks.push_back(std::move(Exit));
  return LF;
}

LowFunc readAndScanfThenStrcpyLow(
    va_t ReadVA, va_t ScanfVA, va_t StrcpyVA,
    std::optional<uint64_t> RequiredReadBytes = std::nullopt) {
  constexpr uint64_t kRax = 0;
  constexpr uint64_t kFlag = 200;
  constexpr va_t kEntry = 0x400000;
  constexpr va_t kScanfBlock = 0x400020;
  LowFunc LF;
  LF.Entry = kEntry;
  LowBlock Entry, Scanf, Sink, Exit;
  Entry.Id = 0;
  Entry.StartAddr = kEntry;
  Entry.EndAddr = kScanfBlock;
  Entry.Ops.push_back(
      lop(NdOp::CALL, NdVar::reg(kRax, 8), {NdVar::cst(0x9100, 8)}, ReadVA));
  Entry.Ops.push_back(
      lop(RequiredReadBytes ? NdOp::INT_EQUAL : NdOp::INT_SLESS,
          NdVar::reg(kFlag, 1),
          {NdVar::cst(RequiredReadBytes.value_or(0), 8), NdVar::reg(kRax, 8)},
          ReadVA + 1));
  Entry.Ops.push_back(lop(NdOp::COND_BR, NdVar{},
                          {NdVar::cst(kScanfBlock, 8), NdVar::reg(kFlag, 1)},
                          ReadVA + 2));
  Entry.Succs = {1, 3};
  Scanf.Id = 1;
  Scanf.StartAddr = kScanfBlock;
  Scanf.EndAddr = StrcpyVA;
  Scanf.Preds = {0};
  Scanf.Ops.push_back(
      lop(NdOp::CALL, NdVar::reg(kRax, 8), {NdVar::cst(0x9200, 8)}, ScanfVA));
  Scanf.Ops.push_back(lop(NdOp::SUBBYTES, NdVar::reg(kRax, 4),
                          {NdVar::reg(kRax, 8), NdVar::cst(0, 4)},
                          ScanfVA + 1));
  Scanf.Ops.push_back(lop(NdOp::INT_SLESS, NdVar::reg(kFlag, 1),
                          {NdVar::cst(0, 4), NdVar::reg(kRax, 4)},
                          ScanfVA + 2));
  Scanf.Ops.push_back(lop(NdOp::COND_BR, NdVar{},
                          {NdVar::cst(StrcpyVA, 8), NdVar::reg(kFlag, 1)},
                          ScanfVA + 3));
  Scanf.Succs = {2, 3};
  Sink.Id = 2;
  Sink.StartAddr = StrcpyVA;
  Sink.EndAddr = StrcpyVA + 8;
  Sink.Preds = {1};
  Sink.Succs = {3};
  Sink.Ops.push_back(
      lop(NdOp::CALL, NdVar{}, {NdVar::cst(0x9000, 8)}, StrcpyVA));
  Exit.Id = 3;
  Exit.StartAddr = StrcpyVA + 8;
  Exit.EndAddr = StrcpyVA + 16;
  Exit.Preds = {0, 1, 2};
  Exit.Ops.push_back(lop(NdOp::RETURN, NdVar{}, {}));
  LF.Blocks.push_back(std::move(Entry));
  LF.Blocks.push_back(std::move(Scanf));
  LF.Blocks.push_back(std::move(Sink));
  LF.Blocks.push_back(std::move(Exit));
  return LF;
}

LowFunc readThenStrcpyLow(va_t ReadVA, va_t StrcpyVA,
                          uint64_t RequiredReadBytes) {
  constexpr uint64_t kRax = 0;
  constexpr uint64_t kFlag = 200;
  constexpr va_t kEntry = 0x400000;
  LowFunc LF;
  LF.Entry = kEntry;
  LowBlock Entry, Sink, Exit;
  Entry.Id = 0;
  Entry.StartAddr = kEntry;
  Entry.EndAddr = StrcpyVA;
  Entry.Ops.push_back(
      lop(NdOp::CALL, NdVar::reg(kRax, 8), {NdVar::cst(0x9100, 8)}, ReadVA));
  Entry.Ops.push_back(
      lop(NdOp::INT_EQUAL, NdVar::reg(kFlag, 1),
          {NdVar::reg(kRax, 8), NdVar::cst(RequiredReadBytes, 8)}, ReadVA + 1));
  Entry.Ops.push_back(lop(NdOp::COND_BR, NdVar{},
                          {NdVar::cst(StrcpyVA, 8), NdVar::reg(kFlag, 1)},
                          ReadVA + 2));
  Entry.Succs = {1, 2};
  Sink.Id = 1;
  Sink.StartAddr = StrcpyVA;
  Sink.EndAddr = StrcpyVA + 8;
  Sink.Preds = {0};
  Sink.Succs = {2};
  Sink.Ops.push_back(
      lop(NdOp::CALL, NdVar{}, {NdVar::cst(0x9000, 8)}, StrcpyVA));
  Exit.Id = 2;
  Exit.StartAddr = StrcpyVA + 8;
  Exit.EndAddr = StrcpyVA + 16;
  Exit.Preds = {0, 1};
  Exit.Ops.push_back(lop(NdOp::RETURN, NdVar{}, {}, StrcpyVA + 8));
  LF.Blocks.push_back(std::move(Entry));
  LF.Blocks.push_back(std::move(Sink));
  LF.Blocks.push_back(std::move(Exit));
  return LF;
}

LowFunc environmentThenStrcpyLow(va_t SourceVA, va_t StrcpyVA,
                                 uint32_t BufferChars) {
  constexpr uint64_t kRax = 0;
  constexpr uint64_t kPositive = 200;
  constexpr uint64_t kFits = 201;
  constexpr uint64_t kSuccess = 202;
  constexpr va_t kEntry = 0x400000;
  LowFunc LF;
  LF.Entry = kEntry;
  LowBlock Entry, Sink, Exit;
  Entry.Id = 0;
  Entry.StartAddr = kEntry;
  Entry.EndAddr = StrcpyVA;
  Entry.Ops.push_back(
      lop(NdOp::CALL, NdVar::reg(kRax, 8), {NdVar::cst(0x9100, 8)}, SourceVA));
  Entry.Ops.push_back(lop(NdOp::SUBBYTES, NdVar::reg(kRax, 4),
                          {NdVar::reg(kRax, 8), NdVar::cst(0, 4)},
                          SourceVA + 1));
  Entry.Ops.push_back(lop(NdOp::INT_LESS, NdVar::reg(kPositive, 1),
                          {NdVar::cst(0, 4), NdVar::reg(kRax, 4)},
                          SourceVA + 2));
  Entry.Ops.push_back(lop(NdOp::INT_LESS, NdVar::reg(kFits, 1),
                          {NdVar::reg(kRax, 4), NdVar::cst(BufferChars, 4)},
                          SourceVA + 3));
  Entry.Ops.push_back(lop(NdOp::INT_AND, NdVar::reg(kSuccess, 1),
                          {NdVar::reg(kPositive, 1), NdVar::reg(kFits, 1)},
                          SourceVA + 4));
  Entry.Ops.push_back(lop(NdOp::COND_BR, NdVar{},
                          {NdVar::cst(StrcpyVA, 8), NdVar::reg(kSuccess, 1)},
                          SourceVA + 5));
  Entry.Succs = {1, 2};
  Sink.Id = 1;
  Sink.StartAddr = StrcpyVA;
  Sink.EndAddr = StrcpyVA + 8;
  Sink.Ops.push_back(
      lop(NdOp::CALL, NdVar{}, {NdVar::cst(0x9000, 8)}, StrcpyVA));
  Sink.Succs = {2};
  Sink.Preds = {0};
  Exit.Id = 2;
  Exit.StartAddr = StrcpyVA + 8;
  Exit.EndAddr = StrcpyVA + 16;
  Exit.Ops.push_back(lop(NdOp::RETURN, NdVar{}, {}));
  Exit.Preds = {0, 1};
  LF.Blocks.push_back(std::move(Entry));
  LF.Blocks.push_back(std::move(Sink));
  LF.Blocks.push_back(std::move(Exit));
  return LF;
}

LowFunc readThenTerminateStrcpyLow(va_t ReadVA, va_t StrcpyVA, va_t SourceVA,
                                   uint64_t TerminatorOffset = 0) {
  constexpr va_t kEntry = 0x400000;
  LowFunc LF;
  LF.Entry = kEntry;
  LowBlock Block;
  Block.Id = 0;
  Block.StartAddr = kEntry;
  Block.EndAddr = StrcpyVA + 8;
  Block.Ops.push_back(
      lop(NdOp::CALL, NdVar::reg(0, 8), {NdVar::cst(0x9100, 8)}, ReadVA));
  Block.Ops.push_back(
      lop(NdOp::STORE, NdVar{},
          {NdVar::cst(SourceVA + TerminatorOffset, 8), NdVar::cst(0, 1)},
          ReadVA + 4));
  Block.Ops.push_back(
      lop(NdOp::CALL, NdVar{}, {NdVar::cst(0x9000, 8)}, StrcpyVA));
  Block.Ops.push_back(lop(NdOp::RETURN, NdVar{}, {}, StrcpyVA + 4));
  LF.Blocks.push_back(std::move(Block));
  return LF;
}

LowFunc scanfThenOverlappingReadStrcpyLow(va_t ScanfVA, va_t ReadVA,
                                          va_t StrcpyVA) {
  constexpr uint64_t kRax = 0;
  constexpr uint64_t kFlag = 300;
  constexpr va_t kEntry = 0x400000;
  LowFunc LF;
  LF.Entry = kEntry;
  LowBlock Entry, Read, Sink, Exit;
  Entry.Id = 0;
  Entry.StartAddr = kEntry;
  Entry.EndAddr = ReadVA;
  Entry.Ops.push_back(
      lop(NdOp::CALL, NdVar::reg(kRax, 8), {NdVar::cst(0x9100, 8)}, ScanfVA));
  Entry.Ops.push_back(lop(NdOp::SUBBYTES, NdVar::reg(kRax, 4),
                          {NdVar::reg(kRax, 8), NdVar::cst(0, 4)},
                          ScanfVA + 1));
  Entry.Ops.push_back(lop(NdOp::INT_SLESS, NdVar::reg(kFlag, 1),
                          {NdVar::cst(0, 4), NdVar::reg(kRax, 4)},
                          ScanfVA + 2));
  Entry.Ops.push_back(lop(NdOp::COND_BR, NdVar{},
                          {NdVar::cst(ReadVA, 8), NdVar::reg(kFlag, 1)},
                          ScanfVA + 3));
  Entry.Succs = {1, 3};

  Read.Id = 1;
  Read.StartAddr = ReadVA;
  Read.EndAddr = StrcpyVA;
  Read.Preds = {0};
  Read.Ops.push_back(
      lop(NdOp::CALL, NdVar::reg(kRax, 8), {NdVar::cst(0x9200, 8)}, ReadVA));
  Read.Ops.push_back(lop(NdOp::INT_SLESS, NdVar::reg(kFlag, 1),
                         {NdVar::cst(0, 8), NdVar::reg(kRax, 8)}, ReadVA + 1));
  Read.Ops.push_back(lop(NdOp::COND_BR, NdVar{},
                         {NdVar::cst(StrcpyVA, 8), NdVar::reg(kFlag, 1)},
                         ReadVA + 2));
  Read.Succs = {2, 3};

  Sink.Id = 2;
  Sink.StartAddr = StrcpyVA;
  Sink.EndAddr = StrcpyVA + 8;
  Sink.Preds = {1};
  Sink.Succs = {3};
  Sink.Ops.push_back(
      lop(NdOp::CALL, NdVar{}, {NdVar::cst(0x9000, 8)}, StrcpyVA));

  Exit.Id = 3;
  Exit.StartAddr = StrcpyVA + 8;
  Exit.EndAddr = StrcpyVA + 16;
  Exit.Preds = {0, 1, 2};
  Exit.Ops.push_back(lop(NdOp::RETURN, NdVar{}, {}, StrcpyVA + 8));

  LF.Blocks.push_back(std::move(Entry));
  LF.Blocks.push_back(std::move(Read));
  LF.Blocks.push_back(std::move(Sink));
  LF.Blocks.push_back(std::move(Exit));
  return LF;
}

LowFunc unmodelledThenSinkLow(va_t SinkVA) {
  LowFunc LF = reachableSinkLow(SinkVA);
  LF.Blocks[0].Ops.insert(
      LF.Blocks[0].Ops.begin(),
      lop(NdOp::INTRINSIC, NdVar::tmp(7, 8), {NdVar::reg(0, 8)}, SinkVA - 4));
  LF.Blocks[0].StartAddr = SinkVA - 4;
  return LF;
}

LowFunc unresolvedIndirectThenSinkLow(va_t SinkVA) {
  constexpr va_t kEntry = 0x400000;
  LowFunc LF;
  LF.Entry = kEntry;
  LowBlock Entry, Sink, Exit;
  Entry.Id = 0;
  Entry.StartAddr = kEntry;
  Entry.EndAddr = kEntry + 8;
  Entry.Ops.push_back(
      lop(NdOp::INDIR_BR, NdVar{}, {NdVar::reg(64, 8)}, kEntry));
  Entry.Succs = {1, 2};
  Sink.Id = 1;
  Sink.StartAddr = SinkVA;
  Sink.EndAddr = SinkVA + 8;
  Sink.Ops.push_back(lop(NdOp::CALL, NdVar{}, {NdVar::cst(0x9000, 8)}, SinkVA));
  Sink.Preds = {0};
  Exit.Id = 2;
  Exit.StartAddr = SinkVA + 8;
  Exit.EndAddr = SinkVA + 16;
  Exit.Ops.push_back(lop(NdOp::RETURN, NdVar{}, {}));
  Exit.Preds = {0};
  LF.Blocks.push_back(std::move(Entry));
  LF.Blocks.push_back(std::move(Sink));
  LF.Blocks.push_back(std::move(Exit));
  return LF;
}

MedVar rdxLen() {
  MedVar V;
  V.Kind = MedVar::Reg;
  V.RegOff = 16;
  V.Size = 8;
  V.TheArch = Arch::X64;
  return V;
}

} // namespace

TEST(HuntEngine, TaintedStrcpyIntoStackBufferIsUnsafe) {
  Builder B("main");
  B.op(NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x30, 8)});
  B.op(NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(0x8, 8)});
  B.call("strcpy", temp(0), {temp(10), param(2)});
  B.F.Blocks[0].Ops.back().Addr = 0x400010;
  LowFunc LF = reachableSinkLow(0x400010);

  auto Fnd = hunt(B.F, /*StackRegs=*/true, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unsafe);
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High);
  EXPECT_FALSE(Fnd->Witness.empty());
}

TEST(HuntEngine, ReachableUnboundedInputIntoStackBufferIsUnsafe) {
  Builder B("main");
  B.op(NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x30, 8)});
  B.op(NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(0x8, 8)});
  B.call("gets", temp(0), {temp(10)});
  B.F.Blocks[0].Ops.back().Addr = 0x400010;
  LowFunc LF = reachableSinkLow(0x400010);

  auto Fnd = hunt(B.F, /*StackRegs=*/true, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unsafe);
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High);
  EXPECT_FALSE(Fnd->Witness.empty());
}

TEST(HuntEngine, CustomSourceSinkIsNotImplicitlyUnbounded) {
  SinkCatalog Cat = SinkCatalog::defaults();
  SinkEntry Wrapper;
  Wrapper.Name = "bounded_input";
  Wrapper.Class = VulnClass::BufferOverflow;
  Wrapper.Kind = SinkKind::Copy;
  Wrapper.DstArg = 0;
  Cat.addSink(std::move(Wrapper));
  Cat.addSource(SourceEntry{"bounded_input", 0});

  Builder B("main");
  B.op(NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x30, 8)});
  B.op(NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(0x8, 8)});
  B.call("bounded_input", temp(0), {temp(10)});
  B.F.Blocks[0].Ops.back().Addr = 0x400010;
  LowFunc LF = reachableSinkLow(0x400010);

  auto Fnd = hunt(B.F, /*StackRegs=*/true, &LF, Arch::X64, {},
                  BinaryFormat::Unknown, /*Image=*/nullptr, &Cat);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown) << Fnd->Detail;
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, CustomReturnSourceDrivesReachableCopyLength) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400010;
  SinkCatalog Cat = SinkCatalog::defaults();
  Cat.addSource(SourceEntry{"custom_input", -1});

  Builder B("main");
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("custom_input", temp(5), {});
  B.F.Blocks[0].Ops.back().Addr = SourceVA;
  B.call("memcpy", temp(0), {temp(1), temp(2), temp(5)});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;
  LowFunc LF = sourceReturnThenMemcpyLow(SourceVA, SinkVA,
                                         /*RequireNonnegative=*/false);

  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {},
                  BinaryFormat::Unknown, /*Image=*/nullptr, &Cat);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unsafe) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High);
  ASSERT_FALSE(Fnd->Witness.empty());
  EXPECT_TRUE(std::any_of(
      Fnd->Witness.begin(), Fnd->Witness.end(),
      [](const auto &Item) { return Item.first == "custom_input"; }));
}

TEST(HuntEngine, ConstantCopyFitsSizedGlobalDestination) {
  BinaryImage Img;
  Segment Data;
  Data.Name = "data";
  Data.VA = 0x5000;
  Data.Size = 0x20;
  Data.FileSz = 0x20;
  Data.Data.resize(0x20);
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Img.Segments.push_back(std::move(Data));
  Img.Symbols.push_back(Symbol{"buffer", 0x5000, 8, false});

  Builder B("main");
  B.call("memcpy", temp(0),
         {MedVar::makeConst(0x5000, 8, ConstantAddressProvenance::DataAddress,
                            0x5000),
          param(2), MedVar::makeConst(8, 8)});

  auto Fnd = hunt(B.F, /*StackRegs=*/false, /*LF=*/nullptr, Arch::X64, {},
                  BinaryFormat::ELF, &Img);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Safe) << Fnd->Detail;
  EXPECT_EQ(Fnd->Capacity, 8u);
  EXPECT_TRUE(Fnd->CapacityExact);
}

TEST(HuntEngine, ConstantCopyExceedsSizedGlobalOnReachablePath) {
  constexpr va_t SinkVA = 0x400010;
  BinaryImage Img;
  Segment Data;
  Data.Name = "data";
  Data.VA = 0x5000;
  Data.Size = 0x20;
  Data.FileSz = 0x20;
  Data.Data.resize(0x20);
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Img.Segments.push_back(std::move(Data));
  Img.Symbols.push_back(Symbol{"buffer", 0x5000, 8, false});

  Builder B("main");
  B.call("memcpy", temp(0),
         {MedVar::makeConst(0x5000, 8, ConstantAddressProvenance::DataAddress,
                            0x5000),
          param(2), MedVar::makeConst(9, 8)});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;
  LowFunc LF = reachableSinkLow(SinkVA);

  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {},
                  BinaryFormat::ELF, &Img);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unsafe) << Fnd->Detail;
  EXPECT_EQ(Fnd->Capacity, 8u);
  EXPECT_TRUE(Fnd->CapacityExact);
  EXPECT_FALSE(Fnd->Witness.empty());
}

TEST(HuntEngine, NarrowCopyDestinationPointerFailsClosed) {
  constexpr va_t SinkVA = 0x400010;
  constexpr va_t BufferVA = 0x50;
  BinaryImage Img;
  Segment Data;
  Data.Name = "data";
  Data.VA = BufferVA;
  Data.Size = 0x20;
  Data.FileSz = 0x20;
  Data.Data.resize(0x20);
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Img.Segments.push_back(std::move(Data));
  Img.Symbols.push_back(Symbol{"buffer", BufferVA, 8, false});

  Builder B("main");
  B.call("memcpy", temp(0),
         {MedVar::makeConst(BufferVA, 1, ConstantAddressProvenance::DataAddress,
                            BufferVA),
          param(2), MedVar::makeConst(9, 8)});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;
  LowFunc LF = reachableSinkLow(SinkVA);

  const auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {},
                        BinaryFormat::ELF, &Img);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::Low) << Fnd->Detail;
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, TaintedStrcpyWithoutReachabilityIsUnknown) {
  Builder B("main");
  B.op(NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x30, 8)});
  B.op(NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(0x8, 8)});
  B.call("strcpy", temp(0), {temp(10), param(2)});

  auto Fnd = hunt(B.F, /*StackRegs=*/true);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown);
  EXPECT_EQ(Fnd->TheConfidence, Confidence::Low);
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, ConstantFormatStringIsSafe) {
  BinaryImage Img;
  const va_t FormatVA = addCString(Img, "%s");
  Builder B("main");
  B.call("printf", temp(0), {MedVar::makeConst(FormatVA, 8)});

  auto Fnd = hunt(B.F, /*StackRegs=*/false, /*LF=*/nullptr, Arch::X64, {},
                  BinaryFormat::ELF, &Img);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->Class, VulnClass::FormatString);
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Safe);
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High);
  EXPECT_FALSE(Fnd->SkipReason.empty());
}

TEST(HuntEngine, NarrowerThanAbiFormatPointerFailsClosed) {
  constexpr va_t FormatVA = 0x10;
  BinaryImage Img;
  addCString(Img, "%s", FormatVA);
  Builder B("main");
  B.call("printf", temp(0), {MedVar::makeConst(FormatVA, 1)});

  auto Fnd = hunt(B.F, /*StackRegs=*/false, /*LF=*/nullptr, Arch::X64, {},
                  BinaryFormat::ELF, &Img);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::Low) << Fnd->Detail;
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, NarrowConstantFormatAddressUsesDeclaredWidth) {
  struct Case {
    Arch TargetArch;
    BinaryFormat Format;
  } Cases[] = {{Arch::X86, BinaryFormat::COFF}, {Arch::ARM, BinaryFormat::ELF}};

  BinaryImage Img;
  addCString(Img, "%s", UINT64_C(0x80001000));
  for (const Case &C : Cases) {
    SCOPED_TRACE(static_cast<int>(C.TargetArch));
    Builder B("main");
    B.call("printf", temp(0),
           {MedVar::makeConst(UINT64_C(0xffffffff80001000), 4)});

    auto Fnd = hunt(B.F, /*StackRegs=*/false, /*LF=*/nullptr, C.TargetArch, {},
                    C.Format, &Img);
    ASSERT_TRUE(Fnd.has_value());
    EXPECT_EQ(Fnd->Class, VulnClass::FormatString);
    EXPECT_EQ(Fnd->TheVerdict, Verdict::Safe) << Fnd->Detail;
    EXPECT_EQ(Fnd->TheConfidence, Confidence::High) << Fnd->Detail;
  }
}

TEST(HuntEngine, ConstantFormatDoesNotBorrowAnAdjacentRelocationOwner) {
  BinaryImage Img;
  Segment First;
  First.Name = "first";
  First.VA = 0x1000;
  First.Size = 4;
  First.FileSz = 4;
  First.Flags = SegmentFlags::Readable;
  First.Data = {'x', 'x', 'x', 'x'};
  Img.Segments.push_back(std::move(First));
  Segment Second;
  Second.Name = "second";
  Second.VA = 0x1004;
  Second.Flags = SegmentFlags::Readable;
  Second.Data = {'%', 's', 0};
  Second.Size = Second.Data.size();
  Second.FileSz = Second.Data.size();
  Img.Segments.push_back(std::move(Second));

  Builder B("main");
  const MedVar OnePastFirst = MedVar::makeConst(
      0x1004, 8, ConstantAddressProvenance::DataAddress, 0x1000);
  B.call("printf", temp(0), {OnePastFirst});

  auto Fnd = hunt(B.F, /*StackRegs=*/false, /*LF=*/nullptr, Arch::X64, {},
                  BinaryFormat::ELF, &Img);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->Class, VulnClass::FormatString);
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown);
  EXPECT_EQ(Fnd->TheConfidence, Confidence::Low);
  EXPECT_TRUE(Fnd->SkipReason.empty());
}

TEST(HuntEngine, ConstantSprintfFormatDoesNotHideDestinationExtent) {
  BinaryImage Img;
  const va_t FormatVA = addCString(Img, "%s");
  Builder B("main");
  B.call("sprintf", temp(0),
         {param(1), MedVar::makeConst(FormatVA, 8), param(2)});

  auto Fnd = hunt(B.F, /*StackRegs=*/false, /*LF=*/nullptr, Arch::X64, {},
                  BinaryFormat::ELF, &Img);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->Class, VulnClass::BufferOverflow);
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown);
  EXPECT_EQ(Fnd->TheConfidence, Confidence::Low);
  EXPECT_TRUE(Fnd->SkipReason.empty());
  EXPECT_NE(Fnd->Detail.find("destination"), std::string::npos);
}

TEST(HuntEngine, ConstantSprintfOutputFitsRecoveredDestination) {
  BinaryImage Img;
  const va_t FormatVA = addCString(Img, "abc%%");
  Builder B("main");
  B.call("malloc", temp(1), {MedVar::makeConst(5, 8)});
  B.call("sprintf", temp(0), {temp(1), MedVar::makeConst(FormatVA, 8)});

  auto Fnd = hunt(B.F, /*StackRegs=*/false, /*LF=*/nullptr, Arch::X64, {},
                  BinaryFormat::ELF, &Img);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->Class, VulnClass::BufferOverflow);
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Safe) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High);
  EXPECT_EQ(Fnd->Capacity, 5u);
}

TEST(HuntEngine, ConstantSprintfOutputExceedsRecoveredDestination) {
  constexpr va_t SinkVA = 0x400010;
  BinaryImage Img;
  const va_t FormatVA = addCString(Img, "abcd");
  Builder B("main");
  B.call("malloc", temp(1), {MedVar::makeConst(4, 8)});
  B.call("sprintf", temp(0), {temp(1), MedVar::makeConst(FormatVA, 8)});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;
  LowFunc LF = reachableSinkLow(SinkVA);

  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {},
                  BinaryFormat::ELF, &Img);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->Class, VulnClass::BufferOverflow);
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unsafe) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High);
  EXPECT_EQ(Fnd->Capacity, 4u);
  EXPECT_FALSE(Fnd->Witness.empty());
}

TEST(HuntEngine, ConstantSnprintfLimitFitsRecoveredDestination) {
  BinaryImage Img;
  const va_t FormatVA = addCString(Img, "abcdef");
  Builder B("main");
  B.call("malloc", temp(1), {MedVar::makeConst(4, 8)});
  B.call("snprintf", temp(0),
         {temp(1), MedVar::makeConst(4, 8), MedVar::makeConst(FormatVA, 8)});

  auto Fnd = hunt(B.F, /*StackRegs=*/false, /*LF=*/nullptr, Arch::X64, {},
                  BinaryFormat::ELF, &Img);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->Class, VulnClass::BufferOverflow);
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Safe) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High);
}

TEST(HuntEngine, ConstantSnprintfLimitCanExceedRecoveredDestination) {
  constexpr va_t SinkVA = 0x400010;
  BinaryImage Img;
  const va_t FormatVA = addCString(Img, "abcdef");
  Builder B("main");
  B.call("malloc", temp(1), {MedVar::makeConst(4, 8)});
  B.call("snprintf", temp(0),
         {temp(1), MedVar::makeConst(8, 8), MedVar::makeConst(FormatVA, 8)});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;
  LowFunc LF = reachableSinkLow(SinkVA);

  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {},
                  BinaryFormat::ELF, &Img);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->Class, VulnClass::BufferOverflow);
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unsafe) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High);
}

TEST(HuntEngine, ConstantSprintfOverflowWithoutReachabilityIsUnknown) {
  BinaryImage Img;
  const va_t FormatVA = addCString(Img, "abcd");
  Builder B("main");
  B.call("malloc", temp(1), {MedVar::makeConst(4, 8)});
  B.call("sprintf", temp(0), {temp(1), MedVar::makeConst(FormatVA, 8)});

  auto Fnd = hunt(B.F, /*StackRegs=*/false, /*LF=*/nullptr, Arch::X64, {},
                  BinaryFormat::ELF, &Img);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->Class, VulnClass::BufferOverflow);
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown);
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, ConstantSprintfConversionExtentRemainsUnknown) {
  BinaryImage Img;
  const va_t FormatVA = addCString(Img, "%s");
  Builder B("main");
  B.call("malloc", temp(1), {MedVar::makeConst(4, 8)});
  B.call("sprintf", temp(0),
         {temp(1), MedVar::makeConst(FormatVA, 8), param(2)});

  auto Fnd = hunt(B.F, /*StackRegs=*/false, /*LF=*/nullptr, Arch::X64, {},
                  BinaryFormat::ELF, &Img);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->Class, VulnClass::BufferOverflow);
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown);
  EXPECT_EQ(Fnd->Detail, "formatted output extent is unresolved");
}

TEST(HuntEngine, ConstantSnprintfLimitBoundsUnknownConversion) {
  BinaryImage Img;
  const va_t FormatVA = addCString(Img, "%s");
  Builder B("main");
  B.call("malloc", temp(1), {MedVar::makeConst(4, 8)});
  B.call("snprintf", temp(0),
         {temp(1), MedVar::makeConst(4, 8), MedVar::makeConst(FormatVA, 8),
          param(2)});

  auto Fnd = hunt(B.F, /*StackRegs=*/false, /*LF=*/nullptr, Arch::X64, {},
                  BinaryFormat::ELF, &Img);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->Class, VulnClass::BufferOverflow);
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Safe) << Fnd->Detail;
}

TEST(HuntEngine, CheckedSnprintfUsesDeclaredObjectCapacity) {
  BinaryImage Img;
  const va_t FormatVA = addCString(Img, "%s");
  Builder B("main");
  B.call("malloc", temp(1), {MedVar::makeConst(1, 8)});
  B.call("__snprintf_chk", temp(0),
         {temp(1), MedVar::makeConst(8, 8), MedVar::makeConst(2, 4),
          MedVar::makeConst(1, 8), MedVar::makeConst(FormatVA, 8), param(2)});

  auto Fnd = hunt(B.F, /*StackRegs=*/false, /*LF=*/nullptr, Arch::X64, {},
                  BinaryFormat::ELF, &Img);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->Class, VulnClass::BufferOverflow);
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Safe) << Fnd->Detail;
  EXPECT_NE(Fnd->SkipReason.find("fortified"), std::string::npos);
}

TEST(HuntEngine, ReachableTaintedFormatStringIsUnsafe) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400010;
  const MedVar SourceBuffer = mkReg(24, 0);
  Builder B("main");
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("read", temp(5),
         {MedVar::makeConst(0, 8), SourceBuffer, MedVar::makeConst(32, 8)});
  B.F.Blocks[0].Ops.back().Addr = SourceVA;
  B.call("printf", temp(0), {SourceBuffer});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;
  LowFunc LF = sourceReturnThenMemcpyLow(SourceVA, SinkVA,
                                         /*RequireNonnegative=*/false);

  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->Class, VulnClass::FormatString);
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unsafe);
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High);
  EXPECT_FALSE(Fnd->Witness.empty());
}

TEST(HuntEngine, NarrowReadOutputPointerFailsClosed) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400010;
  const MedVar NarrowBuffer = mkReg(24, 0, 1);
  Builder B("main");
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("read", temp(5),
         {MedVar::makeConst(0, 8), NarrowBuffer, MedVar::makeConst(32, 8)});
  B.F.Blocks[0].Ops.back().Addr = SourceVA;
  B.call("printf", temp(0), {NarrowBuffer});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;
  LowFunc LF = sourceReturnThenMemcpyLow(SourceVA, SinkVA,
                                         /*RequireNonnegative=*/false);

  const auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::Low) << Fnd->Detail;
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, ReturnSourceCorroboratesReachableFormatInput) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400020;
  Builder B("main");
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("getenv", mkReg(0, 1), {param(1)});
  B.F.Blocks[0].Ops.back().Addr = SourceVA;
  B.call("printf", temp(0), {mkReg(0, 1)});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;
  LowFunc LF = returnSourceThenFormatLow(SourceVA, SinkVA, std::nullopt);

  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unsafe) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High) << Fnd->Detail;
}

TEST(HuntEngine, NarrowPointerReturnSourceWidthFailsClosed) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400020;
  const MedVar NarrowReturn = mkReg(0, 1, 1);
  Builder B("main");
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("getenv", NarrowReturn, {param(1)});
  B.F.Blocks[0].Ops.back().Addr = SourceVA;
  B.call("printf", temp(0), {NarrowReturn});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;
  LowFunc LF = returnSourceThenFormatLow(SourceVA, SinkVA,
                                         /*RequiredReturn=*/1,
                                         /*ReturnBytes=*/1);
  LF.Blocks[0].Ops[0].Output = NdVar::reg(0, 1);

  const auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::Low) << Fnd->Detail;
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, NullReturnSourceDoesNotCorroborateFormatInput) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400020;
  Builder B("main");
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("getenv", mkReg(0, 1), {param(1)});
  B.F.Blocks[0].Ops.back().Addr = SourceVA;
  B.call("printf", temp(0), {mkReg(0, 1)});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;
  LowFunc LF = returnSourceThenFormatLow(SourceVA, SinkVA, 0);

  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown) << Fnd->Detail;
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, FailedReadDoesNotCorroborateFormatBuffer) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400020;
  const MedVar SourceBuffer = mkReg(24, 0);
  Builder B("main");
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("read", mkReg(0, 1),
         {MedVar::makeConst(0, 8), SourceBuffer, MedVar::makeConst(32, 8)});
  B.F.Blocks[0].Ops.back().Addr = SourceVA;
  B.call("printf", temp(0), {SourceBuffer});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;
  LowFunc LF = returnSourceThenFormatLow(SourceVA, SinkVA, UINT64_MAX);

  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown) << Fnd->Detail;
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, FailedRecvWithNonzeroFlagsDoesNotCorroborateFormatBuffer) {
  for (const char *Name : {"recv", "recvfrom"}) {
    SCOPED_TRACE(Name);
    constexpr va_t SourceVA = 0x400004;
    constexpr va_t SinkVA = 0x400020;
    const MedVar SourceBuffer = mkReg(24, 0);
    Builder B("main");
    B.F.Entry = 0x400000;
    B.F.CC = CallingConv::SysV_AMD64;
    std::vector<MedVar> Args = {MedVar::makeConst(0, 8), SourceBuffer,
                                MedVar::makeConst(32, 8),
                                MedVar::makeConst(1, 8)};
    if (llvm::StringRef(Name) == "recvfrom") {
      Args.push_back(param(3));
      Args.push_back(param(4));
    }
    B.call(Name, mkReg(0, 1), std::move(Args));
    B.F.Blocks[0].Ops.back().Addr = SourceVA;
    B.call("printf", temp(0), {SourceBuffer});
    B.F.Blocks[0].Ops.back().Addr = SinkVA;
    LowFunc LF = returnSourceThenFormatLow(SourceVA, SinkVA, UINT64_MAX);

    auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
    ASSERT_TRUE(Fnd.has_value());
    EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown) << Fnd->Detail;
    EXPECT_TRUE(Fnd->Witness.empty());
  }
}

TEST(HuntEngine,
     FailedWinSockRecvWithNonzeroFlagsDoesNotCorroborateFormatBuffer) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400020;
  const MedVar SourceBuffer = mkReg(24, 0);
  Builder B("main");
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::Win64;
  B.call("recv", mkReg(0, 1, 4),
         {MedVar::makeConst(0, 8), SourceBuffer, MedVar::makeConst(32, 8),
          MedVar::makeConst(1, 8)});
  B.F.Blocks[0].Ops.back().Addr = SourceVA;
  B.call("printf", temp(0), {SourceBuffer});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;
  LowFunc LF = returnSourceThenFormatLow(SourceVA, SinkVA, UINT32_MAX,
                                         /*ReturnBytes=*/4);

  auto Fnd =
      hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {}, BinaryFormat::COFF);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown) << Fnd->Detail;
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, FailedEnvironmentQueryDoesNotCorroborateFormatBuffer) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400020;
  const MedVar SourceBuffer = mkReg(24, 0);
  Builder B("main");
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::Win64;
  B.call("GetEnvironmentVariableA", mkReg(0, 1),
         {param(1), SourceBuffer, MedVar::makeConst(32, 8)});
  B.F.Blocks[0].Ops.back().Addr = SourceVA;
  B.call("printf", temp(0), {SourceBuffer});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;
  LowFunc LF = returnSourceThenFormatLow(SourceVA, SinkVA, 0,
                                         /*ReturnBytes=*/4);

  auto Fnd =
      hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {}, BinaryFormat::COFF);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown) << Fnd->Detail;
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, EnvironmentQueryCorroboratesReachableFormatBuffer) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400020;
  const MedVar SourceBuffer = mkReg(24, 0);
  Builder B("main");
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::Win64;
  B.call("GetEnvironmentVariableA", mkReg(0, 1),
         {param(1), SourceBuffer, MedVar::makeConst(32, 8)});
  B.F.Blocks[0].Ops.back().Addr = SourceVA;
  B.call("printf", temp(0), {SourceBuffer});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;
  LowFunc LF = returnSourceThenFormatLow(SourceVA, SinkVA, std::nullopt);

  auto Fnd =
      hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {}, BinaryFormat::COFF);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unsafe) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High) << Fnd->Detail;
}

TEST(HuntEngine, TruncatedEnvironmentQueryDoesNotCorroborateFormatBuffer) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400020;
  constexpr uint32_t BufferChars = 8;
  const MedVar SourceBuffer = mkReg(24, 0);
  Builder B("main");
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::Win64;
  B.call("GetEnvironmentVariableA", mkReg(0, 1, 4),
         {param(1), SourceBuffer, MedVar::makeConst(BufferChars, 8)});
  B.F.Blocks[0].Ops.back().Addr = SourceVA;
  B.call("printf", temp(0), {SourceBuffer});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;
  LowFunc LF = returnSourceThenFormatLow(SourceVA, SinkVA, BufferChars,
                                         /*ReturnBytes=*/4);

  auto Fnd =
      hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {}, BinaryFormat::COFF);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown) << Fnd->Detail;
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, TruncatedWideEnvironmentQueryDoesNotCorroborateFormatBuffer) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400020;
  constexpr uint32_t BufferChars = 8;
  const MedVar SourceBuffer = mkReg(24, 0);
  Builder B("main");
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::Win64;
  B.call("GetEnvironmentVariableW", mkReg(0, 1, 4),
         {param(1), SourceBuffer, MedVar::makeConst(BufferChars, 8)});
  B.F.Blocks[0].Ops.back().Addr = SourceVA;
  B.call("wprintf", temp(0), {SourceBuffer});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;
  LowFunc LF = returnSourceThenFormatLow(SourceVA, SinkVA, BufferChars,
                                         /*ReturnBytes=*/4);

  auto Fnd =
      hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {}, BinaryFormat::COFF);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown) << Fnd->Detail;
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, SuccessfulWideEnvironmentQueryCorroboratesFormatBuffer) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400020;
  const MedVar SourceBuffer = mkReg(24, 0);
  Builder B("main");
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::Win64;
  B.call("GetEnvironmentVariableW", mkReg(0, 1, 4),
         {param(1), SourceBuffer, MedVar::makeConst(32, 8)});
  B.F.Blocks[0].Ops.back().Addr = SourceVA;
  B.call("wprintf", temp(0), {SourceBuffer});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;
  LowFunc LF = returnSourceThenFormatLow(SourceVA, SinkVA, 7,
                                         /*ReturnBytes=*/4);

  auto Fnd =
      hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {}, BinaryFormat::COFF);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unsafe) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High) << Fnd->Detail;
}

TEST(HuntEngine, SuccessfulEnvironmentQueryBoundsImplicitStringCopy) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400020;
  constexpr uint32_t BufferChars = 8;
  for (const auto &[Capacity, Expected] :
       {std::pair<uint64_t, Verdict>{8, Verdict::Safe},
        std::pair<uint64_t, Verdict>{7, Verdict::Unsafe}}) {
    SCOPED_TRACE(Capacity);
    const MedVar SourceBuffer = param(2);
    Builder B("main");
    B.F.Entry = 0x400000;
    B.F.CC = CallingConv::Win64;
    B.call("malloc", temp(1), {MedVar::makeConst(Capacity, 8)});
    B.call("GetEnvironmentVariableA", mkReg(0, 1, 4),
           {param(1), SourceBuffer, MedVar::makeConst(BufferChars, 8)});
    B.F.Blocks[0].Ops.back().Addr = SourceVA;
    B.call("strcpy", temp(0), {temp(1), SourceBuffer});
    B.F.Blocks[0].Ops.back().Addr = SinkVA;
    LowFunc LF = environmentThenStrcpyLow(SourceVA, SinkVA, BufferChars);

    auto Fnd =
        hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {}, BinaryFormat::COFF);
    ASSERT_TRUE(Fnd.has_value());
    EXPECT_EQ(Fnd->TheVerdict, Expected) << Fnd->Detail;
    EXPECT_EQ(Fnd->TheConfidence, Confidence::High) << Fnd->Detail;
  }
}

TEST(HuntEngine, FailedScanfDoesNotCorroborateFormatBuffer) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400020;
  BinaryImage Img;
  const va_t FormatVA = addCString(Img, "%s");
  const MedVar SourceBuffer = mkReg(24, 0);
  Builder B("main");
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("scanf", mkReg(0, 1, 4),
         {MedVar::makeConst(FormatVA, 8), SourceBuffer});
  B.F.Blocks[0].Ops.back().Addr = SourceVA;
  B.call("printf", temp(0), {SourceBuffer});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;
  LowFunc LF = returnSourceThenFormatLow(SourceVA, SinkVA, UINT32_MAX,
                                         /*ReturnBytes=*/4);

  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {},
                  BinaryFormat::ELF, &Img);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown) << Fnd->Detail;
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, WritableConstantAddressDoesNotHideTaintedFormatString) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400010;
  BinaryImage Img;
  const va_t FormatVA = addCString(Img, "%s");
  Img.Segments.back().Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  const MedVar FormatBuffer = MedVar::makeConst(
      FormatVA, 8, ConstantAddressProvenance::DataAddress, FormatVA);
  Builder B("main");
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("read", temp(5),
         {MedVar::makeConst(0, 8), FormatBuffer, MedVar::makeConst(32, 8)});
  B.F.Blocks[0].Ops.back().Addr = SourceVA;
  B.call("printf", temp(0), {FormatBuffer});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;
  LowFunc LF = sourceReturnThenMemcpyLow(SourceVA, SinkVA,
                                         /*RequireNonnegative=*/false);

  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {},
                  BinaryFormat::ELF, &Img);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->Class, VulnClass::FormatString);
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unsafe);
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High);
}

TEST(HuntEngine, NarrowFormattedDestinationPointerFailsClosed) {
  constexpr va_t SinkVA = 0x400010;
  constexpr va_t BufferVA = 0x50;
  BinaryImage Img;
  Segment Data;
  Data.Name = "data";
  Data.VA = BufferVA;
  Data.Size = 0x20;
  Data.FileSz = 0x20;
  Data.Data.resize(0x20);
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Img.Segments.push_back(std::move(Data));
  Img.Symbols.push_back(Symbol{"buffer", BufferVA, 8, false});
  const va_t FormatVA = addCString(Img, "12345678");
  const MedVar NarrowBuffer = MedVar::makeConst(
      BufferVA, 1, ConstantAddressProvenance::DataAddress, BufferVA);

  Builder B("main");
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("sprintf", temp(0), {NarrowBuffer, MedVar::makeConst(FormatVA, 8)});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;
  LowFunc LF = reachableSinkLow(SinkVA);

  const auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {},
                        BinaryFormat::ELF, &Img);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::Low) << Fnd->Detail;
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, NarrowCopySourcePointerFailsClosed) {
  constexpr va_t SinkVA = 0x400010;
  constexpr va_t BufferVA = 0x50;
  BinaryImage Img;
  Segment Data;
  Data.Name = "data";
  Data.VA = BufferVA;
  Data.Size = 0x40;
  Data.FileSz = 0x40;
  Data.Data.resize(0x40);
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Img.Segments.push_back(std::move(Data));
  Img.Symbols.push_back(Symbol{"buffer", BufferVA, 8, false});
  const MedVar Destination = MedVar::makeConst(
      BufferVA, 8, ConstantAddressProvenance::DataAddress, BufferVA);
  const MedVar NarrowSource = MedVar::makeConst(
      BufferVA + 0x20, 1, ConstantAddressProvenance::DataAddress, BufferVA);

  Builder B("main");
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("memcpy", temp(0),
         {Destination, NarrowSource, MedVar::makeConst(9, 8)});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;
  LowFunc LF = reachableSinkLow(SinkVA);

  const auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {},
                        BinaryFormat::ELF, &Img);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::Low) << Fnd->Detail;
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, TaintedFormatWithoutPathEvidenceIsUnknown) {
  const MedVar SourceBuffer = mkReg(24, 0);
  Builder B("main");
  B.call("read", temp(5),
         {MedVar::makeConst(0, 8), SourceBuffer, MedVar::makeConst(32, 8)});
  B.call("printf", temp(0), {SourceBuffer});

  auto Fnd = hunt(B.F, /*StackRegs=*/false);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->Class, VulnClass::FormatString);
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown);
  EXPECT_EQ(Fnd->TheConfidence, Confidence::Low);
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, UnmodelledOperationBeforeSinkFailsClosed) {
  Builder B("main");
  B.op(NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x30, 8)});
  B.op(NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(0x8, 8)});
  B.call("strcpy", temp(0), {temp(10), param(2)});
  B.F.Blocks[0].Ops.back().Addr = 0x400010;
  LowFunc LF = unmodelledThenSinkLow(0x400010);

  auto Fnd = hunt(B.F, /*StackRegs=*/true, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown);
  EXPECT_EQ(Fnd->TheConfidence, Confidence::Low);
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, IncompleteInstructionLiftFailsClosed) {
  Builder B("main");
  B.op(NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x30, 8)});
  B.op(NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(0x8, 8)});
  B.call("strcpy", temp(0), {temp(10), param(2)});
  B.F.Blocks[0].Ops.back().Addr = 0x400010;
  LowFunc LF = reachableSinkLow(0x400010);
  LF.DecodedInstructionCount = 2;
  LF.LiftedInstructionCount = 1;

  auto Fnd = hunt(B.F, /*StackRegs=*/true, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown);
  EXPECT_EQ(Fnd->TheConfidence, Confidence::Low);
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, UnresolvedIndirectBranchDoesNotGuessASuccessor) {
  Builder B("main");
  B.F.Entry = 0x400000;
  B.op(NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x30, 8)});
  B.op(NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(0x8, 8)});
  B.call("strcpy", temp(0), {temp(10), param(2)});
  B.F.Blocks[0].Ops.back().Addr = 0x400010;
  LowFunc LF = unresolvedIndirectThenSinkLow(0x400010);

  auto Fnd = hunt(B.F, /*StackRegs=*/true, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown);
  EXPECT_EQ(Fnd->TheConfidence, Confidence::Low);
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, MissingCallAddressDoesNotGuessFirstCallAsSink) {
  Builder B("main");
  B.op(NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x30, 8)});
  B.op(NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(0x8, 8)});
  B.call("strcpy", temp(0), {temp(10), param(2)});
  LowFunc LF = reachableSinkLow(0x400010);

  auto Fnd = hunt(B.F, /*StackRegs=*/true, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown);
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, TaintedMemcpyWithoutPathSourceIsUnknown) {
  Builder B;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("read", temp(5), {});
  B.call("memcpy", temp(0), {temp(1), temp(2), temp(5)});
  B.F.Blocks[0].Ops.back().Addr = 0x400010;
  B.F.CC = CallingConv::SysV_AMD64;
  LowFunc LF = reachableSinkLow(0x400010);

  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown);
  ASSERT_TRUE(Fnd->Capacity.has_value());
  EXPECT_EQ(*Fnd->Capacity, 16u);
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, ExplicitLengthSourceAndSinkMustShareFeasiblePath) {
  constexpr va_t MallocVA = 0x400000;
  constexpr va_t SourceVA = 0x400010;
  constexpr va_t SinkVA = 0x400050;

  MedFunc F;
  F.Name = "f";
  F.Entry = MallocVA;
  F.CC = CallingConv::SysV_AMD64;
  F.Blocks.resize(6);
  for (int I = 0; I < 6; ++I)
    F.Blocks[I].Id = I;
  F.Blocks[0].Succs = {1, 2};
  F.Blocks[1].Preds = {0};
  F.Blocks[1].Succs = {3};
  F.Blocks[2].Preds = {0};
  F.Blocks[2].Succs = {3};
  F.Blocks[3].Preds = {1, 2};
  F.Blocks[3].Succs = {4, 5};
  F.Blocks[4].Preds = {3};
  F.Blocks[5].Preds = {3};

  auto addBlockCall = [&](int BlockId, llvm::StringRef Name, MedVar Ret,
                          std::vector<MedVar> Args, va_t Addr) {
    MedBlock &Block = F.Blocks[BlockId];
    const int OpIdx = static_cast<int>(Block.Ops.size());
    MedOp Op;
    Op.Opcode = NdOp::CALL;
    Op.Output = Ret;
    Op.Addr = Addr;
    Op.addInput(MedVar::makeConst(0x9000, 8));
    Block.Ops.push_back(std::move(Op));
    MedCallInfo CI;
    CI.BlockId = BlockId;
    CI.OpIdx = OpIdx;
    CI.TargetName = Name.str();
    CI.Args = std::move(Args);
    F.CallInfos.push_back(std::move(CI));
  };

  addBlockCall(0, "malloc", temp(1), {MedVar::makeConst(16, 8)}, MallocVA);
  addBlockCall(1, "read", temp(5),
               {MedVar::makeConst(0, 8), temp(2), MedVar::makeConst(32, 8)},
               SourceVA);
  addBlockCall(5, "memcpy", temp(0), {temp(1), temp(2), temp(5)}, SinkVA);

  LowFunc LF = mutuallyExclusiveSourceAndSinkLow(MallocVA, SourceVA, SinkVA);
  auto Fnd = hunt(F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown) << Fnd->Detail;
  EXPECT_TRUE(Fnd->Witness.empty());
  EXPECT_EQ(Fnd->Detail, "length provenance unresolved");
}

TEST(HuntEngine, SourceAndSinkMustShareFeasiblePath) {
  constexpr va_t MallocVA = 0x400000;
  constexpr va_t SourceVA = 0x400010;
  constexpr va_t SinkVA = 0x400050;

  MedFunc F;
  F.Name = "f";
  F.Entry = MallocVA;
  F.CC = CallingConv::SysV_AMD64;
  F.Blocks.resize(6);
  for (int I = 0; I < 6; ++I)
    F.Blocks[I].Id = I;
  F.Blocks[0].Succs = {1, 2};
  F.Blocks[1].Preds = {0};
  F.Blocks[1].Succs = {3};
  F.Blocks[2].Preds = {0};
  F.Blocks[2].Succs = {3};
  F.Blocks[3].Preds = {1, 2};
  F.Blocks[3].Succs = {4, 5};
  F.Blocks[4].Preds = {3};
  F.Blocks[5].Preds = {3};
  const MedVar SourceBuffer = mkReg(24, 0);

  auto addBlockCall = [&](int BlockId, llvm::StringRef Name, MedVar Ret,
                          std::vector<MedVar> Args, va_t Addr) {
    MedBlock &Block = F.Blocks[BlockId];
    const int OpIdx = static_cast<int>(Block.Ops.size());
    MedOp Op;
    Op.Opcode = NdOp::CALL;
    Op.Output = Ret;
    Op.Addr = Addr;
    Op.addInput(MedVar::makeConst(0x9000, 8));
    Block.Ops.push_back(std::move(Op));
    MedCallInfo CI;
    CI.BlockId = BlockId;
    CI.OpIdx = OpIdx;
    CI.TargetName = Name.str();
    CI.Args = std::move(Args);
    F.CallInfos.push_back(std::move(CI));
  };

  addBlockCall(0, "malloc", temp(1), {MedVar::makeConst(16, 8)}, MallocVA);
  addBlockCall(
      1, "read", temp(5),
      {MedVar::makeConst(0, 8), SourceBuffer, MedVar::makeConst(32, 8)},
      SourceVA);
  addBlockCall(5, "strcpy", temp(0), {temp(1), SourceBuffer}, SinkVA);

  BinaryImage Img;
  Img.Arch = Arch::X64;
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();
  ArgClassification SourceArg = classifyArgument(In, Cat, F, 2, 1);
  ASSERT_EQ(SourceArg.Flow, ArgFlow::Tainted);
  ASSERT_EQ(SourceArg.TaintSource, "read");

  LowFunc LF = mutuallyExclusiveSourceAndSinkLow(MallocVA, SourceVA, SinkVA);
  auto Fnd = hunt(F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown);
  EXPECT_TRUE(Fnd->Witness.empty());
  EXPECT_EQ(Fnd->Detail, "length provenance unresolved");
}

TEST(HuntEngine, SourceAfterSinkInLoopTaintsTheNextIteration) {
  constexpr va_t MallocVA = 0x400000;
  constexpr va_t SinkVA = 0x400020;
  constexpr va_t SourceVA = 0x400030;
  constexpr va_t SourceBufferVA = 0x5000;
  constexpr uint64_t kSourceBufferReg = 24;

  MedFunc F;
  F.Name = "f";
  F.Entry = MallocVA;
  F.CC = CallingConv::SysV_AMD64;
  F.Blocks.resize(2);
  F.Blocks[0].Id = 0;
  F.Blocks[0].Succs = {1};
  F.Blocks[1].Id = 1;
  F.Blocks[1].Preds = {0, 1};
  F.Blocks[1].Succs = {1};

  auto addBlockCall = [&](int BlockId, llvm::StringRef Name, MedVar Ret,
                          std::vector<MedVar> Args, va_t Addr) {
    MedBlock &Block = F.Blocks[BlockId];
    const int OpIdx = static_cast<int>(Block.Ops.size());
    MedOp Op;
    Op.Opcode = NdOp::CALL;
    Op.Output = Ret;
    Op.Addr = Addr;
    Op.addInput(MedVar::makeConst(0x9000, 8));
    Block.Ops.push_back(std::move(Op));
    MedCallInfo CI;
    CI.BlockId = BlockId;
    CI.OpIdx = OpIdx;
    CI.TargetName = Name.str();
    CI.Args = std::move(Args);
    F.CallInfos.push_back(std::move(CI));
  };

  const MedVar SourceBuffer = mkReg(kSourceBufferReg, 0);
  addBlockCall(0, "malloc", temp(1), {MedVar::makeConst(16, 8)}, MallocVA);
  addBlockCall(1, "strcpy", temp(0), {temp(1), SourceBuffer}, SinkVA);
  addBlockCall(
      1, "read", temp(5),
      {MedVar::makeConst(0, 8), SourceBuffer, MedVar::makeConst(32, 8)},
      SourceVA);

  BinaryImage Img;
  Img.Arch = Arch::X64;
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();
  ArgClassification SourceArg = classifyArgument(In, Cat, F, 1, 1);
  ASSERT_EQ(SourceArg.Flow, ArgFlow::Tainted);
  ASSERT_EQ(SourceArg.TaintSource, "read");

  LowFunc LF =
      sourceAfterSinkLoopLow(MallocVA, SinkVA, SourceVA, SourceBufferVA);
  auto Fnd = hunt(F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unsafe);
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High);
  EXPECT_FALSE(Fnd->Witness.empty());
}

TEST(HuntEngine, ReturnSourceAndSinkMustShareFeasiblePath) {
  constexpr va_t MallocVA = 0x400000;
  constexpr va_t SourceVA = 0x400010;
  constexpr va_t SinkVA = 0x400050;

  MedFunc F;
  F.Name = "f";
  F.Entry = MallocVA;
  F.CC = CallingConv::SysV_AMD64;
  F.Blocks.resize(6);
  for (int I = 0; I < 6; ++I)
    F.Blocks[I].Id = I;
  F.Blocks[0].Succs = {1, 2};
  F.Blocks[1].Preds = {0};
  F.Blocks[1].Succs = {3};
  F.Blocks[2].Preds = {0};
  F.Blocks[2].Succs = {3};
  F.Blocks[3].Preds = {1, 2};
  F.Blocks[3].Succs = {4, 5};
  F.Blocks[4].Preds = {3};
  F.Blocks[5].Preds = {3};

  auto addBlockCall = [&](int BlockId, llvm::StringRef Name, MedVar Ret,
                          std::vector<MedVar> Args, va_t Addr) {
    MedBlock &Block = F.Blocks[BlockId];
    const int OpIdx = static_cast<int>(Block.Ops.size());
    MedOp Op;
    Op.Opcode = NdOp::CALL;
    Op.Output = Ret;
    Op.Addr = Addr;
    Op.addInput(MedVar::makeConst(0x9000, 8));
    Block.Ops.push_back(std::move(Op));
    MedCallInfo CI;
    CI.BlockId = BlockId;
    CI.OpIdx = OpIdx;
    CI.TargetName = Name.str();
    CI.Args = std::move(Args);
    F.CallInfos.push_back(std::move(CI));
  };

  addBlockCall(0, "malloc", temp(1), {MedVar::makeConst(16, 8)}, MallocVA);
  addBlockCall(1, "getenv", temp(5), {param(1)}, SourceVA);
  addBlockCall(5, "strcpy", temp(0), {temp(1), temp(5)}, SinkVA);

  BinaryImage Img;
  Img.Arch = Arch::X64;
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();
  ArgClassification SourceArg = classifyArgument(In, Cat, F, 2, 1);
  ASSERT_EQ(SourceArg.Flow, ArgFlow::Tainted);
  ASSERT_EQ(SourceArg.TaintSource, "getenv");

  LowFunc LF = mutuallyExclusiveSourceAndSinkLow(MallocVA, SourceVA, SinkVA);
  auto Fnd = hunt(F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown) << Fnd->Detail;
  EXPECT_TRUE(Fnd->Witness.empty());
  EXPECT_EQ(Fnd->Detail, "length provenance unresolved");
}

TEST(HuntEngine, ReturnSourceOnSinkPathCorroboratesImplicitCopy) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400020;
  const MedVar SourceResult = mkReg(0, 1);
  Builder B("main");
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("getenv", SourceResult, {param(1)});
  B.F.Blocks[0].Ops.back().Addr = SourceVA;
  B.call("strcpy", temp(0), {temp(1), SourceResult});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;
  LowFunc LF = returnSourceThenFormatLow(SourceVA, SinkVA,
                                         /*RequiredReturn=*/1);

  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unsafe) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High) << Fnd->Detail;
  EXPECT_FALSE(Fnd->Witness.empty());
}

TEST(HuntEngine, ScanfOutputAndSinkMustShareFeasiblePath) {
  constexpr va_t MallocVA = 0x400000;
  constexpr va_t SourceVA = 0x400010;
  constexpr va_t SinkVA = 0x400050;
  BinaryImage Img;
  const va_t FormatVA = addCString(Img, "%s");

  MedFunc F;
  F.Name = "f";
  F.Entry = MallocVA;
  F.CC = CallingConv::SysV_AMD64;
  F.Blocks.resize(6);
  for (int I = 0; I < 6; ++I)
    F.Blocks[I].Id = I;
  F.Blocks[0].Succs = {1, 2};
  F.Blocks[1].Preds = {0};
  F.Blocks[1].Succs = {3};
  F.Blocks[2].Preds = {0};
  F.Blocks[2].Succs = {3};
  F.Blocks[3].Preds = {1, 2};
  F.Blocks[3].Succs = {4, 5};
  F.Blocks[4].Preds = {3};
  F.Blocks[5].Preds = {3};
  const MedVar SourceBuffer = mkReg(24, 0);

  auto addBlockCall = [&](int BlockId, llvm::StringRef Name, MedVar Ret,
                          std::vector<MedVar> Args, va_t Addr) {
    MedBlock &Block = F.Blocks[BlockId];
    const int OpIdx = static_cast<int>(Block.Ops.size());
    MedOp Op;
    Op.Opcode = NdOp::CALL;
    Op.Output = Ret;
    Op.Addr = Addr;
    Op.addInput(MedVar::makeConst(0x9000, 8));
    Block.Ops.push_back(std::move(Op));
    MedCallInfo CI;
    CI.BlockId = BlockId;
    CI.OpIdx = OpIdx;
    CI.TargetName = Name.str();
    CI.Args = std::move(Args);
    F.CallInfos.push_back(std::move(CI));
  };

  addBlockCall(0, "malloc", temp(1), {MedVar::makeConst(16, 8)}, MallocVA);
  addBlockCall(1, "scanf", temp(5),
               {MedVar::makeConst(FormatVA, 8), SourceBuffer}, SourceVA);
  addBlockCall(5, "strcpy", temp(0), {temp(1), SourceBuffer}, SinkVA);

  LowFunc LF = mutuallyExclusiveSourceAndSinkLow(MallocVA, SourceVA, SinkVA);
  auto Fnd = hunt(F, /*StackRegs=*/false, &LF, Arch::X64, {},
                  BinaryFormat::Unknown, &Img);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown);
  EXPECT_TRUE(Fnd->Witness.empty());
  EXPECT_EQ(Fnd->Detail, "length provenance unresolved");
}

TEST(HuntEngine, ReachableUnboundedScanfOutputIsUnsafe) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400010;
  BinaryImage Img;
  const va_t FormatVA = addCString(Img, "%s");
  const MedVar SourceBuffer = mkReg(24, 0);

  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("scanf", temp(5), {MedVar::makeConst(FormatVA, 8), SourceBuffer});
  B.F.Blocks[0].Ops.back().Addr = SourceVA;
  B.call("strcpy", temp(0), {temp(1), SourceBuffer});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;

  LowFunc LF = sourceReturnThenMemcpyLow(SourceVA, SinkVA,
                                         /*RequireNonnegative=*/false);
  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {},
                  BinaryFormat::Unknown, &Img);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unsafe);
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High);
  EXPECT_FALSE(Fnd->Witness.empty());
}

TEST(HuntEngine, GuardedBoundedScanfStringFitsImplicitCopy) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400010;
  BinaryImage Img;
  const va_t FormatVA = addCString(Img, "%7s");
  const MedVar SourceBuffer = mkReg(24, 0);

  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(8, 8)});
  B.call("scanf", temp(5, 4), {MedVar::makeConst(FormatVA, 8), SourceBuffer});
  B.F.Blocks[0].Ops.back().Addr = SourceVA;
  B.call("strcpy", temp(0), {temp(1), SourceBuffer});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;

  LowFunc LF = scanfThenStrcpyLow(SourceVA, SinkVA);
  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {},
                  BinaryFormat::ELF, &Img);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Safe) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High) << Fnd->Detail;
}

TEST(HuntEngine, GuardedBoundedScanfStringCanOverflowSmallerDestination) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400010;
  BinaryImage Img;
  const va_t FormatVA = addCString(Img, "%7s");
  const MedVar SourceBuffer = mkReg(24, 0);

  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(7, 8)});
  B.call("scanf", temp(5, 4), {MedVar::makeConst(FormatVA, 8), SourceBuffer});
  B.F.Blocks[0].Ops.back().Addr = SourceVA;
  B.call("strcpy", temp(0), {temp(1), SourceBuffer});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;

  LowFunc LF = scanfThenStrcpyLow(SourceVA, SinkVA);
  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {},
                  BinaryFormat::ELF, &Img);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unsafe) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High) << Fnd->Detail;
  EXPECT_TRUE(std::any_of(
      Fnd->Witness.begin(), Fnd->Witness.end(), [](const auto &Item) {
        return Item.first == "copy_length" && Item.second == "8";
      }));
}

TEST(HuntEngine, ScanfCharacterOutputHasNoImplicitStringBound) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400010;
  for (const char *Format : {"%c", "%7c"}) {
    SCOPED_TRACE(Format);
    BinaryImage Img;
    const va_t FormatVA = addCString(Img, Format);
    const MedVar SourceBuffer = mkReg(24, 0);

    Builder B;
    B.F.Entry = 0x400000;
    B.F.CC = CallingConv::SysV_AMD64;
    B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
    B.call("scanf", temp(5, 4), {MedVar::makeConst(FormatVA, 8), SourceBuffer});
    B.F.Blocks[0].Ops.back().Addr = SourceVA;
    B.call("strcpy", temp(0), {temp(1), SourceBuffer});
    B.F.Blocks[0].Ops.back().Addr = SinkVA;

    LowFunc LF = scanfThenStrcpyLow(SourceVA, SinkVA);
    auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {},
                    BinaryFormat::ELF, &Img);
    ASSERT_TRUE(Fnd.has_value());
    EXPECT_EQ(Fnd->TheVerdict, Verdict::Unsafe) << Fnd->Detail;
    EXPECT_EQ(Fnd->TheConfidence, Confidence::High) << Fnd->Detail;
  }
}

TEST(HuntEngine, ScanfOutputRequiresItsOwnAssignmentCount) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400010;
  struct Case {
    const char *Format;
    uint32_t AssignmentCount;
    Verdict Expected;
  };
  for (const Case C :
       {Case{"%7s%7s", 1, Verdict::Unknown}, Case{"%7s%7s", 2, Verdict::Safe},
        Case{"%7s%s", 1, Verdict::Unknown},
        Case{"%7s%s", 2, Verdict::Unsafe}}) {
    SCOPED_TRACE(C.Format);
    SCOPED_TRACE(C.AssignmentCount);
    BinaryImage Img;
    const va_t FormatVA = addCString(Img, C.Format);
    const MedVar FirstOutput = mkReg(24, 0);
    const MedVar SecondOutput = mkReg(32, 0);

    Builder B;
    B.F.Entry = 0x400000;
    B.F.CC = CallingConv::SysV_AMD64;
    B.call("malloc", temp(1), {MedVar::makeConst(8, 8)});
    B.call("scanf", temp(5, 4),
           {MedVar::makeConst(FormatVA, 8), FirstOutput, SecondOutput});
    B.F.Blocks[0].Ops.back().Addr = SourceVA;
    B.call("strcpy", temp(0), {temp(1), SecondOutput});
    B.F.Blocks[0].Ops.back().Addr = SinkVA;

    LowFunc LF =
        scanfExactCountThenStrcpyLow(SourceVA, SinkVA, C.AssignmentCount);
    auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {},
                    BinaryFormat::ELF, &Img);
    ASSERT_TRUE(Fnd.has_value());
    EXPECT_EQ(Fnd->TheVerdict, C.Expected) << Fnd->Detail;
    if (C.Expected != Verdict::Unknown)
      EXPECT_EQ(Fnd->TheConfidence, Confidence::High) << Fnd->Detail;
  }
}

TEST(HuntEngine, LaterUnboundedScanfInvalidatesEarlierStringBound) {
  constexpr va_t FirstSourceVA = 0x400004;
  constexpr va_t SecondSourceVA = 0x400008;
  constexpr va_t SinkVA = 0x400010;
  BinaryImage Img;
  const va_t BoundedFormatVA = addCString(Img, "%7s", 0x1000);
  const va_t UnboundedFormatVA = addCString(Img, "%s", 0x1100);
  const MedVar SourceBuffer = mkReg(24, 0);

  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(8, 8)});
  B.call("scanf", temp(5, 4),
         {MedVar::makeConst(BoundedFormatVA, 8), SourceBuffer});
  B.F.Blocks[0].Ops.back().Addr = FirstSourceVA;
  B.call("scanf", temp(6, 4),
         {MedVar::makeConst(UnboundedFormatVA, 8), SourceBuffer});
  B.F.Blocks[0].Ops.back().Addr = SecondSourceVA;
  B.call("strcpy", temp(0), {temp(1), SourceBuffer});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;

  LowFunc LF = twoScanfThenStrcpyLow(FirstSourceVA, SecondSourceVA, SinkVA);
  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {},
                  BinaryFormat::ELF, &Img);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unsafe) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High) << Fnd->Detail;
}

TEST(HuntEngine, TaintedSscanfInputDrivesReachableUnboundedCopy) {
  constexpr va_t ReadVA = 0x400004;
  constexpr va_t ScanfVA = 0x400020;
  constexpr va_t SinkVA = 0x400040;
  BinaryImage Img;
  const va_t FormatVA = addCString(Img, "%s");
  const MedVar InputBuffer = param(3);
  const MedVar OutputBuffer = param(4);

  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("read", temp(5),
         {MedVar::makeConst(0, 8), InputBuffer, MedVar::makeConst(32, 8)});
  B.F.Blocks[0].Ops.back().Addr = ReadVA;
  B.call("sscanf", temp(6, 4),
         {InputBuffer, MedVar::makeConst(FormatVA, 8), OutputBuffer});
  B.F.Blocks[0].Ops.back().Addr = ScanfVA;
  B.call("strcpy", temp(0), {temp(1), OutputBuffer});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;

  LowFunc LF = readAndScanfThenStrcpyLow(ReadVA, ScanfVA, SinkVA);
  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {},
                  BinaryFormat::ELF, &Img);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unsafe) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High) << Fnd->Detail;
}

TEST(HuntEngine, TaintedSscanfSubrangeRequiresWrittenInput) {
  constexpr va_t ReadVA = 0x400004;
  constexpr va_t ScanfVA = 0x400020;
  constexpr va_t SinkVA = 0x400040;
  constexpr va_t InputBufferVA = 0x7000;
  BinaryImage Img;
  const va_t FormatVA = addCString(Img, "%s");
  const MedVar InputBuffer = MedVar::makeConst(
      InputBufferVA, 8, ConstantAddressProvenance::DataAddress, InputBufferVA);
  const MedVar InputSubrange =
      MedVar::makeConst(InputBufferVA + 1, 8,
                        ConstantAddressProvenance::DataAddress, InputBufferVA);
  const MedVar OutputBuffer = param(4);

  for (const auto &[ReadBytes, Expected] :
       {std::pair<uint64_t, Verdict>{2, Verdict::Unsafe},
        {1, Verdict::Unknown}}) {
    SCOPED_TRACE(ReadBytes);
    Builder B;
    B.F.Entry = 0x400000;
    B.F.CC = CallingConv::SysV_AMD64;
    B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
    B.call("read", temp(5),
           {MedVar::makeConst(0, 8), InputBuffer, MedVar::makeConst(32, 8)});
    B.F.Blocks[0].Ops.back().Addr = ReadVA;
    B.call("sscanf", temp(6, 4),
           {InputSubrange, MedVar::makeConst(FormatVA, 8), OutputBuffer});
    B.F.Blocks[0].Ops.back().Addr = ScanfVA;
    B.call("strcpy", temp(0), {temp(1), OutputBuffer});
    B.F.Blocks[0].Ops.back().Addr = SinkVA;

    AnalysisInput In;
    In.Img = &Img;
    SinkCatalog Cat = SinkCatalog::defaults();
    const ArgClassification Source =
        classifyArgument(In, Cat, B.F, /*CallInfoIndex=*/3, /*ArgIndex=*/1);
    ASSERT_EQ(Source.Flow, ArgFlow::Tainted);
    ASSERT_EQ(Source.TaintSource, "sscanf");

    LowFunc LF = readAndScanfThenStrcpyLow(ReadVA, ScanfVA, SinkVA, ReadBytes);
    const auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {},
                          BinaryFormat::ELF, &Img);
    ASSERT_TRUE(Fnd.has_value());
    EXPECT_EQ(Fnd->TheVerdict, Expected) << Fnd->Detail;
    EXPECT_EQ(Fnd->TheConfidence,
              Expected == Verdict::Unsafe ? Confidence::High : Confidence::Low)
        << Fnd->Detail;
  }
}

TEST(HuntEngine, DirectStringSourceSubrangeRequiresWrittenInput) {
  constexpr va_t ReadVA = 0x400004;
  constexpr va_t SinkVA = 0x400020;
  constexpr va_t InputBufferVA = 0x7000;
  const MedVar InputBuffer = MedVar::makeConst(
      InputBufferVA, 8, ConstantAddressProvenance::DataAddress, InputBufferVA);
  const MedVar InputSubrange =
      MedVar::makeConst(InputBufferVA + 1, 8,
                        ConstantAddressProvenance::DataAddress, InputBufferVA);

  for (const auto &[ReadBytes, Expected] :
       {std::pair<uint64_t, Verdict>{2, Verdict::Unsafe},
        {1, Verdict::Unknown}}) {
    SCOPED_TRACE(ReadBytes);
    Builder B;
    B.F.Entry = 0x400000;
    B.F.CC = CallingConv::SysV_AMD64;
    B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
    B.call("read", temp(5),
           {MedVar::makeConst(0, 8), InputBuffer, MedVar::makeConst(32, 8)});
    B.F.Blocks[0].Ops.back().Addr = ReadVA;
    B.call("strcpy", temp(0), {temp(1), InputSubrange});
    B.F.Blocks[0].Ops.back().Addr = SinkVA;

    LowFunc LF = readThenStrcpyLow(ReadVA, SinkVA, ReadBytes);
    const auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
    ASSERT_TRUE(Fnd.has_value());
    EXPECT_EQ(Fnd->TheVerdict, Expected) << Fnd->Detail;
    EXPECT_EQ(Fnd->TheConfidence,
              Expected == Verdict::Unsafe ? Confidence::High : Confidence::Low)
        << Fnd->Detail;
  }
}

TEST(HuntEngine, DirectStringSourceSubrangeRequiresTheSameOwner) {
  constexpr va_t ScanfVA = 0x400004;
  constexpr va_t SinkVA = 0x400020;
  constexpr va_t InputBufferVA = 0x7000;
  BinaryImage Img;
  const va_t FormatVA = addCString(Img, "%7s");
  const MedVar InputBuffer = MedVar::makeConst(
      InputBufferVA, 8, ConstantAddressProvenance::DataAddress, InputBufferVA);
  const MedVar DifferentOwner = MedVar::makeConst(
      InputBufferVA + 1, 8, ConstantAddressProvenance::DataAddress, 0x8000);

  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(8, 8)});
  B.call("scanf", temp(5, 4), {MedVar::makeConst(FormatVA, 8), InputBuffer});
  B.F.Blocks[0].Ops.back().Addr = ScanfVA;
  B.call("strcpy", temp(0), {temp(1), DifferentOwner});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;

  LowFunc LF = scanfThenStrcpyLow(ScanfVA, SinkVA);
  const auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {},
                        BinaryFormat::ELF, &Img);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::Low) << Fnd->Detail;
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, TaintedSscanfInputPreservesBoundedStringExtent) {
  constexpr va_t ReadVA = 0x400004;
  constexpr va_t ScanfVA = 0x400020;
  constexpr va_t SinkVA = 0x400040;
  BinaryImage Img;
  const va_t FormatVA = addCString(Img, "%7s");
  const MedVar InputBuffer = param(3);
  const MedVar OutputBuffer = param(4);

  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(8, 8)});
  B.call("read", temp(5),
         {MedVar::makeConst(0, 8), InputBuffer, MedVar::makeConst(32, 8)});
  B.F.Blocks[0].Ops.back().Addr = ReadVA;
  B.call("sscanf", temp(6, 4),
         {InputBuffer, MedVar::makeConst(FormatVA, 8), OutputBuffer});
  B.F.Blocks[0].Ops.back().Addr = ScanfVA;
  B.call("strcpy", temp(0), {temp(1), OutputBuffer});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;

  LowFunc LF = readAndScanfThenStrcpyLow(ReadVA, ScanfVA, SinkVA);
  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {},
                  BinaryFormat::ELF, &Img);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Safe) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High) << Fnd->Detail;
}

TEST(HuntEngine, ScanfNumericOutputCanDriveReachableOverflow) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400010;
  constexpr va_t LengthSlotVA = 0x7000;
  BinaryImage Img;
  const va_t FormatVA = addCString(Img, "%zu");
  const MedVar LengthSlot = MedVar::makeConst(
      LengthSlotVA, 8, ConstantAddressProvenance::DataAddress, LengthSlotVA);

  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("scanf", temp(5), {MedVar::makeConst(FormatVA, 8), LengthSlot});
  B.F.Blocks[0].Ops.back().Addr = SourceVA;
  B.op(NdOp::LOAD, temp(6), {LengthSlot});
  B.call("memcpy", temp(0), {temp(1), temp(2), temp(6)});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;

  LowFunc LF = formattedScalarThenMemcpyLow(SourceVA, SinkVA, LengthSlotVA);
  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {},
                  BinaryFormat::ELF, &Img);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unsafe) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High);
  EXPECT_TRUE(
      std::any_of(Fnd->Witness.begin(), Fnd->Witness.end(),
                  [](const auto &Item) { return Item.first == "scanf"; }));
}

TEST(HuntEngine, ScanfMalformedFormatPointerWidthDoesNotAuthenticateOutput) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400010;
  constexpr va_t FormatVA = 0x10;
  constexpr va_t LengthSlotVA = 0x7000;
  BinaryImage Img;
  addCString(Img, "%hhu", FormatVA);
  const MedVar LengthSlot = MedVar::makeConst(
      LengthSlotVA, 8, ConstantAddressProvenance::DataAddress, LengthSlotVA);

  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("scanf", temp(5, 4), {MedVar::makeConst(FormatVA, 1), LengthSlot});
  B.F.Blocks[0].Ops.back().Addr = SourceVA;
  B.op(NdOp::LOAD, temp(6, 1), {LengthSlot});
  B.op(NdOp::INT_ZEXT, temp(7), {temp(6, 1)});
  B.call("memcpy", temp(0), {temp(1), temp(2), temp(7)});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;

  LowFunc LF = formattedScalarThenMemcpyLow(SourceVA, SinkVA, LengthSlotVA,
                                            /*RequiredAssignments=*/1,
                                            /*ScalarBytes=*/1);
  const auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {},
                        BinaryFormat::ELF, &Img);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_NE(Fnd->Flow, ArgFlow::Tainted) << Fnd->Detail;
  EXPECT_FALSE(std::any_of(
      Fnd->Witness.begin(), Fnd->Witness.end(), [](const auto &Item) {
        return Item.first == "scanf" || Item.second == "scanf";
      }));
}

TEST(HuntEngine, NarrowScanfReturnWidthFailsClosed) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400010;
  constexpr va_t LengthSlotVA = 0x7000;
  BinaryImage Img;
  const va_t FormatVA = addCString(Img, "%hhu");
  const MedVar LengthSlot = MedVar::makeConst(
      LengthSlotVA, 8, ConstantAddressProvenance::DataAddress, LengthSlotVA);

  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("scanf", temp(5, 4), {MedVar::makeConst(FormatVA, 8), LengthSlot});
  B.F.Blocks[0].Ops.back().Addr = SourceVA;
  B.op(NdOp::LOAD, temp(6, 1), {LengthSlot});
  B.op(NdOp::INT_ZEXT, temp(7), {temp(6, 1)});
  B.call("memcpy", temp(0), {temp(1), temp(2), temp(7)});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;

  LowFunc LF = formattedScalarThenMemcpyLow(SourceVA, SinkVA, LengthSlotVA,
                                            /*RequiredAssignments=*/1,
                                            /*ScalarBytes=*/1);
  LF.Blocks[0].Ops[0].Output.Size = 1;
  const auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {},
                        BinaryFormat::ELF, &Img);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::Low) << Fnd->Detail;
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, ZeroWidthScanfReturnFailsClosed) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400010;
  constexpr va_t LengthSlotVA = 0x7000;
  BinaryImage Img;
  const va_t FormatVA = addCString(Img, "%hhu");
  const MedVar LengthSlot = MedVar::makeConst(
      LengthSlotVA, 8, ConstantAddressProvenance::DataAddress, LengthSlotVA);

  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("scanf", temp(5, 4), {MedVar::makeConst(FormatVA, 8), LengthSlot});
  B.F.Blocks[0].Ops.back().Addr = SourceVA;
  B.op(NdOp::LOAD, temp(6, 1), {LengthSlot});
  B.op(NdOp::INT_ZEXT, temp(7), {temp(6, 1)});
  B.call("memcpy", temp(0), {temp(1), temp(2), temp(7)});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;

  LowFunc LF = formattedScalarThenMemcpyLow(SourceVA, SinkVA, LengthSlotVA,
                                            /*RequiredAssignments=*/1,
                                            /*ScalarBytes=*/1);
  LF.Blocks[0].Ops[0].Output.Size = 0;
  const auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {},
                        BinaryFormat::ELF, &Img);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::Low) << Fnd->Detail;
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, NarrowScanfOutputPointerFailsClosed) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400010;
  constexpr va_t LengthSlotVA = 0x70;
  BinaryImage Img;
  const va_t FormatVA = addCString(Img, "%hhu");
  MedVar NarrowLengthSlot = MedVar::makeConst(
      LengthSlotVA, 1, ConstantAddressProvenance::DataAddress, LengthSlotVA);

  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("scanf", temp(5, 4),
         {MedVar::makeConst(FormatVA, 8), NarrowLengthSlot});
  B.F.Blocks[0].Ops.back().Addr = SourceVA;
  B.op(NdOp::LOAD, temp(6, 1), {NarrowLengthSlot});
  B.op(NdOp::INT_ZEXT, temp(7), {temp(6, 1)});
  B.call("memcpy", temp(0), {temp(1), temp(2), temp(7)});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;

  LowFunc LF = formattedScalarThenMemcpyLow(SourceVA, SinkVA, LengthSlotVA,
                                            /*RequiredAssignments=*/1,
                                            /*ScalarBytes=*/1);
  const auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {},
                        BinaryFormat::ELF, &Img);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::Low) << Fnd->Detail;
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, NarrowCountedSourceReturnWidthFailsClosed) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400010;
  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("read", temp(5),
         {MedVar::makeConst(0, 8), temp(2), MedVar::makeConst(8, 8)});
  B.F.Blocks[0].Ops.back().Addr = SourceVA;
  B.call("memcpy", temp(0), {temp(1), temp(2), temp(5)});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;

  LowFunc LF = sourceReturnThenMemcpyLow(SourceVA, SinkVA,
                                         /*RequireNonnegative=*/true);
  LF.Blocks[0].Ops[0].Output.Size = 1;
  const auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::Low) << Fnd->Detail;
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, NarrowReadFileReturnWidthFailsClosed) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400020;
  constexpr va_t BytesReadVA = 0x700000;
  const MedVar SourceBuffer = param(2);
  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::Win64;
  B.call("ReadFile", temp(5, 4),
         {MedVar::makeConst(1, 8), SourceBuffer, MedVar::makeConst(32, 8),
          MedVar::makeConst(BytesReadVA, 8), MedVar::makeConst(0, 8)});
  B.F.Blocks[0].Ops.back().Addr = SourceVA;
  B.call("printf", temp(0), {SourceBuffer});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;

  LowFunc LF = readFileResultThenFormatLow(SourceVA, SinkVA, BytesReadVA,
                                           /*ReturnValue=*/1,
                                           /*BytesRead=*/1);
  LF.Blocks[0].Ops[0].Output.Size = 1;
  const auto Fnd =
      hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {}, BinaryFormat::COFF);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::Low) << Fnd->Detail;
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, NarrowFgetsReturnWidthFailsClosed) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400010;
  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(8, 8)});
  B.call("fgets", temp(5), {param(2), MedVar::makeConst(8, 8), temp(6)});
  B.F.Blocks[0].Ops.back().Addr = SourceVA;
  B.call("strcpy", temp(0), {temp(1), param(2)});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;

  LowFunc LF = fgetsThenStrcpyLow(SourceVA, SinkVA,
                                  /*RequireSuccess=*/true);
  LF.Blocks[0].Ops[0].Output.Size = 1;
  const auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::Low) << Fnd->Detail;
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, NarrowEnvironmentQueryReturnWidthFailsClosed) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400020;
  const MedVar SourceBuffer = mkReg(24, 0);
  Builder B("main");
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::Win64;
  B.call("GetEnvironmentVariableA", mkReg(0, 1, 4),
         {param(1), SourceBuffer, MedVar::makeConst(32, 8)});
  B.F.Blocks[0].Ops.back().Addr = SourceVA;
  B.call("printf", temp(0), {SourceBuffer});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;

  LowFunc LF = returnSourceThenFormatLow(SourceVA, SinkVA,
                                         /*RequiredReturn=*/1,
                                         /*ReturnBytes=*/1);
  LF.Blocks[0].Ops[0].Output.Size = 1;
  const auto Fnd =
      hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {}, BinaryFormat::COFF);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::Low) << Fnd->Detail;
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, NonAbiStringLengthReturnWidthFailsClosed) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400010;
  for (const uint16_t ReturnBytes : {uint16_t(1), uint16_t(16)}) {
    SCOPED_TRACE(ReturnBytes);
    Builder B;
    B.F.Entry = 0x400000;
    B.F.CC = CallingConv::SysV_AMD64;
    B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
    B.call("strlen", temp(5, ReturnBytes), {param(2)});
    B.F.Blocks[0].Ops.back().Addr = SourceVA;
    B.call("strcpy", temp(0), {temp(1), param(2)});
    B.F.Blocks[0].Ops.back().Addr = SinkVA;

    LowFunc LF = strlenGuardedStrcpyLow(SourceVA, SinkVA,
                                        /*OverflowGuard=*/false);
    LF.Blocks[0].Ops[0].Output = NdVar::reg(0, ReturnBytes);
    const auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
    ASSERT_TRUE(Fnd.has_value());
    EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown) << Fnd->Detail;
    EXPECT_EQ(Fnd->TheConfidence, Confidence::Low) << Fnd->Detail;
    EXPECT_TRUE(Fnd->Witness.empty());
  }
}

TEST(HuntEngine, ScanfCharacterOutputCanDriveReachableOverflow) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400010;
  constexpr va_t LengthSlotVA = 0x7000;
  for (const auto &[Format, Bytes] :
       {std::pair<const char *, uint16_t>{"%c", 1}, {"%7c", 7}}) {
    SCOPED_TRACE(Format);
    BinaryImage Img;
    const va_t FormatVA = addCString(Img, Format);
    const MedVar LengthSlot = MedVar::makeConst(
        LengthSlotVA, 8, ConstantAddressProvenance::DataAddress, LengthSlotVA);

    Builder B;
    B.F.Entry = 0x400000;
    B.F.CC = CallingConv::SysV_AMD64;
    B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
    B.call("scanf", temp(5, 4), {MedVar::makeConst(FormatVA, 8), LengthSlot});
    B.F.Blocks[0].Ops.back().Addr = SourceVA;
    B.op(NdOp::LOAD, temp(6, Bytes), {LengthSlot});
    B.op(NdOp::INT_ZEXT, temp(7), {temp(6, Bytes)});
    B.call("memcpy", temp(0), {temp(1), temp(2), temp(7)});
    B.F.Blocks[0].Ops.back().Addr = SinkVA;

    LowFunc LF = formattedScalarThenMemcpyLow(SourceVA, SinkVA, LengthSlotVA,
                                              /*RequiredAssignments=*/1, Bytes);
    const auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {},
                          BinaryFormat::ELF, &Img);
    ASSERT_TRUE(Fnd.has_value());
    EXPECT_EQ(Fnd->TheVerdict, Verdict::Unsafe) << Fnd->Detail;
    EXPECT_EQ(Fnd->TheConfidence, Confidence::High) << Fnd->Detail;
    EXPECT_TRUE(
        std::any_of(Fnd->Witness.begin(), Fnd->Witness.end(),
                    [](const auto &Item) { return Item.first == "scanf"; }));
  }
}

TEST(HuntEngine, ScanfWideCharacterOutputUsesTargetABIWidth) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400010;
  constexpr va_t LengthSlotVA = 0x7000;
  struct Case {
    const char *Format;
    BinaryFormat Binary;
    uint16_t Bytes;
  };
  for (const Case &C :
       {Case{"%lc", BinaryFormat::COFF, 2}, Case{"%2lc", BinaryFormat::COFF, 4},
        Case{"%lc", BinaryFormat::ELF, 4}, Case{"%2lc", BinaryFormat::ELF, 8},
        Case{"%lc", BinaryFormat::MachO, 4}}) {
    SCOPED_TRACE(C.Format);
    SCOPED_TRACE(static_cast<int>(C.Binary));
    BinaryImage Img;
    Img.Format = C.Binary;
    const va_t FormatVA = addCString(Img, C.Format);
    const MedVar LengthSlot = MedVar::makeConst(
        LengthSlotVA, 8, ConstantAddressProvenance::DataAddress, LengthSlotVA);

    Builder B;
    B.F.Entry = 0x400000;
    B.F.CC = CallingConv::SysV_AMD64;
    B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
    B.call("scanf", temp(5, 4), {MedVar::makeConst(FormatVA, 8), LengthSlot});
    B.F.Blocks[0].Ops.back().Addr = SourceVA;
    B.op(NdOp::LOAD, temp(6, C.Bytes), {LengthSlot});
    B.op(NdOp::INT_ZEXT, temp(7), {temp(6, C.Bytes)});
    B.call("memcpy", temp(0), {temp(1), temp(2), temp(7)});
    B.F.Blocks[0].Ops.back().Addr = SinkVA;

    LowFunc LF = formattedScalarThenMemcpyLow(
        SourceVA, SinkVA, LengthSlotVA, /*RequiredAssignments=*/1, C.Bytes);
    const auto Fnd =
        hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {}, C.Binary, &Img);
    ASSERT_TRUE(Fnd.has_value());
    EXPECT_EQ(Fnd->TheVerdict, Verdict::Unsafe) << Fnd->Detail;
    EXPECT_EQ(Fnd->TheConfidence, Confidence::High) << Fnd->Detail;
    EXPECT_TRUE(
        std::any_of(Fnd->Witness.begin(), Fnd->Witness.end(),
                    [](const auto &Item) { return Item.first == "scanf"; }));
  }
}

TEST(HuntEngine, FailedScanfNumericAssignmentDoesNotDriveOverflow) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400010;
  constexpr va_t LengthSlotVA = 0x7000;
  BinaryImage Img;
  const va_t FormatVA = addCString(Img, "%zu");
  const MedVar LengthSlot = MedVar::makeConst(
      LengthSlotVA, 8, ConstantAddressProvenance::DataAddress, LengthSlotVA);

  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("scanf", temp(5), {MedVar::makeConst(FormatVA, 8), LengthSlot});
  B.F.Blocks[0].Ops.back().Addr = SourceVA;
  B.op(NdOp::LOAD, temp(6), {LengthSlot});
  B.call("memcpy", temp(0), {temp(1), temp(2), temp(6)});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;

  LowFunc LF = formattedScalarThenMemcpyLow(SourceVA, SinkVA, LengthSlotVA, 0);
  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {},
                  BinaryFormat::ELF, &Img);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown) << Fnd->Detail;
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, SuppressedScanfFailureDoesNotDrivePercentNOverflow) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400010;
  constexpr va_t LengthSlotVA = 0x7000;
  BinaryImage Img;
  const va_t FormatVA = addCString(Img, "%*s%n");
  const MedVar LengthSlot = MedVar::makeConst(
      LengthSlotVA, 8, ConstantAddressProvenance::DataAddress, LengthSlotVA);

  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("scanf", temp(5), {MedVar::makeConst(FormatVA, 8), LengthSlot});
  B.F.Blocks[0].Ops.back().Addr = SourceVA;
  B.op(NdOp::LOAD, temp(6), {LengthSlot});
  B.call("memcpy", temp(0), {temp(1), temp(2), temp(6)});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;

  LowFunc LF = formattedScalarThenMemcpyLow(SourceVA, SinkVA, LengthSlotVA, 0);
  const auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {},
                        BinaryFormat::ELF, &Img);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown) << Fnd->Detail;
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, GuardedFgetsBoundsImplicitStringCopy) {
  for (const char *Name : {"fgets", "fgets_unlocked"}) {
    SCOPED_TRACE(Name);
    constexpr va_t SourceVA = 0x400004;
    constexpr va_t SinkVA = 0x400010;
    Builder B;
    B.F.Entry = 0x400000;
    B.F.CC = CallingConv::SysV_AMD64;
    B.call("malloc", temp(1), {MedVar::makeConst(8, 8)});
    B.call(Name, temp(5), {param(2), MedVar::makeConst(8, 8), temp(6)});
    B.F.Blocks[0].Ops.back().Addr = SourceVA;
    B.call("strcpy", temp(0), {temp(1), param(2)});
    B.F.Blocks[0].Ops.back().Addr = SinkVA;
    LowFunc LF = fgetsThenStrcpyLow(SourceVA, SinkVA, /*RequireSuccess=*/true);

    auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
    ASSERT_TRUE(Fnd.has_value());
    EXPECT_EQ(Fnd->TheVerdict, Verdict::Safe) << Fnd->Detail;
    EXPECT_EQ(Fnd->TheConfidence, Confidence::High) << Fnd->Detail;
  }
}

TEST(HuntEngine, OverwrittenPointerSpillDoesNotInheritPriorFormattedBound) {
  constexpr va_t ScanfVA = 0x400020;
  constexpr va_t StrcpyVA = 0x400038;
  constexpr va_t FirstBuffer = 0x7000;
  constexpr va_t SecondBuffer = 0x8000;
  constexpr uint64_t kBufferReg = 64;
  BinaryImage Img;
  const va_t FormatVA = addCString(Img, "%7s");

  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(8, 8)});
  B.call("scanf", temp(5, 4),
         {MedVar::makeConst(FormatVA, 8), mkReg(kBufferReg, 0)});
  B.F.Blocks[0].Ops.back().Addr = ScanfVA;
  B.call("strcpy", temp(0), {temp(1), mkReg(kBufferReg, 1)});
  B.F.Blocks[0].Ops.back().Addr = StrcpyVA;

  LowFunc LF = overwrittenPointerSpillThenStrcpyLow(ScanfVA, StrcpyVA,
                                                    FirstBuffer, SecondBuffer);
  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {},
                  BinaryFormat::ELF, &Img);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::Low) << Fnd->Detail;
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, WriteAfterBoundedScanfInvalidatesItsStringBound) {
  constexpr va_t SourceVA = 0x7000;
  constexpr va_t ScanfVA = 0x400004;
  constexpr va_t StrcpyVA = 0x400010;
  BinaryImage Img;
  const va_t FormatVA = addCString(Img, "%7s");
  MedVar Source = MedVar::makeConst(SourceVA, 8);
  Source.Provenance = ConstantAddressProvenance::DataAddress;
  Source.AddressOwnerVA = SourceVA;

  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(8, 8)});
  B.call("scanf", temp(5, 4), {MedVar::makeConst(FormatVA, 8), Source});
  B.F.Blocks[0].Ops.back().Addr = ScanfVA;
  B.call("strcpy", temp(0), {temp(1), Source});
  B.F.Blocks[0].Ops.back().Addr = StrcpyVA;
  LowFunc LF = scanfThenStrcpyLow(ScanfVA, StrcpyVA);
  LF.Blocks[0].Ops.insert(LF.Blocks[0].Ops.begin() + 1,
                          lop(NdOp::STORE, NdVar{},
                              {NdVar::cst(SourceVA + 7, 8), NdVar::cst('X', 1)},
                              ScanfVA + 1));

  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {},
                  BinaryFormat::ELF, &Img);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unsafe) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High) << Fnd->Detail;
  EXPECT_FALSE(Fnd->Witness.empty());
}

TEST(HuntEngine, MixedWideStoreCannotPreserveOverwrittenTerminator) {
  constexpr va_t SourceVA = 0x7000;
  constexpr va_t ScanfVA = 0x400004;
  constexpr va_t StrcpyVA = 0x400010;
  BinaryImage Img;
  const va_t FormatVA = addCString(Img, "%7s");
  MedVar Source = MedVar::makeConst(SourceVA, 8);
  Source.Provenance = ConstantAddressProvenance::DataAddress;
  Source.AddressOwnerVA = SourceVA;

  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(8, 8)});
  B.call("scanf", temp(5, 4), {MedVar::makeConst(FormatVA, 8), Source});
  B.F.Blocks[0].Ops.back().Addr = ScanfVA;
  B.call("strcpy", temp(0), {temp(1), Source});
  B.F.Blocks[0].Ops.back().Addr = StrcpyVA;
  LowFunc LF = scanfThenStrcpyLow(ScanfVA, StrcpyVA);
  LF.Blocks[0].Ops.insert(
      LF.Blocks[0].Ops.begin() + 1,
      lop(NdOp::STORE, NdVar{},
          {NdVar::cst(SourceVA + 6, 8), NdVar::cst(0x00585858, 4)},
          ScanfVA + 1));

  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {},
                  BinaryFormat::ELF, &Img);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unsafe) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High) << Fnd->Detail;
  EXPECT_FALSE(Fnd->Witness.empty());
}

TEST(HuntEngine, AddressSpaceStoreInvalidatesOverwrittenTerminator) {
  constexpr va_t SourceVA = 0x7000;
  constexpr va_t ScanfVA = 0x400004;
  constexpr va_t StrcpyVA = 0x400010;
  BinaryImage Img;
  const va_t FormatVA = addCString(Img, "%7s");
  MedVar Source = MedVar::makeConst(SourceVA, 8);
  Source.Provenance = ConstantAddressProvenance::DataAddress;
  Source.AddressOwnerVA = SourceVA;

  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(8, 8)});
  B.call("scanf", temp(5, 4), {MedVar::makeConst(FormatVA, 8), Source});
  B.F.Blocks[0].Ops.back().Addr = ScanfVA;
  B.call("strcpy", temp(0), {temp(1), Source});
  B.F.Blocks[0].Ops.back().Addr = StrcpyVA;
  LowFunc LF = scanfThenStrcpyLow(ScanfVA, StrcpyVA);
  LF.Blocks[0].Ops.insert(
      LF.Blocks[0].Ops.begin() + 1,
      lop(NdOp::STORE, NdVar{},
          {NdVar::cst(0, 1), NdVar::cst(SourceVA + 7, 8), NdVar::cst('X', 1)},
          ScanfVA + 1));

  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {},
                  BinaryFormat::ELF, &Img);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unsafe) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High) << Fnd->Detail;
  EXPECT_FALSE(Fnd->Witness.empty());
}

TEST(HuntEngine, SourceCallRespectsBoundedScanfStringRange) {
  constexpr va_t SourceVA = 0x7000;
  constexpr va_t ScanfVA = 0x400004;
  constexpr va_t ReadVA = 0x400008;
  constexpr va_t StrcpyVA = 0x400010;
  BinaryImage Img;
  const va_t FormatVA = addCString(Img, "%7s");
  for (const auto &[Offset, Expected] :
       {std::pair<uint64_t, Verdict>{7, Verdict::Unsafe},
        std::pair<uint64_t, Verdict>{8, Verdict::Safe}}) {
    SCOPED_TRACE(Offset);
    MedVar Source = MedVar::makeConst(SourceVA, 8);
    Source.Provenance = ConstantAddressProvenance::DataAddress;
    Source.AddressOwnerVA = SourceVA;
    MedVar SecondOutput = MedVar::makeConst(SourceVA + Offset, 8);
    SecondOutput.Provenance = ConstantAddressProvenance::DataAddress;
    SecondOutput.AddressOwnerVA = SourceVA;

    Builder B;
    B.F.Entry = 0x400000;
    B.F.CC = CallingConv::SysV_AMD64;
    B.call("malloc", temp(1), {MedVar::makeConst(8, 8)});
    B.call("scanf", temp(5, 4), {MedVar::makeConst(FormatVA, 8), Source});
    B.F.Blocks[0].Ops.back().Addr = ScanfVA;
    B.call("read", temp(6),
           {MedVar::makeConst(0, 8), SecondOutput, MedVar::makeConst(1, 8)});
    B.F.Blocks[0].Ops.back().Addr = ReadVA;
    B.call("strcpy", temp(0), {temp(1), Source});
    B.F.Blocks[0].Ops.back().Addr = StrcpyVA;
    LowFunc LF = scanfThenOverlappingReadStrcpyLow(ScanfVA, ReadVA, StrcpyVA);

    auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {},
                    BinaryFormat::ELF, &Img);
    ASSERT_TRUE(Fnd.has_value());
    EXPECT_EQ(Fnd->TheVerdict, Expected) << Fnd->Detail;
    EXPECT_EQ(Fnd->TheConfidence, Confidence::High) << Fnd->Detail;
    EXPECT_EQ(Fnd->Witness.empty(), Expected == Verdict::Safe);
  }
}

TEST(HuntEngine, GuardedFortifiedFgetsBoundsImplicitStringCopy) {
  for (const char *Name : {"__fgets_chk", "__fgets_unlocked_chk"}) {
    SCOPED_TRACE(Name);
    constexpr va_t SourceVA = 0x400004;
    constexpr va_t SinkVA = 0x400010;
    Builder B;
    B.F.Entry = 0x400000;
    B.F.CC = CallingConv::SysV_AMD64;
    B.call("malloc", temp(1), {MedVar::makeConst(8, 8)});
    B.call(
        Name, temp(5),
        {param(2), MedVar::makeConst(32, 8), MedVar::makeConst(8, 8), temp(6)});
    B.F.Blocks[0].Ops.back().Addr = SourceVA;
    B.call("strcpy", temp(0), {temp(1), param(2)});
    B.F.Blocks[0].Ops.back().Addr = SinkVA;
    LowFunc LF = fgetsThenStrcpyLow(SourceVA, SinkVA, /*RequireSuccess=*/true);

    auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
    ASSERT_TRUE(Fnd.has_value());
    EXPECT_EQ(Fnd->TheVerdict, Verdict::Safe) << Fnd->Detail;
    EXPECT_EQ(Fnd->TheConfidence, Confidence::High) << Fnd->Detail;
  }
}

TEST(HuntEngine, FortifiedFgetsRejectsBoundBeyondObjectSize) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400010;
  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(8, 8)});
  B.call("__fgets_chk", temp(5),
         {param(2), MedVar::makeConst(8, 8), MedVar::makeConst(9, 8), temp(6)});
  B.F.Blocks[0].Ops.back().Addr = SourceVA;
  B.call("strcpy", temp(0), {temp(1), param(2)});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;
  LowFunc LF = fgetsThenStrcpyLow(SourceVA, SinkVA, /*RequireSuccess=*/true);

  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::Low) << Fnd->Detail;
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, UncheckedFgetsFailureDoesNotClaimBoundedString) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400010;
  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(8, 8)});
  B.call("fgets", temp(5), {param(2), MedVar::makeConst(8, 8), temp(6)});
  B.F.Blocks[0].Ops.back().Addr = SourceVA;
  B.call("strcpy", temp(0), {temp(1), param(2)});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;
  LowFunc LF = fgetsThenStrcpyLow(SourceVA, SinkVA, /*RequireSuccess=*/false);

  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown);
  EXPECT_EQ(Fnd->TheConfidence, Confidence::Low);
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, GuardedFgetsBoundCanWitnessImplicitCopyOverflow) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400010;
  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(8, 8)});
  B.call("fgets", temp(5), {param(2), MedVar::makeConst(9, 8), temp(6)});
  B.F.Blocks[0].Ops.back().Addr = SourceVA;
  B.call("strcpy", temp(0), {temp(1), param(2)});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;
  LowFunc LF = fgetsThenStrcpyLow(SourceVA, SinkVA, /*RequireSuccess=*/true);

  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unsafe) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High) << Fnd->Detail;
  EXPECT_FALSE(Fnd->Witness.empty());
}

TEST(HuntEngine, NegativeFgetsBoundCannotProduceInput) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400010;
  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(8, 8)});
  B.call("fgets", temp(5),
         {param(2), MedVar::makeConst(UINT32_MAX, 8), temp(6)});
  B.F.Blocks[0].Ops.back().Addr = SourceVA;
  B.call("strcpy", temp(0), {temp(1), param(2)});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;
  LowFunc LF = fgetsThenStrcpyLow(SourceVA, SinkVA, /*RequireSuccess=*/true);

  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::Low) << Fnd->Detail;
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, GuardedReadReturnHonorsRequestedCount) {
  for (const char *Name :
       {"read", "pread", "_read", "__read_chk", "__pread_chk"}) {
    SCOPED_TRACE(Name);
    const bool WindowsRead = llvm::StringRef(Name) == "_read";
    const bool Pread = llvm::StringRef(Name).contains("pread");
    const bool Fortified = llvm::StringRef(Name).contains("_chk");
    Builder B;
    B.F.Entry = 0x400000;
    B.F.CC = CallingConv::SysV_AMD64;
    B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
    std::vector<MedVar> Args = {MedVar::makeConst(0, 8), temp(2),
                                MedVar::makeConst(8, 8)};
    if (Pread)
      Args.push_back(MedVar::makeConst(0, 8));
    if (Fortified)
      Args.push_back(MedVar::makeConst(16, 8));
    B.call(Name, temp(5, WindowsRead ? 4 : 8), std::move(Args));
    B.F.Blocks[0].Ops.back().Addr = 0x400004;
    MedVar CopyLength = temp(5, WindowsRead ? 4 : 8);
    if (WindowsRead) {
      B.op(NdOp::INT_SEXT, temp(6), {CopyLength});
      CopyLength = temp(6);
    }
    B.call("memcpy", temp(0), {temp(1), temp(2), CopyLength});
    B.F.Blocks[0].Ops.back().Addr = 0x400010;
    LowFunc LF = sourceReturnThenMemcpyLow(0x400004, 0x400010,
                                           /*RequireNonnegative=*/true,
                                           WindowsRead ? 4 : 8);

    auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {},
                    WindowsRead ? BinaryFormat::COFF : BinaryFormat::Unknown);
    ASSERT_TRUE(Fnd.has_value());
    EXPECT_EQ(Fnd->TheVerdict, Verdict::Safe);
    EXPECT_EQ(Fnd->TheConfidence, Confidence::High);
  }
}

TEST(HuntEngine, NarrowCountedSourceBoundUsesDeclaredArgumentWidth) {
  struct Case {
    const char *Name;
    Arch TargetArch;
    BinaryFormat Format;
  } Cases[] = {{"_read", Arch::X86, BinaryFormat::COFF},
               {"read", Arch::ARM, BinaryFormat::ELF}};

  for (const Case &C : Cases) {
    SCOPED_TRACE(C.Name);
    Builder B;
    B.F.Entry = 0x400000;
    B.call("malloc", temp(1, 4), {MedVar::makeConst(16, 4)});
    B.call(C.Name, temp(5, 4),
           {MedVar::makeConst(0, 4), temp(2, 4),
            MedVar::makeConst(UINT64_C(0x100000008), 4)});
    B.F.Blocks[0].Ops.back().Addr = 0x400004;
    B.op(NdOp::INT_SEXT, mkReg(16, 1), {temp(5, 4)});
    B.call("memcpy", temp(0), {temp(1, 4), temp(2, 4), mkReg(16, 1)});
    B.F.Blocks[0].Ops.back().Addr = 0x400010;
    LowFunc LF = sourceReturnThenMemcpyLow(0x400004, 0x400010,
                                           /*RequireNonnegative=*/true,
                                           /*ReturnBytes=*/4,
                                           /*CopyLengthReg=*/16,
                                           /*CallOutputBytes=*/
                                           C.TargetArch == Arch::ARM ? 4 : 8);

    auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, C.TargetArch, {}, C.Format);
    ASSERT_TRUE(Fnd.has_value());
    EXPECT_EQ(Fnd->TheVerdict, Verdict::Safe) << Fnd->Detail;
    EXPECT_EQ(Fnd->TheConfidence, Confidence::High) << Fnd->Detail;
  }
}

TEST(HuntEngine, ZeroWidthCountedSourceBoundFailsClosed) {
  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("read", temp(5),
         {MedVar::makeConst(0, 8), temp(2), MedVar::makeConst(8, 0)});
  B.F.Blocks[0].Ops.back().Addr = 0x400004;
  B.call("memcpy", temp(0), {temp(1), temp(2), temp(5)});
  B.F.Blocks[0].Ops.back().Addr = 0x400010;
  LowFunc LF = sourceReturnThenMemcpyLow(0x400004, 0x400010,
                                         /*RequireNonnegative=*/true);

  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::Low) << Fnd->Detail;
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, I386CdeclSourceBoundUsesOutgoingStackArgument) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400010;
  constexpr uint64_t CallSP = 0x7000;
  const TargetRegInfo &TRI = getTargetRegInfo(Arch::X86);

  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::CDECL;
  B.call("malloc", temp(1, 4), {MedVar::makeConst(16, 4)});
  B.call("read", temp(5, 4),
         {MedVar::makeConst(0, 4), param(2, 4), temp(12, 4)});
  B.F.Blocks[0].Ops.back().Addr = SourceVA;
  B.op(NdOp::INT_SEXT, mkReg(16, 1), {temp(5, 4)});
  B.call("memcpy", temp(0), {temp(1, 4), param(2, 4), mkReg(16, 1)});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;

  LowFunc LF = sourceReturnThenMemcpyLow(SourceVA, SinkVA,
                                         /*RequireNonnegative=*/true,
                                         /*ReturnBytes=*/4);
  LF.Blocks[0].Ops[0].Output = NdVar::reg(TRI.IntReturnReg, 4);
  LF.Blocks[0].Ops.insert(LF.Blocks[0].Ops.begin(),
                          lop(NdOp::COPY, NdVar::reg(TRI.StackPointer, 4),
                              {NdVar::cst(CallSP, 4)}, SourceVA - 2));
  LF.Blocks[0].Ops.insert(LF.Blocks[0].Ops.begin() + 1,
                          lop(NdOp::STORE, NdVar{},
                              {NdVar::cst(CallSP + 2 * 4, 4), NdVar::cst(8, 4)},
                              SourceVA - 1));

  auto Fnd =
      hunt(B.F, /*StackRegs=*/false, &LF, Arch::X86, {}, BinaryFormat::ELF);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Safe) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High) << Fnd->Detail;
}

TEST(HuntEngine, StackPassedSourceOutputUsesPlatformAbiLayout) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400010;
  constexpr uint64_t CallSP = 0x7000;
  constexpr uint64_t SourceBufferVA = 0x7100;
  struct Case {
    Arch TheArch;
    CallingConv CC;
    BinaryFormat Format;
    int OutputArg;
    uint64_t StackOffset;
  };
  const Case Cases[] = {
      {Arch::X64, CallingConv::Win64, BinaryFormat::COFF, 4, 4 * 8},
      {Arch::X64, CallingConv::SysV_AMD64, BinaryFormat::ELF, 6, 0},
      {Arch::AArch64, CallingConv::ARM_AAPCS, BinaryFormat::ELF, 8, 0},
      {Arch::ARM, CallingConv::ARM_AAPCS, BinaryFormat::ELF, 4, 0},
  };

  for (const Case &C : Cases) {
    SCOPED_TRACE(static_cast<int>(C.TheArch));
    const TargetRegInfo &TRI = getTargetRegInfo(C.TheArch);
    const uint16_t PointerSize = TRI.PointerSize;

    SinkCatalog Cat = SinkCatalog::defaults();
    SourceEntry Source{"custom_stack_input", C.OutputArg};
    Source.TaintedReturn = false;
    Cat.addSource(std::move(Source));

    Builder B("main");
    B.F.Entry = 0x400000;
    B.F.CC = C.CC;
    std::vector<MedVar> SourceArgs(static_cast<size_t>(C.OutputArg + 1),
                                   MedVar::makeConst(0, PointerSize));
    SourceArgs[C.OutputArg] = temp(50, PointerSize);
    B.call("custom_stack_input", temp(5, PointerSize), std::move(SourceArgs));
    B.F.Blocks[0].Ops.back().Addr = SourceVA;
    B.call("printf", temp(0, PointerSize), {temp(50, PointerSize)});
    B.F.Blocks[0].Ops.back().Addr = SinkVA;

    LowFunc LF;
    LF.Entry = B.F.Entry;
    LowBlock Block;
    Block.Id = 0;
    Block.StartAddr = LF.Entry;
    Block.EndAddr = SinkVA + 8;
    Block.Ops.push_back(lop(NdOp::COPY,
                            NdVar::reg(TRI.StackPointer, PointerSize),
                            {NdVar::cst(CallSP, PointerSize)}, SourceVA - 2));
    Block.Ops.push_back(lop(NdOp::STORE, NdVar{},
                            {NdVar::cst(CallSP + C.StackOffset, PointerSize),
                             NdVar::cst(SourceBufferVA, PointerSize)},
                            SourceVA - 1));
    Block.Ops.push_back(lop(NdOp::CALL,
                            NdVar::reg(TRI.IntReturnReg, PointerSize),
                            {NdVar::cst(0x9000, PointerSize)}, SourceVA));
    const llvm::ArrayRef<uint64_t> ParamRegs =
        C.CC == CallingConv::Win64 ? TRI.Win64ParamRegs : TRI.IntParamRegs;
    ASSERT_FALSE(ParamRegs.empty());
    Block.Ops.push_back(
        lop(NdOp::COPY, NdVar::reg(ParamRegs.front(), PointerSize),
            {NdVar::cst(SourceBufferVA, PointerSize)}, SinkVA - 1));
    Block.Ops.push_back(
        lop(NdOp::CALL, NdVar{}, {NdVar::cst(0x9000, PointerSize)}, SinkVA));
    Block.Ops.push_back(lop(NdOp::RETURN, NdVar{}, {}, SinkVA + 1));
    LF.Blocks.push_back(std::move(Block));

    const auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, C.TheArch, {},
                          C.Format, /*Image=*/nullptr, &Cat);
    ASSERT_TRUE(Fnd.has_value());
    EXPECT_EQ(Fnd->TheVerdict, Verdict::Unsafe) << Fnd->Detail;
    EXPECT_EQ(Fnd->TheConfidence, Confidence::High) << Fnd->Detail;
    EXPECT_TRUE(std::any_of(
        Fnd->Witness.begin(), Fnd->Witness.end(), [](const auto &Item) {
          return Item.first == "source" && Item.second == "custom_stack_input";
        }));
  }
}

TEST(HuntEngine, FullyWrittenReadBufferCanDriveReachableOverflow) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400010;
  constexpr va_t LengthSlotVA = 0x7000;
  const MedVar LengthSlot = MedVar::makeConst(
      LengthSlotVA, 8, ConstantAddressProvenance::DataAddress, LengthSlotVA);

  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("read", temp(5),
         {MedVar::makeConst(0, 8), LengthSlot, MedVar::makeConst(8, 8)});
  B.F.Blocks[0].Ops.back().Addr = SourceVA;
  B.op(NdOp::LOAD, temp(6), {LengthSlot});
  B.call("memcpy", temp(0), {temp(1), temp(2), temp(6)});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;

  LowFunc LF = sourceBufferScalarThenMemcpyLow(SourceVA, SinkVA, LengthSlotVA,
                                               /*RequiredWrittenBytes=*/8);
  const auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unsafe) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High) << Fnd->Detail;
  EXPECT_TRUE(
      std::any_of(Fnd->Witness.begin(), Fnd->Witness.end(),
                  [](const auto &Item) { return Item.first == "read"; }));
}

TEST(HuntEngine, FullyWrittenReadSubrangeCanDriveReachableOverflow) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400010;
  constexpr va_t BufferVA = 0x7000;
  constexpr uint64_t LoadOffset = 8;
  const MedVar Buffer = MedVar::makeConst(
      BufferVA, 8, ConstantAddressProvenance::DataAddress, BufferVA);
  const MedVar LengthSlot =
      MedVar::makeConst(BufferVA + LoadOffset, 8,
                        ConstantAddressProvenance::DataAddress, BufferVA);

  for (const auto &[WrittenBytes, ExpectReadWitness] :
       {std::pair<uint64_t, bool>{16, true}, {15, false}}) {
    SCOPED_TRACE(WrittenBytes);
    Builder B;
    B.F.Entry = 0x400000;
    B.F.CC = CallingConv::SysV_AMD64;
    B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
    B.call("read", temp(5),
           {MedVar::makeConst(0, 8), Buffer, MedVar::makeConst(16, 8)});
    B.F.Blocks[0].Ops.back().Addr = SourceVA;
    B.op(NdOp::LOAD, temp(6), {LengthSlot});
    B.call("memcpy", temp(0), {temp(1), temp(2), temp(6)});
    B.F.Blocks[0].Ops.back().Addr = SinkVA;

    LowFunc LF = sourceBufferScalarThenMemcpyLow(SourceVA, SinkVA, BufferVA,
                                                 WrittenBytes,
                                                 /*ScalarBytes=*/8, LoadOffset);
    const auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
    ASSERT_TRUE(Fnd.has_value());
    EXPECT_EQ(Fnd->TheVerdict,
              ExpectReadWitness ? Verdict::Unsafe : Verdict::Unknown)
        << Fnd->Detail;
    EXPECT_EQ(Fnd->TheConfidence,
              ExpectReadWitness ? Confidence::High : Confidence::Low)
        << Fnd->Detail;
    EXPECT_EQ(
        std::any_of(Fnd->Witness.begin(), Fnd->Witness.end(),
                    [](const auto &Item) { return Item.first == "read"; }),
        ExpectReadWitness);
  }
}

TEST(HuntEngine, LaterReadInvalidatesOnlyOverlappingCoverage) {
  constexpr va_t FirstReadVA = 0x400004;
  constexpr va_t SecondReadVA = 0x400010;
  constexpr va_t SinkVA = 0x400020;
  constexpr va_t BufferVA = 0x7000;
  const MedVar Buffer = MedVar::makeConst(
      BufferVA, 8, ConstantAddressProvenance::DataAddress, BufferVA);
  for (const auto &[SecondOffset, Expected] :
       {std::pair<uint64_t, Verdict>{1, Verdict::Unknown},
        {16, Verdict::Unsafe}}) {
    SCOPED_TRACE(SecondOffset);
    const MedVar SecondBuffer =
        MedVar::makeConst(BufferVA + SecondOffset, 8,
                          ConstantAddressProvenance::DataAddress, BufferVA);
    Builder B;
    B.F.Entry = 0x400000;
    B.F.CC = CallingConv::SysV_AMD64;
    B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
    B.call("read", temp(5),
           {MedVar::makeConst(0, 8), Buffer, MedVar::makeConst(8, 8)});
    B.F.Blocks[0].Ops.back().Addr = FirstReadVA;
    B.call("read", temp(6),
           {MedVar::makeConst(0, 8), SecondBuffer, MedVar::makeConst(1, 8)});
    B.F.Blocks[0].Ops.back().Addr = SecondReadVA;
    B.op(NdOp::LOAD, temp(7), {Buffer});
    B.call("memcpy", temp(0), {temp(1), temp(2), temp(7)});
    B.F.Blocks[0].Ops.back().Addr = SinkVA;

    LowFunc LF = overlappingReadsThenBufferScalarLow(FirstReadVA, SecondReadVA,
                                                     SinkVA, BufferVA);
    const auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
    ASSERT_TRUE(Fnd.has_value());
    EXPECT_EQ(Fnd->TheVerdict, Expected) << Fnd->Detail;
    EXPECT_EQ(Fnd->TheConfidence,
              Expected == Verdict::Unsafe ? Confidence::High : Confidence::Low)
        << Fnd->Detail;
    EXPECT_EQ(Fnd->Witness.empty(), Expected == Verdict::Unknown);
  }
}

TEST(HuntEngine, LoadBeforeSourceWriteDoesNotBorrowLaterProvenance) {
  constexpr va_t MallocVA = 0x400000;
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400010;
  constexpr va_t LengthSlotVA = 0x7000;
  const MedVar LengthSlot = MedVar::makeConst(
      LengthSlotVA, 8, ConstantAddressProvenance::DataAddress, LengthSlotVA);

  MedFunc F;
  F.Name = "f";
  F.Entry = MallocVA;
  F.CC = CallingConv::SysV_AMD64;
  F.Blocks.resize(2);
  F.Blocks[0].Id = 0;
  F.Blocks[0].Preds = {1};
  F.Blocks[0].Succs = {1};
  F.Blocks[1].Id = 1;
  F.Blocks[1].Preds = {0};
  F.Blocks[1].Succs = {0};

  auto addBlockCall = [&](int BlockId, llvm::StringRef Name, MedVar Ret,
                          std::vector<MedVar> Args, va_t Addr) {
    MedBlock &Block = F.Blocks[BlockId];
    const int OpIdx = static_cast<int>(Block.Ops.size());
    MedOp Op;
    Op.Opcode = NdOp::CALL;
    Op.Output = Ret;
    Op.Addr = Addr;
    Op.addInput(MedVar::makeConst(0x9000, 8));
    Block.Ops.push_back(std::move(Op));
    MedCallInfo CI;
    CI.BlockId = BlockId;
    CI.OpIdx = OpIdx;
    CI.TargetName = Name.str();
    CI.Args = std::move(Args);
    F.CallInfos.push_back(std::move(CI));
  };

  addBlockCall(0, "malloc", temp(1), {MedVar::makeConst(16, 8)}, MallocVA);
  MedOp Load;
  Load.Opcode = NdOp::LOAD;
  Load.Output = temp(6);
  Load.addInput(LengthSlot);
  F.Blocks[0].Ops.push_back(std::move(Load));
  addBlockCall(1, "read", temp(5),
               {MedVar::makeConst(0, 8), LengthSlot, MedVar::makeConst(8, 8)},
               SourceVA);
  addBlockCall(1, "memcpy", temp(0), {temp(1), temp(2), temp(6)}, SinkVA);

  BinaryImage Img;
  Img.Arch = Arch::X64;
  AnalysisInput In;
  In.Img = &Img;
  SinkCatalog Cat = SinkCatalog::defaults();
  ArgClassification LengthArg = classifyArgument(In, Cat, F, 2, 2);
  ASSERT_EQ(LengthArg.Flow, ArgFlow::Tainted);
  ASSERT_EQ(LengthArg.TaintSource, "read");

  LowFunc LF =
      loadBeforeSourceInLoopLow(MallocVA, SourceVA, SinkVA, LengthSlotVA);
  const auto Fnd = hunt(F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::Low) << Fnd->Detail;
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, WidenedFullyWrittenReadBufferCanDriveReachableOverflow) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400010;
  constexpr va_t LengthSlotVA = 0x7000;
  const MedVar LengthSlot = MedVar::makeConst(
      LengthSlotVA, 8, ConstantAddressProvenance::DataAddress, LengthSlotVA);

  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("read", temp(5),
         {MedVar::makeConst(0, 8), LengthSlot, MedVar::makeConst(4, 8)});
  B.F.Blocks[0].Ops.back().Addr = SourceVA;
  B.op(NdOp::LOAD, temp(6, 4), {LengthSlot});
  B.op(NdOp::INT_ZEXT, temp(7), {temp(6, 4)});
  B.call("memcpy", temp(0), {temp(1), temp(2), temp(7)});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;

  LowFunc LF = sourceBufferScalarThenMemcpyLow(SourceVA, SinkVA, LengthSlotVA,
                                               /*RequiredWrittenBytes=*/4,
                                               /*ScalarBytes=*/4);
  const auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unsafe) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High) << Fnd->Detail;
  EXPECT_TRUE(
      std::any_of(Fnd->Witness.begin(), Fnd->Witness.end(),
                  [](const auto &Item) { return Item.first == "read"; }));
}

TEST(HuntEngine, MixedLoadOriginsDoNotBorrowReadBufferProvenance) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400010;
  constexpr va_t LengthSlotVA = 0x7000;
  constexpr va_t OtherSlotVA = 0x7100;
  const MedVar LengthSlot = MedVar::makeConst(
      LengthSlotVA, 8, ConstantAddressProvenance::DataAddress, LengthSlotVA);
  const MedVar OtherSlot = MedVar::makeConst(
      OtherSlotVA, 8, ConstantAddressProvenance::DataAddress, OtherSlotVA);

  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("read", temp(5),
         {MedVar::makeConst(0, 8), LengthSlot, MedVar::makeConst(8, 8)});
  B.F.Blocks[0].Ops.back().Addr = SourceVA;
  B.op(NdOp::LOAD, temp(6), {LengthSlot});
  B.op(NdOp::LOAD, temp(7), {OtherSlot});
  B.op(NdOp::INT_ADD, temp(8), {temp(6), temp(7)});
  B.call("memcpy", temp(0), {temp(1), temp(2), temp(8)});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;

  LowFunc LF = sourceBufferScalarThenMemcpyLow(SourceVA, SinkVA, LengthSlotVA,
                                               /*RequiredWrittenBytes=*/8);
  LF.Blocks[1].Ops.insert(LF.Blocks[1].Ops.begin() + 1,
                          lop(NdOp::LOAD, NdVar::reg(24, 8),
                              {NdVar::cst(OtherSlotVA, 8)}, SourceVA + 4));
  LF.Blocks[1].Ops.insert(LF.Blocks[1].Ops.begin() + 2,
                          lop(NdOp::INT_ADD, NdVar::reg(16, 8),
                              {NdVar::reg(16, 8), NdVar::reg(24, 8)},
                              SourceVA + 5));
  const auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::Low) << Fnd->Detail;
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, PartiallyWrittenReadBufferDoesNotClaimWholeScalar) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400010;
  constexpr va_t LengthSlotVA = 0x7000;
  const MedVar LengthSlot = MedVar::makeConst(
      LengthSlotVA, 8, ConstantAddressProvenance::DataAddress, LengthSlotVA);

  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("read", temp(5),
         {MedVar::makeConst(0, 8), LengthSlot, MedVar::makeConst(8, 8)});
  B.F.Blocks[0].Ops.back().Addr = SourceVA;
  B.op(NdOp::LOAD, temp(6), {LengthSlot});
  B.call("memcpy", temp(0), {temp(1), temp(2), temp(6)});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;

  LowFunc LF = sourceBufferScalarThenMemcpyLow(SourceVA, SinkVA, LengthSlotVA,
                                               /*RequiredWrittenBytes=*/1);
  const auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::Low) << Fnd->Detail;
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, OverwrittenReadBufferDoesNotRetainLengthProvenance) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400010;
  constexpr va_t LengthSlotVA = 0x7000;
  const MedVar LengthSlot = MedVar::makeConst(
      LengthSlotVA, 8, ConstantAddressProvenance::DataAddress, LengthSlotVA);

  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("read", temp(5),
         {MedVar::makeConst(0, 8), LengthSlot, MedVar::makeConst(8, 8)});
  B.F.Blocks[0].Ops.back().Addr = SourceVA;
  B.op(NdOp::STORE, MedVar{}, {LengthSlot, MedVar::makeConst(0, 8)});
  B.op(NdOp::LOAD, temp(6), {LengthSlot});
  B.call("memcpy", temp(0), {temp(1), temp(2), temp(6)});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;

  LowFunc LF = sourceBufferScalarThenMemcpyLow(SourceVA, SinkVA, LengthSlotVA,
                                               /*RequiredWrittenBytes=*/8);
  LF.Blocks[1].Ops.insert(LF.Blocks[1].Ops.begin(),
                          lop(NdOp::STORE, NdVar{},
                              {NdVar::cst(LengthSlotVA, 8), NdVar::cst(0, 8)},
                              SourceVA + 3));
  const auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::Low) << Fnd->Detail;
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, FreadElementSizeDeterminesWrittenBufferBytes) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400010;
  constexpr va_t LengthSlotVA = 0x7000;
  const MedVar LengthSlot = MedVar::makeConst(
      LengthSlotVA, 8, ConstantAddressProvenance::DataAddress, LengthSlotVA);

  for (const auto &[ReturnedElements, Expected] :
       {std::pair<uint64_t, Verdict>{2, Verdict::Unsafe},
        std::pair<uint64_t, Verdict>{1, Verdict::Unknown}}) {
    SCOPED_TRACE(ReturnedElements);
    Builder B;
    B.F.Entry = 0x400000;
    B.F.CC = CallingConv::SysV_AMD64;
    B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
    B.call("fread", temp(5),
           {LengthSlot, MedVar::makeConst(4, 8), MedVar::makeConst(2, 8),
            temp(7)});
    B.F.Blocks[0].Ops.back().Addr = SourceVA;
    B.op(NdOp::LOAD, temp(6), {LengthSlot});
    B.call("memcpy", temp(0), {temp(1), temp(2), temp(6)});
    B.F.Blocks[0].Ops.back().Addr = SinkVA;

    LowFunc LF = sourceBufferScalarThenMemcpyLow(SourceVA, SinkVA, LengthSlotVA,
                                                 ReturnedElements);
    const auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
    ASSERT_TRUE(Fnd.has_value());
    EXPECT_EQ(Fnd->TheVerdict, Expected) << Fnd->Detail;
    EXPECT_EQ(Fnd->TheConfidence,
              Expected == Verdict::Unsafe ? Confidence::High : Confidence::Low)
        << Fnd->Detail;
    EXPECT_EQ(Fnd->Witness.empty(), Expected == Verdict::Unknown);
  }
}

TEST(HuntEngine, FortifiedReadRejectsRequestBeyondObjectSize) {
  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("__read_chk", temp(5),
         {MedVar::makeConst(0, 8), temp(2), MedVar::makeConst(32, 8),
          MedVar::makeConst(8, 8)});
  B.F.Blocks[0].Ops.back().Addr = 0x400004;
  B.call("memcpy", temp(0), {temp(1), temp(2), temp(5)});
  B.F.Blocks[0].Ops.back().Addr = 0x400010;
  LowFunc LF = sourceReturnThenMemcpyLow(0x400004, 0x400010,
                                         /*RequireNonnegative=*/true);

  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::Low) << Fnd->Detail;
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, GuardedRecvWithoutFlagsHonorsRequestedCount) {
  for (const char *Name :
       {"recv", "recvfrom", "__recv_chk", "__recvfrom_chk"}) {
    SCOPED_TRACE(Name);
    const bool From = llvm::StringRef(Name).contains("recvfrom");
    const bool Fortified = llvm::StringRef(Name).contains("_chk");
    Builder B;
    B.F.Entry = 0x400000;
    B.F.CC = CallingConv::SysV_AMD64;
    B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
    std::vector<MedVar> Args = {MedVar::makeConst(0, 8), param(2),
                                MedVar::makeConst(8, 8)};
    if (Fortified)
      Args.push_back(MedVar::makeConst(16, 8));
    Args.push_back(MedVar::makeConst(0, 8));
    if (From) {
      Args.push_back(param(3));
      Args.push_back(param(4));
    }
    B.call(Name, temp(5), std::move(Args));
    B.F.Blocks[0].Ops.back().Addr = 0x400004;
    B.call("memcpy", temp(0), {temp(1), param(2), temp(5)});
    B.F.Blocks[0].Ops.back().Addr = 0x400010;
    LowFunc LF = sourceReturnThenMemcpyLow(0x400004, 0x400010,
                                           /*RequireNonnegative=*/true);

    auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
    ASSERT_TRUE(Fnd.has_value());
    EXPECT_EQ(Fnd->TheVerdict, Verdict::Safe) << Fnd->Detail;
    EXPECT_EQ(Fnd->TheConfidence, Confidence::High) << Fnd->Detail;
  }
}

TEST(HuntEngine, NonzeroRecvFlagsDoNotAssumeRequestedCount) {
  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("recv", temp(5),
         {MedVar::makeConst(0, 8), param(2), MedVar::makeConst(8, 8),
          MedVar::makeConst(1, 8)});
  B.F.Blocks[0].Ops.back().Addr = 0x400004;
  B.call("memcpy", temp(0), {temp(1), param(2), temp(5)});
  B.F.Blocks[0].Ops.back().Addr = 0x400010;
  LowFunc LF = sourceReturnThenMemcpyLow(0x400004, 0x400010,
                                         /*RequireNonnegative=*/true);

  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unsafe) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High) << Fnd->Detail;
}

TEST(HuntEngine, NonzeroFortifiedRecvFlagsDoNotAssumeRequestedCount) {
  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("__recv_chk", temp(5),
         {MedVar::makeConst(0, 8), param(2), MedVar::makeConst(8, 8),
          MedVar::makeConst(16, 8), MedVar::makeConst(1, 8)});
  B.F.Blocks[0].Ops.back().Addr = 0x400004;
  B.call("memcpy", temp(0), {temp(1), param(2), temp(5)});
  B.F.Blocks[0].Ops.back().Addr = 0x400010;
  LowFunc LF = sourceReturnThenMemcpyLow(0x400004, 0x400010,
                                         /*RequireNonnegative=*/true);

  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unsafe) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High) << Fnd->Detail;
}

TEST(HuntEngine, GuardedWinSockRecvUsesSigned32BitReturn) {
  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::Win64;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("recv", temp(5, 4),
         {MedVar::makeConst(0, 8), param(2), MedVar::makeConst(8, 8),
          MedVar::makeConst(0, 8)});
  B.F.Blocks[0].Ops.back().Addr = 0x400004;
  B.op(NdOp::INT_SEXT, temp(6), {temp(5, 4)});
  B.call("memcpy", temp(0), {temp(1), param(2), temp(6)});
  B.F.Blocks[0].Ops.back().Addr = 0x400010;
  LowFunc LF = sourceReturnThenMemcpyLow(
      0x400004, 0x400010,
      /*RequireNonnegative=*/true,
      /*ReturnBytes=*/4, getTargetRegInfo(Arch::X64).Win64ParamRegs[2]);

  auto Fnd =
      hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {}, BinaryFormat::COFF);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Safe) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High) << Fnd->Detail;
}

TEST(HuntEngine, WindowsCountedSourcesTruncateRequestedCountTo32Bits) {
  constexpr uint64_t EncodedCount = 0x100000008ULL;
  for (const char *Name : {"_read", "recv"}) {
    SCOPED_TRACE(Name);
    Builder B;
    B.F.Entry = 0x400000;
    B.F.CC = CallingConv::Win64;
    B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
    std::vector<MedVar> Args = {MedVar::makeConst(0, 8), temp(2),
                                MedVar::makeConst(EncodedCount, 8)};
    if (llvm::StringRef(Name) == "recv")
      Args.push_back(MedVar::makeConst(0, 8));
    B.call(Name, temp(5, 4), std::move(Args));
    B.F.Blocks[0].Ops.back().Addr = 0x400004;
    B.op(NdOp::INT_SEXT, temp(6), {temp(5, 4)});
    B.call("memcpy", temp(0), {temp(1), temp(2), temp(6)});
    B.F.Blocks[0].Ops.back().Addr = 0x400010;
    LowFunc LF = sourceReturnThenMemcpyLow(
        0x400004, 0x400010,
        /*RequireNonnegative=*/true,
        /*ReturnBytes=*/4, getTargetRegInfo(Arch::X64).Win64ParamRegs[2]);

    auto Fnd =
        hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {}, BinaryFormat::COFF);
    ASSERT_TRUE(Fnd.has_value());
    EXPECT_EQ(Fnd->TheVerdict, Verdict::Safe)
        << Fnd->Detail << " constraints=" << Fnd->Constraints;
    EXPECT_EQ(Fnd->TheConfidence, Confidence::High) << Fnd->Detail;
  }
}

TEST(HuntEngine, NegativeWinSockLengthCannotProduceSuccessfulCopy) {
  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::Win64;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("recv", temp(5, 4),
         {MedVar::makeConst(0, 8), temp(2), MedVar::makeConst(UINT32_MAX, 8),
          MedVar::makeConst(0, 8)});
  B.F.Blocks[0].Ops.back().Addr = 0x400004;
  B.op(NdOp::INT_SEXT, temp(6), {temp(5, 4)});
  B.call("memcpy", temp(0), {temp(1), temp(2), temp(6)});
  B.F.Blocks[0].Ops.back().Addr = 0x400010;
  LowFunc LF = sourceReturnThenMemcpyLow(
      0x400004, 0x400010, /*RequireNonnegative=*/true,
      /*ReturnBytes=*/4, getTargetRegInfo(Arch::X64).Win64ParamRegs[2]);

  auto Fnd =
      hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {}, BinaryFormat::COFF);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::Low) << Fnd->Detail;
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, ReadFileByteCountUsesCOFFOutputContract) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400010;
  constexpr va_t BytesReadVA = 0x700000;
  constexpr uint64_t CopyLengthReg = 24;
  struct Case {
    uint64_t Requested;
    BinaryFormat Format;
    Verdict Expected;
  };
  for (const Case C : {Case{8, BinaryFormat::COFF, Verdict::Safe},
                       Case{32, BinaryFormat::COFF, Verdict::Unsafe},
                       Case{8, BinaryFormat::ELF, Verdict::Unsafe}}) {
    SCOPED_TRACE(static_cast<int>(C.Format));
    SCOPED_TRACE(C.Requested);
    Builder B;
    B.F.Entry = 0x400000;
    B.F.CC = CallingConv::Win64;
    B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
    B.call("KERNEL32.dll!__imp__ReadFile@20", temp(5, 4),
           {MedVar::makeConst(1, 8), param(2),
            MedVar::makeConst(C.Requested, 8),
            MedVar::makeConst(BytesReadVA, 8), MedVar::makeConst(0, 8)});
    B.F.Blocks[0].Ops.back().Addr = SourceVA;
    B.op(NdOp::LOAD, mkReg(CopyLengthReg, 1, 4),
         {MedVar::makeConst(BytesReadVA, 8)});
    B.call("memcpy", temp(0), {temp(1), param(2), mkReg(CopyLengthReg, 1, 4)});
    B.F.Blocks[0].Ops.back().Addr = SinkVA;
    LowFunc LF =
        readFileThenMemcpyLow(SourceVA, SinkVA, BytesReadVA, CopyLengthReg);

    auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {}, C.Format);
    ASSERT_TRUE(Fnd.has_value());
    EXPECT_EQ(Fnd->TheVerdict, C.Expected) << Fnd->Detail;
    EXPECT_EQ(Fnd->TheConfidence, Confidence::High) << Fnd->Detail;
  }
}

TEST(HuntEngine, ReadFileCountDeterminesWrittenBufferBytes) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400010;
  constexpr va_t LengthSlotVA = 0x7000;
  constexpr va_t BytesReadVA = 0x7100;
  const MedVar LengthSlot = MedVar::makeConst(
      LengthSlotVA, 8, ConstantAddressProvenance::DataAddress, LengthSlotVA);
  const MedVar BytesRead = MedVar::makeConst(
      BytesReadVA, 8, ConstantAddressProvenance::DataAddress, BytesReadVA);

  for (const auto &[WrittenBytes, Expected] :
       {std::pair<uint32_t, Verdict>{8, Verdict::Unsafe},
        std::pair<uint32_t, Verdict>{4, Verdict::Unknown}}) {
    SCOPED_TRACE(WrittenBytes);
    Builder B;
    B.F.Entry = 0x400000;
    B.F.CC = CallingConv::Win64;
    B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
    B.call("ReadFile", temp(5, 4),
           {MedVar::makeConst(1, 8), LengthSlot, MedVar::makeConst(8, 8),
            BytesRead, MedVar::makeConst(0, 8)});
    B.F.Blocks[0].Ops.back().Addr = SourceVA;
    B.op(NdOp::LOAD, temp(6), {LengthSlot});
    B.call("memcpy", temp(0), {temp(1), temp(2), temp(6)});
    B.F.Blocks[0].Ops.back().Addr = SinkVA;

    LowFunc LF = readFileBufferScalarThenMemcpyLow(
        SourceVA, SinkVA, LengthSlotVA, BytesReadVA, WrittenBytes);
    const auto Fnd =
        hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {}, BinaryFormat::COFF);
    ASSERT_TRUE(Fnd.has_value());
    EXPECT_EQ(Fnd->TheVerdict, Expected) << Fnd->Detail;
    EXPECT_EQ(Fnd->TheConfidence,
              Expected == Verdict::Unsafe ? Confidence::High : Confidence::Low)
        << Fnd->Detail;
    EXPECT_EQ(Fnd->Witness.empty(), Expected == Verdict::Unknown);
  }
}

TEST(HuntEngine, ReadFileFailureZeroesTheReportedByteCount) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400020;
  constexpr va_t BytesReadVA = 0x700000;
  constexpr uint64_t CopyLengthReg = 24;
  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::Win64;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("ReadFile", temp(5, 4),
         {MedVar::makeConst(1, 8), param(2), MedVar::makeConst(32, 8),
          MedVar::makeConst(BytesReadVA, 8), MedVar::makeConst(0, 8)});
  B.F.Blocks[0].Ops.back().Addr = SourceVA;
  B.op(NdOp::LOAD, mkReg(CopyLengthReg, 1, 4),
       {MedVar::makeConst(BytesReadVA, 8)});
  B.call("memcpy", temp(0), {temp(1), param(2), mkReg(CopyLengthReg, 1, 4)});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;
  LowFunc LF = readFileFailureThenSinkLow(SourceVA, SinkVA, BytesReadVA,
                                          /*LoadCount=*/true, CopyLengthReg);

  auto Fnd =
      hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {}, BinaryFormat::COFF);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Safe) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High) << Fnd->Detail;
}

TEST(HuntEngine, ReadFileFailureDoesNotCorroborateStringInput) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400020;
  constexpr va_t BytesReadVA = 0x700000;
  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::Win64;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("ReadFile", temp(5, 4),
         {MedVar::makeConst(1, 8), param(2), MedVar::makeConst(32, 8),
          MedVar::makeConst(BytesReadVA, 8), MedVar::makeConst(0, 8)});
  B.F.Blocks[0].Ops.back().Addr = SourceVA;
  B.call("strcpy", temp(0), {temp(1), param(2)});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;
  LowFunc LF = readFileFailureThenSinkLow(SourceVA, SinkVA, BytesReadVA,
                                          /*LoadCount=*/false);

  auto Fnd =
      hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {}, BinaryFormat::COFF);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown) << Fnd->Detail;
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, ReadFileByteCountGatesStringInput) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400020;
  constexpr va_t BytesReadVA = 0x700000;
  for (const auto &[BytesRead, Expected] :
       {std::pair<uint32_t, Verdict>{0, Verdict::Unknown},
        std::pair<uint32_t, Verdict>{1, Verdict::Unsafe}}) {
    SCOPED_TRACE(BytesRead);
    const MedVar SourceBuffer = param(2);
    Builder B;
    B.F.Entry = 0x400000;
    B.F.CC = CallingConv::Win64;
    B.call("ReadFile", temp(5, 4),
           {MedVar::makeConst(1, 8), SourceBuffer, MedVar::makeConst(32, 8),
            MedVar::makeConst(BytesReadVA, 8), MedVar::makeConst(0, 8)});
    B.F.Blocks[0].Ops.back().Addr = SourceVA;
    B.call("printf", temp(0), {SourceBuffer});
    B.F.Blocks[0].Ops.back().Addr = SinkVA;
    LowFunc LF = readFileResultThenFormatLow(SourceVA, SinkVA, BytesReadVA,
                                             /*ReturnValue=*/1, BytesRead);

    auto Fnd =
        hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {}, BinaryFormat::COFF);
    ASSERT_TRUE(Fnd.has_value());
    EXPECT_EQ(Fnd->TheVerdict, Expected) << Fnd->Detail;
    EXPECT_EQ(Fnd->Witness.empty(), Expected != Verdict::Unsafe);
  }
}

TEST(HuntEngine, NarrowReadFileCountPointerFailsClosed) {
  constexpr va_t SourceVA = 0x400004;
  constexpr va_t SinkVA = 0x400020;
  constexpr va_t BytesReadVA = 0x70;
  const MedVar SourceBuffer = param(2);
  const MedVar NarrowBytesRead = MedVar::makeConst(
      BytesReadVA, 1, ConstantAddressProvenance::DataAddress, BytesReadVA);
  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::Win64;
  B.call("ReadFile", temp(5, 4),
         {MedVar::makeConst(1, 8), SourceBuffer, MedVar::makeConst(32, 8),
          NarrowBytesRead, MedVar::makeConst(0, 8)});
  B.F.Blocks[0].Ops.back().Addr = SourceVA;
  B.call("printf", temp(0), {SourceBuffer});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;

  LowFunc LF = readFileResultThenFormatLow(SourceVA, SinkVA, BytesReadVA,
                                           /*ReturnValue=*/1,
                                           /*BytesRead=*/1);
  const auto Fnd =
      hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {}, BinaryFormat::COFF);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::Low) << Fnd->Detail;
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, ZeroByteReadFilePreservesEarlierInputEvidence) {
  constexpr va_t FirstSourceVA = 0x400004;
  constexpr va_t SecondSourceVA = 0x400010;
  constexpr va_t SinkVA = 0x400020;
  constexpr va_t FirstBytesReadVA = 0x700000;
  constexpr va_t SecondBytesReadVA = 0x700004;
  const MedVar SourceBuffer = param(2);
  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::Win64;
  B.call("ReadFile", temp(5, 4),
         {MedVar::makeConst(1, 8), SourceBuffer, MedVar::makeConst(32, 8),
          MedVar::makeConst(FirstBytesReadVA, 8), MedVar::makeConst(0, 8)});
  B.F.Blocks[0].Ops.back().Addr = FirstSourceVA;
  B.call("ReadFile", temp(6, 4),
         {MedVar::makeConst(1, 8), SourceBuffer, MedVar::makeConst(32, 8),
          MedVar::makeConst(SecondBytesReadVA, 8), MedVar::makeConst(0, 8)});
  B.F.Blocks[0].Ops.back().Addr = SecondSourceVA;
  B.call("printf", temp(0), {SourceBuffer});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;
  LowFunc LF =
      repeatedReadFileThenFormatLow(FirstSourceVA, SecondSourceVA, SinkVA,
                                    FirstBytesReadVA, SecondBytesReadVA);

  auto Fnd =
      hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {}, BinaryFormat::COFF);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unsafe) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High) << Fnd->Detail;
}

TEST(HuntEngine, FreadReturnHonorsElementCount) {
  for (const char *Name :
       {"fread", "fread_unlocked", "__fread_chk", "__fread_unlocked_chk"}) {
    SCOPED_TRACE(Name);
    const bool Fortified = llvm::StringRef(Name).contains("_chk");
    Builder B;
    B.F.Entry = 0x400000;
    B.F.CC = CallingConv::SysV_AMD64;
    B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
    std::vector<MedVar> Args = {temp(2)};
    if (Fortified)
      Args.push_back(MedVar::makeConst(16, 8));
    Args.push_back(MedVar::makeConst(4, 8));
    Args.push_back(MedVar::makeConst(3, 8));
    Args.push_back(temp(6));
    B.call(Name, temp(5), std::move(Args));
    B.F.Blocks[0].Ops.back().Addr = 0x400004;
    B.call("memcpy", temp(0), {temp(1), temp(2), temp(5)});
    B.F.Blocks[0].Ops.back().Addr = 0x400010;
    LowFunc LF = sourceReturnThenMemcpyLow(0x400004, 0x400010,
                                           /*RequireNonnegative=*/false);

    auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
    ASSERT_TRUE(Fnd.has_value());
    EXPECT_EQ(Fnd->TheVerdict, Verdict::Safe) << Fnd->Detail;
    EXPECT_EQ(Fnd->TheConfidence, Confidence::High) << Fnd->Detail;
  }
}

TEST(HuntEngine, FreadZeroElementSizeReturnsZero) {
  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call(
      "fread", temp(5),
      {temp(2), MedVar::makeConst(0, 8), MedVar::makeConst(100, 8), temp(6)});
  B.F.Blocks[0].Ops.back().Addr = 0x400004;
  B.call("memcpy", temp(0), {temp(1), temp(2), temp(5)});
  B.F.Blocks[0].Ops.back().Addr = 0x400010;
  LowFunc LF = sourceReturnThenMemcpyLow(0x400004, 0x400010,
                                         /*RequireNonnegative=*/false);

  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Safe) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High) << Fnd->Detail;
}

TEST(HuntEngine, FortifiedFreadRejectsProductBeyondObjectSize) {
  for (const char *Name : {"__fread_chk", "__fread_unlocked_chk"}) {
    SCOPED_TRACE(Name);
    Builder B;
    B.F.Entry = 0x400000;
    B.F.CC = CallingConv::SysV_AMD64;
    B.call("malloc", temp(1), {MedVar::makeConst(2, 8)});
    B.call(Name, temp(5),
           {temp(2), MedVar::makeConst(16, 8), MedVar::makeConst(8, 8),
            MedVar::makeConst(3, 8), temp(6)});
    B.F.Blocks[0].Ops.back().Addr = 0x400004;
    B.call("memcpy", temp(0), {temp(1), temp(2), temp(5)});
    B.F.Blocks[0].Ops.back().Addr = 0x400010;
    LowFunc LF = sourceReturnThenMemcpyLow(0x400004, 0x400010,
                                           /*RequireNonnegative=*/false);

    auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
    ASSERT_TRUE(Fnd.has_value());
    EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown) << Fnd->Detail;
    EXPECT_EQ(Fnd->TheConfidence, Confidence::Low) << Fnd->Detail;
    EXPECT_TRUE(Fnd->Witness.empty());
  }
}

TEST(HuntEngine, UncheckedReadErrorRemainsUnsafe) {
  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("read", temp(5),
         {MedVar::makeConst(0, 8), temp(2), MedVar::makeConst(8, 8)});
  B.F.Blocks[0].Ops.back().Addr = 0x400004;
  B.call("memcpy", temp(0), {temp(1), temp(2), temp(5)});
  B.F.Blocks[0].Ops.back().Addr = 0x400010;
  LowFunc LF = sourceReturnThenMemcpyLow(0x400004, 0x400010,
                                         /*RequireNonnegative=*/false);

  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unsafe);
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High);
  EXPECT_FALSE(Fnd->Witness.empty());
}

TEST(HuntEngine, UncheckedWindowsReadErrorUsesSigned32BitReturn) {
  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("_read", temp(5, 4),
         {MedVar::makeConst(0, 8), temp(2), MedVar::makeConst(8, 8)});
  B.F.Blocks[0].Ops.back().Addr = 0x400004;
  B.op(NdOp::INT_SEXT, temp(6), {temp(5, 4)});
  B.call("memcpy", temp(0), {temp(1), temp(2), temp(6)});
  B.F.Blocks[0].Ops.back().Addr = 0x400010;
  LowFunc LF = sourceReturnThenMemcpyLow(0x400004, 0x400010,
                                         /*RequireNonnegative=*/false,
                                         /*ReturnBytes=*/4);

  auto Fnd =
      hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {}, BinaryFormat::COFF);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unsafe);
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High);
  EXPECT_FALSE(Fnd->Witness.empty());
}

TEST(HuntEngine, ConstLengthWithinCapacityIsSafe) {
  Builder B;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("memcpy", temp(0), {temp(1), temp(2), MedVar::makeConst(8, 8)});

  auto Fnd = hunt(B.F);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Safe);
}

TEST(HuntEngine, UsesDeclaredWidthForNarrowConstantCallArguments) {
  struct Case {
    Arch TargetArch;
    BinaryFormat Format;
  } Cases[] = {{Arch::X86, BinaryFormat::COFF}, {Arch::ARM, BinaryFormat::ELF}};

  for (const Case &C : Cases) {
    SCOPED_TRACE(static_cast<int>(C.TargetArch));
    Builder B;
    B.call("malloc", temp(1), {MedVar::makeConst(16, 4)});
    B.call(
        "memcpy", temp(0),
        {temp(1, 4), temp(2, 4), MedVar::makeConst(UINT64_C(0x100000008), 4)});

    auto Fnd = hunt(B.F, /*StackRegs=*/false, /*LF=*/nullptr, C.TargetArch, {},
                    C.Format);
    ASSERT_TRUE(Fnd.has_value());
    EXPECT_EQ(Fnd->TheVerdict, Verdict::Safe) << Fnd->Detail;
  }
}

TEST(HuntEngine, ZeroWidthCopyLengthFailsClosed) {
  constexpr va_t SinkVA = 0x400010;
  Builder B;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("memcpy", temp(0), {temp(1), temp(2), MedVar::makeConst(8, 0)});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;
  LowFunc LF = reachableSinkLow(SinkVA);

  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::Low) << Fnd->Detail;
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, ArmEabiMemsetUsesItsSecondArgumentAsLength) {
  struct Case {
    uint64_t Count;
    uint64_t Value;
    Verdict Expected;
  } Cases[] = {{8, 32, Verdict::Safe}, {32, 0, Verdict::Unsafe}};

  for (const Case &C : Cases) {
    SCOPED_TRACE(C.Count);
    SCOPED_TRACE(C.Value);
    Builder B;
    B.call("malloc", temp(1, 4), {MedVar::makeConst(16, 4)});
    B.call("__aeabi_memset", temp(0, 4),
           {temp(1, 4), MedVar::makeConst(C.Count, 4),
            MedVar::makeConst(C.Value, 4)});
    B.F.Blocks[0].Ops.back().Addr = 0x400010;
    LowFunc LF = reachableSinkLow(0x400010);

    auto Fnd =
        hunt(B.F, /*StackRegs=*/false, &LF, Arch::ARM, {}, BinaryFormat::ELF);
    ASSERT_TRUE(Fnd.has_value());
    EXPECT_EQ(Fnd->TheVerdict, C.Expected) << Fnd->Detail;
  }
}

TEST(HuntEngine, ConstLengthExceedingCapacityWithoutReachabilityIsUnknown) {
  Builder B;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("memcpy", temp(0), {temp(1), temp(2), MedVar::makeConst(32, 8)});

  auto Fnd = hunt(B.F);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown);
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, ReachableConstLengthExceedingCapacityIsUnsafe) {
  Builder B;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("memcpy", temp(0), {temp(1), temp(2), MedVar::makeConst(32, 8)});
  B.F.Blocks[0].Ops.back().Addr = 0x400010;
  LowFunc LF = reachableSinkLow(0x400010);

  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unsafe);
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High);
}

TEST(HuntEngine, ConstLengthWithinStackFrameBoundIsUnknown) {
  Builder B;
  B.op(NdOp::INT_SUB, mkReg(kSP, 1),
       {mkReg(kSP, 0), MedVar::makeConst(0x30, 8)});
  B.op(NdOp::INT_ADD, temp(10), {mkReg(kSP, 1), MedVar::makeConst(0x8, 8)});
  B.call("memcpy", temp(0), {temp(10), temp(2), MedVar::makeConst(16, 8)});

  auto Fnd = hunt(B.F, /*StackRegs=*/true);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown);
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High);
  EXPECT_FALSE(Fnd->SkipReason.empty());
}

TEST(HuntEngine, StrlenWithoutDestinationGuardIsUnknown) {
  Builder B;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("strlen", temp(5), {});
  B.call("memcpy", temp(0), {temp(1), temp(2), temp(5)});

  auto Fnd = hunt(B.F);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown);
  EXPECT_TRUE(Fnd->SkipReason.empty());
}

TEST(HuntEngine, MaskBoundMustFitDestinationBeforeSafeSkip) {
  Builder B;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.op(NdOp::INT_AND, temp(5), {param(1), MedVar::makeConst(0xff, 8)});
  B.call("memcpy", temp(0), {temp(1), temp(2), temp(5)});

  auto Fnd = hunt(B.F);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown);
  EXPECT_TRUE(Fnd->SkipReason.empty());
}

TEST(HuntEngine, MaskBoundWithinDestinationIsSafeSkip) {
  Builder B;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.op(NdOp::INT_AND, temp(5), {param(1), MedVar::makeConst(0x0f, 8)});
  B.call("memcpy", temp(0), {temp(1), temp(2), temp(5)});

  auto Fnd = hunt(B.F);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Safe);
  EXPECT_FALSE(Fnd->SkipReason.empty());
}

TEST(HuntEngine, FortifiedCopyIsSafe) {
  Builder B;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  // __strcpy_chk(dst, src, dstlen)
  B.call("___strcpy_chk", temp(0),
         {temp(1), param(2), MedVar::makeConst(16, 8)});

  auto Fnd = hunt(B.F);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Safe);
  EXPECT_NE(Fnd->Detail.find("fortified"), std::string::npos);
}

TEST(HuntEngine, FortifiedCopyWithoutDestinationCapacityIsUnknown) {
  Builder B;
  B.call("___strcpy_chk", temp(0),
         {temp(1), param(2), MedVar::makeConst(16, 8)});

  auto Fnd = hunt(B.F);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown);
}

TEST(HuntEngine, FortifiedBoundLargerThanObjectStillAllowsOverflow) {
  Builder B;
  B.call("malloc", temp(1), {MedVar::makeConst(8, 8)});
  B.call(
      "___memcpy_chk", temp(0),
      {temp(1), temp(2), MedVar::makeConst(12, 8), MedVar::makeConst(16, 8)});
  B.F.Blocks[0].Ops.back().Addr = 0x400010;
  LowFunc LF = reachableSinkLow(0x400010);

  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unsafe);
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High);
}

TEST(HuntEngine, FortifiedBoundRejectingCopyPreventsOverflow) {
  Builder B;
  B.call("malloc", temp(1), {MedVar::makeConst(8, 8)});
  B.call("___memcpy_chk", temp(0),
         {temp(1), temp(2), MedVar::makeConst(12, 8), MedVar::makeConst(8, 8)});

  auto Fnd = hunt(B.F);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Safe);
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High);
}

TEST(HuntEngine, FortifiedRuntimeBoundParticipatesInOverflowQuery) {
  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(8, 8)});
  B.call("___memcpy_chk", temp(0), {temp(1), temp(2), rdxLen(), mkReg(24, 0)});
  B.F.Blocks[0].Ops.back().Addr = 0x400010;
  LowFunc LF = fortifiedMemcpyLow(0x400010);

  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Safe);
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High);
}

TEST(HuntEngine, ConstantStringPointerIsNotAConstantStringLength) {
  Builder B;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("strcpy", temp(0), {temp(1), MedVar::makeConst(0x100000, 8)});

  auto Fnd = hunt(B.F);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown);
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, AppendRequiresDestinationStringState) {
  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("strlen", temp(5), {param(2)});
  B.F.Blocks[0].Ops.back().Addr = 0x400004;
  B.call("strcat", temp(0), {temp(1), param(2)});
  B.F.Blocks[0].Ops.back().Addr = 0x400010;
  LowFunc LF = strlenGuardedStrcpyLow(0x400004, 0x400010,
                                      /*OverflowGuard=*/false);

  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown);
}

TEST(HuntEngine, SizeLimitedStringCopyRequiresSourceLength) {
  Builder B;
  B.call("malloc", temp(1), {MedVar::makeConst(8, 8)});
  B.call("strlcpy", temp(0),
         {temp(1), MedVar::makeConst(0x100000, 8), MedVar::makeConst(16, 8)});

  auto Fnd = hunt(B.F);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown);
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, StrlcpyTotalSizeWithinDestinationIsSafe) {
  Builder B;
  B.call("malloc", temp(1), {MedVar::makeConst(8, 8)});
  B.call("strlcpy", temp(0), {temp(1), param(2), MedVar::makeConst(8, 8)});

  auto Fnd = hunt(B.F);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Safe) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High);
  EXPECT_FALSE(Fnd->SkipReason.empty());
}

TEST(HuntEngine, StrlcatTotalSizeWithinDestinationIsSafe) {
  Builder B;
  B.call("malloc", temp(1), {MedVar::makeConst(8, 8)});
  B.call("strlcat", temp(0), {temp(1), param(2), MedVar::makeConst(8, 8)});

  auto Fnd = hunt(B.F);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Safe) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High);
  EXPECT_FALSE(Fnd->SkipReason.empty());
}

TEST(HuntEngine, StrncatCountAloneDoesNotBoundTheFinalString) {
  Builder B;
  B.call("malloc", temp(1), {MedVar::makeConst(8, 8)});
  B.call("strncat", temp(0), {temp(1), param(2), MedVar::makeConst(4, 8)});

  auto Fnd = hunt(B.F);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown);
  EXPECT_TRUE(Fnd->SkipReason.empty());
}

TEST(HuntEngine, WideCopyFailsClosedUntilElementWidthIsModelled) {
  Builder B;
  B.call("malloc", temp(1), {MedVar::makeConst(8, 8)});
  B.call("wmemcpy", temp(0), {temp(1), temp(2), MedVar::makeConst(4, 8)});

  auto Fnd = hunt(B.F);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown);
}

TEST(HuntEngine, WideCopyUsesThePlatformElementWidth) {
  struct Case {
    const char *Name;
    BinaryFormat Format;
    uint64_t Count;
    Verdict Expected;
  } Cases[] = {
      {"wmemcpy", BinaryFormat::COFF, 4, Verdict::Safe},
      {"wmemmove", BinaryFormat::COFF, 4, Verdict::Safe},
      {"wmemcpy", BinaryFormat::ELF, 2, Verdict::Safe},
      {"wmemmove", BinaryFormat::MachO, 2, Verdict::Safe},
      {"wmemcpy", BinaryFormat::ELF, 4, Verdict::Unknown},
      {"wmemcpy", BinaryFormat::Unknown, 2, Verdict::Unknown},
  };

  for (const Case &C : Cases) {
    SCOPED_TRACE(C.Name);
    SCOPED_TRACE(static_cast<int>(C.Format));
    SCOPED_TRACE(C.Count);
    Builder B;
    B.call("malloc", temp(1), {MedVar::makeConst(8, 8)});
    B.call(C.Name, temp(0), {temp(1), temp(2), MedVar::makeConst(C.Count, 8)});

    auto Fnd =
        hunt(B.F, /*StackRegs=*/false, /*LF=*/nullptr, Arch::X64, {}, C.Format);
    ASSERT_TRUE(Fnd.has_value());
    EXPECT_EQ(Fnd->TheVerdict, C.Expected) << Fnd->Detail;
  }
}

TEST(HuntEngine, ReachableWideCopyOverflowUsesPlatformElementWidth) {
  constexpr va_t SinkVA = 0x400010;
  Builder B;
  B.call("malloc", temp(1), {MedVar::makeConst(8, 8)});
  B.call("wmemcpy", temp(0), {temp(1), temp(2), MedVar::makeConst(3, 8)});
  B.F.Blocks[0].Ops.back().Addr = SinkVA;
  B.F.CC = CallingConv::SysV_AMD64;
  LowFunc LF = reachableSinkLow(SinkVA);

  auto Fnd =
      hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, {}, BinaryFormat::ELF);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unsafe) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High);
  ASSERT_FALSE(Fnd->Witness.empty());
  EXPECT_EQ(Fnd->Witness.front().second, "12");
}

TEST(HuntEngine, WideStringCopyStillRequiresAStringExtent) {
  Builder B;
  B.call("malloc", temp(1), {MedVar::makeConst(8, 8)});
  B.call("wcscpy", temp(0), {temp(1), temp(2)});

  auto Fnd = hunt(B.F, /*StackRegs=*/false, /*LF=*/nullptr, Arch::X64, {},
                  BinaryFormat::COFF);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown);
}

TEST(HuntEngine, UnknownCapacityFailsClosed) {
  Builder B;
  // dst is a bare load; length comes from read (tainted) but capacity unknown.
  B.op(NdOp::LOAD, temp(1), {MedVar::makeConst(0x4000, 8)});
  B.call("read", temp(5), {});
  B.call("memcpy", temp(0), {temp(1), temp(2), temp(5)});

  auto Fnd = hunt(B.F);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown);
}

TEST(HuntEngine, PathConstraintKeepsCopyInBound) {
  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("memcpy", temp(0), {temp(1), temp(2), rdxLen()});
  B.F.Blocks[0].Ops.back().Addr = 0x400010;
  LowFunc LF = guardedMemcpyLow(0x400010, /*OverflowGuard=*/false);
  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Safe);
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High);
}

TEST(HuntEngine, SolverBudgetExhaustionIsReported) {
  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("memcpy", temp(0), {temp(1), temp(2), rdxLen()});
  B.F.Blocks[0].Ops.back().Addr = 0x400010;
  LowFunc LF = solverHeavyGuardedMemcpyLow(0x400010);
  SafetyBudgets Budgets;
  Budgets.SolverConflicts = 1;

  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64, Budgets);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown);
  EXPECT_TRUE(Fnd->BudgetHit) << Fnd->Detail;
}

TEST(HuntEngine, FunctionEntrySelectsTheStartBlock) {
  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("memcpy", temp(0), {temp(1), temp(2), rdxLen()});
  B.F.Blocks[0].Ops.back().Addr = 0x400010;
  LowFunc LF = guardedMemcpyLow(0x400010, /*OverflowGuard=*/false);
  std::swap(LF.Blocks[0], LF.Blocks[1]);

  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Safe);
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High);
}

TEST(HuntEngine, PathConstraintWitnessesOverflow) {
  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("memcpy", temp(0), {temp(1), temp(2), rdxLen()});
  B.F.Blocks[0].Ops.back().Addr = 0x400010;
  LowFunc LF = guardedMemcpyLow(0x400010, /*OverflowGuard=*/true);
  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unsafe);
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High);
  EXPECT_FALSE(Fnd->Witness.empty());
  EXPECT_FALSE(Fnd->Constraints.empty());
}

TEST(HuntEngine, StrlenGuardKeepsStrcpyInBound) {
  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("strlen", temp(5), {param(2)});
  B.F.Blocks[0].Ops.back().Addr = 0x400004;
  B.call("strcpy", temp(0), {temp(1), param(2)});
  B.F.Blocks[0].Ops.back().Addr = 0x400010;
  LowFunc LF =
      strlenGuardedStrcpyLow(0x400004, 0x400010, /*OverflowGuard=*/false);
  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Safe);
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High);
}

TEST(HuntEngine, UnrelatedStrlenDoesNotWitnessStrcpyOverflow) {
  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("strlen", temp(5), {param(3)});
  B.F.Blocks[0].Ops.back().Addr = 0x400004;
  B.call("strcpy", temp(0), {temp(1), param(2)});
  B.F.Blocks[0].Ops.back().Addr = 0x400010;
  LowFunc LF =
      strlenGuardedStrcpyLow(0x400004, 0x400010, /*OverflowGuard=*/true);

  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown);
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, MemoryWriteInvalidatesEarlierStringLength) {
  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("strlen", temp(5), {param(2)});
  B.F.Blocks[0].Ops.back().Addr = 0x400004;
  B.call("strcpy", temp(0), {temp(1), param(2)});
  B.F.Blocks[0].Ops.back().Addr = 0x400010;
  LowFunc LF = strlenThenStoreGuardedStrcpyLow(0x400004, 0x400010);

  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown);
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, NullTerminatorAtSourceBoundsEarlierInput) {
  constexpr va_t ReadVA = 0x400004;
  constexpr va_t StrcpyVA = 0x400010;
  constexpr va_t SourceVA = 0x7000;
  MedVar Source = MedVar::makeConst(SourceVA, 8);
  Source.Provenance = ConstantAddressProvenance::DataAddress;
  Source.AddressOwnerVA = SourceVA;

  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("read", temp(5),
         {MedVar::makeConst(0, 8), Source, MedVar::makeConst(32, 8)});
  B.F.Blocks[0].Ops.back().Addr = ReadVA;
  B.call("strcpy", temp(0), {temp(1), Source});
  B.F.Blocks[0].Ops.back().Addr = StrcpyVA;
  LowFunc LF = readThenTerminateStrcpyLow(ReadVA, StrcpyVA, SourceVA);

  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Safe) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High) << Fnd->Detail;
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, ZeroWidthTerminatorDoesNotProveStringBound) {
  constexpr va_t ReadVA = 0x400004;
  constexpr va_t StrcpyVA = 0x400010;
  constexpr va_t SourceVA = 0x7000;
  MedVar Source = MedVar::makeConst(SourceVA, 8);
  Source.Provenance = ConstantAddressProvenance::DataAddress;
  Source.AddressOwnerVA = SourceVA;

  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("read", temp(5),
         {MedVar::makeConst(0, 8), Source, MedVar::makeConst(32, 8)});
  B.F.Blocks[0].Ops.back().Addr = ReadVA;
  B.call("strcpy", temp(0), {temp(1), Source});
  B.F.Blocks[0].Ops.back().Addr = StrcpyVA;
  LowFunc LF = readThenTerminateStrcpyLow(ReadVA, StrcpyVA, SourceVA);
  LF.Blocks[0].Ops[1] =
      lop(NdOp::STORE, NdVar{}, {NdVar::cst(SourceVA, 8), NdVar::cst(0, 0)},
          ReadVA + 4);

  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::Low) << Fnd->Detail;
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, OversizedLowInputCountFailsClosed) {
  constexpr va_t ReadVA = 0x400004;
  constexpr va_t StrcpyVA = 0x400010;
  constexpr va_t SourceVA = 0x7000;
  MedVar Source = MedVar::makeConst(SourceVA, 8);
  Source.Provenance = ConstantAddressProvenance::DataAddress;
  Source.AddressOwnerVA = SourceVA;

  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("read", temp(5),
         {MedVar::makeConst(0, 8), Source, MedVar::makeConst(32, 8)});
  B.F.Blocks[0].Ops.back().Addr = ReadVA;
  B.call("strcpy", temp(0), {temp(1), Source});
  B.F.Blocks[0].Ops.back().Addr = StrcpyVA;
  LowFunc LF = readThenTerminateStrcpyLow(ReadVA, StrcpyVA, SourceVA);
  ASSERT_LT(1u, LF.Blocks[0].Ops.size());
  LF.Blocks[0].Ops[1].NumInputs = 7;

  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unknown) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::Low) << Fnd->Detail;
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, NullTerminatorBoundsEarlierSourceEvidence) {
  constexpr va_t ReadVA = 0x400004;
  constexpr va_t StrcpyVA = 0x400010;
  constexpr va_t SourceVA = 0x7000;
  MedVar Source = MedVar::makeConst(SourceVA, 8);
  Source.Provenance = ConstantAddressProvenance::DataAddress;
  Source.AddressOwnerVA = SourceVA;

  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("read", temp(5),
         {MedVar::makeConst(0, 8), Source, MedVar::makeConst(32, 8)});
  B.F.Blocks[0].Ops.back().Addr = ReadVA;
  B.call("strcpy", temp(0), {temp(1), Source});
  B.F.Blocks[0].Ops.back().Addr = StrcpyVA;
  LowFunc LF =
      readThenTerminateStrcpyLow(ReadVA, StrcpyVA, SourceVA, /*Offset=*/1);

  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Safe) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High) << Fnd->Detail;
  EXPECT_TRUE(Fnd->Witness.empty());
}

TEST(HuntEngine, WideZeroStoreBoundsEarlierSourceEvidence) {
  constexpr va_t ReadVA = 0x400004;
  constexpr va_t StrcpyVA = 0x400010;
  constexpr va_t SourceVA = 0x7000;
  for (const bool HasAddressSpace : {false, true}) {
    SCOPED_TRACE(HasAddressSpace);
    MedVar Source = MedVar::makeConst(SourceVA, 8);
    Source.Provenance = ConstantAddressProvenance::DataAddress;
    Source.AddressOwnerVA = SourceVA;

    Builder B;
    B.F.Entry = 0x400000;
    B.F.CC = CallingConv::SysV_AMD64;
    B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
    B.call("read", temp(5),
           {MedVar::makeConst(0, 8), Source, MedVar::makeConst(32, 8)});
    B.F.Blocks[0].Ops.back().Addr = ReadVA;
    B.call("strcpy", temp(0), {temp(1), Source});
    B.F.Blocks[0].Ops.back().Addr = StrcpyVA;
    LowFunc LF = readThenTerminateStrcpyLow(ReadVA, StrcpyVA, SourceVA);
    std::vector<NdVar> StoreInputs;
    if (HasAddressSpace)
      StoreInputs.push_back(NdVar::cst(0, 1));
    StoreInputs.push_back(NdVar::cst(SourceVA + 1, 8));
    StoreInputs.push_back(NdVar::cst(0, 4));
    LF.Blocks[0].Ops[1] =
        lop(NdOp::STORE, NdVar{}, std::move(StoreInputs), ReadVA + 4);

    auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
    ASSERT_TRUE(Fnd.has_value());
    EXPECT_EQ(Fnd->TheVerdict, Verdict::Safe) << Fnd->Detail;
    EXPECT_EQ(Fnd->TheConfidence, Confidence::High) << Fnd->Detail;
    EXPECT_TRUE(Fnd->Witness.empty());
  }
}

TEST(HuntEngine, WrappedTerminatorAddressDoesNotBoundEarlierSource) {
  constexpr va_t ReadVA = 0x400004;
  constexpr va_t StrcpyVA = 0x400010;
  constexpr va_t SourceVA = UINT64_MAX;
  MedVar Source = MedVar::makeConst(SourceVA, 8);
  Source.Provenance = ConstantAddressProvenance::DataAddress;
  Source.AddressOwnerVA = SourceVA;

  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("read", temp(5),
         {MedVar::makeConst(0, 8), Source, MedVar::makeConst(32, 8)});
  B.F.Blocks[0].Ops.back().Addr = ReadVA;
  B.call("strcpy", temp(0), {temp(1), Source});
  B.F.Blocks[0].Ops.back().Addr = StrcpyVA;
  LowFunc LF =
      readThenTerminateStrcpyLow(ReadVA, StrcpyVA, SourceVA, /*Offset=*/1);

  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unsafe) << Fnd->Detail;
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High) << Fnd->Detail;
  EXPECT_FALSE(Fnd->Witness.empty());
}

TEST(HuntEngine, StrlenGuardWitnessesOverflow) {
  Builder B;
  B.F.Entry = 0x400000;
  B.F.CC = CallingConv::SysV_AMD64;
  B.call("malloc", temp(1), {MedVar::makeConst(16, 8)});
  B.call("strlen", temp(5), {param(2)});
  B.F.Blocks[0].Ops.back().Addr = 0x400004;
  B.call("strcpy", temp(0), {temp(1), param(2)});
  B.F.Blocks[0].Ops.back().Addr = 0x400010;
  LowFunc LF =
      strlenGuardedStrcpyLow(0x400004, 0x400010, /*OverflowGuard=*/true);
  auto Fnd = hunt(B.F, /*StackRegs=*/false, &LF, Arch::X64);
  ASSERT_TRUE(Fnd.has_value());
  EXPECT_EQ(Fnd->TheVerdict, Verdict::Unsafe);
  EXPECT_EQ(Fnd->TheConfidence, Confidence::High);
  EXPECT_FALSE(Fnd->Witness.empty());
}
