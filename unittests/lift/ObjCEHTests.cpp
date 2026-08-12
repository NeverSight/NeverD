//===- ObjCEHTests.cpp - Objective-C exception machinery tests ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// Coverage for the exception machinery of the three Objective-C runtimes.
///
/// All three emit an Itanium LSDA, so what these tests exercise is not a table
/// format but a reading of one: the type-table slot means something different
/// under each runtime, and under the GNU runtime it is not even a pointer.  A
/// fixture compiled by one toolchain could only ever show one of the three, so
/// the tables are assembled here byte by byte instead.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/loader/DWARF/ItaniumEH.h"
#include "neverd/loader/LanguageRuntime.h"
#include "neverd/loader/ObjC/ObjCEH.h"

#include <cstring>

using namespace neverd;

namespace {

//===----------------------------------------------------------------------===//
// Byte-buffer builder
//===----------------------------------------------------------------------===//

class ByteBuilder {
public:
  void u8(uint8_t V) { Bytes.push_back(V); }
  void u32(uint32_t V) { append(&V, sizeof(V)); }
  void u64(uint64_t V) { append(&V, sizeof(V)); }
  void i32(int32_t V) { append(&V, sizeof(V)); }

  void uleb(uint64_t V) {
    do {
      uint8_t Byte = V & 0x7f;
      V >>= 7;
      if (V)
        Byte |= 0x80;
      Bytes.push_back(Byte);
    } while (V);
  }

  void sleb(int64_t V) {
    bool More = true;
    while (More) {
      uint8_t Byte = V & 0x7f;
      V >>= 7;
      if ((V == 0 && !(Byte & 0x40)) || (V == -1 && (Byte & 0x40)))
        More = false;
      else
        Byte |= 0x80;
      Bytes.push_back(Byte);
    }
  }

  void str(const char *S) {
    while (*S)
      Bytes.push_back(static_cast<uint8_t>(*S++));
    Bytes.push_back(0);
  }

  size_t size() const { return Bytes.size(); }
  const std::vector<uint8_t> &data() const { return Bytes; }

  void patch32(size_t Offset, uint32_t Value) {
    std::memcpy(Bytes.data() + Offset, &Value, sizeof(Value));
  }

private:
  void append(const void *P, size_t N) {
    const auto *B = static_cast<const uint8_t *>(P);
    Bytes.insert(Bytes.end(), B, B + N);
  }
  std::vector<uint8_t> Bytes;
};

//===----------------------------------------------------------------------===//
// Image layout
//===----------------------------------------------------------------------===//

constexpr va_t kTextVA = 0x400000;
constexpr va_t kDataVA = 0x500000;
constexpr va_t kFuncVA = kTextVA + 0x100;
constexpr uint64_t kFuncSize = 0x80;
constexpr va_t kFrameVA = kDataVA;
constexpr va_t kLSDAVA = kDataVA + 0x400;
/// Where the fixtures place `objc_typeinfo` records and class-name strings.
constexpr va_t kDescriptorVA = kDataVA + 0x600;
constexpr va_t kStringVA = kDataVA + 0x700;
constexpr va_t kClassVA = kDataVA + 0x780;
/// Where the fixtures place the runtime entry points a body calls.
constexpr va_t kRuntimeVA = kTextVA + 0x800;

BinaryImage makeImage() {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Format = BinaryFormat::ELF;
  Img.Bits = Bitness::Bits64;
  Img.Base = kTextVA;
  Img.Entry = kTextVA;

  Segment Text;
  Text.Name = ".text";
  Text.VA = kTextVA;
  Text.Size = 0x1000;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(0x1000, 0x90);
  Img.Segments.push_back(std::move(Text));

  Segment Data;
  Data.Name = ".data";
  Data.VA = kDataVA;
  Data.Size = 0x1000;
  Data.Flags = SegmentFlags::Readable;
  Data.Data.assign(0x1000, 0);
  Img.Segments.push_back(std::move(Data));
  return Img;
}

void writeData(BinaryImage &Img, va_t VA, const std::vector<uint8_t> &Bytes) {
  ASSERT_TRUE(Img.writeVA(VA, Bytes.data(), Bytes.size()));
}

void addSymbol(BinaryImage &Img, const char *Name, va_t Addr,
               bool IsFunc = true) {
  Symbol S;
  S.Name = Name;
  S.Addr = Addr;
  S.IsFunc = IsFunc;
  Img.Symbols.push_back(std::move(S));
}

//===----------------------------------------------------------------------===//
// `.eh_frame`
//===----------------------------------------------------------------------===//

/// One CIE naming \p PersonalityVA plus one FDE covering the fixture function
/// and pointing at \ref kLSDAVA.
std::vector<uint8_t> buildFrame(va_t PersonalityVA) {
  ByteBuilder B;

  const size_t CIELengthSlot = B.size();
  B.u32(0);
  const size_t CIEBodyStart = B.size();
  B.u32(0); // CIE id
  B.u8(1);  // version
  B.str("zPLR");
  B.uleb(1);  // code alignment factor
  B.sleb(-8); // data alignment factor
  B.u8(16);   // return address register
  {
    ByteBuilder Aug;
    Aug.u8(0x00); // 'P': DW_EH_PE_absptr
    Aug.u64(PersonalityVA);
    Aug.u8(0x1b); // 'L': DW_EH_PE_pcrel | DW_EH_PE_sdata4
    Aug.u8(0x1b); // 'R'
    B.uleb(Aug.size());
    for (uint8_t Byte : Aug.data())
      B.u8(Byte);
  }
  B.u8(0x0c); // DW_CFA_def_cfa
  B.uleb(7);
  B.uleb(8);
  while ((B.size() - CIEBodyStart) % 8 != 0)
    B.u8(0);
  B.patch32(CIELengthSlot, static_cast<uint32_t>(B.size() - CIEBodyStart));

  const size_t FDELengthSlot = B.size();
  B.u32(0);
  const size_t FDEBodyStart = B.size();
  B.u32(static_cast<uint32_t>(FDEBodyStart));

  const va_t InitialLocVA = kFrameVA + B.size();
  B.i32(static_cast<int32_t>(static_cast<int64_t>(kFuncVA) -
                             static_cast<int64_t>(InitialLocVA)));
  B.u32(static_cast<uint32_t>(kFuncSize));
  {
    ByteBuilder Aug;
    const va_t LSDASlotVA = kFrameVA + B.size() + 1;
    Aug.i32(static_cast<int32_t>(static_cast<int64_t>(kLSDAVA) -
                                 static_cast<int64_t>(LSDASlotVA)));
    B.uleb(Aug.size());
    for (uint8_t Byte : Aug.data())
      B.u8(Byte);
  }
  B.u8(0x41); // DW_CFA_advance_loc 1
  while ((B.size() - FDEBodyStart) % 8 != 0)
    B.u8(0);
  B.patch32(FDELengthSlot, static_cast<uint32_t>(B.size() - FDEBodyStart));

  B.u32(0); // section terminator
  return B.data();
}

//===----------------------------------------------------------------------===//
// `.gcc_except_table`
//===----------------------------------------------------------------------===//

/// One protected region and the pad it reaches.
struct SiteSpec {
  uint32_t Start = 0;
  uint32_t Length = 0x10;
  uint32_t Pad = 0;
  /// 1-based action-table index, as a call site spells it.  Zero is a pad with
  /// no action at all, which is pure cleanup.
  uint64_t Action = 0;
};

/// An LSDA whose type table holds \p Slots, read as `DW_EH_PE_absptr`, and
/// whose action table holds one single-link catch record per slot: record
/// `2 * (I - 1)` selects slot `I`.  That is the shape clang emits for a `@try`
/// with one `@catch` clause per type.
std::vector<uint8_t> buildLSDA(const std::vector<va_t> &Slots,
                               const std::vector<SiteSpec> &Sites) {
  ByteBuilder B;
  B.u8(0xff); // landing-pad base defaults to the function start
  B.u8(0x00); // DW_EH_PE_absptr type-table entries

  ByteBuilder Body;
  {
    ByteBuilder CallSites;
    for (const SiteSpec &Site : Sites) {
      CallSites.u32(Site.Start);
      CallSites.u32(Site.Length);
      CallSites.u32(Site.Pad);
      CallSites.uleb(Site.Action);
    }
    Body.u8(0x03); // DW_EH_PE_udata4 call sites
    Body.uleb(CallSites.size());
    for (uint8_t Byte : CallSites.data())
      Body.u8(Byte);

    for (size_t I = 1; I <= Slots.size(); ++I) {
      Body.sleb(static_cast<int64_t>(I)); // catch on slot I
      Body.sleb(0);                       // end of chain
    }
  }

  // Entries grow downward from the base, so slot N is emitted first and the
  // base lands just past slot 1.
  const size_t TypeTableBaseOffset = Body.size() + 8 * Slots.size();
  B.uleb(TypeTableBaseOffset);
  const size_t AfterOffsetField = B.size();
  for (uint8_t Byte : Body.data())
    B.u8(Byte);
  for (size_t I = Slots.size(); I >= 1; --I)
    B.u64(Slots[I - 1]);
  EXPECT_EQ(AfterOffsetField + TypeTableBaseOffset, B.size());
  return B.data();
}

//===----------------------------------------------------------------------===//
// Objective-C fixtures
//===----------------------------------------------------------------------===//

/// Write an Apple `objc_typeinfo` at \ref kDescriptorVA.
///
/// The record is `{ const void **vtable; const char *name; Class cls; }`, and
/// its first two fields are laid out as `std::type_info`'s are precisely so
/// that one type table can hold both an Objective-C class and a C++ type.
void writeAppleDescriptor(BinaryImage &Img, const char *ClassName) {
  if (ClassName) {
    ByteBuilder Name;
    Name.str(ClassName);
    writeData(Img, kStringVA, Name.data());
  }
  ByteBuilder Desc;
  Desc.u64(kDataVA + 0x7f0);        // vtable: objc_ehtype_vtable + 2
  Desc.u64(ClassName ? kStringVA : 0);
  Desc.u64(kClassVA);
  writeData(Img, kDescriptorVA, Desc.data());
}

/// Write a bare class-name string at \ref kStringVA, which is the whole of
/// what a GNU-runtime type-table slot holds.
void writeNameString(BinaryImage &Img, const char *Text) {
  ByteBuilder Name;
  Name.str(Text);
  writeData(Img, kStringVA, Name.data());
}

/// Place a direct `call rel32` to \p TargetVA at \p SiteVA.
void writeCall(BinaryImage &Img, va_t SiteVA, va_t TargetVA) {
  ByteBuilder Call;
  Call.u8(0xe8);
  Call.i32(static_cast<int32_t>(static_cast<int64_t>(TargetVA) -
                               static_cast<int64_t>(SiteVA + 5)));
  writeData(Img, SiteVA, Call.data());
}

/// An image with one function whose frame installs \p Personality and whose
/// LSDA holds \p Slots and \p Sites.
BinaryImage makeObjCImage(const char *Personality,
                          const std::vector<va_t> &Slots,
                          const std::vector<SiteSpec> &Sites) {
  BinaryImage Img = makeImage();
  const va_t PersonalityVA = kTextVA + 0x900;
  addSymbol(Img, Personality, PersonalityVA);
  // Every Objective-C image sends messages; without this the runtime gate
  // would be answering the personality's question twice over.
  addSymbol(Img, "objc_msgSend", kTextVA + 0x910);

  const std::vector<uint8_t> Frame = buildFrame(PersonalityVA);
  writeData(Img, kFrameVA, Frame);

  Section EhFrame;
  EhFrame.Name = ".eh_frame";
  EhFrame.VA = kFrameVA;
  EhFrame.Size = Frame.size();
  EhFrame.Data = Frame;
  Img.Sections.push_back(std::move(EhFrame));

  writeData(Img, kLSDAVA, buildLSDA(Slots, Sites));
  return Img;
}

/// Decode \p Img and return its single exception record.
const ExceptionFunction &decode(BinaryImage &Img) {
  Img.ExceptionMetadata.Runtime = detectLanguageRuntime(Img);
  dwarf_eh::parseItaniumExceptions(Img);
  objc_eh::parseObjCExceptions(Img);
  EXPECT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
  return Img.ExceptionMetadata.Functions.front();
}

//===----------------------------------------------------------------------===//
// Personality identification
//===----------------------------------------------------------------------===//

TEST(ObjCPersonality, NamesEveryRuntimesRoutine) {
  struct Case {
    const char *Symbol;
    ExceptionPersonality Personality;
    ObjCRuntimeKind Runtime;
  } const Cases[] = {
      {"__objc_personality_v0", ExceptionPersonality::ObjCPersonalityV0,
       ObjCRuntimeKind::AppleNonFragile},
      {"__gnu_objc_personality_v0",
       ExceptionPersonality::GnuObjCPersonalityV0, ObjCRuntimeKind::GNU},
      {"__gnu_objc_personality_seh0",
       ExceptionPersonality::GnuObjCPersonalitySEH0, ObjCRuntimeKind::GNU},
      {"__gnu_objc_personality_sj0",
       ExceptionPersonality::GnuObjCPersonalitySJ0, ObjCRuntimeKind::GNU},
      {"__gnustep_objc_personality_v0",
       ExceptionPersonality::GNUstepObjCPersonalityV0, ObjCRuntimeKind::GNU},
      {"__gnustep_objcxx_personality_v0",
       ExceptionPersonality::GNUstepObjCXXPersonalityV0,
       ObjCRuntimeKind::GNUstepObjCXX},
  };
  for (const Case &C : Cases) {
    EXPECT_EQ(classifyPersonalityName(C.Symbol), C.Personality) << C.Symbol;
    EXPECT_EQ(getPersonalityRuntime(C.Personality),
              SourceLanguageRuntime::ObjectiveC)
        << C.Symbol;
    ASSERT_TRUE(getObjCRuntimeForPersonality(C.Personality).has_value())
        << C.Symbol;
    EXPECT_EQ(*getObjCRuntimeForPersonality(C.Personality), C.Runtime)
        << C.Symbol;
    EXPECT_TRUE(isItaniumPersonality(C.Personality)) << C.Symbol;
    EXPECT_STREQ(getExceptionPersonalityName(C.Personality), C.Symbol);
  }
}

TEST(ObjCPersonality, DetectsAGnuRuntimeImageWithNoAppleSections) {
  BinaryImage Img = makeImage();
  addSymbol(Img, "__gnu_objc_personality_v0", kTextVA + 0x900);
  const LanguageRuntimeInfo Info = detectLanguageRuntime(Img);
  EXPECT_TRUE(Info.is(SourceLanguageRuntime::ObjectiveC));
}

TEST(ObjCPersonality, DetectsAGnuRuntimeImageFromItsMessageSend) {
  BinaryImage Img = makeImage();
  addSymbol(Img, "objc_msg_lookup", kTextVA + 0x900);
  const LanguageRuntimeInfo Info = detectLanguageRuntime(Img);
  EXPECT_TRUE(Info.is(SourceLanguageRuntime::ObjectiveC));
}

//===----------------------------------------------------------------------===//
// Apple's non-fragile runtime
//===----------------------------------------------------------------------===//

TEST(ObjCAppleEH, ReadsAClassClauseThroughItsDescriptor) {
  BinaryImage Img =
      makeObjCImage("__objc_personality_v0", {kDescriptorVA},
                    {SiteSpec{0x10, 0x10, 0x40, /*Action=*/1}});
  writeAppleDescriptor(Img, "NSException");

  const ExceptionFunction &F = decode(Img);
  ASSERT_TRUE(F.ObjC.has_value());
  EXPECT_EQ(F.ObjC->Runtime, ObjCRuntimeKind::AppleNonFragile);
  ASSERT_EQ(F.ObjC->LandingPads.size(), 1u);

  const ObjCLandingPad &Pad = F.ObjC->LandingPads.front();
  EXPECT_EQ(Pad.Kind, ObjCPadKind::Catch);
  EXPECT_EQ(Pad.PadVA, kFuncVA + 0x40);
  ASSERT_EQ(Pad.Catches.size(), 1u);

  const ObjCCatchClause &Clause = Pad.Catches.front();
  EXPECT_EQ(Clause.Kind, ObjCCatchKind::Class);
  EXPECT_EQ(Clause.ClassName, "NSException");
  EXPECT_EQ(Clause.TypeInfoVA, kDescriptorVA);
  // The third field is Objective-C's own, and reading it is what turns a name
  // into the class object the image carries.
  EXPECT_EQ(Clause.ClassVA, kClassVA);
  EXPECT_FALSE(Clause.IsCxxType);
}

TEST(ObjCAppleEH, TellsCatchIdApartFromCatchAll) {
  // `@catch(id)` names `OBJC_EHTYPE_id`, a real symbol, and `@catch(...)` a
  // null slot.  The two are not the same handler: an `id` clause takes any
  // Objective-C object and lets a foreign exception continue past it.
  BinaryImage Img = makeObjCImage(
      "__objc_personality_v0", {kDescriptorVA, 0},
      {SiteSpec{0x10, 0x10, 0x40, /*Action=*/1},
       SiteSpec{0x20, 0x10, 0x50, /*Action=*/3}});
  // A descriptor imported from libobjc holds no name in this image; the symbol
  // that names it is all there is, which is exactly the real case.
  writeAppleDescriptor(Img, /*ClassName=*/nullptr);
  addSymbol(Img, "_OBJC_EHTYPE_id", kDescriptorVA, /*IsFunc=*/false);

  const ExceptionFunction &F = decode(Img);
  ASSERT_TRUE(F.ObjC.has_value());
  ASSERT_EQ(F.ObjC->LandingPads.size(), 2u);

  ASSERT_EQ(F.ObjC->LandingPads[0].Catches.size(), 1u);
  EXPECT_EQ(F.ObjC->LandingPads[0].Catches[0].Kind, ObjCCatchKind::AnyObject);
  EXPECT_TRUE(F.ObjC->LandingPads[0].Catches[0].ClassName.empty());

  ASSERT_EQ(F.ObjC->LandingPads[1].Catches.size(), 1u);
  EXPECT_EQ(F.ObjC->LandingPads[1].Catches[0].Kind, ObjCCatchKind::CatchAll);
}

TEST(ObjCAppleEH, KeepsACxxTypeInAnObjectiveCxxTableApart) {
  // One table holds both, because Apple's descriptor is `std::type_info`
  // shaped precisely so that it can.  A `catch (std::runtime_error &)` there
  // is not a clause any Objective-C object can satisfy.
  BinaryImage Img =
      makeObjCImage("__objc_personality_v0", {kDescriptorVA},
                    {SiteSpec{0x10, 0x10, 0x40, /*Action=*/1}});
  writeAppleDescriptor(Img, "St13runtime_error");
  addSymbol(Img, "_ZTISt13runtime_error", kDescriptorVA, /*IsFunc=*/false);

  const ExceptionFunction &F = decode(Img);
  ASSERT_TRUE(F.ObjC.has_value());
  ASSERT_EQ(F.ObjC->LandingPads.size(), 1u);
  ASSERT_EQ(F.ObjC->LandingPads[0].Catches.size(), 1u);
  EXPECT_TRUE(F.ObjC->LandingPads[0].Catches[0].IsCxxType);
}

TEST(ObjCAppleEH, UnwrapsAnImportedDescriptorsSymbolIntoAClassName) {
  BinaryImage Img =
      makeObjCImage("__objc_personality_v0", {kDescriptorVA},
                    {SiteSpec{0x10, 0x10, 0x40, /*Action=*/1}});
  writeAppleDescriptor(Img, /*ClassName=*/nullptr);
  addSymbol(Img, "_OBJC_EHTYPE_$_NSFileHandleOperationException",
            kDescriptorVA, /*IsFunc=*/false);

  const ExceptionFunction &F = decode(Img);
  ASSERT_TRUE(F.ObjC.has_value());
  ASSERT_EQ(F.ObjC->LandingPads.size(), 1u);
  ASSERT_EQ(F.ObjC->LandingPads[0].Catches.size(), 1u);
  const ObjCCatchClause &Clause = F.ObjC->LandingPads[0].Catches[0];
  EXPECT_EQ(Clause.Kind, ObjCCatchKind::Class);
  EXPECT_EQ(Clause.ClassName, "NSFileHandleOperationException");
}

TEST(ObjCAppleEH, LeavesACleanupPadWithoutClauses) {
  BinaryImage Img = makeObjCImage("__objc_personality_v0", {},
                                  {SiteSpec{0x10, 0x10, 0x40, /*Action=*/0}});
  const ExceptionFunction &F = decode(Img);
  ASSERT_TRUE(F.ObjC.has_value());
  ASSERT_EQ(F.ObjC->LandingPads.size(), 1u);
  EXPECT_EQ(F.ObjC->LandingPads[0].Kind, ObjCPadKind::Cleanup);
  EXPECT_TRUE(F.ObjC->LandingPads[0].Catches.empty());
  EXPECT_FALSE(F.ObjC->catchesAnything());
}

//===----------------------------------------------------------------------===//
// The GNU runtime
//===----------------------------------------------------------------------===//

TEST(ObjCGnuEH, ReadsTheClassNameOutOfTheSlotItself) {
  // The GNU runtime puts the class name string in the type-table slot rather
  // than the address of a descriptor.  Reading it the Itanium way -- as an
  // address whose `+ sizeof(void *)` field is a name pointer -- would follow a
  // pointer assembled out of the string's own characters.
  BinaryImage Img =
      makeObjCImage("__gnu_objc_personality_v0", {kStringVA},
                    {SiteSpec{0x10, 0x10, 0x40, /*Action=*/1}});
  writeNameString(Img, "MyException");

  const ExceptionFunction &F = decode(Img);
  ASSERT_TRUE(F.ObjC.has_value());
  EXPECT_EQ(F.ObjC->Runtime, ObjCRuntimeKind::GNU);
  ASSERT_EQ(F.ObjC->LandingPads.size(), 1u);
  ASSERT_EQ(F.ObjC->LandingPads[0].Catches.size(), 1u);

  const ObjCCatchClause &Clause = F.ObjC->LandingPads[0].Catches[0];
  EXPECT_EQ(Clause.Kind, ObjCCatchKind::Class);
  EXPECT_EQ(Clause.ClassName, "MyException");
  // Nothing is dereferenced, so there is no class object to name.
  EXPECT_EQ(Clause.ClassVA, 0u);
}

TEST(ObjCGnuEH, ReadsTheNonFragileCatchIdMarker) {
  // The non-fragile GNU ABI needs `@catch(id)` to be distinct from a real
  // catch-all so that a foreign exception is not swallowed, and spells the
  // difference with the string `@id`.
  BinaryImage Img =
      makeObjCImage("__gnu_objc_personality_v0", {kStringVA},
                    {SiteSpec{0x10, 0x10, 0x40, /*Action=*/1}});
  writeNameString(Img, "@id");

  const ExceptionFunction &F = decode(Img);
  ASSERT_TRUE(F.ObjC.has_value());
  ASSERT_EQ(F.ObjC->LandingPads.size(), 1u);
  ASSERT_EQ(F.ObjC->LandingPads[0].Catches.size(), 1u);
  EXPECT_EQ(F.ObjC->LandingPads[0].Catches[0].Kind, ObjCCatchKind::AnyObject);
}

TEST(ObjCGnuEH, ReadsTheFragileCatchAll) {
  // The fragile ABI had only one kind of catch-all and spelled it with a null
  // slot, which is what breaks foreign exceptions there.
  BinaryImage Img = makeObjCImage("__gnu_objc_personality_v0", {0},
                                  {SiteSpec{0x10, 0x10, 0x40, /*Action=*/1}});
  const ExceptionFunction &F = decode(Img);
  ASSERT_TRUE(F.ObjC.has_value());
  ASSERT_EQ(F.ObjC->LandingPads.size(), 1u);
  ASSERT_EQ(F.ObjC->LandingPads[0].Catches.size(), 1u);
  EXPECT_EQ(F.ObjC->LandingPads[0].Catches[0].Kind, ObjCCatchKind::CatchAll);
}

TEST(ObjCGnustepEH, ReadsAnObjectiveCxxTypeInfo) {
  // GNUstep's Objective-C++ routine puts a real `std::type_info` subclass in
  // the slot so that C++ and Objective-C types can share a table, which means
  // the slot is read exactly the way an Itanium one is.
  BinaryImage Img =
      makeObjCImage("__gnustep_objcxx_personality_v0", {kDescriptorVA},
                    {SiteSpec{0x10, 0x10, 0x40, /*Action=*/1}});
  writeAppleDescriptor(Img, "MyException");

  const ExceptionFunction &F = decode(Img);
  ASSERT_TRUE(F.ObjC.has_value());
  EXPECT_EQ(F.ObjC->Runtime, ObjCRuntimeKind::GNUstepObjCXX);
  ASSERT_EQ(F.ObjC->LandingPads.size(), 1u);
  ASSERT_EQ(F.ObjC->LandingPads[0].Catches.size(), 1u);

  const ObjCCatchClause &Clause = F.ObjC->LandingPads[0].Catches[0];
  EXPECT_EQ(Clause.Kind, ObjCCatchKind::Class);
  EXPECT_EQ(Clause.ClassName, "MyException");
  // GNUstep names a class by string and leaves the runtime to look it up, so
  // there is no class field to read even though the record has three words.
  EXPECT_EQ(Clause.ClassVA, 0u);
}

//===----------------------------------------------------------------------===//
// Runtime call sites
//===----------------------------------------------------------------------===//

TEST(ObjCRuntimeCalls, ClassifiesEveryEntryPointAFrameCanReach) {
  struct Case {
    const char *Symbol;
    ObjCRuntimeCallKind Kind;
  } const Cases[] = {
      {"objc_exception_throw", ObjCRuntimeCallKind::Throw},
      {"objc_exception_rethrow", ObjCRuntimeCallKind::Rethrow},
      {"objc_begin_catch", ObjCRuntimeCallKind::BeginCatch},
      {"objc_end_catch", ObjCRuntimeCallKind::EndCatch},
      {"objc_sync_enter", ObjCRuntimeCallKind::SyncEnter},
      {"objc_sync_exit", ObjCRuntimeCallKind::SyncExit},
      {"objc_terminate", ObjCRuntimeCallKind::Terminate},
      {"objc_release", ObjCRuntimeCallKind::ARCCleanup},
      {"objc_storeStrong", ObjCRuntimeCallKind::ARCCleanup},
      {"objc_exception_try_enter", ObjCRuntimeCallKind::FragileTry},
      {"objc_exception_match", ObjCRuntimeCallKind::FragileTry},
  };
  for (const Case &C : Cases) {
    BinaryImage Img =
        makeObjCImage("__objc_personality_v0", {},
                      {SiteSpec{0x10, 0x10, 0x40, /*Action=*/0}});
    addSymbol(Img, C.Symbol, kRuntimeVA);
    writeCall(Img, kFuncVA, kRuntimeVA);

    const ExceptionFunction &F = decode(Img);
    ASSERT_TRUE(F.ObjC.has_value()) << C.Symbol;
    ASSERT_EQ(F.ObjC->RuntimeCalls.size(), 1u) << C.Symbol;
    EXPECT_EQ(F.ObjC->RuntimeCalls[0].Kind, C.Kind) << C.Symbol;
    EXPECT_EQ(F.ObjC->RuntimeCalls[0].CallVA, kFuncVA) << C.Symbol;
    EXPECT_EQ(F.ObjC->RuntimeCalls[0].TargetVA, kRuntimeVA) << C.Symbol;
  }
}

TEST(ObjCRuntimeCalls, DoesNotTakeAReturnValueOptimizationForACleanup) {
  // `objc_releaseReturnValue` optimizes a return sequence; it is not cleanup,
  // and a prefix match would sweep it in with the releases that are.
  BinaryImage Img = makeObjCImage("__objc_personality_v0", {},
                                  {SiteSpec{0x10, 0x10, 0x40, /*Action=*/0}});
  addSymbol(Img, "objc_releaseReturnValue", kRuntimeVA);
  writeCall(Img, kFuncVA, kRuntimeVA);

  const ExceptionFunction &F = decode(Img);
  ASSERT_TRUE(F.ObjC.has_value());
  EXPECT_TRUE(F.ObjC->RuntimeCalls.empty());
}

TEST(ObjCRuntimeCalls, MarksThePadThatClosesASynchronizedBody) {
  BinaryImage Img = makeObjCImage("__objc_personality_v0", {},
                                  {SiteSpec{0x10, 0x10, 0x40, /*Action=*/0}});
  addSymbol(Img, "objc_sync_enter", kRuntimeVA);
  addSymbol(Img, "objc_sync_exit", kRuntimeVA + 0x10);
  writeCall(Img, kFuncVA + 0x10, kRuntimeVA);
  writeCall(Img, kFuncVA + 0x40, kRuntimeVA + 0x10);

  const ExceptionFunction &F = decode(Img);
  ASSERT_TRUE(F.ObjC.has_value());
  EXPECT_TRUE(F.ObjC->guardsASynchronizedBody());
  ASSERT_EQ(F.ObjC->LandingPads.size(), 1u);
  EXPECT_EQ(F.ObjC->LandingPads[0].Kind, ObjCPadKind::SynchronizedExit);
  ASSERT_TRUE(Img.ExceptionMetadata.ObjCRuntime.has_value());
  EXPECT_EQ(Img.ExceptionMetadata.ObjCRuntime->SynchronizedFrames, 1u);
}

TEST(ObjCRuntimeCalls, ClaimsAFragileTryThatHasNoTableAtAll) {
  // The fragile runtime's `@try` is a setjmp buffer and a chain of matches, so
  // the frame carries no landing pad and its calls are the only evidence it
  // handles anything.  Its personality is C's, not Objective-C's.
  BinaryImage Img = makeObjCImage("__gcc_personality_v0", {},
                                  {SiteSpec{0x10, 0x10, 0x00, /*Action=*/0}});
  addSymbol(Img, "objc_exception_try_enter", kRuntimeVA);
  addSymbol(Img, "objc_exception_extract", kRuntimeVA + 0x10);
  writeCall(Img, kFuncVA, kRuntimeVA);
  writeCall(Img, kFuncVA + 0x20, kRuntimeVA + 0x10);

  const ExceptionFunction &F = decode(Img);
  ASSERT_TRUE(F.ObjC.has_value());
  EXPECT_TRUE(F.ObjC->UsesFragileSetjmp);
  EXPECT_TRUE(F.ObjC->LandingPads.empty());
  ASSERT_TRUE(Img.ExceptionMetadata.ObjCRuntime.has_value());
  EXPECT_EQ(Img.ExceptionMetadata.ObjCRuntime->FragileTryFrames, 1u);
}

TEST(ObjCRuntimeCalls, CountsThrowSitesButNotResumes) {
  BinaryImage Img = makeObjCImage("__objc_personality_v0", {},
                                  {SiteSpec{0x10, 0x10, 0x40, /*Action=*/0}});
  addSymbol(Img, "objc_exception_throw", kRuntimeVA);
  addSymbol(Img, "objc_exception_rethrow", kRuntimeVA + 0x10);
  writeCall(Img, kFuncVA, kRuntimeVA);
  writeCall(Img, kFuncVA + 0x20, kRuntimeVA + 0x10);

  const ExceptionFunction &F = decode(Img);
  ASSERT_TRUE(F.ObjC.has_value());
  EXPECT_TRUE(F.ObjC->throwsAnException());
  ASSERT_TRUE(Img.ExceptionMetadata.ObjCRuntime.has_value());
  EXPECT_EQ(Img.ExceptionMetadata.ObjCRuntime->ThrowSites, 1u);
}

//===----------------------------------------------------------------------===//
// Image-wide state
//===----------------------------------------------------------------------===//

TEST(ObjCRuntimeState, PrefersThePersonalityToTheImagesShape) {
  BinaryImage Img =
      makeObjCImage("__gnu_objc_personality_v0", {kStringVA},
                    {SiteSpec{0x10, 0x10, 0x40, /*Action=*/1}});
  writeNameString(Img, "MyException");
  decode(Img);

  ASSERT_TRUE(Img.ExceptionMetadata.ObjCRuntime.has_value());
  EXPECT_EQ(Img.ExceptionMetadata.ObjCRuntime->Runtime, ObjCRuntimeKind::GNU);
  EXPECT_TRUE(
      Img.ExceptionMetadata.ObjCRuntime->RuntimeProvenByPersonality);
}

TEST(ObjCRuntimeState, RecognizesARC) {
  BinaryImage Img = makeObjCImage("__objc_personality_v0", {},
                                  {SiteSpec{0x10, 0x10, 0x40, /*Action=*/0}});
  addSymbol(Img, "objc_storeStrong", kRuntimeVA);
  decode(Img);
  ASSERT_TRUE(Img.ExceptionMetadata.ObjCRuntime.has_value());
  EXPECT_TRUE(Img.ExceptionMetadata.ObjCRuntime->UsesARC);
}

TEST(ObjCRuntimeState, IgnoresAnImageWithoutTheObjectiveCRuntime) {
  BinaryImage Img = makeImage();
  addSymbol(Img, "__gxx_personality_v0", kTextVA + 0x900);
  objc_eh::parseObjCExceptions(Img);
  EXPECT_FALSE(Img.ExceptionMetadata.ObjCRuntime.has_value());
}

TEST(ObjCRuntimeState, LeavesACxxFrameAlone) {
  // A C++ frame in an image that also contains Objective-C is still a C++
  // frame: its personality said so, and nothing it calls says otherwise.
  BinaryImage Img =
      makeObjCImage("__gxx_personality_v0", {kDescriptorVA},
                    {SiteSpec{0x10, 0x10, 0x40, /*Action=*/1}});
  writeAppleDescriptor(Img, "St13runtime_error");

  const ExceptionFunction &F = decode(Img);
  EXPECT_FALSE(F.ObjC.has_value());
}

//===----------------------------------------------------------------------===//
// setjmp/longjmp call-site tables
//===----------------------------------------------------------------------===//

TEST(ObjCSJLJ, RefusesToReadIndexFormCallSitesAsAddresses) {
  // Under the SJLJ form the first two columns of a call-site record are an
  // index and an action, not an address and a length.  A reader that applies
  // the address form does not fail; it invents guarded ranges and landing pads
  // the program never named.  Every SJLJ personality has to be recognized for
  // that not to happen, not just C++'s.
  for (const char *Personality :
       {"__gxx_personality_sj0", "__gcc_personality_sj0",
        "__gnu_objc_personality_sj0"}) {
    BinaryImage Img = makeObjCImage(
        Personality, {}, {SiteSpec{0x00, 0x01, 0x02, /*Action=*/0}});
    Img.ExceptionMetadata.Runtime = detectLanguageRuntime(Img);
    dwarf_eh::parseItaniumExceptions(Img);

    ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u) << Personality;
    const ExceptionFunction &F = Img.ExceptionMetadata.Functions.front();
    ASSERT_TRUE(F.Itanium.has_value()) << Personality;
    EXPECT_FALSE(F.Itanium->IsCallSiteAddressForm) << Personality;
    EXPECT_TRUE(F.Itanium->CallSites.empty()) << Personality;
    EXPECT_EQ(F.ParseStatus, ExceptionParseStatus::Partial) << Personality;
  }
}

TEST(ObjCSJLJ, StillReadsTheAddressFormForANonSJLJPersonality) {
  BinaryImage Img =
      makeObjCImage("__gnu_objc_personality_v0", {},
                    {SiteSpec{0x10, 0x10, 0x40, /*Action=*/0}});
  const ExceptionFunction &F = decode(Img);
  ASSERT_TRUE(F.Itanium.has_value());
  EXPECT_TRUE(F.Itanium->IsCallSiteAddressForm);
  ASSERT_EQ(F.Itanium->CallSites.size(), 1u);
  EXPECT_EQ(F.Itanium->CallSites[0].LandingPadVA, kFuncVA + 0x40);
}

} // namespace
