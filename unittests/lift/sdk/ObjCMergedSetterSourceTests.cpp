#include "../../../lib/sdk/capi/ObjCMergedSetterSources.h"
#include "../core/MappedSectionFlags.h"
#include "../core/SourceCallExecution.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/lift/AArch64/AArch64Regs.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Program.h"

#include <cstring>
#include <filesystem>
#include <fstream>

using namespace neverd;
using namespace neverd::sdk;

namespace {
struct SetterFixture {
  static constexpr va_t Root = 0x1000, OtherRoot = 0x1020;
  static constexpr va_t Helper = 0x1100, OtherHelper = 0x1300;
  static constexpr va_t Accessor = 0x1500, Metadata = 0x2800;
  static constexpr va_t Profile = 0x2400, Selector = 0x2500, Mask = 0x2f20;
  static constexpr va_t LayoutStub = 0x1800, LayoutSelector = 0x2510;
  static constexpr va_t LayoutImport = 0x4180, LayoutName = 0x4100;
  BinaryImage Image;
  llvm::LLVMContext Context;
  PipelineResult Result;

  void word(va_t Address, uint32_t Value) {
    uint8_t Bytes[4];
    llvm::support::endian::write32le(Bytes, Value);
    ASSERT_TRUE(Image.writeVA(Address, Bytes, 4));
  }
  void pointer(va_t Address, uint64_t Value) {
    uint8_t Bytes[8];
    llvm::support::endian::write64le(Bytes, Value);
    ASSERT_TRUE(Image.writeVA(Address, Bytes, 8));
  }
  void string(va_t Address, const std::string &Value) {
    ASSERT_TRUE(Image.writeVA(Address,
                              reinterpret_cast<const uint8_t *>(Value.c_str()),
                              Value.size() + 1));
  }
  static uint32_t branch(va_t Site, va_t Target, bool Link = true) {
    return (Link ? 0x94000000u : 0x14000000u) |
           (uint32_t((int64_t(Target) - int64_t(Site)) / 4) & 0x03ffffff);
  }
  HighFunc &high(va_t Entry) {
    for (auto &F : Result.HighFuncs)
      if (F.Entry == Entry)
        return F;
    throw std::runtime_error("fixture function missing");
  }
  LowFunc &low(va_t Entry) {
    for (auto &F : Result.LowFuncs)
      if (F.Entry == Entry)
        return F;
    throw std::runtime_error("fixture low function missing");
  }
  Section &section(va_t Address) {
    for (auto &S : Image.Sections)
      if (S.contains(Address))
        return S;
    throw std::runtime_error("fixture section missing");
  }
  void fileHeader() {
    using namespace llvm::MachO;
    auto &Bytes = Image.Segments[0].Data;
    auto *Header = reinterpret_cast<mach_header_64 *>(Bytes.data());
    Header->magic = MH_MAGIC_64;
    Header->cputype = CPU_TYPE_ARM64;
    Header->filetype = MH_EXECUTE;
    Header->ncmds = 4;
    Header->sizeofcmds = 3 * sizeof(segment_command_64) +
                         Image.Sections.size() * sizeof(section_64) +
                         sizeof(symtab_command);
    size_t Offset = sizeof(*Header);
    for (const auto &S : Image.Segments) {
      auto *C = reinterpret_cast<segment_command_64 *>(Bytes.data() + Offset);
      C->cmd = LC_SEGMENT_64;
      C->nsects = std::count_if(
          Image.Sections.begin(), Image.Sections.end(),
          [&](const auto &Sec) { return Sec.SegmentName == S.Name; });
      C->cmdsize = sizeof(*C) + C->nsects * sizeof(section_64);
      std::strcpy(C->segname, S.Name.c_str());
      C->vmaddr = S.VA;
      C->vmsize = S.Size;
      C->fileoff = S.FileOff;
      C->filesize = S.FileSz;
      C->initprot = C->maxprot = VM_PROT_READ |
                                 (S.isWritable() ? VM_PROT_WRITE : 0) |
                                 (S.isExecutable() ? VM_PROT_EXECUTE : 0);
      auto *D = reinterpret_cast<section_64 *>(C + 1);
      for (const auto &Sec : Image.Sections)
        if (Sec.SegmentName == S.Name) {
          std::strcpy(D->sectname, Sec.Name.c_str());
          std::strcpy(D->segname, S.Name.c_str());
          D->addr = Sec.VA;
          D->size = Sec.Size;
          D->offset = Sec.FileOff;
          D->align = 2;
          D->flags = Sec.Type;
          ++D;
        }
      Offset += C->cmdsize;
    }
    auto *C = reinterpret_cast<symtab_command *>(Bytes.data() + Offset);
    C->cmd = LC_SYMTAB;
    C->cmdsize = sizeof(*C);
    C->symoff = 0x3400;
    C->nsyms = 5;
    C->stroff = 0x3500;
    C->strsize = 128;
    auto &RO = Image.Segments[2].Data;
    size_t I = 0, Str = 1;
    for (auto Entry : {Root, OtherRoot, Helper, OtherHelper, Accessor}) {
      auto *N = reinterpret_cast<nlist_64 *>(RO.data() + 0x400) + I++;
      const std::string Name = "_local_" + std::to_string(Entry);
      N->n_strx = Str;
      N->n_type = N_SECT;
      N->n_sect = 1;
      N->n_value = Entry;
      std::memcpy(RO.data() + 0x500 + Str, Name.c_str(), Name.size() + 1);
      Str += Name.size() + 1;
    }
  }
  explicit SetterFixture(bool Rect = false) {
    Image.Format = BinaryFormat::MachO;
    Image.Arch = Arch::AArch64;
    Image.Bits = Bitness::Bits64;
    Image.Entry = Root;
    for (const auto [VA, Name, Flags] :
         {std::tuple{0x1000, "__TEXT",
                     SegmentFlags::Readable | SegmentFlags::Executable},
          std::tuple{0x2000, "__DATA",
                     SegmentFlags::Readable | SegmentFlags::Writable},
          std::tuple{0x4000, "__CONST", SegmentFlags::Readable}}) {
      Segment S;
      S.Name = Name;
      S.VA = VA;
      S.FileOff = VA;
      S.Size = S.FileSz = 0x1000;
      S.Flags = Flags;
      S.Data.resize(S.Size);
      Image.Segments.push_back(std::move(S));
    }
    Image.Segments[0].VA = Image.Segments[0].FileOff = 0;
    Image.Segments[0].Size = Image.Segments[0].FileSz = 0x2000;
    Image.Segments[0].Data.resize(0x2000);
    Image.Segments[2].FileOff = 0x3000;
    for (const auto [VA, Size, Name, SegmentIndex] :
         {std::tuple{0x1000, 0x1000, "__text", 0},
          std::tuple{0x2000, 0x400, "__data", 1},
          std::tuple{0x2400, 0x100, "__llvm_prf_cnts", 1},
          std::tuple{0x2500, 0xb00, "__data", 1},
          std::tuple{0x4000, 0x200, "__const", 2}}) {
      Section S;
      S.Name = Name;
      S.SegmentName = Image.Segments[SegmentIndex].Name;
      S.VA = VA;
      S.FileOff = VA;
      S.Size = S.FileSz = Size;
      if (SegmentIndex == 2)
        S.FileOff = 0x3000;
      S.Flags = Image.Segments[SegmentIndex].Flags;
      S.Type = SegmentIndex == 0
                   ? uint32_t(llvm::MachO::S_ATTR_PURE_INSTRUCTIONS)
                   : uint32_t(llvm::MachO::S_REGULAR);
      Image.Sections.push_back(S);
    }
    for (unsigned Variant = 0; Variant < 2; ++Variant) {
      const va_t R = Variant ? OtherRoot : Root,
                 H = Variant ? OtherHelper : Helper;
      word(R, 0xb0000003);
      word(R + 4, 0x91000063 | (((Selector + Variant * 8) & 4095) << 10));
      word(R + 8, 0xb0000004);
      word(R + 12, 0x91000084 | (((Profile + Variant * 8) & 4095) << 10));
      word(R + 16, branch(R + 16, H, false));
      std::vector<uint32_t> Words = {0xd10103ff, 0xa90157f6,
                                     0xa9024ff4, 0xa9037bfd,
                                     0x9100c3fd, 0xaa0403f3,
                                     0xaa0303f4, 0xaa0203f5,
                                     0xaa0003f6, branch(H + 36, Accessor),
                                     0xa90003f6, 0xf9400294};
      if (Variant)
        Words.push_back(0xaa1603e0);
      Words.push_back(branch(H + Words.size() * 4, Variant ? 0x17c0 : 0x1780));
      Words.insert(Words.end(),
                   {0xaa0003f6, 0x910003e0, 0xaa1403e1, 0xaa1503e2});
      Words.push_back(branch(H + Words.size() * 4, 0x1760));
      Words.insert(Words.end(),
                   {0xf9400268, 0x91000508, 0xf9000268, 0xf94002c8, 0xb0000009,
                    0xf9400129 | (((Mask & 4095) / 8) << 10), 0xf9400129,
                    0x8a080128, 0xf9403108, 0xaa1603f4, 0xd63f0100});
      if (Variant)
        Words.push_back(0xaa1603e0);
      Words.push_back(branch(H + Words.size() * 4, Variant ? 0x17e0 : 0x17a0));
      Words.insert(Words.end(), {0xa9437bfd, 0xa9424ff4, 0xa94157f6, 0x910103ff,
                                 0xd65f03c0});
      for (unsigned I = 0; I < Words.size(); ++I)
        word(H + I * 4, Words[I]);
    }
    const uint32_t AccessorWords[] = {0xa9bf7bfd,
                                      0x910003fd,
                                      0xb0000000,
                                      0x91200000,
                                      branch(Accessor + 16, 0x1740),
                                      0xd2800001,
                                      0xa8c17bfd,
                                      0xd65f03c0};
    for (unsigned I = 0; I < std::size(AccessorWords); ++I)
      word(Accessor + I * 4, AccessorWords[I]);
    for (const auto [Entry, Slot, Name] :
         {std::tuple{0x1740, 0x2f00, "_objc_opt_self"},
          std::tuple{0x1760, 0x2f08, "_objc_msgSendSuper2"},
          std::tuple{0x1780, 0x2f10,
                     Rect ? "_objc_retain_x21" : "_objc_retain_x22"},
          std::tuple{0x17a0, 0x2f18,
                     Rect ? "_objc_release_x21" : "_objc_release_x22"},
          std::tuple{0x17c0, 0x2f28, "_objc_retain"},
          std::tuple{0x17e0, 0x2f30, "_objc_release"}}) {
      word(Entry, 0xb0000010);
      word(Entry + 4, 0xf9400210 | (((Slot & 4095) / 8) << 10));
      word(Entry + 8, 0xd61f0200);
      Image.Symbols.push_back({Name, va_t(Entry), 12, true});
      Image.ImportPtrSlots[Slot] = Name;
      EXPECT_TRUE(Image.recordDyldBindSlot(Slot, Name, 0,
                                           "/usr/lib/libobjc.A.dylib", false));
    }
    Image.ImportPtrSlots[Mask] = "_swift_isaMask";
    EXPECT_TRUE(Image.recordDyldBindSlot(
        Mask, "_swift_isaMask", 0, "/usr/lib/swift/libswiftCore.dylib", false));
    pointer(Metadata, 0x2900);
    pointer(Metadata + 32, 0x2a02);
    pointer(0x2920, 0x2a80);
    word(0x2a80, 1);
    pointer(0x2a18, 0x2b00);
    pointer(0x2a98, 0x2b00);
    const std::string ClassName = "_TtC4Test8Receiver";
    string(0x2b00, ClassName);
    pointer(Metadata + 56, uint64_t(24) << 32 | 136);
    pointer(Metadata + 64, 0x4000);
    pointer(Metadata + 96, 0x1600);
    Image.DataPtrRelocSlots.insert(Metadata + 64);
    Image.DataPtrRelocTargetOwners[Metadata + 64] = 0x4000;
    Image.CodePtrRelocSlots.insert(Metadata + 96);
    word(0x4000, 0x80000050);
    word(0x4004, 0xc0 - 4);
    word(0x4008, 0x80 - 8);
    word(0x4018, 3);
    word(0x401c, 14);
    word(0x4020, 1);
    word(0x402c, 12);
    word(0x4030, 1);
    word(0x4034, 0x10);
    word(0x4038, uint32_t(0x1600 - 0x4038));
    word(0x40c0, 0);
    word(0x40c8, 0xe0 - 0xc8);
    string(0x4080, "Receiver");
    string(0x40e0, "Test");
    word(0x1600, 0xd65f03c0);
    Image.Symbols.push_back({"_$s4Test8ReceiverC6layoutyyF", 0x1600, 4, true});
    ObjCClass Class;
    Class.Address = Metadata;
    Class.Name = ClassName;
    Class.SuperclassName = "UIControl";
    Class.InheritanceStatus = "resolved";
    Image.ObjCClasses.push_back(Class);
    for (unsigned I = 0; I < 2; ++I) {
      ObjCMethod M;
      M.Status = "supported";
      M.Implementation = I ? OtherRoot : Root;
      M.ClassName = ClassName;
      M.ClassAddress = Metadata;
      M.Selector = I ? "setSelected:" : "setHighlighted:";
      M.TypeEncoding = "v20@0:8B16";
      M.TypeHint = parseObjCMethodEncoding(M.Selector, M.TypeEncoding);
      std::string Error;
      EXPECT_TRUE(assignDarwinObjCSourceABI(*M.TypeHint, Image.Arch, Error));
      Image.ObjCMethods.push_back(M);
      Image.ObjCSourceReferences[Selector + I * 8] = {
          ObjCSourceReference::Kind::Selector,
          Selector + I * 8,
          8,
          M.Selector,
          {}};
    }
    for (const auto E : {Root, OtherRoot, Helper, OtherHelper, Accessor}) {
      const auto Name = "setter_" + std::to_string(E);
      Image.Symbols.push_back({Name, E, 0, true});
      Image.Exports.push_back({Name, 0, E});
    }
    if (Rect) {
      Image.DynInfo.NeededLibs.push_back("/usr/lib/libobjc.A.dylib");
      Image.Sections[0].Size = Image.Sections[0].FileSz = 0x800;
      Image.Sections[4].Size = Image.Sections[4].FileSz = 0x180;
      Section Stub = Image.Sections[0];
      Stub.Name = "__objc_stubs";
      Stub.VA = Stub.FileOff = LayoutStub;
      Stub.Size = Stub.FileSz = 20;
      Image.Sections.push_back(Stub);
      Section Imports = Image.Sections[4];
      Imports.Name = "__nl_symbol_ptr";
      Imports.VA = LayoutImport;
      Imports.FileOff = 0x3180;
      Imports.Size = Imports.FileSz = 8;
      Imports.Type = llvm::MachO::S_NON_LAZY_SYMBOL_POINTERS;
      Image.Sections.push_back(Imports);
      Image.ImportPtrSlots[LayoutImport] = "_objc_msgSend";
      EXPECT_TRUE(Image.recordDyldBindSlot(LayoutImport, "_objc_msgSend", 0,
                                           "/usr/lib/libobjc.A.dylib", false));
      pointer(LayoutSelector, LayoutName);
      Image.DataPtrRelocSlots.insert(LayoutSelector);
      Image.DataPtrRelocTargetOwners[LayoutSelector] = 0x4000;
      string(LayoutName, "setNeedsLayout");
      Image.ObjCSourceReferences[LayoutSelector] = {
          ObjCSourceReference::Kind::Selector,
          LayoutSelector,
          8,
          "setNeedsLayout",
          {}};
      const uint32_t StubWords[] = {
          0xb0000001, 0xf9400021 | (((LayoutSelector & 4095) / 8) << 10),
          0xf0000010, 0xf9400210 | (((LayoutImport & 4095) / 8) << 10),
          0xd61f0200};
      for (unsigned I = 0; I < std::size(StubWords); ++I)
        word(LayoutStub + I * 4, StubWords[I]);
      for (unsigned Variant = 0; Variant != 2; ++Variant) {
        const va_t R = Variant ? OtherRoot : Root,
                   H = Variant ? OtherHelper : Helper;
        word(R, 0xb0000002);
        word(R + 4, 0x91000042 | (((Selector + Variant * 8) & 4095) << 10));
        word(R + 8, 0xb0000003);
        word(R + 12, 0x91000063 | (((Profile + Variant * 8) & 4095) << 10));
        const uint32_t Words[] = {0xd10183ff,
                                  0x6d012beb,
                                  0x6d0223e9,
                                  0xa90357f6,
                                  0xa9044ff4,
                                  0xa9057bfd,
                                  0x910143fd,
                                  0xaa0303f3,
                                  0xaa0203f4,
                                  0x4ea31c68,
                                  0x4ea21c49,
                                  0x4ea11c2a,
                                  0x4ea01c0b,
                                  0xaa0003f5,
                                  branch(H + 56, Accessor),
                                  0xa90003f5,
                                  0xf9400294,
                                  branch(H + 68, 0x1780),
                                  0xaa0003f5,
                                  0x910003e0,
                                  0xaa1403e1,
                                  0x4eab1d60,
                                  0x4eaa1d41,
                                  0x4ea91d22,
                                  0x4ea81d03,
                                  branch(H + 100, 0x1760),
                                  0xf9400268,
                                  0x91000508,
                                  0xf9000268,
                                  0xaa1503e0,
                                  branch(H + 120, LayoutStub),
                                  branch(H + 124, 0x17a0),
                                  0xa9457bfd,
                                  0xa9444ff4,
                                  0xa94357f6,
                                  0x6d4223e9,
                                  0x6d412beb,
                                  0x910183ff,
                                  0xd65f03c0};
        for (unsigned I = 0; I < std::size(Words); ++I)
          word(H + I * 4, Words[I]);
        auto &M = Image.ObjCMethods[Variant];
        M.Selector = Variant ? "setBounds:" : "setFrame:";
        M.TypeEncoding = "v48@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16";
        M.TypeHint = parseObjCMethodEncoding(M.Selector, M.TypeEncoding);
        std::string Error;
        EXPECT_TRUE(assignDarwinObjCSourceABI(*M.TypeHint, Image.Arch, Error));
        Image.ObjCSourceReferences[Selector + Variant * 8].Name = M.Selector;
      }
      ObjCMethod Layout = Image.ObjCMethods.front();
      Layout.Implementation = 0x1600;
      Layout.Selector = "setNeedsLayout";
      Layout.TypeEncoding = "v16@0:8";
      Layout.TypeHint =
          parseObjCMethodEncoding(Layout.Selector, Layout.TypeEncoding);
      std::string Error;
      EXPECT_TRUE(
          assignDarwinObjCSourceABI(*Layout.TypeHint, Image.Arch, Error));
      Image.ObjCMethods.push_back(std::move(Layout));
      std::sort(Image.Sections.begin(), Image.Sections.end(),
                [](const auto &A, const auto &B) { return A.VA < B.VA; });
    }
    fileHeader();
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    Options.OnlyFunctionEntries = {Root, OtherRoot, Helper, OtherHelper,
                                   Accessor};
    EXPECT_EQ(seedObjCMergedSetterAccessorHints(Image, Options), 1U);
    Result = Pipeline().run(Image, Context, Options);
    EXPECT_TRUE(Result.Success) << Result.Error;
  }
};

TEST(ObjCMergedSetterSources, ProvesBothARCFormsAndKeepsLiveDispatch) {
  SetterFixture F;
  const ObjCProfileStorage Storage(F.Image);
  const auto Plan = discoverObjCMergedSetterSources(F.Image, F.Result, Storage);
  ASSERT_EQ(Plan.Callers, (std::set<va_t>{F.Root, F.OtherRoot}));
  std::map<va_t, const HighFunc *> Functions;
  for (const auto &H : F.Result.HighFuncs)
    Functions.emplace(H.Entry, &H);
  for (auto Root : {F.Root, F.OtherRoot}) {
    const auto C = validatedObjCMergedSetter(F.Image, Plan, Storage, Root);
    ASSERT_TRUE(C);
    EXPECT_EQ(C->Helper.Parameters.size(), 5U);
    EXPECT_EQ(C->Helper.Parameters[2].Type->Size, 1U);
    EXPECT_EQ(C->Virtual.Parameters[0].Location.RegisterOffset, 160U);
    auto P = projectObjCMergedSetter(F.high(Root), F.Image, Plan, Storage);
    ASSERT_TRUE(P.Projected);
    EXPECT_EQ(P.Dependencies, std::set<va_t>{F.Accessor});
    EXPECT_EQ(P.ProfileSections, std::set<va_t>{F.Profile});
    auto Bound = [&](const HighExpr &E) {
      return objCMergedSetterSourceCallBound(E, F.Image, Plan, Storage,
                                             P.Function, Functions);
    };
    const auto &Call = *P.Function.Body[0].CallExpr;
    EXPECT_TRUE(Bound(Call));
    for (unsigned I = 3; I < 7; ++I)
      EXPECT_TRUE(Bound(*Call.Operands[I]));
    const auto *Audit =
        objc_super_getter_detail::uniqueEntry(F.Result.FunctionAudits, Root);
    EXPECT_TRUE(sourceBodyLimitation(P.Function, *P.Function.SourceTypeHint,
                                     Audit, Bound)
                    .empty());
    std::set<std::string> Shared;
    const auto CSource =
        renderObjCMergedSetterHelpers(F.Image, Plan, Storage, {Root}, Shared);
    EXPECT_NE(CSource.find("objc_retain(self)"), std::string::npos);
    EXPECT_NE(CSource.find("isa & mask"), std::string::npos);
    EXPECT_NE(CSource.find("swift_context"), std::string::npos);
    EXPECT_NE(CSource.find("objc_release(retained)"), std::string::npos);
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    HighCEmitter Writer;
    EXPECT_TRUE(Writer.emit({P.Function}, OS));
    EXPECT_FALSE(Source.empty());
  }
  EXPECT_FALSE(projectObjCMergedSetter(F.high(F.Helper), F.Image, Plan, Storage)
                   .Projected);
}

TEST(ObjCMergedSetterSources, RejectsEveryChangedHelperAndCallerInstruction) {
  for (unsigned Variant = 0; Variant < 2; ++Variant) {
    const unsigned Count = Variant ? 37 : 35;
    for (unsigned I = 0; I < Count + 5; ++I) {
      SCOPED_TRACE(Variant);
      SCOPED_TRACE(I);
      SetterFixture F;
      const ObjCProfileStorage Storage(F.Image);
      const va_t Root = Variant ? F.OtherRoot : F.Root,
                 Helper = Variant ? F.OtherHelper : F.Helper;
      const auto Plan =
          discoverObjCMergedSetterSources(F.Image, F.Result, Storage);
      ASSERT_TRUE(Plan.Callers.count(Root));
      const va_t Address = I < Count ? Helper + I * 4 : Root + (I - Count) * 4;
      const auto Bits =
          llvm::support::endian::read32le(F.Image.readVA(Address, 4));
      F.word(Address, Bits ^ 1U);
      EXPECT_FALSE(validatedObjCMergedSetter(F.Image, Plan, Storage, Root));
    }
  }
}

TEST(ObjCMergedSetterSources, RechecksCurrentDeclarationsStorageAndLowIR) {
  for (unsigned I = 0; I < 25; ++I) {
    SCOPED_TRACE(I);
    SetterFixture F;
    const ObjCProfileStorage Storage(F.Image);
    const auto Plan =
        discoverObjCMergedSetterSources(F.Image, F.Result, Storage);
    ASSERT_TRUE(Plan.Callers.count(F.Root));
    switch (I) {
    case 0:
      F.Image.DyldBindSlots[F.Mask].WeakImport = true;
      break;
    case 1:
      F.Image.DyldBindSlots[F.Mask].Module = "/wrong/provider";
      break;
    case 2:
      F.Image.DyldBindSlots[0x2f10].WeakImport = true;
      break;
    case 3:
      F.Image.DyldBindSlots[0x2f18].Module = "/wrong/provider";
      break;
    case 4:
      F.Image.DyldBindSlots[0x2f08].Addend = 8;
      break;
    case 5:
      F.Image.ObjCSourceReferences[F.Selector].Name = "setEnabled:";
      break;
    case 6:
      F.Image.ObjCSourceReferences[F.Selector].Size = 4;
      break;
    case 7:
      F.Image.Sections[2].Flags = SegmentFlags::Readable;
      break;
    case 8:
      F.Image.DataPtrRelocSlots.insert(F.Profile);
      break;
    case 9:
      F.Image.ObjCMethods[0].ClassAddress += 8;
      break;
    case 10:
      F.Image.ObjCClasses[0].InheritanceStatus = "unresolved";
      break;
    case 11:
      F.Image.ObjCClasses.push_back(F.Image.ObjCClasses[0]);
      break;
    case 12:
      F.word(0x4034, 0x11);
      break;
    case 13:
      F.Image.CodePtrRelocSlots.erase(F.Metadata + 96);
      break;
    case 14:
      F.Image.ObjCMethods[0].TypeHint->Parameters[2].Type = NdType::makeInt(8);
      break;
    case 15:
      F.high(F.Root).Params[2].Type = NdType::makeInt(4);
      break;
    case 16:
      F.high(F.Accessor).SourceTypeHint->ReturnLocation.RegisterOffset = 8;
      break;
    case 17:
      F.low(F.Root).Blocks[0].Ops.back().NumInputs = 0;
      break;
    case 18:
      F.low(F.Helper).Blocks[0].Ops[0].Inputs[0].Offset ^= 16;
      break;
    case 19:
      F.low(F.Helper).Blocks[0].InstructionBoundaries[0].OpCount = 0;
      break;
    case 20:
      F.Result.MedFuncs.push_back(F.Result.MedFuncs.front());
      break;
    case 21:
      for (auto &A : F.Result.FunctionAudits)
        if (A.Entry == F.Helper)
          A.MedIRVerified = false;
      break;
    case 22:
      F.Image.Segments[0].Flags =
          F.Image.Segments[0].Flags | SegmentFlags::Writable;
      break;
    case 23:
      F.high(F.Helper).UnstructuredExceptionRegions = 1;
      break;
    case 24:
      F.pointer(F.Profile, 1);
      break;
    }
    EXPECT_FALSE(validatedObjCMergedSetter(F.Image, Plan, Storage, F.Root));
    std::set<std::string> Shared;
    EXPECT_THROW(
        renderObjCMergedSetterHelpers(F.Image, Plan, Storage, {F.Root}, Shared),
        std::runtime_error);
  }
}

TEST(ObjCMergedSetterSources, PublicationRequiresExactParametersAndOneCall) {
  for (unsigned I = 0; I < 16; ++I) {
    SCOPED_TRACE(I);
    SetterFixture F;
    const ObjCProfileStorage Storage(F.Image);
    const auto Plan =
        discoverObjCMergedSetterSources(F.Image, F.Result, Storage);
    auto P = projectObjCMergedSetter(F.high(F.Root), F.Image, Plan, Storage);
    ASSERT_TRUE(P.Projected);
    std::map<va_t, const HighFunc *> Functions;
    for (const auto &H : F.Result.HighFuncs)
      Functions.emplace(H.Entry, &H);
    auto &Call = *P.Function.Body[0].CallExpr;
    auto Hint = std::make_shared<SourceCallTypeHint>(*Call.SourceCallHint);
    Call.SourceCallHint = Hint;
    std::vector<std::shared_ptr<SourceCallTypeHint>> Addresses;
    for (unsigned J = 3; J < 7; ++J) {
      auto A = std::make_shared<SourceCallTypeHint>(
          *Call.Operands[J]->SourceCallHint);
      Call.Operands[J]->SourceCallHint = A;
      Addresses.push_back(A);
    }
    switch (I) {
    case 0:
      Call.Operands[0]->Var.Id = 1;
      break;
    case 1:
      Call.Operands[2]->Var.Size = 8;
      break;
    case 2:
      Call.Operands[2]->Var.RegOff = 8;
      break;
    case 3:
      Call.Operands[2]->Var.SSAVer = 1;
      break;
    case 4:
      Call.Operands[2]->Var.TheArch = Arch::X64;
      break;
    case 5:
      Call.Operands[2] = HighExpr::makeConst(1, 1);
      break;
    case 6:
      Addresses[0]->TargetAddress += 8;
      break;
    case 7:
      Addresses[1]->TargetName = "other_storage";
      break;
    case 8:
      Addresses[2]->TargetAddress += 4;
      break;
    case 9:
      Addresses[3]->TargetName = "other_mask";
      break;
    case 10:
      Hint->Signature.Parameters[2].Location.ValueBytes = 8;
      break;
    case 11:
      P.Function.Body.push_back(P.Function.Body[0]);
      break;
    case 12:
      P.Function.Body[1].RetVal = Call.Operands[0];
      break;
    case 13:
      P.Function.Params[2].Type = NdType::makeFloat(4);
      break;
    case 14:
      Functions.erase(F.Accessor);
      break;
    case 15:
      Call.IsIndirectCall = true;
      break;
    }
    EXPECT_FALSE(objCMergedSetterSourceCallBound(Call, F.Image, Plan, Storage,
                                                 P.Function, Functions));
  }
}

TEST(ObjCMergedSetterSources,
     CGRectPreservesFourValueCarriersAndLayoutDispatch) {
  SetterFixture F(true);
  const ObjCProfileStorage Storage(F.Image);
  const auto Plan = discoverObjCMergedSetterSources(F.Image, F.Result, Storage);
  ASSERT_EQ(Plan.Callers, (std::set<va_t>{F.Root, F.OtherRoot}));
  std::map<va_t, const HighFunc *> Functions;
  for (const auto &H : F.Result.HighFuncs)
    Functions.emplace(H.Entry, &H);
  for (const auto Root : Plan.Callers) {
    const auto C = validatedObjCMergedSetter(F.Image, Plan, Storage, Root);
    ASSERT_TRUE(C);
    EXPECT_EQ(C->Helper.Parameters.size(), 5U);
    ASSERT_EQ(C->Helper.Parameters[2].Components.size(), 4U);
    EXPECT_EQ(C->Helper.Parameters[2].Type->Size, 32U);
    for (unsigned I = 0; I != 4; ++I) {
      const auto &Part = C->Helper.Parameters[2].Components[I];
      EXPECT_EQ(Part.Kind, SourceABICarrierKind::FloatingRegister);
      EXPECT_EQ(Part.ValueBytes, 8U);
      EXPECT_EQ(Part.RegisterOffset, a64reg::V0 + I * 16);
    }
    EXPECT_EQ(C->Layout.SelectorReferenceAddress, F.LayoutSelector);
    auto P = projectObjCMergedSetter(F.high(Root), F.Image, Plan, Storage);
    ASSERT_TRUE(P.Projected);
    auto Bound = [&](const HighExpr &E) {
      return objCMergedSetterSourceCallBound(E, F.Image, Plan, Storage,
                                             P.Function, Functions);
    };
    const auto &Call = *P.Function.Body[0].CallExpr;
    EXPECT_TRUE(Bound(Call));
    EXPECT_EQ(Call.Operands[2]->Var.Size, 32U);
    EXPECT_EQ(Call.Operands[6]->SourceCallHint->CallKind,
              SourceCallTypeHint::Kind::RuntimeSelectorReferenceAddress);
    const auto *Audit =
        objc_super_getter_detail::uniqueEntry(F.Result.FunctionAudits, Root);
    EXPECT_TRUE(sourceBodyLimitation(P.Function, *P.Function.SourceTypeHint,
                                     Audit, Bound)
                    .empty());
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    CEmitterOptions Options;
    Options.TheArch = Arch::AArch64;
    ASSERT_TRUE(
        HighCEmitter().emit({P.Function, F.high(F.Accessor)}, OS, Options));
    EXPECT_EQ(Source.find("native address has no recovered definition"),
              std::string::npos);
    EXPECT_NE(Source.find("neverd_objc_merged_setter_"), std::string::npos);
  }
  std::set<std::string> Shared;
  const auto Source = renderObjCMergedSetterHelpers(F.Image, Plan, Storage,
                                                    Plan.Callers, Shared);
  EXPECT_NE(Source.find("layout_selector_slot"), std::string::npos);
  EXPECT_NE(Source.find("objc_msgSend)(retained, layout_selector)"),
            std::string::npos);
  EXPECT_EQ(Source.find("swift_context"), std::string::npos);
}

TEST(ObjCMergedSetterSources, CGRectRejectsEveryChangedMachineInstruction) {
  for (unsigned I = 0; I != 49; ++I) {
    SCOPED_TRACE(I);
    SetterFixture F(true);
    const ObjCProfileStorage Storage(F.Image);
    const auto Plan =
        discoverObjCMergedSetterSources(F.Image, F.Result, Storage);
    ASSERT_EQ(Plan.Callers.size(), 2U);
    const va_t Address = I < 39   ? F.Helper + I * 4
                         : I < 44 ? F.Root + (I - 39) * 4
                                  : F.LayoutStub + (I - 44) * 4;
    F.word(Address,
           llvm::support::endian::read32le(F.Image.readVA(Address, 4)) ^ 1U);
    EXPECT_FALSE(validatedObjCMergedSetter(F.Image, Plan, Storage, F.Root));
  }
}

TEST(ObjCMergedSetterSources, CGRectRechecksCurrentLayoutABIAndFrameEvidence) {
  for (unsigned I = 0; I != 30; ++I) {
    SCOPED_TRACE(I);
    SetterFixture F(true);
    const ObjCProfileStorage Storage(F.Image);
    const auto Plan =
        discoverObjCMergedSetterSources(F.Image, F.Result, Storage);
    ASSERT_EQ(Plan.Callers.size(), 2U);
    switch (I) {
    case 0:
      F.Image.DyldBindSlots[F.LayoutImport].WeakImport = true;
      break;
    case 1:
      F.Image.DyldBindSlots[F.LayoutImport].Module = "/wrong/provider";
      break;
    case 2:
      F.Image.ObjCSourceReferences[F.LayoutSelector].Name = "layout";
      break;
    case 3:
      F.Image.ObjCSourceReferences[F.LayoutSelector].Size = 4;
      break;
    case 4:
      F.pointer(F.LayoutSelector, 0x4080);
      break;
    case 5:
      F.string(F.LayoutName, "setNeedsDisplay");
      break;
    case 6:
      F.section(F.LayoutImport).Type = llvm::MachO::S_REGULAR;
      break;
    case 7:
      F.Image.DyldBindSlots[0x2f10].WeakImport = true;
      break;
    case 8:
      F.Image.DyldBindSlots[0x2f18].Module = "/wrong/provider";
      break;
    case 9:
      F.Image.DyldBindSlots[0x2f08].Addend = 8;
      break;
    case 10:
      F.high(F.Root).SourceTypeHint->Parameters[2].Components[2].ValueBytes = 4;
      break;
    case 11:
      F.high(F.Root)
          .SourceTypeHint->Parameters[2]
          .Components[2]
          .RegisterOffset += 16;
      break;
    case 12:
      F.high(F.Root).Params[2].Type = NdType::makeFloat(8);
      break;
    case 13:
      F.high(F.Root).SourceTypeHint->Parameters[2].Components.pop_back();
      break;
    case 14:
      F.Image.ObjCClasses[0].InheritanceStatus = "unresolved";
      break;
    case 15:
      F.Image.ObjCClasses[0].Address += 8;
      break;
    case 16:
      F.high(F.Accessor).SourceTypeHint->ReturnLocation.RegisterOffset = 8;
      break;
    case 17:
      F.low(F.Helper).Blocks[0].InstructionBoundaries[1].OpCount = 0;
      break;
    case 18:
      F.low(F.Helper).Blocks[0].Ops[0].Inputs[0].Offset ^= 16;
      break;
    case 19:
      F.Result.MedFuncs.push_back(F.Result.MedFuncs.front());
      break;
    case 20:
      for (auto &A : F.Result.FunctionAudits)
        if (A.Entry == F.Helper)
          A.HasLowIR = false;
      break;
    case 21:
      F.section(F.Profile).Flags = SegmentFlags::Readable;
      break;
    case 22:
      F.Image.DataPtrRelocSlots.insert(F.Profile);
      break;
    case 23:
      F.pointer(F.Profile, 1);
      break;
    case 24:
      F.Image.DynInfo.NeededLibs.clear();
      break;
    case 25:
      F.Image.ObjCMethods.back().TypeEncoding = "Q16@0:8";
      break;
    case 26:
      F.Image.ObjCMethods.back().TypeHint->Parameters[1].Type =
          NdType::makeInt(8);
      break;
    case 27:
      setMappedSectionFlags(F.Image, F.section(F.LayoutImport),
                            SegmentFlags::Readable | SegmentFlags::Writable);
      break;
    case 28:
      F.Image.DataPtrRelocTargetOwners.erase(F.LayoutSelector);
      break;
    case 29:
      F.Image.DataPtrRelocSlots.erase(F.LayoutSelector);
      break;
    }
    EXPECT_FALSE(validatedObjCMergedSetter(F.Image, Plan, Storage, F.Root));
    std::set<std::string> Shared;
    EXPECT_THROW(
        renderObjCMergedSetterHelpers(F.Image, Plan, Storage, {F.Root}, Shared),
        std::runtime_error);
  }
}

TEST(ObjCMergedSetterSources,
     CGRectPublicationRechecksLogicalParameterAndLayoutSlot) {
  for (unsigned I = 0; I != 12; ++I) {
    SCOPED_TRACE(I);
    SetterFixture F(true);
    const ObjCProfileStorage Storage(F.Image);
    const auto Plan =
        discoverObjCMergedSetterSources(F.Image, F.Result, Storage);
    auto P = projectObjCMergedSetter(F.high(F.Root), F.Image, Plan, Storage);
    ASSERT_TRUE(P.Projected);
    std::map<va_t, const HighFunc *> Functions;
    for (const auto &H : F.Result.HighFuncs)
      Functions.emplace(H.Entry, &H);
    auto &Call = *P.Function.Body[0].CallExpr;
    auto Hint = std::make_shared<SourceCallTypeHint>(*Call.SourceCallHint);
    Call.SourceCallHint = Hint;
    auto Layout =
        std::make_shared<SourceCallTypeHint>(*Call.Operands[6]->SourceCallHint);
    Call.Operands[6]->SourceCallHint = Layout;
    switch (I) {
    case 0:
      Call.Operands[2]->Var.Size = 8;
      break;
    case 1:
      Call.Operands[2]->Var.RegOff = 16;
      break;
    case 2:
      Call.Operands[2]->Var.SSAVer = 1;
      break;
    case 3:
      Call.Operands[2]->Var.Id = 0;
      break;
    case 4:
      Hint->Signature.Parameters[2].Components[0].ValueBytes = 4;
      break;
    case 5:
      Hint->Signature.Parameters[2].Components[0].RegisterOffset += 16;
      break;
    case 6:
      Layout->TargetAddress += 8;
      break;
    case 7:
      Layout->CallKind = SourceCallTypeHint::Kind::NativeAddress;
      break;
    case 8:
      P.Function.Body[1].Addr += 4;
      break;
    case 9:
      P.Function.Body.push_back(P.Function.Body.front());
      break;
    case 10:
      Functions.erase(F.Accessor);
      break;
    case 11:
      Call.Operands[2]->Type = NdType::makeInt(8);
      break;
    }
    EXPECT_FALSE(objCMergedSetterSourceCallBound(Call, F.Image, Plan, Storage,
                                                 P.Function, Functions));
  }
}

TEST(ObjCMergedSetterSources, CGRectGeneratedCMatchesOriginalARM64AtO0AndO2) {
  SetterFixture F(true);
  const ObjCProfileStorage Storage(F.Image);
  const auto Plan = discoverObjCMergedSetterSources(F.Image, F.Result, Storage);
  ASSERT_EQ(Plan.Callers.size(), 2U);
  std::vector<HighFunc> Functions;
  for (const auto Root : {F.Root, F.OtherRoot}) {
    auto P = projectObjCMergedSetter(F.high(Root), F.Image, Plan, Storage);
    ASSERT_TRUE(P.Projected);
    P.Function.Name = Root == F.Root ? "projected_frame" : "projected_bounds";
    Functions.push_back(std::move(P.Function));
  }
  auto Provider = F.high(F.Accessor);
  Provider.Name = "metadata";
  Functions.push_back(std::move(Provider));
  std::string Emitted;
  llvm::raw_string_ostream OS(Emitted);
  CEmitterOptions Options;
  Options.TheArch = Arch::AArch64;
  Options.Image = &F.Image;
  ASSERT_TRUE(HighCEmitter().emit(Functions, OS, Options));
  std::set<std::string> Helpers;
  auto Source = renderObjCMergedSetterHelpers(F.Image, Plan, Storage,
                                              Plan.Callers, Helpers);
  const std::string Header = "#include <objc/runtime.h>";
  const auto Position = Source.find(Header);
  ASSERT_NE(Position, std::string::npos);
  Source.replace(Position, Header.size(),
                 "void *sel_registerName(const char *name);\n");
  const auto Record =
      typeToC(F.Image.ObjCMethods[0].TypeHint->Parameters[2].Type);
  std::set<std::string> StorageNames;
  std::string Program = "#include <string.h>\n" + Emitted + Source +
                        Storage.render({F.Profile}, StorageNames);
  Program += R"C(
static void *setter_selectors[2], *layout_selector;
static uint64_t counters[4], expected_count;
static unsigned variant, trace, bad;
static const uint64_t bits[4] = {0x3fe0000000000000ULL, 0x8000000000000000ULL,
                                 0x7ff0000000000000ULL, 0x7ff8000000000011ULL};
void *sel_registerName(const char *name) {
  return (void *)(uintptr_t)(!strcmp(name, "setNeedsLayout") ? 0x4440 : 0x2220);
}
void *objc_opt_self(void *current_class) {
  bad |= trace != 0 || current_class != (void *)0x2800;
  trace = 1;
  setter_selectors[variant] = (void *)0x2220;
  return (void *)0x3330;
}
void *objc_retain(void *self) {
  bad |= trace != 1 || self != (void *)0x1110;
  trace = 2;
  return (void *)0x1118;
}
void objc_release(void *self) {
  bad |= trace != 4 || self != (void *)0x1118;
  trace = 5;
}
)C";
  Program +=
      "void test_super(void *p, void *selector, " + Record + R"C( value) {
  void *receiver, *current_class;
  memcpy(&receiver, p, 8);
  memcpy(&current_class, (const unsigned char *)p + 8, 8);
  bad |= trace != 2 || receiver != (void *)0x1110 ||
         current_class != (void *)0x3330 || selector != (void *)0x2220 ||
         memcmp(&value, bits, 32) || counters[variant + 1] != expected_count;
  trace = 3;
  layout_selector = (void *)0x4440;
}
void test_layout(void *self, void *selector) {
  bad |= trace != 3 || self != (void *)0x1118 || selector != (void *)0x4440 ||
         counters[variant + 1] != expected_count + 1;
  trace = 4;
}
#if defined(__aarch64__) && defined(__APPLE__)
__asm__(".text\n.p2align 2\n"
        ".globl _objc_msgSendSuper2\n_objc_msgSendSuper2:\nb _test_super\n"
        ".globl _objc_msgSend\n_objc_msgSend:\nb _test_layout\n"
        "Lretain_x21:\nmov x0,x21\nb _objc_retain\n"
        "Lrelease_x21:\nmov x0,x21\nb _objc_release\n"
        "Llayout_stub:\nadrp x1,_layout_selector@PAGE\n"
        "ldr x1,[x1,_layout_selector@PAGEOFF]\nb _test_layout\n"
)C";
  for (unsigned V = 0; V != 2; ++V) {
    Program += "\".globl _original_setter" + std::to_string(V) +
               "\\n_original_setter" + std::to_string(V) + ":\\n\"\n";
    const auto SelectorOffset = V ? "+8" : "";
    const auto CounterOffset = "+" + std::to_string(8 + V * 8);
    Program += "\"adrp x2,_setter_selectors" + std::string(SelectorOffset) +
               "@PAGE\\nadd x2,x2,_setter_selectors" + SelectorOffset +
               "@PAGEOFF\\n\"\n";
    Program += "\"adrp x3,_counters" + CounterOffset +
               "@PAGE\\nadd x3,x3,_counters" + CounterOffset +
               "@PAGEOFF\\n.long 0x14000001\\n\"\n";
    const va_t H = V ? F.OtherHelper : F.Helper;
    for (unsigned I = 0; I != 39; ++I) {
      const char *Target = I == 14   ? "_metadata"
                           : I == 17 ? "Lretain_x21"
                           : I == 25 ? "_objc_msgSendSuper2"
                           : I == 30 ? "Llayout_stub"
                           : I == 31 ? "Lrelease_x21"
                                     : nullptr;
      Program += Target ? std::string("\"bl ") + Target + "\\n\"\n"
                        : "\".long " +
                              std::to_string(llvm::support::endian::read32le(
                                  F.Image.readVA(H + I * 4, 4))) +
                              "\\n\"\n";
    }
  }
  Program += ");\nvoid original_setter0(void *, void *, " + Record +
             ");\nvoid original_setter1(void *, void *, " + Record + R"C();
#else
void portable_super(void *, void *, )C" +
             Record + R"C()
#if defined(__APPLE__)
__asm__("_objc_msgSendSuper2");
#else
__asm__("objc_msgSendSuper2");
#endif
void portable_super(void *p, void *s, )C" +
             Record + R"C( v) { test_super(p,s,v); }
void portable_layout(void *, void *)
#if defined(__APPLE__)
__asm__("_objc_msgSend");
#else
__asm__("objc_msgSend");
#endif
void portable_layout(void *p, void *s) { test_layout(p,s); }
#endif
int main(void) {
)C";
  Program += Record + R"C( value; memcpy(&value, bits, 32);
  for (variant = 0; variant != 2; ++variant) for (unsigned i = 0; i != 32; ++i)
    for (unsigned original = 0; original != 2; ++original) {
      trace = bad = 0;
      expected_count = i % 3 ? UINT64_C(0xabc0000000000000) + i : UINT64_MAX;
      counters[0] = UINT64_C(0xabcd1234); counters[3] = UINT64_C(0xdcba4321);
      counters[variant + 1] = expected_count; counters[2 - variant] = 0x5678;
      setter_selectors[variant] = (void *)0x5550;
      layout_selector = (void *)0x6660;
#if defined(__aarch64__) && defined(__APPLE__)
      if (original) {
        if (variant) original_setter1((void *)0x1110, (void *)0x7770, value);
        else original_setter0((void *)0x1110, (void *)0x8880, value);
      } else
#else
      (void)original;
#endif
      if (variant) neverd_objc_merged_setter_1020((void *)0x1110, (void *)0x7770,
          value, &setter_selectors[variant], &counters[1], (void *)metadata,
          &layout_selector);
      else neverd_objc_merged_setter_1000((void *)0x1110, (void *)0x8880,
          value, &setter_selectors[variant], &counters[1], (void *)metadata,
          &layout_selector);
      if (bad || trace != 5 || counters[variant + 1] != expected_count + 1 ||
          counters[2 - variant] != 0x5678 || counters[0] != 0xabcd1234 ||
          counters[3] != 0xdcba4321 || memcmp(&value, bits, 32)) return 1;
    }
  return 0;
}
)C";
  source_call_execution_test::compileAndRun(Program, {"-O0"});
  source_call_execution_test::compileAndRun(Program, {"-O2"});
}

TEST(ObjCMergedSetterSources,
     OriginalARM64MatchesGeneratedCWithRealObjCRuntime) {
#if defined(NEVERD_TEST_CLANG) && defined(__APPLE__) && defined(__aarch64__)
  SetterFixture F;
  const ObjCProfileStorage Storage(F.Image);
  const auto Plan = discoverObjCMergedSetterSources(F.Image, F.Result, Storage);
  ASSERT_EQ(Plan.Callers.size(), 2U);
  std::set<std::string> Shared;
  const auto Generated = renderObjCMergedSetterHelpers(
      F.Image, Plan, Storage, {F.Root, F.OtherRoot}, Shared);
  std::vector<HighFunc> Projected;
  for (const auto Root : {F.Root, F.OtherRoot})
    Projected.push_back(
        projectObjCMergedSetter(F.high(Root), F.Image, Plan, Storage).Function);
  std::string CallerSource;
  llvm::raw_string_ostream CallerOS(CallerSource);
  CEmitterOptions Options;
  Options.TheArch = Arch::AArch64;
  ASSERT_TRUE(HighCEmitter().emit(Projected, CallerOS, Options));
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("neverd-merged-setter", Directory));
  const std::filesystem::path Work(Directory.str().str());
  struct Cleanup {
    std::filesystem::path Work;
    ~Cleanup() {
      std::error_code E;
      std::filesystem::remove_all(Work, E);
    }
  } Cleanup{Work};
  const auto Path = (Work / "setter.c").string(),
             Exe = (Work / "setter").string(), Err = (Work / "stderr").string();
  const auto CallerPath = (Work / "callers.c").string();
  {
    std::set<std::string> StorageNames;
    std::ofstream Callers(CallerPath);
    Callers << CallerSource << Generated
            << Storage.render({F.Profile}, StorageNames);
  }
  std::ofstream Out(Path);
  Out << "#include <stdint.h>\n#include <string.h>\n"
      << Generated << R"(
#include <stddef.h>
#include <stdio.h>
#include <dlfcn.h>
static Class receiver_class;
static unsigned trace,variant,choice,destroyed,mismatch;
static uint64_t expected_count;
static void *expected_self,*expected_selector;
static unsigned char expected_value;
void *setter_selectors[2];
uint64_t setter_counters[4];
uintptr_t *setter_mask_got;
typedef void __attribute__((swiftcall)) (*virtual_method)(void * __attribute__((swift_context)));
static void virtual_observe(void *self,unsigned which){
  mismatch|=trace!=2 || self!=expected_self || which!=choice || setter_counters[variant+1]!=expected_count+1;
  trace=3;
}
static void __attribute__((swiftcall)) first(void * __attribute__((swift_context)) self){virtual_observe(self,0);}
static void __attribute__((swiftcall)) second(void * __attribute__((swift_context)) self){virtual_observe(self,1);}
static void parent_setter(id self,SEL selector,_Bool value){
  mismatch|=trace!=1 || (void*)self!=expected_self || (void*)selector!=expected_selector || value!=expected_value;
  mismatch|=setter_counters[variant+1]!=expected_count;
  uintptr_t target=(uintptr_t)(choice?second:first);
  memcpy((char*)receiver_class+96,&target,8);
  trace=2;
}
static void destroy(id self,SEL selector){(void)selector;++destroyed;object_dispose(self);}
extern void *objc_opt_self(void *);
void *setter_metadata(void){
  mismatch|=trace!=0;trace=1;
  setter_selectors[variant]=expected_selector;
  return objc_opt_self((void*)receiver_class);
}
extern void setter_original0(void *,void *,uint64_t);
extern void setter_original1(void *,void *,uint64_t);
__asm__(".text\n.p2align 2\n"
)";
  for (unsigned V = 0; V < 2; ++V) {
    Out << "\".globl _setter_original" << V << "\\n_setter_original" << V
        << ":\\n\"\n";
    Out << "\"adrp x3,_setter_selectors" << (V ? "+8" : "") << "@PAGE\\n\"\n"
        << "\"add x3,x3,_setter_selectors" << (V ? "+8" : "")
        << "@PAGEOFF\\n\"\n"
        << "\"adrp x4,_setter_counters+" << 8 + V * 8 << "@PAGE\\n\"\n"
        << "\"add x4,x4,_setter_counters+" << 8 + V * 8 << "@PAGEOFF\\n\"\n"
        << "\"b Lsetter_body" << V << "\\nLsetter_body" << V << ":\\n\"\n";
    const va_t H = V ? F.OtherHelper : F.Helper;
    const unsigned Count = V ? 37 : 35;
    for (unsigned I = 0; I < Count; ++I) {
      if (I == 9)
        Out << "\"bl _setter_metadata\\n\"\n";
      else if (I == 12 + V)
        Out << "\"bl _objc_retain" << (V ? "" : "_x22") << "\\n\"\n";
      else if (I == 17 + V)
        Out << "\"bl _objc_msgSendSuper2\\n\"\n";
      else if (I == 22 + V)
        Out << "\"adrp x9,_setter_mask_got@PAGE\\n\"\n";
      else if (I == 23 + V)
        Out << "\"ldr x9,[x9,_setter_mask_got@PAGEOFF]\\n\"\n";
      else if (I == 29 + 2 * V)
        Out << "\"bl _objc_release" << (V ? "" : "_x22") << "\\n\"\n";
      else
        Out << "\".long "
            << llvm::support::endian::read32le(F.Image.readVA(H + I * 4, 4))
            << "\\n\"\n";
    }
  }
  Out << R"(
);
static int check_machine(const uint32_t *p,const uint32_t *expected,unsigned n,unsigned v){
  for(unsigned i=0;i<n;++i){
    if(i==9||i==12+v||i==17+v||i==29+2*v){if((p[i]&0xfc000000)!=0x94000000)return 1;}
    else if(i==22+v){if((p[i]&0x9f00001f)!=0x90000009)return 2;}
    else if(i==23+v){if((p[i]&0xffc003ff)!=0xf9400129)return 3;}
    else if(p[i]!=expected[i])return 4;
  }return 0;
}
int main(void){
  void *runtime=dlopen("/usr/lib/swift/libswiftCore.dylib",RTLD_NOW|RTLD_LOCAL);
  setter_mask_got=(uintptr_t*)dlsym(runtime,"swift_isaMask");
  if(!setter_mask_got)return 10;
  Class parent=objc_allocateClassPair(objc_getClass("NSObject"),"NDMergedSetterParent",0);
  if(!parent)return 11;
  SEL selectors[2]={sel_registerName("setHighlighted:"),sel_registerName("setSelected:")};
  for(unsigned i=0;i<2;++i)if(!class_addMethod(parent,selectors[i],(IMP)parent_setter,"v20@0:8B16"))return 12;
  objc_registerClassPair(parent);
  receiver_class=objc_allocateClassPair(parent,"NDMergedSetterReceiver",512);
  if(!receiver_class||!class_addMethod(receiver_class,sel_registerName("dealloc"),(IMP)destroy,"v16@0:8"))return 13;
  objc_registerClassPair(receiver_class);
  ptrdiff_t extra=(char*)object_getIndexedIvars((id)receiver_class)-(char*)receiver_class;
  if(extra<0||extra>88)return 14;
  uint64_t guards[2]={UINT64_C(0xabc12345),UINT64_C(0xdef67890)};
  memcpy((char*)receiver_class+88,&guards[0],8);memcpy((char*)receiver_class+104,&guards[1],8);
  const uint32_t *originals[2]={(const uint32_t*)setter_original0,(const uint32_t*)setter_original1};
  for(unsigned v=0;v<2;++v){const uint32_t*p=originals[v];if((p[0]&0x9f00001f)!=0x90000003||(p[1]&0xffc003ff)!=0x91000063||(p[2]&0x9f00001f)!=0x90000004||(p[3]&0xffc003ff)!=0x91000084||p[4]!=0x14000001)return 22;}
)";
  for (unsigned V = 0; V < 2; ++V) {
    Out << "  const uint32_t expected" << V << "[]={";
    for (unsigned I = 0; I < (V ? 37U : 35U); ++I)
      Out << (I ? "," : "")
          << llvm::support::endian::read32le(
                 F.Image.readVA((V ? F.OtherHelper : F.Helper) + I * 4, 4));
    Out << "};\n  if(check_machine(originals[" << V << "]+5,expected" << V
        << "," << (V ? 37 : 35) << "," << V << "))return 15;\n";
  }
  Out << R"(
  for(variant=0;variant<2;++variant)for(unsigned i=0;i<1024;++i){
    id object=class_createInstance(receiver_class,16);if(!object)return 16;
    if((*(uintptr_t*)object&*setter_mask_got)!=(uintptr_t)receiver_class)return 17;
    uint64_t *object_guard=(uint64_t*)object_getIndexedIvars(object);
    object_guard[0]=guards[0];object_guard[1]=guards[1];
    expected_self=object;expected_selector=(void*)selectors[(i>>1)&1];expected_value=i&1;choice=(i>>2)&1;
    expected_count=(i%13)?UINT64_C(0x7342000000000000)+i:UINT64_MAX;
    for(unsigned original=0;original<2;++original){
      trace=0;setter_selectors[variant]=(void*)selectors[!((i>>1)&1)];
      setter_counters[0]=guards[0];setter_counters[3]=guards[1];
      setter_counters[variant+1]=expected_count;setter_counters[2-variant]=0x12345678;
      uintptr_t wrong=(uintptr_t)(choice?first:second);memcpy((char*)receiver_class+96,&wrong,8);
      if(original){uint64_t carrier=((uint64_t)i<<32)|expected_value;
        if(variant)setter_original1(object,(void*)(uintptr_t)0x1234,carrier);
        else setter_original0(object,(void*)(uintptr_t)0x5678,carrier);
      }else if(variant)neverd_objc_merged_setter_1020(object,(void*)(uintptr_t)0x1234,expected_value,&setter_selectors[variant],&setter_counters[1],(void*)setter_metadata,setter_mask_got);
      else neverd_objc_merged_setter_1000(object,(void*)(uintptr_t)0x5678,expected_value,&setter_selectors[variant],&setter_counters[1],(void*)setter_metadata,setter_mask_got);
      if(trace!=3||mismatch||setter_counters[variant+1]!=expected_count+1||setter_counters[2-variant]!=0x12345678||setter_counters[0]!=guards[0]||setter_counters[3]!=guards[1])return 18;
      if(object_guard[0]!=guards[0]||object_guard[1]!=guards[1])return 19;
      uint64_t a,b;memcpy(&a,(char*)receiver_class+88,8);memcpy(&b,(char*)receiver_class+104,8);if(a!=guards[0]||b!=guards[1])return 20;
    }
    unsigned before=destroyed;objc_release(object);if(destroyed!=before+1)return 21;
  }
  return 0;
}
)";
  Out.close();
  for (const std::string Optimization : {"-O0", "-O2"}) {
    SCOPED_TRACE(Optimization);
    const std::string Compiler = NEVERD_TEST_CLANG;
    const std::vector<llvm::StringRef> Args = {
        Compiler, Optimization, "-std=c11",   "-fstack-protector-all",
        Path,     "-framework", "Foundation", "-lobjc",
        "-o",     Exe};
    const std::array<std::optional<llvm::StringRef>, 3> Redirects = {
        std::nullopt, std::nullopt, Err};
    std::string Error;
    const std::vector<llvm::StringRef> Syntax = {
        Compiler, Optimization, "-std=c11", "-fsyntax-only", CallerPath};
    const int SyntaxStatus = llvm::sys::ExecuteAndWait(
        Compiler, Syntax, std::nullopt, Redirects, 60, 0, &Error);
    std::ifstream SyntaxIn(Err);
    const std::string SyntaxDiagnostics(
        (std::istreambuf_iterator<char>(SyntaxIn)), {});
    ASSERT_EQ(SyntaxStatus, 0) << Error << SyntaxDiagnostics;
    int Status = llvm::sys::ExecuteAndWait(Compiler, Args, std::nullopt,
                                           Redirects, 60, 0, &Error);
    std::ifstream In(Err);
    std::string Diagnostics((std::istreambuf_iterator<char>(In)), {});
    ASSERT_EQ(Status, 0) << Error << Diagnostics;
    Status = llvm::sys::ExecuteAndWait(Exe, {Exe}, std::nullopt, Redirects, 60,
                                       0, &Error);
    ASSERT_EQ(Status, 0) << Error;
  }
#else
  GTEST_SKIP() << "requires Apple AArch64 and Clang";
#endif
}
} // namespace
