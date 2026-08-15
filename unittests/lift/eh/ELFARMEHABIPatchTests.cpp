//===- ELFARMEHABIPatchTests.cpp - ARM EHABI table rewrite tests ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/backend/codegen/ELF/ELFARMEHABIPatch.h"
#include "neverd/loader/ExceptionUnwindOp.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace neverd;

// A synthetic ELF32 ARM image laid out so that every offset the rewrite has to
// reason about is known in advance: one PT_LOAD over the whole file, a
// PT_ARM_EXIDX over `.ARM.exidx`, and `.text` / `.ARM.extab` / `.ARM.exidx`
// carrying two original functions' unwind information.
constexpr uint64_t kBase = 0x10000;
constexpr uint64_t kPhOff = 52;
constexpr uint64_t kPhEnt = 32;
constexpr uint64_t kShEnt = 40;
constexpr uint64_t kSectionCount = 5;

constexpr uint64_t kTextOff = 128;
constexpr uint64_t kTextSize = 32;
constexpr uint64_t kTabOff = 160;
constexpr uint64_t kTabSize = 16;
constexpr uint64_t kIdxOff = 256;
constexpr uint64_t kIdxSize = 16;

constexpr uint64_t kTextVA = kBase + kTextOff;
constexpr uint64_t kTabVA = kBase + kTabOff;
constexpr uint64_t kIdxVA = kBase + kIdxOff;

// Two functions the image already describes, and one that sorts between them
// so that registering it has to insert rather than append.
constexpr uint64_t kFunc0VA = kTextVA;        // cannot be unwound through
constexpr uint64_t kFunc2VA = kTextVA + 0x10; // described by `.ARM.extab`
constexpr uint64_t kFunc1VA = kTextVA + 0x08; // the one being registered
constexpr uint64_t kPersonalityVA = kTextVA + 0x18;

// Where a compile's own output would land, past everything the image maps.
constexpr uint64_t kAppendedSegmentVA = 0x20000;

// Name offsets into the synthetic `.shstrtab`.
constexpr uint32_t kNameText = 1;
constexpr uint32_t kNameTab = 7;
constexpr uint32_t kNameIdx = 18;
constexpr uint32_t kNameStr = 29;
constexpr uint32_t kNameRodata = 39;
constexpr uint64_t kStrTabSize = 47;

constexpr uint32_t kCantUnwind = 1;
constexpr uint32_t kCompactBit = 0x80000000u;
constexpr uint64_t kIndexEntrySize = 8;
constexpr uint8_t kFinish = 0xB0;

void putU16(std::vector<uint8_t> &B, size_t Off, uint16_t V) {
  B[Off] = V & 0xff;
  B[Off + 1] = (V >> 8) & 0xff;
}

void putU32(std::vector<uint8_t> &B, size_t Off, uint32_t V) {
  for (int I = 0; I < 4; ++I)
    B[Off + I] = (V >> (8 * I)) & 0xff;
}

uint32_t getU32(llvm::ArrayRef<uint8_t> B, size_t Off) {
  return static_cast<uint32_t>(B[Off]) |
         (static_cast<uint32_t>(B[Off + 1]) << 8) |
         (static_cast<uint32_t>(B[Off + 2]) << 16) |
         (static_cast<uint32_t>(B[Off + 3]) << 24);
}

void pushU32(std::vector<uint8_t> &B, uint32_t V) {
  for (int I = 0; I < 4; ++I)
    B.push_back((V >> (8 * I)) & 0xff);
}

// The `prel31` a field at \p FieldVA needs in order to name \p TargetVA, and
// the address such a field resolves to.  Stated here independently of the
// rewrite so that a test reads the encoding rather than trusting it.
uint32_t prel31From(uint64_t FieldVA, uint64_t TargetVA) {
  const int64_t Displacement =
      static_cast<int64_t>(TargetVA) - static_cast<int64_t>(FieldVA);
  return static_cast<uint32_t>(Displacement) & 0x7FFFFFFFu;
}

uint64_t prel31Target(uint32_t Word, uint64_t FieldVA) {
  const int32_t Displacement = static_cast<int32_t>(Word << 1) >> 1;
  return (FieldVA + static_cast<uint64_t>(static_cast<int64_t>(Displacement))) &
         0xFFFFFFFFull;
}

// How much room the image leaves after `.ARM.exidx`, which is what decides
// whether a grown index fits.
struct ELFOptions {
  uint64_t IndexSlack = 240;
  // Names the descriptor section something else, leaving the image with an
  // index and nowhere to put a descriptor.
  bool WithTable = true;
};

std::vector<uint8_t> makeARMELFWithEHABI(const ELFOptions &Opts = {}) {
  const uint64_t StrOff = kIdxOff + kIdxSize + Opts.IndexSlack;
  const uint64_t ShOff = (StrOff + kStrTabSize + 3) & ~uint64_t(3);
  const uint64_t FileEnd = ShOff + kSectionCount * kShEnt;
  std::vector<uint8_t> B(static_cast<size_t>(FileEnd), 0);

  std::memcpy(B.data(),
              "\x7f"
              "ELF",
              4);
  B[llvm::ELF::EI_CLASS] = llvm::ELF::ELFCLASS32;
  B[llvm::ELF::EI_DATA] = llvm::ELF::ELFDATA2LSB;
  B[llvm::ELF::EI_VERSION] = llvm::ELF::EV_CURRENT;
  putU16(B, 16, llvm::ELF::ET_DYN);
  putU16(B, 18, llvm::ELF::EM_ARM);
  putU32(B, 20, llvm::ELF::EV_CURRENT);
  putU32(B, 24, static_cast<uint32_t>(kTextVA)); // e_entry
  putU32(B, 28, static_cast<uint32_t>(kPhOff));  // e_phoff
  putU32(B, 32, static_cast<uint32_t>(ShOff));   // e_shoff
  putU16(B, 40, 52);                             // e_ehsize
  putU16(B, 42, kPhEnt);                         // e_phentsize
  putU16(B, 44, 2);                              // e_phnum
  putU16(B, 46, kShEnt);                         // e_shentsize
  putU16(B, 48, kSectionCount);                  // e_shnum
  putU16(B, 50, 3);                              // e_shstrndx

  auto phdr = [&](unsigned Idx, uint32_t Type, uint32_t Off, uint64_t VA,
                  uint32_t Size, uint32_t Flags, uint32_t Align) {
    const uint64_t H = kPhOff + Idx * kPhEnt;
    putU32(B, H + 0, Type);
    putU32(B, H + 4, Off);
    putU32(B, H + 8, static_cast<uint32_t>(VA));
    putU32(B, H + 12, static_cast<uint32_t>(VA));
    putU32(B, H + 16, Size);
    putU32(B, H + 20, Size);
    putU32(B, H + 24, Flags);
    putU32(B, H + 28, Align);
  };
  phdr(0, llvm::ELF::PT_LOAD, 0, kBase, static_cast<uint32_t>(FileEnd),
       llvm::ELF::PF_R | llvm::ELF::PF_X, 0x1000);
  phdr(1, llvm::ELF::PT_ARM_EXIDX, static_cast<uint32_t>(kIdxOff), kIdxVA,
       static_cast<uint32_t>(kIdxSize), llvm::ELF::PF_R, 4);

  for (uint64_t I = 0; I < kTextSize; ++I)
    B[kTextOff + I] = 0xFF;

  // `.ARM.extab`: one generic descriptor, naming a personality routine by
  // address and carrying eight bytes of that routine's own data.
  putU32(B, kTabOff + 0, prel31From(kTabVA, kPersonalityVA));
  putU32(B, kTabOff + 4,
         (uint32_t(0xA8) << 16) | (uint32_t(kFinish) << 8) | kFinish);
  putU32(B, kTabOff + 8, 0x11223344);
  putU32(B, kTabOff + 12, 0x55667788);

  // `.ARM.exidx`: the first function may not be unwound through, the second is
  // described by the descriptor above.
  putU32(B, kIdxOff + 0, prel31From(kIdxVA, kFunc0VA));
  putU32(B, kIdxOff + 4, kCantUnwind);
  putU32(B, kIdxOff + 8, prel31From(kIdxVA + 8, kFunc2VA));
  putU32(B, kIdxOff + 12, prel31From(kIdxVA + 12, kTabVA));

  const char Names[kStrTabSize] = "\0.text\0.ARM.extab\0.ARM.exidx\0"
                                  ".shstrtab\0.rodata";
  std::memcpy(B.data() + StrOff, Names, kStrTabSize);

  auto shdr = [&](unsigned Idx, uint32_t Name, uint32_t Type, uint32_t Flags,
                  uint64_t Addr, uint64_t Off, uint64_t Size, uint32_t Align) {
    const uint64_t H = ShOff + Idx * kShEnt;
    putU32(B, H + 0, Name);
    putU32(B, H + 4, Type);
    putU32(B, H + 8, Flags);
    putU32(B, H + 12, static_cast<uint32_t>(Addr));
    putU32(B, H + 16, static_cast<uint32_t>(Off));
    putU32(B, H + 20, static_cast<uint32_t>(Size));
    putU32(B, H + 32, Align);
  };
  shdr(0, 0, llvm::ELF::SHT_NULL, 0, 0, 0, 0, 0);
  shdr(1, kNameText, llvm::ELF::SHT_PROGBITS,
       llvm::ELF::SHF_ALLOC | llvm::ELF::SHF_EXECINSTR, kTextVA, kTextOff,
       kTextSize, 4);
  shdr(2, Opts.WithTable ? kNameTab : kNameRodata, llvm::ELF::SHT_PROGBITS,
       llvm::ELF::SHF_ALLOC, kTabVA, kTabOff, kTabSize, 4);
  shdr(3, kNameStr, llvm::ELF::SHT_STRTAB, 0, 0, StrOff, kStrTabSize, 1);
  shdr(4, kNameIdx, llvm::ELF::SHT_ARM_EXIDX, llvm::ELF::SHF_ALLOC, kIdxVA,
       kIdxOff, kIdxSize, 4);
  return B;
}

uint64_t sectionSizeOff(const std::vector<uint8_t> &B, unsigned Idx) {
  return getU32(B, 32) + Idx * kShEnt + 20;
}

std::unique_ptr<llvm::Module> makeModule(llvm::LLVMContext &C, bool WithEH) {
  auto M = std::make_unique<llvm::Module>("m", C);
  auto *FT = llvm::FunctionType::get(llvm::Type::getVoidTy(C), false);
  auto *F = llvm::Function::Create(FT, llvm::GlobalValue::ExternalLinkage, "f",
                                   M.get());
  llvm::IRBuilder<> B(llvm::BasicBlock::Create(C, "entry", F));
  B.CreateRetVoid();
  if (WithEH) {
    auto *PT = llvm::FunctionType::get(llvm::Type::getInt32Ty(C), true);
    auto *P = llvm::Function::Create(PT, llvm::GlobalValue::ExternalLinkage,
                                     "__gxx_personality_v0", M.get());
    F->setPersonalityFn(P);
  }
  return M;
}

const CompiledSection *findCompiledSection(const CompiledImage &Compiled,
                                           llvm::StringRef Prefix) {
  for (const CompiledSection &Section : Compiled.Sections)
    if (llvm::StringRef(Section.Name).starts_with(Prefix))
      return &Section;
  return nullptr;
}

llvm::ArrayRef<uint8_t> sectionBytes(const CompiledImage &Compiled,
                                     const CompiledSection &Section) {
  if (!Section.IsInImage)
    return Section.ExternalBytes;
  if (Section.Offset > Compiled.Bytes.size() ||
      Section.Size > Compiled.Bytes.size() - Section.Offset)
    return {};
  return llvm::ArrayRef<uint8_t>(Compiled.Bytes)
      .slice(static_cast<size_t>(Section.Offset),
             static_cast<size_t>(Section.Size));
}

CompiledImage compileWithFixedIndexVA(llvm::LLVMContext &C, uint64_t TextVA,
                                      uint64_t IndexVA) {
  auto M = makeModule(C, /*WithEH=*/false);
  llvm::Function *F = M->getFunction("f");
  if (!F)
    return {};
  F->setUWTableKind(llvm::UWTableKind::Default);

  auto Resolve = [](llvm::StringRef, uint32_t) -> std::optional<uint64_t> {
    return std::nullopt;
  };
  auto FixedIndex = [IndexVA](llvm::StringRef Name) -> std::optional<uint64_t> {
    if (Name.starts_with(".ARM.exidx"))
      return IndexVA;
    return std::nullopt;
  };
  return compileImageForPatchWithFixedSectionVAs(
      *M, Arch::ARM, BinaryFormat::ELF, TextVA, Resolve, FixedIndex);
}

bool hadError(llvm::Error E) {
  const bool Failed = static_cast<bool>(E);
  llvm::consumeError(std::move(E));
  return Failed;
}

std::string errorText(llvm::Error E) { return llvm::toString(std::move(E)); }

// `pop {r4,r5,lr}`, `vpush {d8,d9}`, a frame too large for the short codes,
// and the `finish` that ends every program.
std::vector<UnwindOperation> makeOperations() {
  std::vector<UnwindOperation> Ops;

  UnwindOperation Pop;
  Pop.Kind = UnwindOperationKind::SaveRegisterPairPreIndexed;
  Pop.RegisterClass = UnwindRegisterClass::GeneralPurpose;
  Pop.RegisterMask = (1u << 4) | (1u << 5) | (1u << 14);
  Pop.Register = 4;
  Pop.StackOffset = 12;
  Ops.push_back(Pop);

  UnwindOperation Vfp;
  Vfp.Kind = UnwindOperationKind::SaveRegisterPairPreIndexed;
  Vfp.RegisterClass = UnwindRegisterClass::FloatingPoint;
  Vfp.RegisterMask = (1u << 8) | (1u << 9);
  Vfp.Register = 8;
  Vfp.StackOffset = 16;
  Ops.push_back(Vfp);

  UnwindOperation Alloc;
  Alloc.Kind = UnwindOperationKind::AllocateStack;
  Alloc.StackOffset = 0x210;
  Ops.push_back(Alloc);

  UnwindOperation Finish;
  Finish.Kind = UnwindOperationKind::End;
  Ops.push_back(Finish);
  return Ops;
}

ELFARMEHABIRecord makeGenericRecord(uint64_t FunctionVA,
                                    std::vector<uint8_t> Opcodes) {
  ELFARMEHABIRecord Record;
  Record.FunctionVA = FunctionVA;
  Record.Model = ELFARMEHABIModel::Generic;
  Record.PersonalityVA = kPersonalityVA;
  Record.Opcodes = std::move(Opcodes);
  Record.HandlerData = {0xDE, 0xAD, 0xBE, 0xEF};
  return Record;
}

TEST(ELFARMEHABIPatch, FindsIndexAndTable) {
  const std::vector<uint8_t> Bin = makeARMELFWithEHABI();
  auto Region = findELFARMEHABIRegion(Bin);
  ASSERT_TRUE(Region.has_value());
  EXPECT_EQ(Region->IndexVA, kIdxVA);
  EXPECT_EQ(Region->IndexFileOff, kIdxOff);
  EXPECT_EQ(Region->IndexSize, kIdxSize);
  EXPECT_EQ(Region->ExidxPhdrOff, kPhOff + kPhEnt);
  // The index may grow up to the section laid out after it.
  EXPECT_EQ(Region->IndexLimitFileOff, kIdxOff + kIdxSize + 240);

  ASSERT_TRUE(Region->HasTable);
  EXPECT_EQ(Region->TableVA, kTabVA);
  EXPECT_EQ(Region->TableFileOff, kTabOff);
  EXPECT_EQ(Region->TableSize, kTabSize);
  EXPECT_EQ(Region->TableLimitFileOff, kIdxOff);
}

TEST(ELFARMEHABIPatch, RejectsImagesTheRewriteDoesNotModel) {
  // Another machine's processor-specific section is not an index.
  std::vector<uint8_t> WrongMachine = makeARMELFWithEHABI();
  putU16(WrongMachine, 18, llvm::ELF::EM_386);
  EXPECT_FALSE(findELFARMEHABIRegion(WrongMachine).has_value());

  // EHABI is defined for 32-bit ARM alone.
  std::vector<uint8_t> Wrong64 = makeARMELFWithEHABI();
  Wrong64[llvm::ELF::EI_CLASS] = llvm::ELF::ELFCLASS64;
  EXPECT_FALSE(findELFARMEHABIRegion(Wrong64).has_value());

  // An index the program headers do not publish is one the unwinder cannot
  // find, however correctly it is written.
  std::vector<uint8_t> NoPhdr = makeARMELFWithEHABI();
  putU32(NoPhdr, kPhOff + kPhEnt, llvm::ELF::PT_NULL);
  EXPECT_FALSE(findELFARMEHABIRegion(NoPhdr).has_value());

  // A truncated buffer declares an index it does not carry.
  const std::vector<uint8_t> Whole = makeARMELFWithEHABI();
  const std::vector<uint8_t> Short(Whole.begin(), Whole.begin() + kIdxOff);
  EXPECT_FALSE(findELFARMEHABIRegion(Short).has_value());

  // Every header is bounds-checked against the entry size the file declares
  // but read as a whole structure, so a file declaring a shorter entry than
  // it is read as would be read past its own end.
  std::vector<uint8_t> ShortSections = makeARMELFWithEHABI();
  putU16(ShortSections, 46, 8); // e_shentsize
  EXPECT_FALSE(findELFARMEHABIRegion(ShortSections).has_value());

  std::vector<uint8_t> ShortSegments = makeARMELFWithEHABI();
  putU16(ShortSegments, 42, 8); // e_phentsize
  EXPECT_FALSE(findELFARMEHABIRegion(ShortSegments).has_value());

  // A section named exactly `.ARM.extab` is part of the EHABI identity.  If
  // it is malformed, treating it as if the image simply had no table would
  // hide corrupted unwind state from an inline-only rewrite.
  const uint64_t TableHeader = getU32(Whole, 32) + 2 * kShEnt;
  std::vector<uint8_t> WrongTableType = makeARMELFWithEHABI();
  putU32(WrongTableType, TableHeader + 4, llvm::ELF::SHT_NOBITS);
  EXPECT_FALSE(findELFARMEHABIRegion(WrongTableType).has_value());

  std::vector<uint8_t> UnallocatedTable = makeARMELFWithEHABI();
  putU32(UnallocatedTable, TableHeader + 8, 0);
  EXPECT_FALSE(findELFARMEHABIRegion(UnallocatedTable).has_value());

  std::vector<uint8_t> MismappedTable = makeARMELFWithEHABI();
  putU32(MismappedTable, TableHeader + 12, static_cast<uint32_t>(kTabVA + 4));
  EXPECT_FALSE(findELFARMEHABIRegion(MismappedTable).has_value());

  // An image with no index at all leaves nothing to register in.
  const std::vector<uint8_t> Empty;
  EXPECT_FALSE(findELFARMEHABIRegion(Empty).has_value());
}

TEST(ELFARMEHABIPatch, LeavesImageAloneWithoutTableWhenModelNeedsOne) {
  ELFOptions Opts;
  Opts.WithTable = false;
  std::vector<uint8_t> Bin = makeARMELFWithEHABI(Opts);
  const std::vector<uint8_t> Before = Bin;

  auto Region = findELFARMEHABIRegion(Bin);
  ASSERT_TRUE(Region.has_value());
  EXPECT_FALSE(Region->HasTable);

  const ELFARMEHABIRecord Record =
      makeGenericRecord(kFunc1VA, {0xA9, kFinish, kFinish});
  const std::string Text =
      errorText(installELFARMEHABIRecords(Bin, *Region, Record));
  EXPECT_NE(Text.find(".ARM.extab"), std::string::npos) << Text;
  // A rejected install leaves the image exactly as it was.
  EXPECT_EQ(Bin, Before);
}

TEST(ELFARMEHABIPatch, EncodesUnwindOperationsAsOpcodes) {
  auto Encoded = encodeARMEHABIUnwindOpcodes(makeOperations());
  ASSERT_TRUE(static_cast<bool>(Encoded)) << errorText(Encoded.takeError());
  const std::vector<uint8_t> Expected = {
      0xA9,          // pop {r4,r5,lr}
      0xD1,          // pop {d8,d9} with no spare word
      0xB2,    0x03, // vsp += 0x204 + (3 << 2)
      kFinish,
  };
  EXPECT_EQ(*Encoded, Expected);
}

TEST(ELFARMEHABIPatch, RefusesOperationsNoOpcodeExpresses) {
  std::vector<UnwindOperation> Ops;
  UnwindOperation Signed;
  // Pointer authentication is an ARM64 notion; EHABI has no code for it, and
  // an unwind program that is nearly right faults where none at all refuses.
  Signed.Kind = UnwindOperationKind::SignReturnAddress;
  Ops.push_back(Signed);
  UnwindOperation End;
  End.Kind = UnwindOperationKind::End;
  Ops.push_back(End);
  auto Encoded = encodeARMEHABIUnwindOpcodes(Ops);
  EXPECT_FALSE(static_cast<bool>(Encoded));
  llvm::consumeError(Encoded.takeError());

  // A stack adjustment that is not a whole number of words has no encoding
  // either, because every code counts words.
  std::vector<UnwindOperation> Unaligned;
  UnwindOperation Alloc;
  Alloc.Kind = UnwindOperationKind::AllocateStack;
  Alloc.StackOffset = 6;
  Unaligned.push_back(Alloc);
  Unaligned.push_back(End);
  auto Bad = encodeARMEHABIUnwindOpcodes(Unaligned);
  EXPECT_FALSE(static_cast<bool>(Bad));
  llvm::consumeError(Bad.takeError());

  // A shrinking adjustment uses repeated short opcodes.  Reject a value that
  // cannot fit any descriptor before iterating once per 256-byte chunk.
  std::vector<UnwindOperation> Enormous;
  UnwindOperation Shrink;
  Shrink.Kind = UnwindOperationKind::DeallocateStack;
  Shrink.StackOffset = std::numeric_limits<uint64_t>::max() - 3;
  Enormous.push_back(Shrink);
  Enormous.push_back(End);
  auto EnormousEncoded = encodeARMEHABIUnwindOpcodes(Enormous);
  EXPECT_FALSE(static_cast<bool>(EnormousEncoded));
  llvm::consumeError(EnormousEncoded.takeError());
}

TEST(ELFARMEHABIPatch, RejectsInconsistentNormalizedUnwindOperations) {
  auto Rejects = [](std::vector<UnwindOperation> Operations) {
    UnwindOperation End;
    End.Kind = UnwindOperationKind::End;
    Operations.push_back(End);
    auto Encoded = encodeARMEHABIUnwindOpcodes(Operations);
    EXPECT_FALSE(static_cast<bool>(Encoded));
    llvm::consumeError(Encoded.takeError());
  };

  UnwindOperation WrongStackMove;
  WrongStackMove.Kind = UnwindOperationKind::SaveRegisterPairPreIndexed;
  WrongStackMove.RegisterClass = UnwindRegisterClass::GeneralPurpose;
  WrongStackMove.Register = 4;
  WrongStackMove.RegisterMask = (1u << 4) | (1u << 5);
  WrongStackMove.StackOffset = 4;
  Rejects({WrongStackMove});

  UnwindOperation WrongKind = WrongStackMove;
  WrongKind.Kind = UnwindOperationKind::SaveRegisterPreIndexed;
  WrongKind.StackOffset = 8;
  Rejects({WrongKind});

  UnwindOperation WrongLowest = WrongStackMove;
  WrongLowest.Register = 5;
  WrongLowest.StackOffset = 8;
  Rejects({WrongLowest});

  UnwindOperation WrongClass;
  WrongClass.Kind = UnwindOperationKind::SetStackPointerFromRegister;
  WrongClass.RegisterClass = UnwindRegisterClass::FloatingPoint;
  WrongClass.Register = 4;
  WrongClass.RegisterMask = 1u << 4;
  Rejects({WrongClass});

  UnwindOperation WrongMask = WrongClass;
  WrongMask.RegisterClass = UnwindRegisterClass::GeneralPurpose;
  WrongMask.RegisterMask = 1u << 5;
  Rejects({WrongMask});
}

TEST(ELFARMEHABIPatch, RequiresExplicitFinishAtAnExactWordBoundary) {
  std::vector<UnwindOperation> Operations(3);
  for (UnwindOperation &Operation : Operations) {
    Operation.Kind = UnwindOperationKind::AllocateStack;
    Operation.StackOffset = 4;
  }
  auto Encoded = encodeARMEHABIUnwindOpcodes(Operations);
  EXPECT_FALSE(static_cast<bool>(Encoded));
  llvm::consumeError(Encoded.takeError());

  UnwindOperation End;
  End.Kind = UnwindOperationKind::End;
  Operations.push_back(End);
  Encoded = encodeARMEHABIUnwindOpcodes(Operations);
  ASSERT_TRUE(static_cast<bool>(Encoded)) << errorText(Encoded.takeError());
  ASSERT_EQ(Encoded->size(), 4u);
  EXPECT_EQ(Encoded->back(), kFinish);
}

TEST(ELFARMEHABIPatch, InsertsEntryAndKeepsIndexSorted) {
  std::vector<uint8_t> Bin = makeARMELFWithEHABI();
  auto Region = findELFARMEHABIRegion(Bin);
  ASSERT_TRUE(Region.has_value());

  auto Opcodes = encodeARMEHABIUnwindOpcodes(makeOperations());
  ASSERT_TRUE(static_cast<bool>(Opcodes)) << errorText(Opcodes.takeError());
  const ELFARMEHABIRecord Record = makeGenericRecord(kFunc1VA, *Opcodes);
  ASSERT_FALSE(hadError(installELFARMEHABIRecords(Bin, *Region, Record)));

  // Three entries now, in address order, each function address recovered from
  // a `prel31` measured against the entry's own new position.
  const uint64_t Expected[] = {kFunc0VA, kFunc1VA, kFunc2VA};
  for (unsigned I = 0; I < 3; ++I) {
    const uint64_t EntryVA = kIdxVA + I * 8;
    EXPECT_EQ(prel31Target(getU32(Bin, kIdxOff + I * 8), EntryVA), Expected[I])
        << "entry " << I;
  }

  // The entry that was already there kept its meaning: still "cannot unwind".
  EXPECT_EQ(getU32(Bin, kIdxOff + 4), kCantUnwind);

  // The displaced entry moved by one slot, so its descriptor `prel31` had to
  // be recomputed to keep naming the same descriptor.
  EXPECT_EQ(prel31Target(getU32(Bin, kIdxOff + 20), kIdxVA + 20), kTabVA);

  // The index and the segment that publishes it both grew to three entries.
  EXPECT_EQ(getU32(Bin, sectionSizeOff(Bin, 4)), 24u);
  EXPECT_EQ(getU32(Bin, kPhOff + kPhEnt + 16), 24u); // p_filesz
  EXPECT_EQ(getU32(Bin, kPhOff + kPhEnt + 20), 24u); // p_memsz
}

TEST(ELFARMEHABIPatch, AppendsWellFormedGenericDescriptor) {
  std::vector<uint8_t> Bin = makeARMELFWithEHABI();
  auto Region = findELFARMEHABIRegion(Bin);
  ASSERT_TRUE(Region.has_value());

  auto Opcodes = encodeARMEHABIUnwindOpcodes(makeOperations());
  ASSERT_TRUE(static_cast<bool>(Opcodes)) << errorText(Opcodes.takeError());
  const ELFARMEHABIRecord Record = makeGenericRecord(kFunc1VA, *Opcodes);
  ASSERT_FALSE(hadError(installELFARMEHABIRecords(Bin, *Region, Record)));

  // The new entry names a descriptor placed just past the table the image
  // shipped, word aligned as every descriptor must be.
  const uint64_t DescriptorVA =
      prel31Target(getU32(Bin, kIdxOff + 12), kIdxVA + 12);
  EXPECT_EQ(DescriptorVA, kTabVA + kTabSize);
  EXPECT_EQ(DescriptorVA % 4, 0u);
  const uint64_t DescriptorOff = kTabOff + (DescriptorVA - kTabVA);

  // First word: the personality routine, named by a `prel31` measured from the
  // descriptor's own address.
  EXPECT_EQ(prel31Target(getU32(Bin, DescriptorOff), DescriptorVA),
            kPersonalityVA);
  // Its top bit stays clear, which is what tells the reader it is a generic
  // descriptor rather than a compact one.
  EXPECT_EQ(getU32(Bin, DescriptorOff) & kCompactBit, 0u);

  // Second word: one opcode word past the header, then the first three opcode
  // bytes, most significant first.
  const uint32_t Header = getU32(Bin, DescriptorOff + 4);
  EXPECT_EQ(Header >> 24, 1u);
  EXPECT_EQ((Header >> 16) & 0xFF, (*Opcodes)[0]);
  EXPECT_EQ((Header >> 8) & 0xFF, (*Opcodes)[1]);
  EXPECT_EQ(Header & 0xFF, (*Opcodes)[2]);

  // Third word: the two opcode bytes that spilled, padded out with `finish`.
  const uint32_t Spill = getU32(Bin, DescriptorOff + 8);
  EXPECT_EQ(Spill >> 24, (*Opcodes)[3]);
  EXPECT_EQ((Spill >> 16) & 0xFF, (*Opcodes)[4]);
  EXPECT_EQ((Spill >> 8) & 0xFF, kFinish);
  EXPECT_EQ(Spill & 0xFF, kFinish);

  // The personality routine's own data follows the opcodes verbatim.
  EXPECT_EQ(getU32(Bin, DescriptorOff + 12), 0xEFBEADDEu);

  // The descriptor the image already carried was not disturbed.
  EXPECT_EQ(prel31Target(getU32(Bin, kTabOff), kTabVA), kPersonalityVA);
  // `.ARM.extab` grew by exactly the appended descriptor.
  EXPECT_EQ(getU32(Bin, sectionSizeOff(Bin, 2)), kTabSize + 16);
}

TEST(ELFARMEHABIPatch, ReplacesEntryForAFunctionAlreadyDescribed) {
  std::vector<uint8_t> Bin = makeARMELFWithEHABI();
  auto Region = findELFARMEHABIRegion(Bin);
  ASSERT_TRUE(Region.has_value());

  // Rewritten where it stands, the first function gains an inline description
  // in place of the "cannot unwind" the image shipped.
  ELFARMEHABIRecord Record;
  Record.FunctionVA = kFunc0VA;
  Record.Model = ELFARMEHABIModel::Inline;
  Record.Opcodes = {0xA9, kFinish, kFinish};
  ASSERT_FALSE(hadError(installELFARMEHABIRecords(Bin, *Region, Record)));

  // Still two entries: the record replaced rather than duplicated.
  EXPECT_EQ(getU32(Bin, sectionSizeOff(Bin, 4)), kIdxSize);
  EXPECT_EQ(prel31Target(getU32(Bin, kIdxOff), kIdxVA), kFunc0VA);
  const uint32_t Word = getU32(Bin, kIdxOff + 4);
  EXPECT_EQ(Word, kCompactBit | (uint32_t(0xA9) << 16) |
                      (uint32_t(kFinish) << 8) | kFinish);
}

TEST(ELFARMEHABIPatch, RejectsMalformedInputIndexWithoutChangingBytes) {
  ELFARMEHABIRecord Replacement;
  Replacement.FunctionVA = kFunc0VA;
  Replacement.Model = ELFARMEHABIModel::CantUnwind;

  auto RejectsUnchanged = [&](std::vector<uint8_t> Bin) {
    const std::vector<uint8_t> Before = Bin;
    auto Region = findELFARMEHABIRegion(Bin);
    ASSERT_TRUE(Region.has_value());
    EXPECT_TRUE(hadError(installELFARMEHABIRecords(Bin, *Region, Replacement)));
    EXPECT_EQ(Bin, Before);
  };

  // Only personality routine zero fits in an inline index word.
  std::vector<uint8_t> InlinePersonality = makeARMELFWithEHABI();
  putU32(InlinePersonality, kIdxOff + 4, kCompactBit | (1u << 24));
  RejectsUnchanged(std::move(InlinePersonality));

  // Vendor bits other than zero have no ARM-defined interpretation.
  std::vector<uint8_t> InlineVendor = makeARMELFWithEHABI();
  putU32(InlineVendor, kIdxOff + 4, kCompactBit | 0x10000000u);
  RejectsUnchanged(std::move(InlineVendor));

  // An inline opcode program must contain a semantic finish.  A finish byte
  // used as the operand of a two-byte opcode does not terminate the program.
  std::vector<uint8_t> InlineWithoutFinish = makeARMELFWithEHABI();
  putU32(InlineWithoutFinish, kIdxOff + 4,
         kCompactBit | (uint32_t(0x80) << 16) | (uint32_t(kFinish) << 8) |
             0x01);
  RejectsUnchanged(std::move(InlineWithoutFinish));

  // An out-of-line word must resolve inside the image's one allocated extab.
  std::vector<uint8_t> OutsideTable = makeARMELFWithEHABI();
  putU32(OutsideTable, kIdxOff + 12, prel31From(kIdxVA + 12, kTextVA));
  RejectsUnchanged(std::move(OutsideTable));

  // A generic descriptor needs a resolved personality and every opcode word
  // its header count claims.
  std::vector<uint8_t> UnresolvedPersonality = makeARMELFWithEHABI();
  putU32(UnresolvedPersonality, kTabOff, 0);
  RejectsUnchanged(std::move(UnresolvedPersonality));

  std::vector<uint8_t> TruncatedProgram = makeARMELFWithEHABI();
  putU32(TruncatedProgram, kTabOff + 4, 4u << 24);
  RejectsUnchanged(std::move(TruncatedProgram));

  std::vector<uint8_t> DescriptorWithoutFinish = makeARMELFWithEHABI();
  putU32(DescriptorWithoutFinish, kTabOff + 4,
         (uint32_t(0x80) << 16) | (uint32_t(kFinish) << 8) | 0x01);
  RejectsUnchanged(std::move(DescriptorWithoutFinish));

  // Equal function starts make the binary-search index ambiguous.
  std::vector<uint8_t> DuplicateFunction = makeARMELFWithEHABI();
  putU32(DuplicateFunction, kIdxOff + 8, prel31From(kIdxVA + 8, kFunc0VA));
  RejectsUnchanged(std::move(DuplicateFunction));
}

TEST(ELFARMEHABIPatch, RejectsForgedPublicRegionWithoutChangingBytes) {
  std::vector<uint8_t> Bin = makeARMELFWithEHABI();
  auto Region = findELFARMEHABIRegion(Bin);
  ASSERT_TRUE(Region.has_value());
  const std::vector<uint8_t> Before = Bin;

  // A caller-provided growth limit is security-sensitive: accepting a larger
  // value would let the rewritten index overwrite the following section.
  ++Region->IndexLimitFileOff;
  ELFARMEHABIRecord Record;
  Record.FunctionVA = kFunc1VA;
  Record.Model = ELFARMEHABIModel::CantUnwind;
  EXPECT_TRUE(hadError(installELFARMEHABIRecords(Bin, *Region, Record)));
  EXPECT_EQ(Bin, Before);
}

TEST(ELFARMEHABIPatch, RegistersEntriesACompileAlreadyPlaced) {
  llvm::LLVMContext C;
  std::vector<uint8_t> Bin = makeARMELFWithEHABI();
  auto Region = findELFARMEHABIRegion(Bin);
  ASSERT_TRUE(Region.has_value());

  // Codegen places its own descriptor in the appended segment, so the entry
  // only has to point at it; what the image has to supply is a place in its
  // own sorted index for the function it describes.
  const uint64_t PlacedDescriptorVA = kAppendedSegmentVA + 0x100;
  CompiledSection Index;
  Index.Name = ".ARM.exidx";
  Index.IsAllocated = true;
  Index.IsInImage = false;
  Index.VA = kAppendedSegmentVA;
  pushU32(Index.ExternalBytes, prel31From(Index.VA, kFunc1VA));
  pushU32(Index.ExternalBytes, prel31From(Index.VA + 4, PlacedDescriptorVA));
  Index.Size = Index.ExternalBytes.size();

  CompiledSection Descriptor;
  Descriptor.Name = ".ARM.extab";
  Descriptor.IsAllocated = true;
  Descriptor.IsInImage = false;
  Descriptor.VA = PlacedDescriptorVA;
  pushU32(Descriptor.ExternalBytes, kCompactBit | (uint32_t(kFinish) << 16) |
                                        (uint32_t(kFinish) << 8) | kFinish);
  Descriptor.Size = Descriptor.ExternalBytes.size();

  // Every generated executable run gets an explicit end marker.  This one
  // ends exactly where the image's next live entry starts, so registration
  // must retain that existing boundary instead of replacing it.
  CompiledSection Code;
  Code.Name = ".text";
  Code.IsAllocated = true;
  Code.IsInImage = false;
  Code.VA = kFunc1VA;
  Code.Size = kFunc2VA - kFunc1VA;
  Code.Kind = llvm::mc_rewrite::RewriteSectionKind::Code;
  Code.ExternalBytes.resize(static_cast<size_t>(Code.Size));

  CompiledImage Img;
  Img.Success = true;
  Img.Sections.push_back(Index);
  Img.Sections.push_back(Descriptor);
  Img.Sections.push_back(Code);
  Img.SymbolAddrs["f"] = kFunc1VA;
  Img.FunctionOwnerAddrs["f"] = kFunc1VA;
  Img.SourceFunctionOwners.push_back({"f", "f", kFunc1VA});
  EXPECT_TRUE(hasGeneratedELFARMEHABI(Img));

  ASSERT_FALSE(hadError(
      installELFARMEHABI(Bin, Region, Img, *makeModule(C, /*WithEH=*/true))));

  EXPECT_EQ(getU32(Bin, sectionSizeOff(Bin, 4)), 24u);
  EXPECT_EQ(prel31Target(getU32(Bin, kIdxOff + 8), kIdxVA + 8), kFunc1VA);
  EXPECT_EQ(prel31Target(getU32(Bin, kIdxOff + 12), kIdxVA + 12),
            PlacedDescriptorVA);
  // Nothing was appended to `.ARM.extab`, whose descriptor rides along in the
  // compile's own segment.
  EXPECT_EQ(getU32(Bin, sectionSizeOff(Bin, 2)), kTabSize);
}

TEST(ELFARMEHABIPatch,
     RejectsGeneratedInlineProgramWithoutFinishWithoutChangingBytes) {
  llvm::LLVMContext C;
  std::vector<uint8_t> Bin = makeARMELFWithEHABI();
  const std::vector<uint8_t> Before = Bin;
  auto Region = findELFARMEHABIRegion(Bin);
  ASSERT_TRUE(Region.has_value());

  CompiledSection Index;
  Index.Name = ".ARM.exidx";
  Index.IsAllocated = true;
  Index.IsInImage = false;
  Index.VA = kAppendedSegmentVA;
  pushU32(Index.ExternalBytes, prel31From(Index.VA, kFunc1VA));
  // 0x80 consumes the following byte, so the B0 here is an operand rather
  // than a finish opcode.  The final 0x01 does not terminate the program.
  pushU32(Index.ExternalBytes, kCompactBit | (uint32_t(0x80) << 16) |
                                   (uint32_t(kFinish) << 8) | 0x01);
  Index.Size = Index.ExternalBytes.size();

  CompiledSection Code;
  Code.Name = ".text";
  Code.IsAllocated = true;
  Code.IsInImage = false;
  Code.VA = kFunc1VA;
  Code.Size = kFunc2VA - kFunc1VA;
  Code.Kind = llvm::mc_rewrite::RewriteSectionKind::Code;
  Code.ExternalBytes.resize(static_cast<size_t>(Code.Size));

  CompiledImage Img;
  Img.Success = true;
  Img.Sections = {Index, Code};
  Img.SymbolAddrs["f"] = kFunc1VA;
  EXPECT_TRUE(hadError(
      installELFARMEHABI(Bin, Region, Img, *makeModule(C, /*WithEH=*/true))));
  EXPECT_EQ(Bin, Before);
}

TEST(ELFARMEHABIPatch,
     RejectsGeneratedFunctionOutsideCodeAndExistingKeysWithoutChangingBytes) {
  llvm::LLVMContext C;
  auto RejectsUnchanged = [&](uint64_t FunctionVA, uint64_t CodeVA,
                              uint64_t CodeSize, unsigned DuplicateCount = 1) {
    std::vector<uint8_t> Bin = makeARMELFWithEHABI();
    const std::vector<uint8_t> Before = Bin;
    auto Region = findELFARMEHABIRegion(Bin);
    ASSERT_TRUE(Region.has_value());

    CompiledSection Index;
    Index.Name = ".ARM.exidx";
    Index.IsAllocated = true;
    Index.IsInImage = false;
    Index.VA = kAppendedSegmentVA;
    for (unsigned I = 0; I < DuplicateCount; ++I) {
      const uint64_t EntryVA = Index.VA + I * kIndexEntrySize;
      pushU32(Index.ExternalBytes, prel31From(EntryVA, FunctionVA));
      pushU32(Index.ExternalBytes, kCompactBit | (uint32_t(kFinish) << 16) |
                                       (uint32_t(kFinish) << 8) | kFinish);
    }
    Index.Size = Index.ExternalBytes.size();

    CompiledSection Code;
    Code.Name = ".text";
    Code.IsAllocated = true;
    Code.IsInImage = false;
    Code.VA = CodeVA;
    Code.Size = CodeSize;
    Code.Kind = llvm::mc_rewrite::RewriteSectionKind::Code;
    Code.ExternalBytes.resize(static_cast<size_t>(Code.Size));

    CompiledImage Img;
    Img.Success = true;
    Img.Sections = {Index, Code};
    Img.SymbolAddrs["f"] = FunctionVA;
    EXPECT_TRUE(hadError(
        installELFARMEHABI(Bin, Region, Img, *makeModule(C, /*WithEH=*/true))));
    EXPECT_EQ(Bin, Before);
  };

  // An index entry cannot register code that the compile did not emit.
  RejectsUnchanged(kFunc1VA, kAppendedSegmentVA + 0x200, 8);
  // Generated fragments are additive.  Replacing an input key is reserved for
  // the explicit in-place record API, where the caller names that intent.
  RejectsUnchanged(kFunc0VA, kFunc0VA, 8);
  // Two generated entries for the same start are ambiguous before merging.
  RejectsUnchanged(kFunc1VA, kFunc1VA, 8, 2);
}

TEST(ELFARMEHABIPatch,
     RejectsGeneratedAddressesOutsideTargetWidthWithoutChangingBytes) {
  llvm::LLVMContext C;
  std::vector<uint8_t> Bin = makeARMELFWithEHABI();
  const std::vector<uint8_t> Before = Bin;
  auto Region = findELFARMEHABIRegion(Bin);
  ASSERT_TRUE(Region.has_value());

  constexpr uint64_t WideVA = uint64_t(1) << 32;
  CompiledSection Index;
  Index.Name = ".ARM.exidx";
  Index.IsAllocated = true;
  Index.IsInImage = false;
  Index.VA = WideVA;
  pushU32(Index.ExternalBytes, prel31From(Index.VA, WideVA + 0x100));
  pushU32(Index.ExternalBytes, kCompactBit | (uint32_t(kFinish) << 16) |
                                   (uint32_t(kFinish) << 8) | kFinish);
  Index.Size = Index.ExternalBytes.size();

  CompiledSection Code;
  Code.Name = ".text";
  Code.IsAllocated = true;
  Code.IsInImage = false;
  Code.VA = WideVA + 0x100;
  Code.Size = 8;
  Code.Kind = llvm::mc_rewrite::RewriteSectionKind::Code;
  Code.ExternalBytes.resize(static_cast<size_t>(Code.Size));

  CompiledImage Img;
  Img.Success = true;
  Img.Sections = {Index, Code};
  Img.SymbolAddrs["f"] = Code.VA;
  EXPECT_TRUE(hadError(
      installELFARMEHABI(Bin, Region, Img, *makeModule(C, /*WithEH=*/true))));
  EXPECT_EQ(Bin, Before);

  // A modulo-2^32 decode is not enough: the same displacement must be
  // representable by the signed PREL31 encoder used when the entry moves into
  // the input index.
  constexpr uint64_t WrappedIndexVA = 0xFFFFFFF0;
  constexpr uint64_t WrappedFunctionVA = 0x10;
  CompiledSection WrappedIndex;
  WrappedIndex.Name = ".ARM.exidx";
  WrappedIndex.IsAllocated = true;
  WrappedIndex.IsInImage = false;
  WrappedIndex.VA = WrappedIndexVA;
  pushU32(WrappedIndex.ExternalBytes,
          prel31From(WrappedIndexVA, WrappedFunctionVA));
  pushU32(WrappedIndex.ExternalBytes, kCompactBit | (uint32_t(kFinish) << 16) |
                                          (uint32_t(kFinish) << 8) | kFinish);
  WrappedIndex.Size = WrappedIndex.ExternalBytes.size();

  CompiledSection WrappedCode;
  WrappedCode.Name = ".text";
  WrappedCode.IsAllocated = true;
  WrappedCode.IsInImage = false;
  WrappedCode.VA = WrappedFunctionVA;
  WrappedCode.Size = 8;
  WrappedCode.Kind = llvm::mc_rewrite::RewriteSectionKind::Code;
  WrappedCode.ExternalBytes.resize(static_cast<size_t>(WrappedCode.Size));

  CompiledImage Wrapped;
  Wrapped.Success = true;
  Wrapped.Sections = {WrappedIndex, WrappedCode};
  Wrapped.SymbolAddrs["f"] = WrappedFunctionVA;
  EXPECT_TRUE(hadError(installELFARMEHABI(Bin, Region, Wrapped,
                                          *makeModule(C, /*WithEH=*/true))));
  EXPECT_EQ(Bin, Before);
}

TEST(ELFARMEHABIPatch, TerminatesEveryGeneratedExecutableRun) {
  llvm::LLVMContext C;
  std::vector<uint8_t> Bin = makeARMELFWithEHABI();
  auto Region = findELFARMEHABIRegion(Bin);
  ASSERT_TRUE(Region.has_value());

  constexpr uint64_t FunctionVA = kAppendedSegmentVA + 0x200;
  constexpr uint64_t FunctionSize = 8;
  CompiledSection Index;
  Index.Name = ".ARM.exidx";
  Index.IsAllocated = true;
  Index.IsInImage = false;
  Index.VA = kAppendedSegmentVA;
  pushU32(Index.ExternalBytes, prel31From(Index.VA, FunctionVA));
  pushU32(Index.ExternalBytes, kCompactBit | (uint32_t(kFinish) << 16) |
                                   (uint32_t(kFinish) << 8) | kFinish);
  Index.Size = Index.ExternalBytes.size();

  CompiledSection Code;
  Code.Name = ".text";
  Code.IsAllocated = true;
  Code.IsInImage = false;
  Code.VA = FunctionVA;
  Code.Size = FunctionSize;
  Code.Kind = llvm::mc_rewrite::RewriteSectionKind::Code;
  Code.ExternalBytes.resize(FunctionSize);

  CompiledImage Img;
  Img.Success = true;
  Img.Sections = {Index, Code};
  Img.SymbolAddrs["f"] = FunctionVA;
  Img.FunctionOwnerAddrs["f"] = FunctionVA;
  Img.SourceFunctionOwners.push_back({"f", "f", FunctionVA});
  ASSERT_FALSE(hadError(
      installELFARMEHABI(Bin, Region, Img, *makeModule(C, /*WithEH=*/true))));

  EXPECT_EQ(getU32(Bin, sectionSizeOff(Bin, 4)), 4 * kIndexEntrySize);
  EXPECT_EQ(prel31Target(getU32(Bin, kIdxOff + 16), kIdxVA + 16), FunctionVA);
  EXPECT_EQ(prel31Target(getU32(Bin, kIdxOff + 24), kIdxVA + 24),
            FunctionVA + FunctionSize);
  EXPECT_EQ(getU32(Bin, kIdxOff + 28), kCantUnwind);
}

TEST(ELFARMEHABIPatch, CodegenResolvesGeneratedIndexAsPrel31) {
  llvm::LLVMContext C;
  auto M = makeModule(C, /*WithEH=*/true);
  llvm::Function *F = M->getFunction("f");
  ASSERT_NE(F, nullptr);
  F->setUWTableKind(llvm::UWTableKind::Default);

  auto Resolve = [](llvm::StringRef Name, uint32_t) -> std::optional<uint64_t> {
    if (Name == "__gxx_personality_v0")
      return kPersonalityVA;
    // The streamer emits an R_ARM_NONE dependency on the compact-model
    // personality at the same offset as the function PREL31. It carries no
    // value, so an absent address-model entry must neither alter the bytes nor
    // report a genuinely unresolved runtime reference.
    return std::nullopt;
  };
  auto NoFixedSection = [](llvm::StringRef) -> std::optional<uint64_t> {
    return std::nullopt;
  };
  CompiledImage Compiled = compileImageForPatchWithFixedSectionVAs(
      *M, Arch::ARM, BinaryFormat::ELF, kAppendedSegmentVA, Resolve,
      NoFixedSection);
  ASSERT_TRUE(Compiled.Success);
  EXPECT_TRUE(Compiled.Unresolved.empty());

  const auto Sym = Compiled.SymbolAddrs.find("f");
  ASSERT_NE(Sym, Compiled.SymbolAddrs.end());
  const CompiledSection *Index = findCompiledSection(Compiled, ".ARM.exidx");
  ASSERT_NE(Index, nullptr);
  ASSERT_TRUE(Index->IsInImage);
  ASSERT_GE(Index->Size, kIndexEntrySize);
  const llvm::ArrayRef<uint8_t> IndexBytes = sectionBytes(Compiled, *Index);
  ASSERT_EQ(IndexBytes.size(), Index->Size);

  const uint32_t FunctionWord = getU32(IndexBytes, 0);
  EXPECT_EQ(FunctionWord & kCompactBit, 0u);
  EXPECT_EQ(prel31Target(FunctionWord, Index->VA), clearThumbBit(Sym->second));

  // The exemption is relocation-specific, not a blanket ARM undefined-symbol
  // escape hatch. A real call target must still reach the unresolved list so
  // required-output policy can fail closed. Compile this scenario from a
  // fresh module: target code generation may mutate a module and is not a
  // reusable analysis operation.
  llvm::LLVMContext MissingContext;
  auto MissingModule = makeModule(MissingContext, /*WithEH=*/true);
  llvm::Function *MissingFunction = MissingModule->getFunction("f");
  ASSERT_NE(MissingFunction, nullptr);
  MissingFunction->setUWTableKind(llvm::UWTableKind::Default);
  llvm::IRBuilder<> Builder(MissingFunction->getEntryBlock().getTerminator());
  llvm::FunctionCallee Missing = MissingModule->getOrInsertFunction(
      "missing_runtime", MissingFunction->getFunctionType());
  Builder.CreateCall(Missing);
  CompiledImage WithMissing = compileImageForPatchWithFixedSectionVAs(
      *MissingModule, Arch::ARM, BinaryFormat::ELF, kAppendedSegmentVA, Resolve,
      NoFixedSection);
  ASSERT_TRUE(WithMissing.Success);
  ASSERT_EQ(WithMissing.Unresolved.size(), 1u);
  EXPECT_EQ(WithMissing.Unresolved.front(), "missing_runtime");
}

TEST(ELFARMEHABIPatch, CodegenAcceptsPrel31SignedRangeEndpoints) {
  struct Case {
    uint64_t TextVA;
    uint64_t IndexVA;
    uint32_t EncodedDelta;
  };
  const Case Cases[] = {
      // The patch image aligns allocated sections to 16 bytes, so this is the
      // greatest positive endpoint that exact public layout can request.
      {0x4ffffff0, 0x10000000, 0x3ffffff0},
      // The negative endpoint itself is word aligned and therefore exact.
      {0x10000000, 0x50000000, 0x40000000},
  };

  for (const Case &Current : Cases) {
    SCOPED_TRACE(::testing::Message() << "text=0x" << std::hex << Current.TextVA
                                      << " index=0x" << Current.IndexVA);
    llvm::LLVMContext C;
    CompiledImage Compiled =
        compileWithFixedIndexVA(C, Current.TextVA, Current.IndexVA);
    ASSERT_TRUE(Compiled.Success);
    const auto Sym = Compiled.SymbolAddrs.find("f");
    ASSERT_NE(Sym, Compiled.SymbolAddrs.end());
    const CompiledSection *Index = findCompiledSection(Compiled, ".ARM.exidx");
    ASSERT_NE(Index, nullptr);
    EXPECT_FALSE(Index->IsInImage);
    EXPECT_EQ(Index->VA, Current.IndexVA);
    const llvm::ArrayRef<uint8_t> Bytes = sectionBytes(Compiled, *Index);
    ASSERT_GE(Bytes.size(), kIndexEntrySize);
    const uint32_t FunctionWord = getU32(Bytes, 0);
    EXPECT_EQ(FunctionWord, Current.EncodedDelta);
    EXPECT_EQ(prel31Target(FunctionWord, Index->VA), Sym->second);
  }
}

TEST(ELFARMEHABIPatch, CodegenRejectsPrel31OutsideSignedRange) {
  const std::pair<uint64_t, uint64_t> Cases[] = {
      // Exact +2^30 is the first value a signed 31-bit displacement cannot
      // represent.
      {0x50000000, 0x10000000},
      // One aligned word below -2^30 crosses the negative boundary.
      {0x10000000, 0x50000010},
  };

  for (const auto &[TextVA, IndexVA] : Cases) {
    SCOPED_TRACE(::testing::Message()
                 << "text=0x" << std::hex << TextVA << " index=0x" << IndexVA);
    llvm::LLVMContext C;
    const CompiledImage Compiled = compileWithFixedIndexVA(C, TextVA, IndexVA);
    EXPECT_FALSE(Compiled.Success);
    EXPECT_TRUE(Compiled.Sections.empty());
    EXPECT_TRUE(Compiled.Bytes.empty());
  }
}

TEST(ELFARMEHABIPatch, FailsClosedOnlyWhenRegistrationIsRequired) {
  llvm::LLVMContext C;
  CompiledImage Empty;
  Empty.Success = true;
  EXPECT_FALSE(hasGeneratedELFARMEHABI(Empty));

  std::vector<uint8_t> Bin = makeARMELFWithEHABI();
  // A module that needs registered unwind information, with no index produced
  // to register, must be rejected rather than written unregistered.
  EXPECT_TRUE(
      hadError(installELFARMEHABI(Bin, findELFARMEHABIRegion(Bin), Empty,
                                  *makeModule(C, /*WithEH=*/true))));
  // A module with no exception contract is no worse off keeping the index the
  // image already had.
  EXPECT_FALSE(
      hadError(installELFARMEHABI(Bin, findELFARMEHABIRegion(Bin), Empty,
                                  *makeModule(C, /*WithEH=*/false))));

  // An image with no index at all cannot register one that was produced.
  CompiledSection Index;
  Index.Name = ".ARM.exidx";
  Index.IsAllocated = true;
  Index.IsInImage = false;
  Index.VA = kAppendedSegmentVA;
  pushU32(Index.ExternalBytes, prel31From(Index.VA, kFunc1VA));
  pushU32(Index.ExternalBytes, kCantUnwind);
  Index.Size = Index.ExternalBytes.size();
  CompiledImage Img;
  Img.Success = true;
  Img.Sections.push_back(Index);
  EXPECT_TRUE(hadError(installELFARMEHABI(Bin, std::nullopt, Img,
                                          *makeModule(C, /*WithEH=*/true))));
}

TEST(ELFARMEHABIPatch, FailsClosedWhenTheGrownIndexHasNoSlack) {
  ELFOptions Opts;
  Opts.IndexSlack = 0;
  std::vector<uint8_t> Bin = makeARMELFWithEHABI(Opts);
  const std::vector<uint8_t> Before = Bin;
  auto Region = findELFARMEHABIRegion(Bin);
  ASSERT_TRUE(Region.has_value());
  EXPECT_EQ(Region->IndexLimitFileOff, kIdxOff + kIdxSize);

  ELFARMEHABIRecord Record;
  Record.FunctionVA = kFunc1VA;
  Record.Model = ELFARMEHABIModel::CantUnwind;
  const std::string Text =
      errorText(installELFARMEHABIRecords(Bin, *Region, Record));
  EXPECT_NE(Text.find("slack"), std::string::npos) << Text;
  EXPECT_EQ(Bin, Before);

  // Replacing an entry needs no room at all, so it still goes through.
  Record.FunctionVA = kFunc0VA;
  EXPECT_FALSE(hadError(installELFARMEHABIRecords(Bin, *Region, Record)));
}

TEST(ELFARMEHABIPatch, FailsClosedOnDescriptionsTheEncodingCannotHold) {
  std::vector<uint8_t> Bin = makeARMELFWithEHABI();
  auto Region = findELFARMEHABIRegion(Bin);
  ASSERT_TRUE(Region.has_value());

  // Only 0, 1, and 2 name a routine ARM defined.
  ELFARMEHABIRecord Unknown;
  Unknown.FunctionVA = kFunc1VA;
  Unknown.Model = ELFARMEHABIModel::Compact;
  Unknown.PersonalityIndex = 3;
  std::string Text =
      errorText(installELFARMEHABIRecords(Bin, *Region, Unknown));
  EXPECT_NE(Text.find("personality routine index"), std::string::npos) << Text;

  // An index word holds three opcode bytes and nothing else, so a personality
  // routine's own data cannot ride along in one.
  ELFARMEHABIRecord Overfull;
  Overfull.FunctionVA = kFunc1VA;
  Overfull.Model = ELFARMEHABIModel::Inline;
  Overfull.Opcodes = {0xA9, kFinish, kFinish};
  Overfull.HandlerData = {0x01, 0x02, 0x03, 0x04};
  EXPECT_TRUE(hadError(installELFARMEHABIRecords(Bin, *Region, Overfull)));

  // Routine 0's whole descriptor is the three opcode bytes beside the index.
  ELFARMEHABIRecord TooManyOpcodes;
  TooManyOpcodes.FunctionVA = kFunc1VA;
  TooManyOpcodes.Model = ELFARMEHABIModel::Inline;
  TooManyOpcodes.Opcodes = {0xA9, 0xD1, 0xB2, 0x03, kFinish};
  EXPECT_TRUE(
      hadError(installELFARMEHABIRecords(Bin, *Region, TooManyOpcodes)));

  // A generic descriptor that names no personality routine reaches nothing.
  ELFARMEHABIRecord NoPersonality;
  NoPersonality.FunctionVA = kFunc1VA;
  NoPersonality.Model = ELFARMEHABIModel::Generic;
  NoPersonality.Opcodes = {kFinish};
  EXPECT_TRUE(hadError(installELFARMEHABIRecords(Bin, *Region, NoPersonality)));

  // Two records for one function leave the index ambiguous.
  ELFARMEHABIRecord First;
  First.FunctionVA = kFunc1VA;
  First.Model = ELFARMEHABIModel::CantUnwind;
  const ELFARMEHABIRecord Both[] = {First, First};
  Text = errorText(installELFARMEHABIRecords(Bin, *Region, Both));
  EXPECT_NE(Text.find("two records"), std::string::npos) << Text;
}

TEST(ELFARMEHABIPatch, ClearsTheThumbBitTheIndexNeverCarries) {
  std::vector<uint8_t> Bin = makeARMELFWithEHABI();
  auto Region = findELFARMEHABIRegion(Bin);
  ASSERT_TRUE(Region.has_value());

  // The table is searched by program counter, which never has the Thumb bit
  // set, so a Thumb function's entry has to name its address without it.
  ELFARMEHABIRecord Record;
  Record.FunctionVA = kFunc1VA | 1;
  Record.Model = ELFARMEHABIModel::CantUnwind;
  ASSERT_FALSE(hadError(installELFARMEHABIRecords(Bin, *Region, Record)));
  EXPECT_EQ(prel31Target(getU32(Bin, kIdxOff + 8), kIdxVA + 8), kFunc1VA);
}

} // namespace
