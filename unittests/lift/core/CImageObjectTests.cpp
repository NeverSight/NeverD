//===- CImageObjectTests.cpp - Image data declared as C objects -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "SourceCallExecution.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/render/CTypeFormat.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/Support/raw_ostream.h"

#include <string>
#include <vector>

using namespace neverd;

namespace {

constexpr auto NotFound = std::string::npos;
constexpr va_t TextVA = 0x1000, RodataVA = 0x2000, DataVA = 0x4000;
// Strings and a format in the readonly segment.
constexpr va_t Gbk = RodataVA, Utf8 = RodataVA + 0x10, Wide = RodataVA + 0x40,
               Format = RodataVA + 0x60;
// Pointer slots and scalars in the writable segment.
constexpr va_t Utf8Slot = DataVA, WideSlot = DataVA + 0x08,
               Counter = DataVA + 0x10, Unnamed = DataVA + 0x18,
               CallSlot = DataVA + 0x20;

void put(std::vector<uint8_t> &Bytes, va_t Offset,
         std::initializer_list<uint8_t> Values) {
  std::copy(Values.begin(), Values.end(), Bytes.begin() + Offset);
}

void putText(std::vector<uint8_t> &Bytes, va_t Offset,
             const std::string &Text) {
  std::copy(Text.begin(), Text.end(), Bytes.begin() + Offset);
}

void putPointer(std::vector<uint8_t> &Bytes, va_t Offset, uint64_t Value) {
  for (unsigned I = 0; I < 8; ++I)
    Bytes[Offset + I] = static_cast<uint8_t>(Value >> (8 * I));
}

Symbol dataSymbol(std::string Name, va_t Addr, uint64_t Size) {
  Symbol Sym;
  Sym.Name = std::move(Name);
  Sym.Addr = Addr;
  Sym.Size = Size;
  return Sym;
}

Segment segment(const char *Name, va_t VA, std::vector<uint8_t> Data,
                SegmentFlags Flags) {
  Segment Seg;
  Seg.Name = Name;
  Seg.VA = VA;
  Seg.Size = Data.size();
  Seg.Flags = Flags;
  Seg.Data = std::move(Data);
  return Seg;
}

/// A program's strings and the data that reaches them, as a compiler lays
/// out `const char *u8s = "中文 hello";` and `const char gbk[12] = ...`.
BinaryImage objectImage() {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::ELF;
  std::vector<uint8_t> Rodata(0x80, 0);
  // "中文字符" in GBK, in an object of twelve bytes.
  put(Rodata, Gbk - RodataVA, {0xD6, 0xD0, 0xCE, 0xC4, 0xD7, 0xD6, 0xB7, 0xFB});
  putText(Rodata, Utf8 - RodataVA, "中文 hello");
  // u"宽字符中文字符串" in UTF-16LE.
  put(Rodata, Wide - RodataVA,
      {0xBD, 0x5B, 0x57, 0x5B, 0x26, 0x7B, 0x2D, 0x4E, 0x87, 0x65, 0x57, 0x5B,
       0x26, 0x7B, 0x32, 0x4E});
  putText(Rodata, Format - RodataVA, "%p\n");
  std::vector<uint8_t> Data(0x28, 0);
  putPointer(Data, Utf8Slot - DataVA, Utf8);
  putPointer(Data, WideSlot - DataVA, Wide);
  put(Data, Counter - DataVA, {5});
  putPointer(Data, Unnamed - DataVA, 0x1234);
  // The call slot stays zero, as a GOT entry the dynamic linker fills.
  Img.Segments.push_back(
      segment(".text", TextVA, std::vector<uint8_t>(0x10),
              SegmentFlags::Readable | SegmentFlags::Executable));
  Img.Segments.push_back(
      segment(".rodata", RodataVA, std::move(Rodata), SegmentFlags::Readable));
  Img.Segments.push_back(
      segment(".data", DataVA, std::move(Data),
              SegmentFlags::Readable | SegmentFlags::Writable));
  Img.DataPtrRelocSlots.insert(Utf8Slot);
  Img.Symbols = {dataSymbol("gbk", Gbk, 12), dataSymbol("u8s", Utf8Slot, 8),
                 dataSymbol("u16s", WideSlot, 8),
                 dataSymbol("counter", Counter, 4)};
  return Img;
}

ExprPtr load(va_t Addr, TypeRef Type) {
  return HighExpr::makeLoad(HighExpr::makeConst(Addr, 8), std::move(Type));
}

HighStmt callStatement(ExprPtr Call) {
  HighStmt Statement;
  Statement.Kind = StmtKind::Call;
  Statement.CallExpr = std::move(Call);
  return Statement;
}

HighStmt returnStatement(ExprPtr Value) {
  HighStmt Statement;
  Statement.Kind = StmtKind::Return;
  Statement.RetVal = std::move(Value);
  return Statement;
}

HighFunc function(const char *Name, TypeRef ReturnType, va_t Entry) {
  HighFunc Func;
  Func.Name = Name;
  Func.Entry = Entry;
  Func.ReturnType = std::move(ReturnType);
  return Func;
}

/// Functions that pass the strings to libc, read the scalars and call through
/// the slot, as decompiled C prints them.
std::string emitObjects(const BinaryImage &Img, std::vector<HighFunc> *Out) {
  std::vector<HighFunc> Funcs;
  HighFunc Show = function("show", NdType::makeVoid(), TextVA);
  Show.Body = {
      callStatement(HighExpr::makeCall(
          "puts", 0x1100,
          {HighExpr::makeConst(Gbk, 8, ConstantAddressProvenance::Address)})),
      callStatement(HighExpr::makeCall(
          "puts", 0x1100, {load(Utf8Slot, NdType::makeInt(8, true))})),
      callStatement(HighExpr::makeCall(
          "printf", 0x1110,
          {HighExpr::makeConst(Format, 8, ConstantAddressProvenance::Address),
           load(WideSlot, NdType::makeInt(8, true))}))};
  Funcs.push_back(std::move(Show));
  HighFunc ReadCounter =
      function("read_counter", NdType::makeInt(4, true), TextVA + 4);
  ReadCounter.Body = {returnStatement(load(Counter, NdType::makeInt(4, true)))};
  Funcs.push_back(std::move(ReadCounter));
  HighFunc ReadUnnamed =
      function("read_unnamed", NdType::makeInt(8, true), TextVA + 8);
  ReadUnnamed.Body = {returnStatement(load(Unnamed, NdType::makeInt(8, true)))};
  Funcs.push_back(std::move(ReadUnnamed));
  HighFunc Resolve = function("resolve", NdType::makeInt(8, true), TextVA + 12);
  auto Through = HighExpr::makeCall("indirect", 0x1120, {});
  Through->IsIndirectCall = true;
  Through->IndirectTarget = load(CallSlot, NdType::makeInt(8, false));
  Through->Type = NdType::makeInt(8, true);
  Resolve.Body = {returnStatement(std::move(Through))};
  Funcs.push_back(std::move(Resolve));
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  Options.Format = BinaryFormat::ELF;
  Options.Image = &Img;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  EXPECT_TRUE(HighCEmitter().emit(Funcs, OS, Options));
  if (Out)
    *Out = std::move(Funcs);
  return Source;
}

TEST(CImageObjects, StringsDeclareTheirTextAndScalarsTheirValues) {
  const BinaryImage Img = objectImage();
  const std::string Source = emitObjects(Img, nullptr);
  // A string reached by its address is its array; the object's size keeps
  // the zero bytes after the text.
  EXPECT_NE(Source.find("const char gbk[12] = "
                        "\"\\xD6\\xD0\\xCE\\xC4\\xD7\\xD6\\xB7\\xFB\"; "
                        "/* GBK \"中文字符\" */"),
            NotFound)
      << Source;
  // A pointer slot holding a read-only string's address is its pointer.
  EXPECT_NE(Source.find("const char *u8s = \"中文 hello\";"), NotFound)
      << Source;
  EXPECT_NE(Source.find("const char16_t *u16s = u\"宽字符中文字符串\";"),
            NotFound)
      << Source;
  EXPECT_NE(Source.find("#include <uchar.h>"), NotFound) << Source;
  EXPECT_NE(Source.find("int32_t counter = 5;"), NotFound) << Source;
  // Unnamed data reads as the listing names it, with the value it holds.
  EXPECT_NE(Source.find("int64_t qword_4018 = 0x1234;"), NotFound) << Source;
  EXPECT_NE(Source.find("off_4020;"), NotFound) << Source;
  EXPECT_NE(Source.find("off_4020)()"), NotFound) << Source;
  // String parameters and variadic arguments take the pointers C has.
  EXPECT_NE(Source.find("puts(gbk"), NotFound) << Source;
  EXPECT_NE(Source.find("puts(u8s"), NotFound) << Source;
  EXPECT_NE(Source.find("printf(\"%p\\n\", u16s"), NotFound) << Source;
}

TEST(CImageObjects, DeclaredObjectsHoldTheImageBytes) {
  const BinaryImage Img = objectImage();
  const std::string Source = emitObjects(Img, nullptr);
  // The C compiles without integer and pointer confusion, and each object
  // holds what the image does.
  const std::string Check = Source + R"(
int main(void) {
  static const unsigned char GbkBytes[12] = {0xD6, 0xD0, 0xCE, 0xC4,
                                             0xD7, 0xD6, 0xB7, 0xFB};
  show();
  if (sizeof gbk != 12 || memcmp(gbk, GbkBytes, 12))
    return 1;
  if (strcmp(u8s, "\xE4\xB8\xAD\xE6\x96\x87 hello"))
    return 2;
  if (u16s[0] != 0x5BBD || u16s[7] != 0x4E32 || u16s[8])
    return 3;
  if (read_counter() != 5 || read_unnamed() != 0x1234)
    return 4;
  return 0;
}
)";
  source_call_execution_test::compileAndRun(
      Check, {"-Werror=int-conversion", "-Werror=incompatible-pointer-types"});
}

TEST(CImageObjects, WrittenSlotsKeepTheirMachineValue) {
  BinaryImage Img = objectImage();
  // A slot the code stores to is no constant string's pointer.
  HighFunc Func = function("replace", NdType::makeVoid(), TextVA);
  HighStmt Store;
  Store.Kind = StmtKind::Store;
  Store.StoreAddr = HighExpr::makeConst(Utf8Slot, 8);
  Store.StoreVal = HighExpr::makeConst(0, 8);
  Store.StoreVal->Type = NdType::makeInt(8, true);
  Func.Body = {std::move(Store)};
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  Options.Format = BinaryFormat::ELF;
  Options.Image = &Img;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  ASSERT_TRUE(HighCEmitter().emit({Func}, OS, Options));
  EXPECT_NE(Source.find("int64_t u8s = 0x2010;"), NotFound) << Source;
  EXPECT_EQ(Source.find("\"中文 hello\""), NotFound) << Source;
}

TEST(CImageObjects, WideAndNonFiniteValuesKeepTheirBytes) {
  // A vector constant pool entry and a NaN have no C literal; each object
  // still holds the image's bytes.
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::ELF;
  std::vector<uint8_t> Rodata(0x20, 0);
  for (unsigned I = 0; I < 16; ++I)
    Rodata[I] = static_cast<uint8_t>(0xF0 + I);
  putPointer(Rodata, 0x10, 0x7FF8000000000123ull);
  Img.Segments.push_back(
      segment(".text", TextVA, std::vector<uint8_t>(0x10),
              SegmentFlags::Readable | SegmentFlags::Executable));
  Img.Segments.push_back(
      segment(".rodata", RodataVA, std::move(Rodata), SegmentFlags::Readable));
  const TypeRef U64 = NdType::makeInt(8, false);
  HighFunc High = function("high_half", U64, TextVA);
  auto Half = HighExpr::makeBinop(NdOp::SUBBYTES,
                                  load(RodataVA, NdType::makeInt(16, true)),
                                  HighExpr::makeConst(8, 4));
  Half->Type = U64;
  High.Body = {returnStatement(std::move(Half))};
  HighFunc Nan = function("nan_bits", U64, TextVA + 4);
  Nan.Body = {returnStatement(
      HighExpr::makeBitCast(load(RodataVA + 0x10, NdType::makeFloat(8)), U64))};
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  Options.Format = BinaryFormat::ELF;
  Options.Image = &Img;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  ASSERT_TRUE(HighCEmitter().emit({High, Nan}, OS, Options));
  OS.flush();
  source_call_execution_test::compileAndRun(Source + R"(
int main(void) {
  if (high_half() != 0xFFFEFDFCFBFAF9F8ull)
    return 1;
  return nan_bits() != 0x7FF8000000000123ull;
}
)");
}

TEST(CImageObjects, AccessesTypeObjectsTheCodeAlsoTakesTheAddressOf) {
  // `__do_global_dtors_aux` stores one byte to `completed.0`, and
  // `deregister_tm_clones` only takes its address. Whichever function comes
  // first, the object is the byte the store writes: declared wider,
  // `completed_x2E_0 = 1;` writes past it.
  constexpr va_t Done = DataVA + 0x14, Marker = DataVA + 0x16;
  BinaryImage Img = objectImage();
  Img.Symbols.push_back(dataSymbol("done", Done, 1));
  // A label with no size, which nothing reads or writes.
  Img.Symbols.push_back(dataSymbol("marker", Marker, 0));
  auto Mark = [] {
    HighFunc Func = function("mark", NdType::makeVoid(), TextVA);
    HighStmt Store;
    Store.Kind = StmtKind::Store;
    Store.StoreAddr = HighExpr::makeConst(Done, 8);
    Store.StoreVal = HighExpr::makeConst(1, 1);
    Store.StoreVal->Type = NdType::makeInt(1, true);
    Func.Body = {std::move(Store)};
    return Func;
  };
  auto AddressOf = [](const char *Name, va_t Addr, va_t Entry) {
    HighFunc Func = function(Name, NdType::makeInt(8, true), Entry);
    Func.Body = {returnStatement(
        HighExpr::makeConst(Addr, 8, ConstantAddressProvenance::Address))};
    return Func;
  };
  for (bool AddressFirst : {true, false}) {
    SCOPED_TRACE(AddressFirst);
    std::vector<HighFunc> Funcs;
    Funcs.push_back(AddressOf("done_address", Done, TextVA + 4));
    Funcs.push_back(Mark());
    if (!AddressFirst)
      std::swap(Funcs[0], Funcs[1]);
    Funcs.push_back(AddressOf("marker_address", Marker, TextVA + 8));
    CEmitterOptions Options;
    Options.TheArch = Arch::X64;
    Options.Format = BinaryFormat::ELF;
    Options.Image = &Img;
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    ASSERT_TRUE(HighCEmitter().emit(Funcs, OS, Options));
    EXPECT_NE(Source.find("int8_t done"), NotFound) << Source;
    // An object no access sizes keeps a two-byte stand-in.
    EXPECT_NE(Source.find("int16_t marker"), NotFound) << Source;
    const std::string Check = Source + R"(
int main(void) {
  mark();
  if (sizeof done != 1 || done != 1)
    return 1;
  if (done_address() != (int64_t)(uintptr_t)&done)
    return 2;
  return 0;
}
)";
    source_call_execution_test::compileAndRun(
        Check,
        {"-Werror=int-conversion", "-Werror=incompatible-pointer-types"});
  }
}

TEST(CImageObjects, AnIndexedTableIsDeclaredWhole) {
  // `table[i & 15]` reads the table at a variable offset: the whole sized
  // object is declared, and the read goes through it, not through the
  // image address it had.
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::ELF;
  std::vector<uint8_t> Rodata(0x20, 0);
  for (unsigned I = 0; I < 16; ++I)
    Rodata[I] = static_cast<uint8_t>(3 * I + 7);
  Img.Segments.push_back(
      segment(".text", TextVA, std::vector<uint8_t>(0x10),
              SegmentFlags::Readable | SegmentFlags::Executable));
  Img.Segments.push_back(
      segment(".rodata", RodataVA, std::move(Rodata), SegmentFlags::Readable));
  Img.Symbols = {dataSymbol("table", RodataVA, 16)};
  const TypeRef U64 = NdType::makeInt(8, false);
  MedVar Index;
  Index.Kind = MedVar::Param;
  Index.Id = 0;
  Index.Size = 8;
  Index.TheArch = Arch::X64;
  auto Masked = HighExpr::makeBinop(
      NdOp::INT_AND, HighExpr::makeVar(Index, U64), HighExpr::makeConst(15, 8));
  Masked->Type = U64;
  auto Address = HighExpr::makeBinop(
      NdOp::INT_ADD, Masked,
      HighExpr::makeConst(RodataVA, 8, ConstantAddressProvenance::Address));
  Address->Type = U64;
  auto Read = HighExpr::makeLoad(Address, NdType::makeInt(1, false));
  auto Wide = HighExpr::makeUnary(NdOp::INT_ZEXT, Read);
  Wide->Type = U64;
  HighFunc Lookup = function("lookup", U64, TextVA);
  Lookup.Params = {{"arg0", U64}};
  Lookup.Body = {returnStatement(Wide)};
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  Options.Format = BinaryFormat::ELF;
  Options.Image = &Img;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  ASSERT_TRUE(HighCEmitter().emit({Lookup}, OS, Options));
  OS.flush();
  EXPECT_NE(Source.find("table[16] = {"), NotFound) << Source;
  EXPECT_NE(Source.find("&table[0] + (arg0 & 15)"), NotFound) << Source;
  source_call_execution_test::compileAndRun(Source + R"(
int main(void) {
  for (uint64_t i = 0; i < 40; ++i)
    if (lookup(i) != 3 * (i & 15) + 7)
      return 1;
  return 0;
}
)");
}

TEST(CImageObjects, AnAddressIntoAnObjectReachesAllOfIt) {
  // `p = &vals[1]` and `vals[1 + i]` start inside the object: the whole
  // object is declared, and each address is the place in it.
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::ELF;
  std::vector<uint8_t> Rodata(0x20, 0);
  for (unsigned I = 0; I < 4; ++I)
    Rodata[4 * I] = static_cast<uint8_t>(10 * (I + 1));
  Img.Segments.push_back(
      segment(".text", TextVA, std::vector<uint8_t>(0x10),
              SegmentFlags::Readable | SegmentFlags::Executable));
  Img.Segments.push_back(
      segment(".rodata", RodataVA, std::move(Rodata), SegmentFlags::Readable));
  Img.Symbols = {dataSymbol("vals", RodataVA, 16)};
  const TypeRef U64 = NdType::makeInt(8, false);
  const TypeRef I32 = NdType::makeInt(4, true);
  MedVar Index;
  Index.Kind = MedVar::Param;
  Index.Id = 0;
  Index.Size = 8;
  Index.TheArch = Arch::X64;
  auto Scaled = HighExpr::makeBinop(
      NdOp::INT_MULT, HighExpr::makeVar(Index, U64), HighExpr::makeConst(4, 8));
  Scaled->Type = U64;
  auto Address = HighExpr::makeBinop(
      NdOp::INT_ADD, Scaled,
      HighExpr::makeConst(RodataVA + 4, 8, ConstantAddressProvenance::Address));
  Address->Type = U64;
  HighFunc After = function("after_first", I32, TextVA);
  After.Params = {{"arg0", U64}};
  After.Body = {returnStatement(HighExpr::makeLoad(Address, I32))};
  HighFunc Third = function("third", I32, TextVA + 4);
  Third.Body = {returnStatement(HighExpr::makeLoad(
      HighExpr::makeConst(RodataVA + 8, 8, ConstantAddressProvenance::Address),
      I32))};
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  Options.Format = BinaryFormat::ELF;
  Options.Image = &Img;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  ASSERT_TRUE(HighCEmitter().emit({After, Third}, OS, Options));
  OS.flush();
  EXPECT_NE(Source.find("vals[16] = {"), NotFound) << Source;
  source_call_execution_test::compileAndRun(Source + R"(
int main(void) {
  if (after_first(0) != 20 || after_first(2) != 40)
    return 1;
  return third() != 30;
}
)");
}

TEST(CImageObjects, AnAddressTheCodeKeepsReachesTheWholeObject) {
  // `return vals;` and `return &vals[2];` hand out pointers that the caller
  // reads past their first element: the whole object is declared.
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::ELF;
  // Control characters: no string literal stands in for any part of it.
  std::vector<uint8_t> Rodata(0x20, 0);
  for (unsigned I = 0; I < 16; ++I)
    Rodata[I] = static_cast<uint8_t>(0x11 + I / 4);
  Img.Segments.push_back(
      segment(".text", TextVA, std::vector<uint8_t>(0x10),
              SegmentFlags::Readable | SegmentFlags::Executable));
  Img.Segments.push_back(
      segment(".rodata", RodataVA, std::move(Rodata), SegmentFlags::Readable));
  Img.Symbols = {dataSymbol("vals", RodataVA, 16)};
  const TypeRef U64 = NdType::makeInt(8, false);
  HighFunc Whole = function("whole", U64, TextVA);
  Whole.Body = {returnStatement(
      HighExpr::makeConst(RodataVA, 8, ConstantAddressProvenance::Address))};
  HighFunc Third = function("third_place", U64, TextVA + 4);
  Third.Body = {returnStatement(HighExpr::makeConst(
      RodataVA + 8, 8, ConstantAddressProvenance::Address))};
  for (const bool BothFunctions : {true, false}) {
    std::vector<HighFunc> Funcs = {Third};
    if (BothFunctions)
      Funcs.push_back(Whole);
    CEmitterOptions Options;
    Options.TheArch = Arch::X64;
    Options.Format = BinaryFormat::ELF;
    Options.Image = &Img;
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    ASSERT_TRUE(HighCEmitter().emit(Funcs, OS, Options));
    OS.flush();
    EXPECT_NE(Source.find("vals[16] = {"), NotFound) << Source;
    source_call_execution_test::compileAndRun(Source + std::string(BothFunctions
                                                                       ? R"(
int main(void) {
  const int32_t *all = (const int32_t *)(uintptr_t)whole();
  const int32_t *third = (const int32_t *)(uintptr_t)third_place();
  return all[3] != 0x14141414 || third[0] != 0x13131313 ||
         third[1] != 0x14141414;
}
)"
                                                                       : R"(
int main(void) {
  const int32_t *third = (const int32_t *)(uintptr_t)third_place();
  return third[-2] != 0x11111111 || third[1] != 0x14141414;
}
)"));
  }
}

TEST(CImageObjects, ATableWhoseBytesReadAsTextStaysATable) {
  // {10, 20, 0x4241, 40}: the first word reads as the UTF-16 text "\n" and
  // the third as "AB", but no literal holds the table the caller reads.
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::ELF;
  std::vector<uint8_t> Rodata(0x20, 0);
  Rodata[0] = 10;
  Rodata[4] = 20;
  Rodata[8] = 'A';
  Rodata[9] = 'B';
  Rodata[12] = 40;
  Img.Segments.push_back(
      segment(".text", TextVA, std::vector<uint8_t>(0x10),
              SegmentFlags::Readable | SegmentFlags::Executable));
  Img.Segments.push_back(
      segment(".rodata", RodataVA, std::move(Rodata), SegmentFlags::Readable));
  Img.Symbols = {dataSymbol("vals", RodataVA, 16)};
  const TypeRef U64 = NdType::makeInt(8, false);
  HighFunc Whole = function("whole", U64, TextVA);
  Whole.Body = {returnStatement(
      HighExpr::makeConst(RodataVA, 8, ConstantAddressProvenance::Address))};
  HighFunc Third = function("third_place", U64, TextVA + 4);
  Third.Body = {returnStatement(HighExpr::makeConst(
      RodataVA + 8, 8, ConstantAddressProvenance::Address))};
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  Options.Format = BinaryFormat::ELF;
  Options.Image = &Img;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  ASSERT_TRUE(HighCEmitter().emit({Whole, Third}, OS, Options));
  OS.flush();
  EXPECT_EQ(Source.find("L\""), NotFound) << Source;
  EXPECT_EQ(Source.find("\"AB\""), NotFound) << Source;
  source_call_execution_test::compileAndRun(Source + R"(
int main(void) {
  const int32_t *all = (const int32_t *)(uintptr_t)whole();
  const int32_t *third = (const int32_t *)(uintptr_t)third_place();
  return all[3] != 40 || third[0] != 0x4241 || third[-2] != 10;
}
)");
}

TEST(CImageObjects, ARelocatedPointerTableNamesWhatItPointsTo) {
  // `static int *const rows[3] = {a, b, c}`: the image relocates each slot,
  // so the table holds the arrays' addresses, which the rebuilt program
  // has elsewhere.  `rows[i][j]` reads through the table.
  constexpr va_t Rows = RodataVA, A = RodataVA + 0x40, B = RodataVA + 0x60,
                 C = RodataVA + 0x80;
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::ELF;
  std::vector<uint8_t> Rodata(0xA0, 0);
  putPointer(Rodata, 0, A);
  putPointer(Rodata, 8, B);
  putPointer(Rodata, 16, C);
  for (unsigned I = 0; I < 4; ++I) {
    Rodata[0x40 + 4 * I] = static_cast<uint8_t>(10 + I);
    Rodata[0x60 + 4 * I] = static_cast<uint8_t>(20 + I);
    Rodata[0x80 + 4 * I] = static_cast<uint8_t>(30 + I);
  }
  Img.Segments.push_back(
      segment(".text", TextVA, std::vector<uint8_t>(0x10),
              SegmentFlags::Readable | SegmentFlags::Executable));
  Img.Segments.push_back(
      segment(".rodata", RodataVA, std::move(Rodata), SegmentFlags::Readable));
  Img.Symbols = {dataSymbol("rows", Rows, 24), dataSymbol("a", A, 16),
                 dataSymbol("b", B, 16), dataSymbol("c", C, 16)};
  Img.DataPtrRelocSlots = {Rows, Rows + 8, Rows + 16};
  const TypeRef U64 = NdType::makeInt(8, false);
  const TypeRef I32 = NdType::makeInt(4, true);
  auto Param = [&](int Id) {
    MedVar V;
    V.Kind = MedVar::Param;
    V.Id = Id;
    V.Size = 8;
    V.TheArch = Arch::X64;
    return HighExpr::makeVar(V, U64);
  };
  auto Scaled = [&](ExprPtr Index, uint64_t Scale) {
    auto E = HighExpr::makeBinop(NdOp::INT_MULT, std::move(Index),
                                 HighExpr::makeConst(Scale, 8));
    E->Type = U64;
    return E;
  };
  auto Sum = [&](ExprPtr L, ExprPtr R) {
    auto E = HighExpr::makeBinop(NdOp::INT_ADD, std::move(L), std::move(R));
    E->Type = U64;
    return E;
  };
  auto Row = HighExpr::makeLoad(
      Sum(Scaled(Param(0), 8),
          HighExpr::makeConst(Rows, 8, ConstantAddressProvenance::Address)),
      U64);
  HighFunc Pick = function("pick", I32, TextVA);
  Pick.Params = {{"arg0", U64}, {"arg1", U64}};
  Pick.Body = {
      returnStatement(HighExpr::makeLoad(Sum(Row, Scaled(Param(1), 4)), I32))};
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  Options.Format = BinaryFormat::ELF;
  Options.Image = &Img;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  ASSERT_TRUE(HighCEmitter().emit({Pick}, OS, Options));
  OS.flush();
  EXPECT_NE(Source.find("(uintptr_t)&b[0]"), NotFound) << Source;
  source_call_execution_test::compileAndRun(Source + R"(
int main(void) {
  for (uint64_t i = 0; i < 3; ++i)
    for (uint64_t j = 0; j < 4; ++j)
      if (pick(i, j) != (int32_t)(10 * (i + 1) + j))
        return 1;
  return 0;
}
)");
}

/// An x86-64 ELF image with \p Rodata and \p Data at RodataVA and DataVA.
BinaryImage dataImage(std::vector<uint8_t> Rodata, std::vector<uint8_t> Data) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::ELF;
  Img.Segments.push_back(
      segment(".text", TextVA, std::vector<uint8_t>(0x20),
              SegmentFlags::Readable | SegmentFlags::Executable));
  Img.Segments.push_back(
      segment(".rodata", RodataVA, std::move(Rodata), SegmentFlags::Readable));
  Img.Segments.push_back(
      segment(".data", DataVA, std::move(Data),
              SegmentFlags::Readable | SegmentFlags::Writable));
  return Img;
}

std::string emitWithImage(const std::vector<HighFunc> &Funcs,
                          const BinaryImage &Img) {
  CEmitterOptions Options;
  Options.TheArch = Img.Arch;
  Options.Format = Img.Format;
  Options.Image = &Img;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  EXPECT_TRUE(HighCEmitter().emit(Funcs, OS, Options));
  OS.flush();
  return Source;
}

ExprPtr dataParam(int Id) {
  MedVar V;
  V.Kind = MedVar::Param;
  V.Id = Id;
  V.Size = 8;
  V.TheArch = Arch::X64;
  return HighExpr::makeVar(V, NdType::makeInt(8, false));
}

TEST(CImageObjects, AnAtomicOnBytesTakesItsTypedPointer) {
  // `q` is read at 8 and at 4 bytes, so it is declared as bytes; the
  // compare-exchange on it takes a uint64_t pointer, not a byte's.
  std::vector<uint8_t> Data(0x10, 0);
  Data[0] = 5;
  BinaryImage Img = dataImage(std::vector<uint8_t>(0x10), std::move(Data));
  Img.Symbols = {dataSymbol("q", DataVA, 8)};
  const TypeRef U64 = NdType::makeInt(8, false);
  auto Cas = HighExpr::makeBinop(
      NdOp::ATOMIC_CMPXCHG,
      HighExpr::makeConst(DataVA, 8, ConstantAddressProvenance::Address),
      dataParam(0));
  Cas->Operands.push_back(dataParam(1));
  Cas->Type = U64;
  Cas->MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
  HighFunc Swap = function("swap", U64, TextVA);
  Swap.Params = {{"arg0", U64}, {"arg1", U64}};
  Swap.Body = {returnStatement(Cas)};
  HighFunc Low = function("low", NdType::makeInt(4, false), TextVA + 0x10);
  Low.Body = {returnStatement(load(DataVA, NdType::makeInt(4, false)))};
  const std::string Source = emitWithImage({Swap, Low}, Img);
  source_call_execution_test::compileAndRun(Source + R"(
int main(void) { return swap(5, 7) != 5 || low() != 7; }
)");
}

TEST(CImageObjects, AStringAddressStoredAsAnIntegerConverts) {
  // The code keeps a string's address in an integer object: the literal is
  // an array, and the object takes its address.
  std::vector<uint8_t> Rodata = {'h', 'i', 0, 0};
  BinaryImage Img = dataImage(std::move(Rodata), std::vector<uint8_t>(0x10));
  Img.Symbols = {dataSymbol("msg", RodataVA, 3), dataSymbol("kept", DataVA, 8)};
  const TypeRef U64 = NdType::makeInt(8, false);
  HighStmt Keep;
  Keep.Kind = StmtKind::Store;
  Keep.StoreAddr =
      HighExpr::makeConst(DataVA, 8, ConstantAddressProvenance::Address);
  Keep.StoreVal =
      HighExpr::makeConst(RodataVA, 8, ConstantAddressProvenance::DataAddress);
  Keep.StoreVal->Type = U64;
  HighFunc Remember = function("remember", U64, TextVA);
  Remember.Body = {Keep, returnStatement(load(DataVA, U64))};
  const std::string Source = emitWithImage({Remember}, Img);
  source_call_execution_test::compileAndRun(Source + R"(
int main(void) {
  const char *text = (const char *)(uintptr_t)remember();
  return text[0] != 'h' || text[1] != 'i' || text[2] != 0;
}
)");
}

TEST(CImageObjects, AnAddressPairKeepsItsObjects) {
  // `{a, b}` stored as one 64-bit value on i386: each half is an address,
  // not the number it has in the image.
  BinaryImage Img = dataImage(std::vector<uint8_t>(0x20), {});
  Img.Arch = Arch::X86;
  Img.Bits = Bitness::Bits32;
  Img.Symbols = {dataSymbol("a", RodataVA, 8),
                 dataSymbol("b", RodataVA + 8, 8)};
  auto Pair = HighExpr::makeBinop(
      NdOp::CONCAT,
      HighExpr::makeConst(RodataVA + 8, 4,
                          ConstantAddressProvenance::DataAddress),
      HighExpr::makeConst(RodataVA, 4, ConstantAddressProvenance::DataAddress));
  Pair->Type = NdType::makeInt(8, false);
  Pair->Operands[0]->Type = Pair->Operands[1]->Type = NdType::makeInt(4, false);
  HighFunc Both = function("both", NdType::makeInt(8, false), TextVA);
  Both.Body = {returnStatement(Pair)};
  const std::string Source = emitWithImage({Both}, Img);
  const size_t Body = Source.find("both(");
  ASSERT_NE(Body, NotFound) << Source;
  EXPECT_EQ(Source.find(std::to_string(RodataVA + 8) + " << 32", Body),
            NotFound)
      << Source;
  EXPECT_NE(Source.find("&b", Body), NotFound) << Source;
}

TEST(CImageObjects, AnObjectWrittenAtAVariableOffsetIsDeclaredWhole) {
  // `count` is read directly and written as `*(base + &count)`, base a
  // reloaded GOT base that is zero: both reach the one object, which a
  // number in the write would miss.
  BinaryImage Img = dataImage({}, std::vector<uint8_t>(0x10));
  Img.Segments.erase(Img.Segments.begin() + 1);
  Img.Symbols = {dataSymbol("count", DataVA, 4)};
  const TypeRef U64 = NdType::makeInt(8, false);
  const TypeRef I32 = NdType::makeInt(4, true);
  auto Address = HighExpr::makeBinop(
      NdOp::INT_ADD, dataParam(0),
      HighExpr::makeConst(DataVA, 8, ConstantAddressProvenance::Unknown));
  Address->Type = U64;
  HighStmt Write;
  Write.Kind = StmtKind::Store;
  Write.StoreAddr = Address;
  Write.StoreVal = HighExpr::makeBinop(NdOp::SUBBYTES, dataParam(1),
                                       HighExpr::makeConst(0, 4));
  Write.StoreVal->Type = I32;
  HighFunc Put = function("put", NdType::makeVoid(), TextVA);
  Put.Params = {{"arg0", U64}, {"arg1", U64}};
  Put.Body = {Write};
  HighFunc Get = function("get", I32, TextVA + 0x10);
  Get.Body = {returnStatement(load(DataVA, I32))};
  const std::string Source = emitWithImage({Put, Get}, Img);
  source_call_execution_test::compileAndRun(Source + R"(
int main(void) {
  put(0, 99);
  return get() != 99;
}
)");
}

TEST(CImageObjects, LiteralsSpellExactlyTheirBytes) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::ELF;
  std::vector<uint8_t> Rodata(0x40, 0);
  // windows-1252 "café crème", and GBK "中文字符" before digits, which a
  // hexadecimal escape would take.
  put(Rodata, 0x00, {'c', 'a', 'f', 0xE9, ' ', 'c', 'r', 0xE8, 'm', 'e', 0});
  put(Rodata, 0x20,
      {0xD6, 0xD0, 0xCE, 0xC4, 0xD7, 0xD6, 0xB7, 0xFB, '2', '0', '2', '4', 0});
  // A trigraph, a quote and a tab.
  putText(Rodata, 0x10, "?\?=\"\tx");
  Img.Segments.push_back(
      segment(".rodata", RodataVA, std::move(Rodata), SegmentFlags::Readable));
  const auto Latin = imageCString(&Img, RodataVA);
  ASSERT_TRUE(Latin);
  EXPECT_EQ(Latin->Literal, "\"caf\\xE9 cr\\xE8me\"");
  EXPECT_EQ(Latin->Bytes, 11u);
  EXPECT_NE(Latin->Note.find("café crème"), NotFound) << Latin->Note;
  const auto Digits = imageCString(&Img, RodataVA + 0x20);
  ASSERT_TRUE(Digits);
  EXPECT_EQ(Digits->Literal,
            "\"\\xD6\\xD0\\xCE\\xC4\\xD7\\xD6\\xB7\\xFB\" \"2024\"");
  const auto Marks = imageCString(&Img, RodataVA + 0x10);
  ASSERT_TRUE(Marks);
  EXPECT_EQ(Marks->Literal, "\"?\\?=\\\"\\tx\"");
  // Nothing starts in the middle of nowhere.
  EXPECT_FALSE(imageCString(&Img, RodataVA + 0x3F));
}

} // namespace
