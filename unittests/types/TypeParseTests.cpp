//===- TypeParseTests.cpp - Local types from C and C++ source -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "TypeShape.h"

#include "neverd/types/Limits.h"
#include "neverd/types/PDBTypeImport.h"
#include "neverd/types/TypeParse.h"
#include "neverd/types/TypeRender.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"

#include <cstdlib>
#include <gtest/gtest.h>
#include <map>
#include <optional>
#include <utility>

using namespace neverd::types;
using neverd::types::test::shapeOf;

namespace {

/// \p Source read into a new library, every problem a failure.
TypeLibrary parsed(llvm::StringRef Source, ParseOptions Options = {},
                   DataModel Model = DataModel::LLP64,
                   std::optional<RecordABI> ABI = std::nullopt) {
  TypeLibrary Library(Model);
  if (ABI)
    Library.setRecordABI(*ABI);
  const ParseResult Result = parseTypes(Source, Library, Options);
  for (const SourceProblem &Problem : Result.Problems)
    ADD_FAILURE() << Problem.Line << ":" << Problem.Column << ": "
                  << Problem.Message;
  return Library;
}

const RecordDef &recordOf(const TypeLibrary &Library, llvm::StringRef Name) {
  static const RecordDef Empty;
  const LocalType *Type = Library.find(Name);
  EXPECT_NE(Type, nullptr) << Name.str();
  return Type && Type->Record ? *Type->Record : Empty;
}

/// Every named member with its bit offset and width, anonymous records
/// looked through: what a layout is, whatever its nesting.
std::string leaves(const RecordDef &Record, uint64_t Base = 0) {
  std::string Out;
  for (const Member &Field : Record.Members) {
    if (Field.isAnonymous()) {
      Out += leaves(*Field.Type->Record, Base + Field.BitOffset);
      continue;
    }
    Out += Field.Name + "@" + std::to_string(Base + Field.BitOffset);
    if (Field.isBitfield())
      Out += ":" + std::to_string(Field.BitWidth);
    Out += " ";
  }
  return Out;
}

} // namespace

TEST(TypeParse, LaysAStructureOutAsMSVC) {
  const TypeLibrary Library = parsed(R"(
    struct A { char c; int i; short s; };
    struct B { unsigned long a : 4; unsigned long b : 4; unsigned short c : 3;
               unsigned long d : 1; };
  )");
  const RecordDef &A = recordOf(Library, "A");
  EXPECT_EQ(shapeOf(A), "struct{c@0 i@4 s@8}");
  EXPECT_EQ(A.Size, 12u);
  // A bitfield of another size begins a unit of its own.
  const RecordDef &B = recordOf(Library, "B");
  EXPECT_EQ(shapeOf(B), "struct{a@0b:4 b@4b:4 c@32b:3 d@64b:1}");
  EXPECT_EQ(B.Size, 12u);
}

TEST(TypeParse, ReportsForwardRecordsWithoutInventingTheirLayout) {
  TypeLibrary Library;
  const auto Read = parseCTypes(R"(
    struct Handle;
    union Entry;
    typedef struct Handle *HANDLE;
    void Use(HANDLE Value, union Entry *Item);
  )",
                                Library);
  ASSERT_TRUE(Read.Problems.empty());
  EXPECT_EQ(Read.Types,
            (std::vector<std::string>{"Handle", "Entry", "HANDLE"}));
  EXPECT_FALSE(recordOf(Library, "Handle").Defined);
  EXPECT_FALSE(recordOf(Library, "Entry").Defined);
  EXPECT_FALSE(Library.sizeOf(*makeNamed("Handle", TypeKind::Struct)));
  EXPECT_EQ(Library.sizeOf(*makeNamed("HANDLE", TypeKind::Typedef)), 8u);
  RenderOptions Options;
  Options.Prototypes = true;
  TypeLibrary Restored;
  const auto Again = parseCTypes(renderTypes(Library, {}, Options), Restored);
  ASSERT_TRUE(Again.Problems.empty());
  EXPECT_TRUE(llvm::is_contained(Again.Types, "Handle"));
  EXPECT_TRUE(llvm::is_contained(Again.Types, "Entry"));
  EXPECT_FALSE(recordOf(Restored, "Handle").Defined);
  EXPECT_FALSE(Restored.sizeOf(*makeNamed("Handle", TypeKind::Struct)));
  ASSERT_NE(Restored.findPrototype("Use"), nullptr);
  EXPECT_EQ(Restored.findPrototype("Use")->Params.size(), 2u);
}

TEST(TypeParse, ReadsOneFunctionsDeclarationAgainstTheLibrary) {
  // IDA's Y: one declaration, its semicolon optional, of the library's types.
  const TypeLibrary Library = parsed(R"(
    typedef struct _DEV { unsigned short Type, Size; } DEV, *PDEV;
    enum MODE { Kernel, User };
  )");
  const size_t Types = Library.types().size();
  FunctionDeclaration Read = parseFunctionDeclaration(
      "long __stdcall Dispatch(PDEV Device, enum MODE Mode, void *Irp)",
      Library);
  ASSERT_TRUE(Read.ok()) << Read.Problems.front().Message;
  EXPECT_EQ(Read.Name, "Dispatch");
  ASSERT_EQ(Read.Function->Params.size(), 3u);
  EXPECT_EQ(Read.Function->Params[0].Name, "Device");
  // As 64-bit x86 ignores __stdcall, so does a 64-bit library.
  EXPECT_EQ(Read.Function->Convention, CallingConvention::Unspecified);
  EXPECT_TRUE(
      parseFunctionDeclaration("void f(struct _DEV *d);", Library).ok());
  // A definition's line, as the decompiler prints it, reads too.
  EXPECT_TRUE(parseFunctionDeclaration("int f(DEV *d) {}", Library).ok());
  // Reading changes nothing.
  EXPECT_EQ(Library.types().size(), Types);
  EXPECT_EQ(Library.findPrototype("Dispatch"), nullptr);

  const auto problem = [&](llvm::StringRef Source) {
    const FunctionDeclaration Bad = parseFunctionDeclaration(Source, Library);
    EXPECT_FALSE(Bad.ok()) << Source.str();
    EXPECT_FALSE(Bad.Function) << Source.str();
    return Bad.Problems.empty() ? std::string()
                                : std::to_string(Bad.Problems.front().Column) +
                                      ": " + Bad.Problems.front().Message;
  };
  EXPECT_EQ(problem("int f(NOPE x)"), "7: 'NOPE' names no type");
  // C lets a declaration name a structure before it is defined; the library
  // must define it.
  EXPECT_NE(problem("int f(struct NOPE *x)").find("NOPE is not a local type"),
            std::string::npos);
  EXPECT_NE(problem("int f(struct NEW { int a; } *x)").find("NEW"),
            std::string::npos);
  EXPECT_NE(problem("int f(void); int g(void)").find("declare one"),
            std::string::npos);
  EXPECT_NE(problem("typedef int T").find("declares no function"),
            std::string::npos);
  EXPECT_NE(problem("int f(int a").find("expected ')'"), std::string::npos);
}

TEST(TypeParse, ExpandedSpellingsSurviveDirectivesAndTheInput) {
  TypeLibrary Library(DataModel::ILP32);
  std::string Source = R"(
#pragma neverd_header("vendor/expanded_types.h")
#pragma neverd_default_calling_convention(__stdcall)
#define WORD unsigned \
int
#define RECORD FirstRecordWithAnExpandedName
#define MEMBER MemberWithAnExpandedName
#define LINK_NAME "external_expanded" "_routine"
#pragma pack(push, saved_layout, 1)
struct RECORD { char Lead; WORD MEMBER; };
int ExpandedRoutine(struct RECORD *Argument) __asm__(LINK_NAME);
#undef WORD
#undef RECORD
#undef MEMBER
#undef LINK_NAME
#define WORD unsigned long long
#define RECORD SecondRecordWithAnExpandedName
#define MEMBER ReplacementMemberWithAnExpandedName
#pragma pack(pop, saved_layout)
#pragma neverd_header("vendor/replacement_types.h")
struct RECORD { char Lead; WORD MEMBER; };
#undef WORD
#undef RECORD
#undef MEMBER
)";
  const auto Result = parseCTypes(Source, Library);
  ASSERT_TRUE(Result.ok());
  // The library owns its names, headers and prototypes after the lexer,
  // macro table, spelling arena and caller's input have all gone away.
  Source.assign(Source.size(), '#');
  EXPECT_EQ(shapeOf(recordOf(Library, "FirstRecordWithAnExpandedName")),
            "struct{Lead@0 MemberWithAnExpandedName@1}");
  EXPECT_EQ(shapeOf(recordOf(Library, "SecondRecordWithAnExpandedName")),
            "struct{Lead@0 ReplacementMemberWithAnExpandedName@8}");
  EXPECT_EQ(Library.find("FirstRecordWithAnExpandedName")->Header,
            "vendor/expanded_types.h");
  EXPECT_EQ(Library.find("SecondRecordWithAnExpandedName")->Header,
            "vendor/replacement_types.h");
  EXPECT_TRUE(
      llvm::is_contained(Result.Types, "FirstRecordWithAnExpandedName"));
  for (const char *Name : {"ExpandedRoutine", "external_expanded_routine"}) {
    const auto *Function = Library.findPrototype(Name);
    ASSERT_NE(Function, nullptr) << Name;
    ASSERT_EQ(Function->Params.size(), 1u);
    EXPECT_EQ(Function->Params[0].Name, "Argument");
    EXPECT_EQ(Function->Convention, CallingConvention::Stdcall);
  }
}

TEST(TypeParse, MacroExpansionKeepsItsDepthBoundaryAndInvocationPosition) {
  const unsigned Limit = neverd::limits::types::kCTypeParserExpandDepthLimit;
  const auto Chain = [](unsigned Last) {
    std::string Source;
    for (unsigned Index = 0; Index <= Last; ++Index)
      Source +=
          "#define M" + std::to_string(Index) + " " +
          (Index == Last ? "unsigned int" : "M" + std::to_string(Index + 1)) +
          "\n";
    return Source + "typedef M0 Expanded;\n";
  };
  TypeLibrary Library;
  const auto Allowed = parseCTypes(Chain(Limit), Library);
  ASSERT_TRUE(Allowed.ok());
  ASSERT_NE(Library.find("Expanded"), nullptr);

  TypeLibrary TooDeep;
  const auto Refused = parseCTypes(Chain(Limit + 1), TooDeep);
  ASSERT_EQ(Refused.Problems.size(), 1u);
  EXPECT_EQ(Refused.Problems[0].Line, Limit + 3);
  EXPECT_EQ(Refused.Problems[0].Column, 9u);
  EXPECT_EQ(TooDeep.find("Expanded"), nullptr);

  const auto Cycle = parseCTypes("#define FIRST SECOND\n#define SECOND FIRST\n"
                                 "typedef FIRST Cyclic;\n",
                                 Library);
  ASSERT_EQ(Cycle.Problems.size(), 1u);
  EXPECT_EQ(Cycle.Problems[0].Line, 3u);
  EXPECT_EQ(Cycle.Problems[0].Column, 9u);
  EXPECT_EQ(Library.find("Cyclic"), nullptr);
  EXPECT_NE(Library.find("Expanded"), nullptr);
}

TEST(TypeParse, ExpandedDiagnosticsOwnTheirTextAndInvocationPosition) {
  TypeLibrary Library;
  std::string Source = "#define UNKNOWN ALongTypeThatWasNeverDeclared\n"
                       "struct Invalid {\n  UNKNOWN Member;\n};\n"
                       "#undef UNKNOWN\n";
  const auto Result = parseCTypes(Source, Library);
  Source.assign(Source.size(), '#');
  ASSERT_EQ(Result.Problems.size(), 1u);
  EXPECT_EQ(Result.Problems[0].Line, 3u);
  EXPECT_EQ(Result.Problems[0].Column, 3u);
  EXPECT_EQ(Result.Problems[0].Message,
            "'ALongTypeThatWasNeverDeclared' names no type");
  EXPECT_EQ(Library.find("Invalid"), nullptr);

  const auto Quoted =
      parseCTypes("#define MESSAGE \"expanded\\n\" \"diagnostic\"\n"
                  "_Static_assert(0, MESSAGE);\n#undef MESSAGE\n",
                  Library);
  ASSERT_EQ(Quoted.Problems.size(), 1u);
  EXPECT_EQ(Quoted.Problems[0].Line, 2u);
  EXPECT_EQ(Quoted.Problems[0].Message,
            "a static assertion fails: expanded\ndiagnostic");
}

TEST(TypeParse, OneDeclarationRetainsExpandedParameterNames) {
  const TypeLibrary Library;
  std::string Source =
      "#define PARAMETER unsigned long long ExpandedParameter\n"
      "int ExpandedFunction(PARAMETER)";
  const auto Result = parseFunctionDeclaration(Source, Library);
  Source.assign(Source.size(), '#');
  ASSERT_TRUE(Result.ok());
  EXPECT_EQ(Result.Name, "ExpandedFunction");
  ASSERT_EQ(Result.Function->Params.size(), 1u);
  EXPECT_EQ(Result.Function->Params[0].Name, "ExpandedParameter");
  ASSERT_TRUE(Result.Function->Params[0].Type);
  EXPECT_EQ(Result.Function->Params[0].Type->TheBuiltin, Builtin::ULongLong);
}

TEST(TypeParse, BitfieldsShareUnitsUnderItanium) {
  const TypeLibrary Library = parsed(
      "struct B { unsigned a : 4; unsigned short c : 3; unsigned d : 30; };",
      {}, DataModel::LP64);
  const RecordDef &B = recordOf(Library, "B");
  // c fits after a; d would cross a 32-bit unit and takes the next.
  EXPECT_EQ(shapeOf(B), "struct{a@0b:4 c@4b:3 d@32b:30}");
  EXPECT_EQ(B.Size, 8u);
}

TEST(TypeParse, ThirtyTwoBitRecordsLayOutAsTheirABISays) {
  // One declaration, three 32-bit ABIs: the i386 System V ABI packs the
  // bit-field into the byte after c and aligns long long and double to
  // four, Microsoft's compilers give the bit-field a unit of its own and
  // align them to eight, as ARM's do after packing the bit-field.
  const char *Source = "struct S { char c; unsigned int b : 4; long long q; "
                       "double d; short t; };";
  const RecordDef I386 = recordOf(
      parsed(Source, {}, DataModel::ILP32, RecordABI::I386SystemV), "S");
  EXPECT_EQ(shapeOf(I386), "struct{c@0 b@8b:4 q@4 d@12 t@20}");
  EXPECT_EQ(I386.Size, 24u);
  const RecordDef Microsoft =
      recordOf(parsed(Source, {}, DataModel::ILP32), "S");
  EXPECT_EQ(shapeOf(Microsoft), "struct{c@0 b@32b:4 q@8 d@16 t@24}");
  EXPECT_EQ(Microsoft.Size, 32u);
  const RecordDef ARM =
      recordOf(parsed(Source, {}, DataModel::ILP32, RecordABI::AAPCS32), "S");
  EXPECT_EQ(shapeOf(ARM), "struct{c@0 b@8b:4 q@8 d@16 t@24}");
  EXPECT_EQ(ARM.Size, 32u);

  // A long long bit-field spans two units of four bytes on i386, where
  // ARM's may not cross one of eight.
  const char *Wide =
      "struct W { char c[5]; unsigned long long x : 40; char after; };";
  const RecordDef I386Wide =
      recordOf(parsed(Wide, {}, DataModel::ILP32, RecordABI::I386SystemV), "W");
  EXPECT_EQ(shapeOf(I386Wide), "struct{c@0 x@40b:40 after@10}");
  EXPECT_EQ(I386Wide.Members[1].StorageBitOffset, 32u);
  EXPECT_EQ(I386Wide.Size, 12u);
  const RecordDef ARMWide =
      recordOf(parsed(Wide, {}, DataModel::ILP32, RecordABI::AAPCS32), "W");
  EXPECT_EQ(shapeOf(ARMWide), "struct{c@0 x@64b:40 after@13}");
  EXPECT_EQ(ARMWide.Size, 16u);
}

TEST(TypeParse, WideCharactersAndLongDoublesAreAsLargeAsTheTargetMakesThem) {
  // A data model says neither: wchar_t is Windows' 16-bit character and four
  // bytes elsewhere, and long double is a double to Microsoft's compilers,
  // ARM's, Apple's 64-bit ARM and Android's i386, the x87's 12 bytes on
  // i386 System V and 16 on the 64-bit System V targets.  Clang lays each
  // out so for its target.
  const char *Source =
      "struct L { char c; long double d; wchar_t w; char e; };";
  const auto layout = [&](DataModel Model, RecordABI ABI) {
    const RecordDef Record = recordOf(parsed(Source, {}, Model, ABI), "L");
    return shapeOf(Record) + " " + std::to_string(Record.Size);
  };
  EXPECT_EQ(layout(DataModel::ILP32, RecordABI::Microsoft),
            "struct{c@0 d@8 w@16 e@18} 24");
  EXPECT_EQ(layout(DataModel::LLP64, RecordABI::Microsoft),
            "struct{c@0 d@8 w@16 e@18} 24");
  EXPECT_EQ(layout(DataModel::ILP32, RecordABI::I386SystemV),
            "struct{c@0 d@4 w@16 e@20} 24");
  EXPECT_EQ(layout(DataModel::ILP32, RecordABI::AndroidI386),
            "struct{c@0 d@4 w@12 e@16} 20");
  EXPECT_EQ(layout(DataModel::ILP32, RecordABI::AAPCS32),
            "struct{c@0 d@8 w@16 e@20} 24");
  EXPECT_EQ(layout(DataModel::LP64, RecordABI::Itanium),
            "struct{c@0 d@16 w@32 e@36} 48");
  EXPECT_EQ(layout(DataModel::LP64, RecordABI::AppleARM64),
            "struct{c@0 d@8 w@16 e@20} 24");
  // MinGW's compilers lay Microsoft's records out with the x87's long
  // double, which on x86 aligns to four where a double aligns to eight.
  EXPECT_EQ(layout(DataModel::LLP64, RecordABI::MinGW64),
            "struct{c@0 d@16 w@32 e@34} 48");
  EXPECT_EQ(layout(DataModel::ILP32, RecordABI::MinGW32),
            "struct{c@0 d@4 w@16 e@18} 20");
}

TEST(TypeParse, ReadsAnonymousRecordsAndWindowsNames) {
  const TypeLibrary Library = parsed(R"(
    struct _THREAD_LIKE
    {
        ULONGLONG Header;                       // 0x00
        union                                   // 0x08
        {
            struct
            {
                ULONG AutoBoostActive : 1;      // 0x08 bit 0
                ULONG MiscSpare : 31;           // 0x08 bits 1..31
            };
            LONG MiscFlags;                     // 0x08
        };
        UCHAR Tail[3];                          // 0x0C
    };
  )");
  const RecordDef &Thread = recordOf(Library, "_THREAD_LIKE");
  EXPECT_EQ(shapeOf(Thread),
            "struct{Header@0 union@8{struct@0{AutoBoostActive@0b:1 "
            "MiscSpare@1b:31} MiscFlags@0} Tail@12}");
  EXPECT_EQ(Thread.Size, 16u);
}

TEST(TypeParse, ReadsMicrosoftsSizedIntegersAsIDAWritesThem) {
  // IDA's local types spell builtins by Microsoft's sized integers.
  const TypeLibrary Library = parsed(R"(
    struct _IDA_LIKE
    {
        unsigned __int8 Type;
        __int16 Size;
        unsigned int Flags;
        __int64 Count;
        unsigned __int64 Mask;
    };
  )");
  const RecordDef &Record = recordOf(Library, "_IDA_LIKE");
  EXPECT_EQ(shapeOf(Record), "struct{Type@0 Size@2 Flags@4 Count@8 Mask@16}");
  EXPECT_EQ(Record.Size, 24u);
  const auto builtinOf = [&](size_t Index) {
    return Record.Members[Index].Type->TheBuiltin;
  };
  EXPECT_EQ(builtinOf(0), Builtin::UChar);
  EXPECT_EQ(builtinOf(1), Builtin::Short);
  EXPECT_EQ(builtinOf(2), Builtin::UInt);
  EXPECT_EQ(builtinOf(3), Builtin::LongLong);
  EXPECT_EQ(builtinOf(4), Builtin::ULongLong);
}

TEST(TypeParse, MicrosoftSizedByteKeepsPlainCharIdentity) {
  // A same-width signed substitute conflicts with the native ARM64 volatile
  // byte intrinsics and changes compatibility with plain-char pointers.
  constexpr llvm::StringLiteral Source = R"(
    __int8 Plain(const volatile __int8 *Value);
    signed __int8 Signed(const volatile signed __int8 *Value);
    unsigned __int8 Unsigned(const volatile unsigned __int8 *Value);
    void Store(volatile __int8 *Where, __int8 Value);
  )";
  for (DataModel Model :
       {DataModel::ILP32, DataModel::LLP64, DataModel::LP64}) {
    const TypeLibrary Library = parsed(Source, {}, Model);
    const auto Check = [](const TypeLibrary &Types) {
      for (auto [Name, Expected] : {std::pair{"Plain", Builtin::Char},
                                    {"Signed", Builtin::SChar},
                                    {"Unsigned", Builtin::UChar}}) {
        const FunctionDef *Function = Types.findPrototype(Name);
        ASSERT_NE(Function, nullptr);
        ASSERT_TRUE(Function->Return);
        EXPECT_EQ(Function->Return->TheBuiltin, Expected) << Name;
        ASSERT_EQ(Function->Params.size(), 1u);
        const auto &Pointer = Function->Params.front().Type;
        ASSERT_TRUE(Pointer);
        ASSERT_EQ(Pointer->Kind, TypeExprKind::Pointer);
        ASSERT_TRUE(Pointer->Element);
        EXPECT_EQ(Pointer->Element->TheBuiltin, Expected) << Name;
        EXPECT_TRUE(Pointer->Element->Const);
        EXPECT_TRUE(Pointer->Element->Volatile);
      }
      const FunctionDef *Store = Types.findPrototype("Store");
      ASSERT_NE(Store, nullptr);
      ASSERT_EQ(Store->Params.size(), 2u);
      const auto &Pointer = Store->Params[0].Type;
      ASSERT_TRUE(Pointer && Pointer->Element);
      EXPECT_EQ(Pointer->Element->TheBuiltin, Builtin::Char);
      EXPECT_FALSE(Pointer->Element->Const);
      EXPECT_TRUE(Pointer->Element->Volatile);
      EXPECT_EQ(Store->Params[1].Type->TheBuiltin, Builtin::Char);
    };
    Check(Library);
    for (BuiltinSpelling Spelling :
         {BuiltinSpelling::Keywords, BuiltinSpelling::Windows,
          BuiltinSpelling::Portable}) {
      RenderOptions Options;
      Options.Prototypes = true;
      Options.Spelling = Spelling;
      const TypeLibrary Again =
          parsed(renderTypes(Library, {}, Options), {}, Model);
      Check(Again);
    }
  }
}

TEST(TypeParse, KeepsPackingAndDeclaredAlignment) {
  const TypeLibrary Library = parsed(R"(
    #pragma pack(push, 1)
    struct P { UCHAR a; ULONG b; USHORT c; };
    #pragma pack(pop)
    struct DECLSPEC_ALIGN(16) Q { ULONGLONG Next; };
    struct R { UCHAR a; struct Q q; };
  )");
  const RecordDef &P = recordOf(Library, "P");
  EXPECT_EQ(shapeOf(P), "struct{a@0 b@1 c@5}");
  EXPECT_EQ(P.Size, 7u);
  EXPECT_EQ(P.Pack, 1u);
  EXPECT_EQ(recordOf(Library, "Q").Size, 16u);
  EXPECT_EQ(recordOf(Library, "Q").Alignment, 16u);
  EXPECT_EQ(shapeOf(recordOf(Library, "R")), "struct{a@0 q@16}");
}

TEST(TypeParse, KeepsADeclaredAlignmentUnderPack) {
  // Microsoft's compiler lowers a type's declared alignment for no pack, as
  // clang's MSVC layout shows: x at 16 in a record of 32 bytes.
  const TypeLibrary Library = parsed(R"(
    typedef struct __declspec(align(16)) _M128A {
      ULONGLONG Low; LONGLONG High;
    } M128A;
    #pragma pack(push, 4)
    struct P4 { CHAR c; M128A x; };
    struct P4m { CHAR c; __declspec(align(8)) LONGLONG y; };
    struct P4n { CHAR c; ULONGLONG z; };
    #pragma pack(pop)
  )");
  EXPECT_EQ(shapeOf(recordOf(Library, "P4")), "struct{c@0 x@16}");
  EXPECT_EQ(recordOf(Library, "P4").Size, 32u);
  EXPECT_EQ(Library.alignOf(*makeNamed("P4", TypeKind::Struct)), 16u);
  EXPECT_EQ(shapeOf(recordOf(Library, "P4m")), "struct{c@0 y@8}");
  // A natural alignment the pack lowers.
  EXPECT_EQ(shapeOf(recordOf(Library, "P4n")), "struct{c@0 z@4}");
  EXPECT_EQ(recordOf(Library, "P4n").Size, 12u);
}

TEST(TypeParse, MicrosoftLegacyDeclspecKeepsAlignmentAndFunctionContracts) {
  for (auto Lang : {Language::C, Language::Cpp}) {
    ParseOptions Options;
    Options.Lang = Lang;
    for (auto Model : {DataModel::ILP32, DataModel::LLP64}) {
      const TypeLibrary Library = parsed(R"(
        typedef struct _declspec(align(16)) Block {
          unsigned long long words[2];
        } Block;
        #pragma pack(push, 1)
        struct Frame { char tag; Block block; char tail; };
        #pragma pack(pop)
        long _declspec(dllimport) __stdcall Read(Block *block, unsigned count);
        _declspec(noreturn) void __cdecl Stop(unsigned code);
        void _declspec(dllimport) Finish(void);
        unsigned _inline Count(const Block *block) {
          return (unsigned)block->words[0];
        }
      )",
                                         Options, Model, RecordABI::Microsoft);
      EXPECT_EQ(shapeOf(recordOf(Library, "Frame")),
                "struct{tag@0 block@16 tail@32}");
      EXPECT_EQ(recordOf(Library, "Frame").Size, 48u);
      EXPECT_EQ(recordOf(Library, "Block").Alignment, 16u);
      const auto *Read = Library.findPrototype("Read");
      ASSERT_NE(Read, nullptr);
      EXPECT_EQ(Read->Params.size(), 2u);
      EXPECT_EQ(Read->Convention, Model == DataModel::ILP32
                                      ? CallingConvention::Stdcall
                                      : CallingConvention::Unspecified);
      const auto *Stop = Library.findPrototype("Stop");
      ASSERT_NE(Stop, nullptr);
      EXPECT_TRUE(Stop->NoReturn);
      ASSERT_NE(Library.findPrototype("Finish"), nullptr);
      EXPECT_TRUE(Library.findPrototype("Finish")->Params.empty());
      EXPECT_EQ(Library.findPrototype("_declspec"), nullptr);
      ASSERT_NE(Library.findPrototype("Count"), nullptr);
      EXPECT_EQ(Library.findPrototype("Count")->Params.size(), 1u);
      EXPECT_EQ(Library.findPrototype("_inline"), nullptr);
    }
  }
}

TEST(TypeParse, MicrosoftLegacyDeclspecRejectsMalformedAttributes) {
  for (llvm::StringRef Source :
       {"struct _declspec(align()) Bad { int x; };",
        "_declspec(noreturn void Bad(void);",
        "typedef int Bad _declspec(vector_size(16));"}) {
    TypeLibrary Library(DataModel::LLP64);
    const auto Result = parseTypes(Source, Library, {});
    EXPECT_FALSE(Result.Problems.empty()) << Source.str();
    EXPECT_EQ(Library.find("Bad"), nullptr);
    EXPECT_EQ(Library.findPrototype("Bad"), nullptr);
  }
}

TEST(TypeParse, RequiredAlignmentSurvivesContainingRecordsAndRoundTrips) {
  // Clang's Microsoft ABI puts nested at 16 and tail at 32, even when an
  // intermediate record has no explicit alignment attribute of its own.
  for (auto Model : {DataModel::LLP64, DataModel::ILP32}) {
    const TypeLibrary Library = parsed(R"(
      typedef struct __declspec(align(16)) Leaf {
        long long lo, hi;
      } Leaf;
      struct Wrapper { Leaf value; };
      struct __declspec(align(16)) Explicit { Leaf value; };
      union Choice { Leaf value; long long other; };
      typedef struct Wrapper Alias;
      #pragma pack(push, 4)
      struct Packed { char tag; Alias nested; char tail; };
      struct PackedExplicit { char tag; struct Explicit nested; char tail; };
      struct PackedUnion { char tag; union Choice nested; char tail; };
      struct PackedArray { char tag; Alias values[2]; char tail; };
      #pragma pack(pop)
      struct __declspec(align(4)) Weak { long long value; };
      struct Natural { long long value; };
      #pragma pack(push, 1)
      struct PackedWeak { char tag; struct Weak value; };
      struct PackedNatural { char tag; struct Natural value; };
      #pragma pack(pop)
    )",
                                       {}, Model, RecordABI::Microsoft);
    for (const char *Name : {"Packed", "PackedExplicit", "PackedUnion"}) {
      EXPECT_EQ(shapeOf(recordOf(Library, Name)),
                "struct{tag@0 nested@16 tail@32}");
      EXPECT_EQ(recordOf(Library, Name).Size, 48u);
      const auto RoundTrip =
          parsed(renderTypes(Library, {Library.find(Name)}, {}), {}, Model,
                 RecordABI::Microsoft);
      const RecordDef &Restored = recordOf(RoundTrip, Name);
      std::map<std::string, uint64_t> Offsets;
      for (const Member &Field : Restored.Members)
        Offsets[Field.Name] = Field.BitOffset;
      // The renderer may name padding bytes. The original fields retain
      // their offsets and the complete record retains its required align.
      EXPECT_EQ(Offsets.at("tag"), 0u);
      EXPECT_EQ(Offsets.at("nested"), 16u * 8);
      EXPECT_EQ(Offsets.at("tail"), 32u * 8);
      EXPECT_EQ(Restored.Size, 48u);
      EXPECT_EQ(Restored.Alignment, 16u);
    }
    EXPECT_EQ(shapeOf(recordOf(Library, "PackedArray")),
              "struct{tag@0 values@16 tail@48}");
    EXPECT_EQ(recordOf(Library, "PackedArray").Size, 64u);
    // An explicit alignment smaller than the natural one still makes the
    // type's full alignment required. An unannotated scalar wrapper packs.
    EXPECT_EQ(shapeOf(recordOf(Library, "PackedWeak")),
              "struct{tag@0 value@8}");
    EXPECT_EQ(recordOf(Library, "PackedWeak").Size, 16u);
    EXPECT_EQ(shapeOf(recordOf(Library, "PackedNatural")),
              "struct{tag@0 value@1}");
    EXPECT_EQ(recordOf(Library, "PackedNatural").Size, 9u);
  }
}

TEST(TypeParse, NamedPackingRestoresTheMatchingStackEntry) {
  TypeLibrary Library(DataModel::LLP64);
  const char *Source = R"(
    #pragma pack(8)
    #pragma pack(push, outer, 1)
    #pragma pack(push, inner, 2)
    #pragma pack(push, outer, 4)
    #pragma pack(show)
    struct Four { char c; long long value; };
    #pragma pack(pop, outer)
    struct Two { char c; long long value; };
    #pragma pack(pop, missing)
    struct StillTwo { char c; long long value; };
    #pragma pack(pop, outer)
    struct Eight { char c; long long value; };
    #pragma pack(push, saved)
    #pragma pack(1)
    #pragma pack(pop, saved, 4)
    struct Override { char c; long long value; };
    #pragma pack(pop, absent, 2)
    struct MissingOverride { char c; long long value; };
  )";
  ParseResult Read = parseCTypes(Source, Library);
  ASSERT_TRUE(Read.ok()) << Read.Problems.front().Message;
  EXPECT_EQ(recordOf(Library, "Four").Size, 12u);
  EXPECT_EQ(recordOf(Library, "Two").Size, 10u);
  EXPECT_EQ(recordOf(Library, "StillTwo").Size, 10u);
  EXPECT_EQ(recordOf(Library, "Eight").Size, 16u);
  EXPECT_EQ(recordOf(Library, "Override").Size, 12u);
  EXPECT_EQ(recordOf(Library, "MissingOverride").Size, 10u);
}

TEST(TypeParse, ReadsEnumerationsWithConstants) {
  const TypeLibrary Library = parsed(R"(
    enum E { A, B = 5, C, D = B << 2, F = -1, G = (int)0x80000000, H = C | 1 };
    enum Small : unsigned char { Low = 1, High = 0xFF };
  )");
  const LocalType *E = Library.find("E");
  ASSERT_TRUE(E && E->Enum);
  std::string Values;
  for (const Enumerator &Value : E->Enum->Values)
    Values += Value.Name + "=" + std::to_string(int64_t(Value.Value)) + " ";
  EXPECT_EQ(Values, "A=0 B=5 C=6 D=20 F=-1 G=-2147483648 H=7 ");
  EXPECT_EQ(Library.find("Small")->Enum->Underlying, Builtin::UChar);
}

TEST(TypeParse, ReadsTypedefsPrototypesAndFunctionPointers) {
  const TypeLibrary Library = parsed(R"(
    typedef struct _DEVICE { LONG (__stdcall *Dispatch)(struct _DEVICE *Self,
                                                         VOID *Irp);
                             struct _DEVICE *Next; } DEVICE, *PDEVICE;
    typedef VOID (NTAPI *PROUTINE)(PDEVICE Device);
    LONG NTAPI IoCallDevice(PDEVICE Device, ULONG Length, ...);
  )");
  EXPECT_EQ(Library.find("DEVICE")->Kind, TypeKind::Typedef);
  EXPECT_EQ(Library.find("PDEVICE")->Target->Kind, TypeExprKind::Pointer);
  const RecordDef &Device = recordOf(Library, "_DEVICE");
  EXPECT_EQ(shapeOf(Device), "struct{Dispatch@0 Next@8}");
  const TypeExpr &Dispatch = *Device.Members[0].Type;
  ASSERT_EQ(Dispatch.Kind, TypeExprKind::Pointer);
  ASSERT_EQ(Dispatch.Element->Kind, TypeExprKind::Function);
  // x64 compilers accept __stdcall and ignore it: no convention is kept.
  EXPECT_EQ(Dispatch.Element->Function->Convention,
            CallingConvention::Unspecified);
  EXPECT_EQ(Dispatch.Element->Function->Params[0].Name, "Self");

  const FunctionDef *Call = Library.findPrototype("IoCallDevice");
  ASSERT_NE(Call, nullptr);
  EXPECT_EQ(Call->Convention, CallingConvention::Unspecified);
  ASSERT_EQ(Call->Params.size(), 2u);
  EXPECT_EQ(Call->Params[1].Name, "Length");
  EXPECT_TRUE(Call->Variadic);
  EXPECT_EQ(Library.find("PROUTINE")->Target->Element->Kind,
            TypeExprKind::Function);

  // 32-bit x86 keeps it.
  TypeLibrary X86(DataModel::ILP32);
  ASSERT_TRUE(parseCTypes("LONG NTAPI IoCallDevice(ULONG Length);", X86).ok());
  EXPECT_EQ(X86.findPrototype("IoCallDevice")->Convention,
            CallingConvention::Stdcall);
}

TEST(TypeParse, ReadsWhatGnuPreprocessedHeadersHold) {
  // As clang -E writes <ntifs.h> for mingw-w64: attributes wherever GNU
  // puts them, __extension__, inline definitions, assembler names, and a
  // #pragma pack that names a macro.
  const TypeLibrary Library = parsed(R"(
    #define PACKING 4
    typedef struct __attribute__ ((__aligned__ (16))) _SLIST {
      unsigned long long A, B;
    } SLIST, *PSLIST;
    #pragma pack(push, PACKING)
    typedef struct _P { char c; long long q; } P;
    #pragma pack(pop)
    typedef struct _M {
      unsigned long a;
      unsigned long b __attribute__((aligned(8)));
      unsigned char __attribute__ ((__aligned__ (8))) c;
    } M;
    typedef struct { int a; } X, *PX;
    typedef enum { A1, A2 = 5 } E, *PE;
    typedef struct _Q { char c; int i; } __attribute__((__packed__, __may_alias__)) Q;
    __extension__ typedef unsigned long long U64;
    typedef union _R {
      unsigned long n;
      __extension__ union { unsigned long w[0]; unsigned long v[0]; };
    } R;
    typedef struct _S { unsigned long n; union { unsigned long w[0]; }; } S;
    extern __inline__ __attribute__((__always_inline__,__gnu_inline__))
    int helper(int x) { return x + 1.5f > 0 ? 1 : 0; }
    void __attribute__((__cdecl__)) quit(int code) __attribute__ ((__noreturn__));
    int renamed(void) __asm__("_renamed");
    void *__attribute__((__cdecl__)) info(void);
    void logv(const char *format, va_list arguments);
    _Static_assert(sizeof(P) == 12, "packed to the macro");
    _Static_assert(__alignof__(SLIST) == 16, "aligned record");
    _Static_assert(__builtin_offsetof(M, b) == 8, "aligned member");
    _Static_assert(sizeof(int) == 4 ? 1 : 0, "a conditional");
  )");
  EXPECT_EQ(recordOf(Library, "_SLIST").Size, 16u);
  EXPECT_EQ(Library.alignOf(*makeNamed("_SLIST", TypeKind::Struct)), 16u);
  EXPECT_EQ(shapeOf(recordOf(Library, "_M")), "struct{a@0 b@8 c@16}");
  EXPECT_EQ(recordOf(Library, "_M").Size, 24u);
  // An anonymous record's typedef names it; the others point to it.
  EXPECT_EQ(Library.find("X")->Kind, TypeKind::Struct);
  EXPECT_EQ(Library.find("PX")->Target->Element->Name, "X");
  EXPECT_EQ(Library.find("E")->Kind, TypeKind::Enum);
  EXPECT_EQ(Library.find("PE")->Target->Element->Name, "E");
  EXPECT_EQ(recordOf(Library, "_Q").Size, 5u);
  // Microsoft's C gives a union of empty arrays four bytes.
  EXPECT_EQ(recordOf(Library, "_R").Size, 4u);
  EXPECT_EQ(recordOf(Library, "_S").Size, 8u);
  ASSERT_NE(Library.findPrototype("helper"), nullptr);
  ASSERT_NE(Library.findPrototype("quit"), nullptr);
  EXPECT_TRUE(Library.findPrototype("quit")->NoReturn);
  EXPECT_FALSE(Library.findPrototype("helper")->NoReturn);
  ASSERT_NE(Library.findPrototype("renamed"), nullptr);
  ASSERT_NE(Library.findPrototype("info"), nullptr);
  // va_list is Windows' pointer to the arguments.
  const FunctionDef &Log = *Library.findPrototype("logv");
  ASSERT_EQ(Log.Params.size(), 2u);
  EXPECT_EQ(Log.Params[1].Type->Kind, TypeExprKind::Pointer);
}

TEST(TypeParse, MicrosoftFixedWidthPointerAliasesKeepTheirWidths) {
  for (const auto Model : {DataModel::ILP32, DataModel::LLP64}) {
    const auto Library = parsed(R"(
      typedef void *P;
      typedef P ALIAS;
      struct Mixed {
        long long Flags;
        void * __ptr32 Small;
        unsigned int Length;
        ALIAS __ptr32 Other;
        ALIAS __ptr64 Wide;
      };
      _Static_assert(sizeof(((Mixed *)0)->Small) == 4, "small pointer");
      _Static_assert(sizeof(((Mixed *)0)->Other) == 4, "alias pointer");
      _Static_assert(sizeof(((Mixed *)0)->Wide) == 8, "wide pointer");
      _Static_assert(__builtin_offsetof(Mixed, Length) == 12, "length");
      _Static_assert(__builtin_offsetof(Mixed, Other) == 16, "other");
      _Static_assert(__builtin_offsetof(Mixed, Wide) == 24, "wide");
      _Static_assert(sizeof(Mixed) == 32, "record");
    )",
                                {}, Model, RecordABI::Microsoft);
    EXPECT_EQ(Library.sizeOf(*makeNamed("P", TypeKind::Typedef)),
              Model == DataModel::ILP32 ? 4u : 8u);
    for (const auto Lang : {Language::C, Language::Cpp}) {
      RenderOptions Options;
      Options.Lang = Lang;
      const LocalType *Roots[] = {Library.find("Mixed")};
      ParseOptions Parse;
      Parse.Lang = Lang;
      const auto RoundTrip = parsed(renderTypes(Library, Roots, Options), Parse,
                                    Model, RecordABI::Microsoft);
      const auto &Fields = recordOf(RoundTrip, "Mixed").Members;
      ASSERT_EQ(Fields.size(), 5u);
      EXPECT_EQ(RoundTrip.sizeOf(*Fields[1].Type), 4u);
      EXPECT_EQ(RoundTrip.sizeOf(*Fields[3].Type), 4u);
      EXPECT_EQ(RoundTrip.sizeOf(*Fields[4].Type), 8u);
      EXPECT_EQ(recordOf(RoundTrip, "Mixed").Size, 32u);
    }
  }
}

TEST(TypeParse, MicrosoftAnonymousTypedefMembersRetainTheirStorage) {
  for (auto [Model, ABI] : {std::pair{DataModel::ILP32, RecordABI::Microsoft},
                            {DataModel::LLP64, RecordABI::Microsoft},
                            {DataModel::ILP32, RecordABI::MinGW32},
                            {DataModel::LLP64, RecordABI::MinGW64}}) {
    const TypeLibrary Library = parsed(R"(
      typedef struct _HEADER { unsigned short Tag; void *Link; } HEADER;
      typedef HEADER HEADER_ALIAS;
      typedef union { unsigned int Code; unsigned short Parts[2]; } VALUE;
      struct Packet { char Prefix; HEADER_ALIAS; unsigned char Payload[3]; };
      struct Promoted { VALUE; unsigned char Tail; };
      struct Nested { struct Inner { unsigned int Value; void *Link; }; void *Tail; };
      #pragma pack(push, 1)
      struct Packed { char Prefix; HEADER_ALIAS; unsigned char Payload[3]; };
      #pragma pack(pop)
      struct Tagged { struct _HEADER; unsigned char Tail; };
      _Static_assert(__builtin_offsetof(Packet, Tag) == sizeof(void *), "Tag");
      _Static_assert(__builtin_offsetof(Packet, Link) == 2 * sizeof(void *), "Link");
      _Static_assert(__builtin_offsetof(Packet, Payload) == 3 * sizeof(void *), "Payload");
      _Static_assert(__builtin_offsetof(Promoted, Tail) == 4, "anonymous union");
      _Static_assert(sizeof(Tagged) == 3 * sizeof(void *), "tag reference has storage in Microsoft C");
      _Static_assert(__builtin_offsetof(Tagged, Link) == sizeof(void *), "tag members are promoted");
      _Static_assert(__builtin_offsetof(Tagged, Tail) == 2 * sizeof(void *), "tag storage precedes tail");
      _Static_assert(sizeof(Nested) == 3 * sizeof(void *), "named definition has storage in C");
      _Static_assert(__builtin_offsetof(Nested, Tail) == 2 * sizeof(void *), "nested storage");
    )",
                                       {}, Model, ABI);
    const uint64_t Pointer = Model == DataModel::LLP64 ? 8 : 4;
    EXPECT_EQ(recordOf(Library, "Packet").Size, 4 * Pointer);
    EXPECT_EQ(recordOf(Library, "Packed").Size, 2 * Pointer + 4);
    EXPECT_EQ(recordOf(Library, "Promoted").Size, 8u);
    ASSERT_EQ(recordOf(Library, "Packet").Members.size(), 3u);
    EXPECT_TRUE(recordOf(Library, "Packet").Members[1].isAnonymous());
    const auto RoundTrip = parsed(
        renderType(Library, *Library.find("Packet"), {}), {}, Model, ABI);
    EXPECT_EQ(recordOf(RoundTrip, "Packet").Size, 4 * Pointer);
    const auto Tagged = parsed(renderType(Library, *Library.find("Tagged"), {}),
                               {}, Model, ABI);
    EXPECT_EQ(recordOf(Tagged, "Tagged").Size, 3 * Pointer);
    EXPECT_TRUE(recordOf(Tagged, "Tagged").Members[0].isAnonymous());
  }
  TypeLibrary Incomplete(DataModel::LLP64);
  EXPECT_FALSE(parseCTypes("typedef struct Unfinished U; "
                           "struct Broken { U; int Tail; };",
                           Incomplete)
                   .ok());
  EXPECT_FALSE(
      parseCTypes("struct Broken { struct Unfinished; int Tail; };", Incomplete)
          .ok());
  ParseOptions Cpp;
  Cpp.Lang = Language::Cpp;
  const auto Declaration = parsed(R"(
    typedef struct H { unsigned short Tag; void *Link; } Header;
    struct P { char Prefix; Header; unsigned char Payload[3]; };
    struct N { struct Inner { void *Value; }; unsigned int Value; };
    struct Q { struct H; unsigned char Tail; };
    static_assert(sizeof(P) == 4, "C++ typedef declaration has no storage");
    static_assert(sizeof(N) == 4, "C++ named record declaration has no storage");
    static_assert(sizeof(Q) == 1, "C++ tag reference has no storage");
  )",
                                  Cpp);
  EXPECT_EQ(recordOf(Declaration, "P").Size, 4u);
  const auto StandardC = parsed(R"(
    struct Header { unsigned long long Value; };
    struct Decl { struct Header; char Tail; };
    _Static_assert(sizeof(Decl) == 1, "standard C tag reference has no storage");
  )",
                                {}, DataModel::LP64, RecordABI::Itanium);
  EXPECT_EQ(recordOf(StandardC, "Decl").Size, 1u);
}

TEST(TypeParse, SizesAStringLiteralWithItsTerminator) {
  // <wininet.h>'s INTERNET_MAX_URL_LENGTH, as -E leaves it, sizes the
  // arrays of <commctrl.h> and <shlobj.h>; adjacent literals are one.
  const TypeLibrary Library = parsed(R"(
    typedef unsigned short WCHAR;
    typedef struct tagLITEM {
      WCHAR szUrl[(2048+32+sizeof("://"))];
      char Joined[sizeof "ab" "c"];
      char Empty[sizeof("")];
    } LITEM;
    _Static_assert(sizeof(LITEM) == 2 * 2084 + 4 + 1 + 1, "LITEM");
  )");
  EXPECT_EQ(shapeOf(recordOf(Library, "tagLITEM")),
            "struct{szUrl@0 Joined@4168 Empty@4172}");
}

TEST(TypeParse, ARoutineDeclaredAgainWithAnotherTypeIsRefused) {
  // EDK II's module kinds each declare _ModuleEntryPoint their way: a text
  // that declares two of them describes no one routine.  A declaration that
  // passes and returns alike, its parameters named otherwise or const,
  // reads; another text, as the user's declarations are, gives the routine
  // a type anew.
  const char *Source = "int Entry(void *ImageHandle, void *SystemTable);\n"
                       "int Entry(void *Handle, void *const Table);\n"
                       "int Entry(void *HobStart);\n";
  TypeLibrary Strict(DataModel::LLP64);
  const ParseResult Refused = parseCTypes(Source, Strict);
  ASSERT_EQ(Refused.Problems.size(), 1u);
  EXPECT_EQ(Refused.Problems[0].Line, 3u);
  EXPECT_EQ(Refused.Problems[0].Message,
            "'Entry' is declared again with another type");
  TypeLibrary Going(DataModel::LLP64);
  ParseOptions Options;
  Options.KeepGoing = true;
  EXPECT_EQ(parseCTypes(Source, Going, Options).Problems.size(), 1u);
  ASSERT_NE(Going.findPrototype("Entry"), nullptr);
  EXPECT_EQ(Going.findPrototype("Entry")->Params.size(), 2u);
  ASSERT_TRUE(parseCTypes("int Entry(void *HobStart);", Going).ok());
  EXPECT_EQ(Going.findPrototype("Entry")->Params.size(), 1u);

  // Two headers that declare one routine through other typedefs, as
  // mingw-w64's and phnt's RtlIpv4AddressToStringA, pass alike.
  TypeLibrary Windows(DataModel::LLP64);
  EXPECT_TRUE(parseCTypes("typedef unsigned long ULONG;\n"
                          "typedef const char *PCSTR;\n"
                          "ULONG Crc(const char *Buffer, unsigned long Size);\n"
                          "unsigned long Crc(PCSTR Buffer, ULONG Size);",
                          Windows)
                  .ok());

  // bionic's strtol_l, which takes a locale, is strtol's symbol: strtol's
  // own declaration describes strtol, before or after it.
  for (const char *Bionic :
       {"long strtol(const char *s, char **end, int base);\n"
        "long strtol_l(const char *s, char **end, int base, void *locale) "
        "__asm__(\"strtol\");",
        "long strtol_l(const char *s, char **end, int base, void *locale) "
        "__asm__(\"strtol\");\n"
        "long strtol(const char *s, char **end, int base);"}) {
    TypeLibrary Android(DataModel::LP64);
    ASSERT_TRUE(parseCTypes(Bionic, Android).ok()) << Bionic;
    ASSERT_NE(Android.findPrototype("strtol"), nullptr);
    EXPECT_EQ(Android.findPrototype("strtol")->Params.size(), 3u) << Bionic;
    ASSERT_NE(Android.findPrototype("strtol_l"), nullptr);
    EXPECT_EQ(Android.findPrototype("strtol_l")->Params.size(), 4u);
  }
}

TEST(TypeParse, SizesWideAndUnicodeLiterals) {
  // EDK II's Base.h checks its characters: L'A' is a wchar_t, two bytes
  // under Microsoft's ABI, which UEFI follows, four under System V's; a
  // character constant without a prefix is an int.
  TypeLibrary Uefi(DataModel::LLP64);
  Uefi.setRecordABI(RecordABI::Microsoft);
  const ParseResult Read = parseCTypes(
      "_Static_assert (sizeof (L'A') == 2, \"L'A'\");\n"
      "_Static_assert (sizeof (L\"A\") == 4, \"L\\\"A\\\"\");\n"
      "_Static_assert (sizeof ('A') == 4, \"'A'\");\n"
      "_Static_assert (sizeof (u\"ab\") == 6, \"u\");\n"
      "_Static_assert (sizeof (U\"ab\") == 12, \"U\");\n"
      "_Static_assert (sizeof (u8\"ab\") == 3, \"u8\");\n"
      "_Static_assert (sizeof (L\"a\" \"bc\") == 8, \"joined\");\n"
      "typedef struct { unsigned short Name[sizeof (L\"Boot\") / 2]; } "
      "BOOT_NAME;",
      Uefi);
  EXPECT_TRUE(Read.ok()) << Read.Problems.front().Message;
  EXPECT_EQ(recordOf(Uefi, "BOOT_NAME").Size, 10u);
  TypeLibrary Linux(DataModel::LP64);
  Linux.setRecordABI(RecordABI::Itanium);
  EXPECT_TRUE(
      parseCTypes("_Static_assert (sizeof (L\"A\") == 8, \"L\");", Linux).ok());
  // Literals of two prefixes do not join.
  EXPECT_FALSE(
      parseCTypes("_Static_assert (sizeof (u\"a\" U\"b\") == 8, \"mixed\");",
                  Linux)
          .ok());
}

TEST(TypeParse, RefusesWhatItCannotLayOutAndKeepsGoingWhenAsked) {
  TypeLibrary Library;
  // A #pragma pack of an unknown name leaves every layout after it unknown.
  ParseResult Pack =
      parseCTypes("#pragma pack(UNKNOWN)\nstruct A { int a; };", Library);
  ASSERT_EQ(Pack.Problems.size(), 1u);
  EXPECT_NE(Pack.Problems[0].Message.find("UNKNOWN"), std::string::npos);

  const char *Source = "typedef int v4 __attribute__((__vector_size__(16)));\n"
                       "struct B { char c; int i __attribute__((packed)); };\n"
                       "struct C { int c; };\n";
  ParseResult Strict = parseCTypes(Source, Library);
  EXPECT_EQ(Strict.Problems.size(), 2u);
  EXPECT_EQ(Library.find("C"), nullptr);
  ParseOptions Going;
  Going.KeepGoing = true;
  ParseResult Kept = parseCTypes(Source, Library, Going);
  EXPECT_EQ(Kept.Problems.size(), 2u);
  EXPECT_EQ(Library.find("v4"), nullptr);
  EXPECT_EQ(Library.find("B"), nullptr);
  EXPECT_NE(Library.find("C"), nullptr);
}

TEST(TypeParse, ASourceUnitPreservesItsDefaultCallingConvention) {
  const char *Source = R"(
    int Before(int value);
    #pragma neverd_default_calling_convention(__stdcall)
    typedef int Routine(int value);
    typedef int (*Callback)(int value);
    struct Operations { int (*Run)(int value); };
    int Default(int value);
    int __cdecl CallerPops(int value);
    int __fastcall RegisterArgs(int value);
    int Variadic(int count, ...);
    int __cdecl WithCallbacks(int (*CalleePops)(int),
                              int (__cdecl *CallerPops)(int));
    #pragma neverd_default_calling_convention()
    int After(int value);
  )";
  for (auto Model : {DataModel::ILP32, DataModel::LLP64}) {
    auto Library = parsed(Source, {}, Model, RecordABI::Microsoft);
    const auto Expected = [Model](CallingConvention Convention) {
      return Model == DataModel::ILP32 ? Convention
                                       : CallingConvention::Unspecified;
    };
    for (const char *Name : {"Before", "After"}) {
      ASSERT_NE(Library.findPrototype(Name), nullptr);
      EXPECT_EQ(Library.findPrototype(Name)->Convention,
                CallingConvention::Unspecified);
    }
    for (const auto &[Name, Convention] :
         std::map<std::string, CallingConvention>{
             {"Default", CallingConvention::Stdcall},
             {"CallerPops", CallingConvention::Cdecl},
             {"RegisterArgs", CallingConvention::Fastcall},
             {"Variadic", CallingConvention::Cdecl},
             {"WithCallbacks", CallingConvention::Cdecl}}) {
      ASSERT_NE(Library.findPrototype(Name), nullptr);
      EXPECT_EQ(Library.findPrototype(Name)->Convention, Expected(Convention))
          << Name;
    }
    ASSERT_NE(Library.find("Routine"), nullptr);
    ASSERT_NE(Library.find("Routine")->Function, nullptr);
    EXPECT_EQ(Library.find("Routine")->Function->Convention,
              Expected(CallingConvention::Stdcall));
    ASSERT_NE(Library.find("Callback"), nullptr);
    const auto Callback = Library.find("Callback")->Target;
    ASSERT_TRUE(Callback && Callback->Element && Callback->Element->Function);
    EXPECT_EQ(Callback->Element->Function->Convention,
              Expected(CallingConvention::Stdcall));
    const auto &Operations = recordOf(Library, "Operations");
    ASSERT_EQ(Operations.Members.size(), 1u);
    EXPECT_EQ(Operations.Members[0].Type->Element->Function->Convention,
              Expected(CallingConvention::Stdcall));
    const auto *WithCallbacks = Library.findPrototype("WithCallbacks");
    ASSERT_EQ(WithCallbacks->Params.size(), 2u);
    EXPECT_EQ(WithCallbacks->Params[0].Type->Element->Function->Convention,
              Expected(CallingConvention::Stdcall));
    EXPECT_EQ(WithCallbacks->Params[1].Type->Element->Function->Convention,
              Expected(CallingConvention::Cdecl));
    // Rendering makes the default explicit, so another parser needs no
    // source-unit pragma and keeps callback conventions on round trip.
    const auto RoundTrip =
        parsed(renderTypes(Library,
                           {Library.find("Routine"), Library.find("Callback"),
                            Library.find("Operations")},
                           {}),
               {}, Model, RecordABI::Microsoft);
    EXPECT_EQ(RoundTrip.find("Routine")->Function->Convention,
              Expected(CallingConvention::Stdcall));
    EXPECT_EQ(RoundTrip.find("Callback")->Target->Element->Function->Convention,
              Expected(CallingConvention::Stdcall));
    // Defaults belong to a parse, not to the library it updates.
    ASSERT_TRUE(
        parseCTypes("#pragma neverd_default_calling_convention(__stdcall)\n"
                    "int Last(int value);",
                    Library)
            .ok());
    ASSERT_TRUE(parseCTypes("int Separate(int value);", Library).ok());
    EXPECT_EQ(Library.findPrototype("Separate")->Convention,
              CallingConvention::Unspecified);
  }
  for (const char *Directive :
       {"(unknown)", "__stdcall", "(__stdcall, __cdecl)"}) {
    TypeLibrary Library(DataModel::ILP32);
    const auto Result =
        parseCTypes("#pragma neverd_default_calling_convention" +
                        std::string(Directive) + "\nint Invalid(int value);",
                    Library);
    EXPECT_FALSE(Result.ok());
    EXPECT_EQ(Library.findPrototype("Invalid"), nullptr);
  }
}

TEST(TypeParse, AVariadicRoutineIsCdeclWhateverItsConvention) {
  // phnt declares RtlInitializeSidEx NTAPI: only its caller knows what a
  // call pushes, so compilers take a convention whose callee pops as cdecl.
  TypeLibrary Library(DataModel::ILP32);
  ASSERT_TRUE(
      parseCTypes("long __stdcall RtlInitializeSidEx(void *Sid, void *Auth, "
                  "unsigned char Count, ...);\n"
                  "long __stdcall RtlInitializeSid(void *Sid, void *Auth, "
                  "unsigned char Count);",
                  Library)
          .ok());
  const FunctionDef *Variadic = Library.findPrototype("RtlInitializeSidEx");
  ASSERT_NE(Variadic, nullptr);
  EXPECT_TRUE(Variadic->Variadic);
  EXPECT_EQ(Variadic->Convention, CallingConvention::Cdecl);
  const FunctionDef *Fixed = Library.findPrototype("RtlInitializeSid");
  ASSERT_NE(Fixed, nullptr);
  EXPECT_EQ(Fixed->Convention, CallingConvention::Stdcall);
}

TEST(TypeParse, AnEnumerationOfNoValuesReadsBackWithItsType) {
  // `typedef enum _CLASS CLASS;` alone, as Microsoft's compilers take it and
  // phnt declares USERTHREADINFOCLASS.  C has no enumeration without
  // enumerators: it renders with its underlying type, which completes it.
  TypeLibrary Library;
  ASSERT_TRUE(parseCTypes("typedef enum _CLASS CLASS;\n"
                          "typedef struct _QUERY { CLASS Class; } QUERY;",
                          Library)
                  .ok());
  for (Language Lang : {Language::C, Language::Cpp}) {
    RenderOptions Render;
    Render.Lang = Lang;
    Render.Asserts = true;
    const std::string Text = renderTypes(Library, {}, Render);
    EXPECT_NE(Text.find("enum _CLASS : int32_t;"), std::string::npos) << Text;
    TypeLibrary Back;
    ParseOptions Read;
    Read.Lang = Lang;
    ASSERT_TRUE(parseTypes(Text, Back, Read).ok()) << Text;
    const LocalType *Class = Back.find("_CLASS");
    ASSERT_NE(Class, nullptr);
    ASSERT_TRUE(Class->Enum);
    EXPECT_TRUE(Class->Enum->Values.empty());
    EXPECT_EQ(Back.sizeOf(*makeNamed("QUERY", TypeKind::Typedef)), 4u);
  }
}

TEST(TypeParse, ARoutineTakesItsPrototypeUnderItsAssemblerName) {
  // glibc's __REDIRECT gives strtol the name a program built with
  // _GNU_SOURCE imports it by, and C99 qualifies a parameter's array.
  TypeLibrary Library(DataModel::LP64);
  ASSERT_TRUE(
      parseCTypes(
          "extern long strtol(const char *__restrict __nptr, char "
          "**__restrict __endptr, int __base) __asm__ (\"\" "
          "\"__isoc23_strtol\") __attribute__ ((__nothrow__ , __leaf__));\n"
          "extern int getaddrinfo_a(int __mode, void *__list[__restrict], "
          "int __ent, void *__restrict __sig);\n"
          "extern int regexec(const void *__preg, const char *__String, "
          "unsigned long __nmatch, int __pmatch[__restrict __nmatch], "
          "int __eflags);",
          Library)
          .ok());
  for (const char *Name : {"strtol", "__isoc23_strtol"}) {
    const FunctionDef *Function = Library.findPrototype(Name);
    ASSERT_NE(Function, nullptr) << Name;
    ASSERT_EQ(Function->Params.size(), 3u) << Name;
    EXPECT_EQ(Function->Params[2].Name, "__base") << Name;
  }
  const FunctionDef *Lookup = Library.findPrototype("getaddrinfo_a");
  ASSERT_NE(Lookup, nullptr);
  ASSERT_EQ(Lookup->Params.size(), 4u);
  EXPECT_EQ(Library.sizeOf(*Lookup->Params[1].Type), 8u);
  // A parameter's array sized by another parameter is a pointer.
  const FunctionDef *Match = Library.findPrototype("regexec");
  ASSERT_NE(Match, nullptr);
  ASSERT_EQ(Match->Params.size(), 5u);
  EXPECT_EQ(Library.sizeOf(*Match->Params[3].Type), 8u);

  // musl's __REDIR declares a 32-bit program's stat again, with the type it
  // has, under the name of its 64-bit time.
  TypeLibrary Musl(DataModel::ILP32);
  ASSERT_TRUE(parseCTypes("struct stat { long long st_mtime; };\n"
                          "int stat(const char *__restrict, struct stat "
                          "*__restrict);\n"
                          "__typeof__(stat) stat __asm__(\"__stat_time64\");",
                          Musl)
                  .ok());
  for (const char *Name : {"stat", "__stat_time64"}) {
    const FunctionDef *Function = Musl.findPrototype(Name);
    ASSERT_NE(Function, nullptr) << Name;
    EXPECT_EQ(Function->Params.size(), 2u) << Name;
  }
  TypeLibrary Expression(DataModel::ILP32);
  EXPECT_FALSE(parseCTypes("__typeof__(1 + 2) three;", Expression).ok());

  // An x64 macOS program imports stat as stat$INODE64, which no C name
  // spells: the rendering gives it to stat as its assembler name, and the
  // reading takes both names back.
  TypeLibrary Darwin(DataModel::LP64);
  ASSERT_TRUE(parseCTypes("struct stat { long long st_size; };\n"
                          "int stat(const char *, struct stat *) "
                          "__asm(\"stat\" \"$INODE64\");",
                          Darwin)
                  .ok());
  ASSERT_NE(Darwin.findPrototype("stat$INODE64"), nullptr);
  RenderOptions Prototypes;
  Prototypes.Prototypes = true;
  const std::string Header = renderTypes(Darwin, {}, Prototypes);
  EXPECT_NE(Header.find("int32_t stat(const char *, struct stat *) "
                        "__asm__(\"stat$INODE64\");"),
            std::string::npos)
      << Header;
  EXPECT_NE(Header.find("2 prototypes"), std::string::npos) << Header;
  TypeLibrary Back(DataModel::LP64);
  ASSERT_TRUE(parseCTypes(Header, Back).ok());
  for (const char *Name : {"stat", "stat$INODE64"}) {
    const FunctionDef *Function = Back.findPrototype(Name);
    ASSERT_NE(Function, nullptr) << Name;
    EXPECT_EQ(Function->Params.size(), 2u) << Name;
  }
}

TEST(TypeParse, ATypedefOfSizeTKeepsItsName) {
  // size_t names the builtin of a pointer's width until the declarations
  // define it, as glibc's headers do; then a prototype names the typedef.
  TypeLibrary Library(DataModel::ILP32);
  ASSERT_TRUE(parseCTypes("void *early(size_t n);\n"
                          "typedef unsigned int size_t;\n"
                          "typedef int ssize_t;\n"
                          "ssize_t read(int fd, void *buf, size_t nbytes);",
                          Library)
                  .ok());
  const FunctionDef *Early = Library.findPrototype("early");
  ASSERT_NE(Early, nullptr);
  EXPECT_EQ(Early->Params[0].Type->Kind, TypeExprKind::Builtin);
  const FunctionDef *Read = Library.findPrototype("read");
  ASSERT_NE(Read, nullptr);
  EXPECT_EQ(Read->Return->Name, "ssize_t");
  EXPECT_EQ(Read->Params[2].Type->Name, "size_t");
  EXPECT_EQ(Library.sizeOf(*Read->Params[2].Type), 4u);
}

TEST(TypeParse, AnLP64ParameterPassesItsVaListByAddress) {
  // x86-64's va_list decays from an array, and AAPCS64 passes a record that
  // large by reference, under each name a C library gives it: glibc's
  // __gnuc_va_list, bionic's __va_list, musl's __isoc_va_list, Darwin's
  // __darwin_va_list.  Elsewhere it is a record local types do not hold.
  TypeLibrary Library(DataModel::LP64);
  ASSERT_TRUE(parseCTypes("int vprintf(const char *, __gnuc_va_list);\n"
                          "int vsyslog(int, const char *, __va_list);\n"
                          "int vfprintf(void *, const char *, "
                          "__isoc_va_list);\n"
                          "int vdprintf(int, const char *, "
                          "__darwin_va_list);",
                          Library)
                  .ok());
  for (const char *Name : {"vprintf", "vsyslog", "vfprintf", "vdprintf"}) {
    const FunctionDef *Function = Library.findPrototype(Name);
    ASSERT_NE(Function, nullptr) << Name;
    const TypeExpr &List = *Function->Params.back().Type;
    EXPECT_EQ(List.Kind, TypeExprKind::Pointer) << Name;
    EXPECT_EQ(Library.sizeOf(List), 8u) << Name;
  }
  TypeLibrary Record(DataModel::LP64);
  EXPECT_FALSE(
      parseCTypes("typedef __builtin_va_list __isoc_va_list;", Record).ok());
}

TEST(TypeParse, AnEnumeratorReadsPastItsAttributes) {
  // Darwin's headers mark an enumerator's availability, or its Swift name,
  // after the enumerator's name.
  TypeLibrary Library(DataModel::LP64);
  ASSERT_TRUE(
      parseCTypes(
          "typedef enum {\n"
          "  _CLOCK_REALTIME __attribute__((availability(macosx,introduced="
          "10.12))) = 0,\n"
          "  _CLOCK_MONOTONIC __attribute__((availability(macosx,introduced="
          "10.12))) = 6,\n"
          "  _CLOCK_NEXT __attribute__((swift_name(\"next\")))\n"
          "} clockid_t;\n"
          "int clock_gettime(clockid_t __clock_id, void *__tp);",
          Library)
          .ok());
  const LocalType *Clock = Library.resolve("clockid_t");
  ASSERT_NE(Clock, nullptr);
  ASSERT_EQ(Clock->Kind, TypeKind::Enum);
  ASSERT_EQ(Clock->Enum->Values.size(), 3u);
  EXPECT_EQ(Clock->Enum->Values[1].Name, "_CLOCK_MONOTONIC");
  EXPECT_EQ(Clock->Enum->Values[1].Value, 6u);
  EXPECT_EQ(Clock->Enum->Values[2].Value, 7u);
  ASSERT_NE(Library.findPrototype("clock_gettime"), nullptr);
}

TEST(TypeParse, ConvertsWithoutAProgram) {
  // A C header read and rendered as Rust and Go, and back.
  TypeLibrary Library;
  ASSERT_TRUE(parseCTypes("typedef LONG NTSTATUS;\n"
                          "typedef struct _PAIR { ULONG Low; LONG High; } "
                          "PAIR, *PPAIR;\n"
                          "NTSTATUS NTAPI Pair(PPAIR Out, ULONG Flags);",
                          Library)
                  .ok());
  for (Language Lang : {Language::Rust, Language::Go, Language::Cpp}) {
    RenderOptions Render;
    Render.Lang = Lang;
    Render.Asserts = true;
    TypeLibrary Back;
    ParseOptions Read;
    Read.Lang = Lang;
    const ParseResult Result =
        parseTypes(renderTypes(Library, {}, Render), Back, Read);
    EXPECT_TRUE(Result.ok()) << languageId(Lang).str();
    EXPECT_EQ(shapeOf(recordOf(Back, "_PAIR")), "struct{Low@0 High@4}");
  }
  RenderOptions Prototypes;
  Prototypes.Prototypes = true;
  const std::string Header = renderTypes(Library, {}, Prototypes);
  EXPECT_NE(Header.find("NTSTATUS Pair(PPAIR Out, uint32_t Flags);"),
            std::string::npos)
      << Header;
  TypeLibrary Back;
  ASSERT_TRUE(parseCTypes(Header, Back).ok());
  ASSERT_NE(Back.findPrototype("Pair"), nullptr);
  EXPECT_EQ(Back.findPrototype("Pair")->Params[1].Name, "Flags");
}

TEST(TypeParse, ExactLongDoublesReadBackAsTheProgramsOwn) {
  // The prelude declares the name twice, once for compilers whose long
  // double is as large; both read as the long double, which only the
  // program's of that size is.
  for (const auto &[Model, ABI] :
       {std::pair(DataModel::LP64, RecordABI::Itanium),
        std::pair(DataModel::ILP32, RecordABI::MinGW32),
        std::pair(DataModel::LLP64, RecordABI::Microsoft)}) {
    TypeLibrary Library(Model);
    Library.setRecordABI(ABI);
    ASSERT_TRUE(parseCTypes("typedef long double real;\n"
                            "struct L { char c; long double d; double e; "
                            "real r; };",
                            Library)
                    .ok());
    for (const BuiltinSpelling Spelling :
         {BuiltinSpelling::Portable, BuiltinSpelling::Windows}) {
      RenderOptions Render;
      Render.Spelling = Spelling;
      Render.Asserts = true;
      const std::string Header = renderTypes(Library, {}, Render);
      TypeLibrary Back(Model);
      Back.setRecordABI(ABI);
      const ParseResult Result = parseCTypes(Header, Back);
      ASSERT_TRUE(Result.ok()) << Header;
      const RecordDef &L = recordOf(Back, "L");
      ASSERT_EQ(L.Members.size(), 4u);
      ASSERT_TRUE(L.Members[1].Type);
      EXPECT_EQ(L.Members[1].Type->Kind, TypeExprKind::Builtin) << Header;
      // The portable spelling of a long double of eight bytes is a double.
      EXPECT_EQ(L.Members[1].Type->TheBuiltin,
                Spelling == BuiltinSpelling::Portable &&
                        Library.builtinBytes(Builtin::LongDouble) == 8
                    ? Builtin::Double
                    : Builtin::LongDouble);
      EXPECT_EQ(shapeOf(L), shapeOf(recordOf(Library, "L")));
      EXPECT_EQ(Back.types().size(), Library.types().size()) << Header;
    }
  }
  TypeLibrary Narrow(DataModel::LLP64);
  const ParseResult Wrong =
      parseCTypes("struct W { neverd_long_double_16 d; };", Narrow);
  ASSERT_EQ(Wrong.Problems.size(), 1u);
  EXPECT_EQ(Wrong.Problems[0].Message,
            "'neverd_long_double_16' is a long double of 16 bytes, where the "
            "program's is of 8");
}

TEST(TypeParse, HeaderPragmasNameTheSystemHeaderOfTheTypesAfterThem) {
  // A library of a system's headers names the header of each type, which
  // a C header of the program's includes on that system.
  TypeLibrary Library;
  ASSERT_TRUE(parseCTypes("struct before { int a; };\n"
                          "#pragma neverd_header(\"sys/stat.h\")\n"
                          "typedef struct { int x; } anon_t;\n"
                          "typedef struct tagged tagged;\n"
                          "struct tagged { int y; };\n"
                          "enum mode { MODE_A };\n"
                          "typedef enum mode mode;\n"
                          "#pragma neverd_header()\n"
                          "struct after { int z; };\n",
                          Library)
                  .ok());
  EXPECT_EQ(Library.find("before")->Header, "");
  EXPECT_EQ(Library.find("anon_t")->Header, "sys/stat.h");
  EXPECT_EQ(Library.find("tagged")->Header, "sys/stat.h");
  EXPECT_EQ(Library.find("after")->Header, "");
  // C names a record by its name alone where a typedef of the name lets it.
  EXPECT_TRUE(Library.find("anon_t")->TypedefNamed);
  EXPECT_TRUE(Library.find("tagged")->TypedefNamed);
  EXPECT_TRUE(Library.find("mode")->TypedefNamed);
  EXPECT_FALSE(Library.find("before")->TypedefNamed);
  TypeLibrary Wrong;
  const ParseResult Bad = parseCTypes(
      "#pragma neverd_header(stdio.h)\nstruct s { int a; };\n", Wrong);
  ASSERT_EQ(Bad.Problems.size(), 1u);
  EXPECT_EQ(Bad.Problems[0].Message,
            "#pragma neverd_header takes a header's name in quotes");
}

TEST(TypeParse, AHeadersOwnRenamesLeaveTheSystemsNames) {
  // A header NeverD renders names the system's types otherwise for another
  // system's compilers (#define X neverd_X); read back, they are the
  // system's.
  TypeLibrary Library(DataModel::LP64);
  ASSERT_TRUE(parseCTypes("#define __off_t neverd___off_t\n"
                          "typedef long __off_t;\n"
                          "struct file { __off_t offset; };\n",
                          Library)
                  .ok());
  EXPECT_NE(Library.find("__off_t"), nullptr);
  EXPECT_EQ(Library.find("neverd___off_t"), nullptr);
}

TEST(TypeParse, APackOfTheABIsOwnLimitIsNoPacking) {
  // The i386 System V ABI aligns long long to four, as pack(4) does; a
  // tighter pack stays the record's.
  TypeLibrary Library(DataModel::ILP32);
  Library.setRecordABI(RecordABI::I386SystemV);
  ASSERT_TRUE(parseCTypes("#pragma pack(push, 4)\n"
                          "struct loose { char c; long long q; };\n"
                          "#pragma pack(pop)\n"
                          "#pragma pack(push, 2)\n"
                          "struct tight { char c; long long q; };\n"
                          "#pragma pack(pop)\n",
                          Library)
                  .ok());
  EXPECT_EQ(recordOf(Library, "loose").Pack, 0u);
  EXPECT_EQ(recordOf(Library, "tight").Pack, 2u);
  EXPECT_EQ(shapeOf(recordOf(Library, "loose")), "struct{c@0 q@4}");
}

TEST(TypeParse, ARecordsTypedefRendersBeforeWhatNamesIt) {
  // EDK II's PEI notification: the record holds a function pointer whose
  // parameter is the record's typedef, which the rendering must declare
  // first; what holds the typedef by value still follows the record.
  TypeLibrary Library(DataModel::LP64);
  ASSERT_TRUE(parseCTypes("typedef struct _NOTIFY NOTIFY;\n"
                          "typedef int (*NOTIFY_ENTRY)(NOTIFY *Notify, "
                          "void *Ppi);\n"
                          "struct _NOTIFY { unsigned long Flags; "
                          "NOTIFY_ENTRY Notify; };\n"
                          "typedef struct { NOTIFY Inner; int Count; } "
                          "HOLDER;\n",
                          Library)
                  .ok());
  for (const LocalType *Root :
       {Library.find("NOTIFY"), Library.find("NOTIFY_ENTRY"),
        Library.find("_NOTIFY"), Library.find("HOLDER")}) {
    ASSERT_NE(Root, nullptr);
    const std::string Header = renderTypes(Library, {Root}, RenderOptions());
    TypeLibrary Back(DataModel::LP64);
    const ParseResult Read = parseCTypes(Header, Back);
    EXPECT_TRUE(Read.ok()) << Root->Name << ": "
                           << Read.Problems.front().Message << "\n"
                           << Header;
  }
  const std::string All = renderTypes(Library, {}, RenderOptions());
  EXPECT_LT(All.find("typedef struct _NOTIFY NOTIFY;"),
            All.find("(*NOTIFY_ENTRY)("))
      << All;
  TypeLibrary Back(DataModel::LP64);
  ASSERT_TRUE(parseCTypes(All, Back).ok()) << All;
  EXPECT_EQ(recordOf(Back, "HOLDER").Size, 24u);
}

TEST(TypeParse, ChecksStaticAssertions) {
  TypeLibrary Library;
  EXPECT_TRUE(parseCTypes(R"(
    struct A { char c; int i; };
    _Static_assert(sizeof(struct A) == 8, "A");
    _Static_assert(offsetof(struct A, i) == 0x4, "A.i");
  )",
                          Library)
                  .ok());
  const ParseResult Wrong = parseCTypes(
      "struct B { int i; };\n_Static_assert(sizeof(struct B) == 3, \"B\");",
      Library);
  ASSERT_EQ(Wrong.Problems.size(), 1u);
  EXPECT_EQ(Wrong.Problems[0].Line, 2u);
  EXPECT_EQ(Wrong.Problems[0].Message, "a static assertion fails: B");
}

TEST(TypeParse, SizesAMemberReachedThroughANullPointer) {
  // As RTL_FIELD_SIZE and RTL_SIZEOF_THROUGH_FIELD spell it.
  TypeLibrary Library;
  EXPECT_TRUE(parseCTypes(R"(
    typedef struct _A {
      char c;
      struct { short s; long long q; };
      struct { int i; char b[3]; } n;
    } A;
    typedef A __unaligned *PA, __unaligned *OTHER_PA;
    typedef PA ALIAS_PA;
    _Static_assert(sizeof(((A *)0)->q) == 8, "A.q");
    _Static_assert(sizeof(((PA)0)->q) == 8, "PA.q");
    _Static_assert(sizeof(((ALIAS_PA)0)->n.b) == 3, "ALIAS_PA.n.b");
    _Static_assert(sizeof(((OTHER_PA)0)->n.b) == 3, "OTHER_PA.n.b");
    _Static_assert(sizeof(((A *)0)->n.b) == 3, "A.n.b");
    _Static_assert(__builtin_offsetof(A, q) + sizeof(((A *)0)->q) == 24,
                   "A through q");
  )",
                          Library)
                  .ok());
  const ParseResult Missing = parseCTypes(
      "_Static_assert(sizeof(((A *)0)->z) == 1, \"A.z\");", Library);
  ASSERT_EQ(Missing.Problems.size(), 1u);
  EXPECT_EQ(Missing.Problems[0].Message, "no member 'z' to take the size of");
}

TEST(TypeParse, ReportsWhereAndChangesNothingOnAProblem) {
  TypeLibrary Library;
  const ParseResult Result = parseCTypes(
      "struct Good { int a; };\nstruct Bad {\n  DWORD x;\n};", Library);
  ASSERT_EQ(Result.Problems.size(), 1u);
  EXPECT_EQ(Result.Problems[0].Line, 3u);
  EXPECT_EQ(Result.Problems[0].Column, 3u);
  EXPECT_EQ(Result.Problems[0].Message, "'DWORD' names no type");
  // The whole source reads, or none of it.
  EXPECT_EQ(Library.find("Good"), nullptr);
}

TEST(TypeParse, ReportsAProblemOnceInATypedefOfARecord) {
  TypeLibrary Library;
  const ParseResult Result =
      parseCTypes("typedef struct _A {\n  PVOID p;\n  ULONG n;\n} A, *PA;\n"
                  "typedef struct _B { HANDLE h; } B;",
                  Library);
  // Each declaration's first problem, none from the declarators after the
  // body a problem left.
  ASSERT_EQ(Result.Problems.size(), 2u);
  EXPECT_EQ(Result.Problems[0].Line, 2u);
  EXPECT_EQ(Result.Problems[1].Line, 5u);
  EXPECT_EQ(Result.Problems[1].Message, "'HANDLE' names no type");
}

TEST(TypeParse, ReadsCpp) {
  ParseOptions Options;
  Options.Lang = Language::Cpp;
  const TypeLibrary Library = parsed(R"(
    struct alignas(8) Point { int x; int y; };
    enum class Mode : unsigned short { Off, On };
    struct Shape { Point Origin; Mode Kind; };
    static_assert(sizeof(Shape) == 0x10, "Shape");
  )",
                                     Options);
  EXPECT_EQ(shapeOf(recordOf(Library, "Shape")), "struct{Origin@0 Kind@8}");
}

namespace {

/// Renders every type of \p Library as C with layout checks, reads it back,
/// and expects each record to lay out as it did: the same size and members.
void expectRoundTrip(const TypeLibrary &Library, Language Lang,
                     BuiltinSpelling Spelling = BuiltinSpelling::Windows) {
  RenderOptions Render;
  Render.Lang = Lang;
  Render.Spelling = Spelling;
  Render.Asserts = true;
  const std::string Source = renderTypes(Library, {}, Render);
  TypeLibrary Back(Library.dataModel());
  ParseOptions Options;
  Options.Lang = Lang;
  const ParseResult Result = parseTypes(Source, Back, Options);
  for (const SourceProblem &Problem : Result.Problems) {
    // Show the line a problem is on.
    llvm::SmallVector<llvm::StringRef, 0> Lines;
    llvm::StringRef(Source).split(Lines, '\n');
    ADD_FAILURE() << Problem.Line << ":" << Problem.Column << ": "
                  << Problem.Message << "\n  "
                  << (Problem.Line && Problem.Line <= Lines.size()
                          ? Lines[Problem.Line - 1].str()
                          : "");
    return;
  }
  unsigned Records = 0;
  for (const LocalType *Type : Library.types()) {
    if (!Type->Record || !Type->Record->Defined)
      continue;
    ++Records;
    const LocalType *Read = Back.find(Type->Name);
    ASSERT_NE(Read, nullptr) << Type->Name;
    ASSERT_TRUE(Read->Record) << Type->Name;
    EXPECT_EQ(Read->Record->Size, Type->Record->Size) << Type->Name;
    // The renderer's fillers move members and hold no data.
    std::string Expected = leaves(*Type->Record);
    std::string Actual;
    const std::string ReadLeaves = leaves(*Read->Record);
    for (llvm::StringRef Leaf : llvm::split(ReadLeaves, ' '))
      if (!Leaf.empty() && !Leaf.starts_with("__filler") &&
          !Leaf.starts_with("__size@") && !Leaf.starts_with("_filler") &&
          !Leaf.starts_with("_size@"))
        Actual += Leaf.str() + " ";
    EXPECT_EQ(Actual, Expected) << Type->Name;
  }
  EXPECT_GT(Records, 0u);
}

} // namespace

TEST(TypeParse, ReadsRust) {
  ParseOptions Options;
  Options.Lang = Language::Rust;
  const TypeLibrary Library = parsed(R"(
    use core::ffi::c_void;
    /// A device.
    #[repr(C)]
    #[derive(Clone, Copy)]
    pub struct Device {
        pub Kind: u16,
        pub Anonymous: Device_0,
        pub Next: *mut Device,
        pub Dispatch: Option<unsafe extern "system" fn(*mut Device, *mut c_void) -> i32>,
        /// Locked bit 0, Count bits 4..10
        pub _bitfield: u32,
    }
    #[repr(C)]
    pub union Device_0 {
        pub Value: u64,
        pub Bytes: [u8; 8],
    }
    impl Device {
        pub const fn Count(&self) -> i32 { ((self._bitfield << 21) as i32) >> 25 }
    }
    pub type Mode = i32;
    pub const Off: Mode = 0;
    pub const On: Mode = 0x10;
    #[repr(C, packed(1))]
    pub struct Packed { pub Tag: u8, pub Value: u32 }
    const _: () = assert!(core::mem::size_of::<Packed>() == 5);
  )",
                                     Options);
  const RecordDef &Device = recordOf(Library, "Device");
  EXPECT_EQ(shapeOf(Device), "struct{Kind@0 union@8{Value@0 Bytes@0} Next@16 "
                             "Dispatch@24 Locked@256b:1 Count@260b:7}");
  EXPECT_EQ(Library.find("Device_0"), nullptr);
  // The accessor reads Count signed.
  EXPECT_EQ(Device.Members[5].Type->TheBuiltin, Builtin::Int);
  EXPECT_EQ(Library.find("Mode")->Enum->Values[1].Value, 0x10u);
  EXPECT_EQ(recordOf(Library, "Packed").Size, 5u);
}

TEST(TypeParse, ReadsGo) {
  ParseOptions Options;
  Options.Lang = Language::Go;
  const TypeLibrary Library = parsed(R"(
    package types

    import "unsafe"

    // Device: 0x30 (48) bytes.
    type Device struct {
    	Kind      uint16
    	_         [6]byte
    	Anonymous Device_0
    	Next      *Device
    	Dispatch  uintptr
    	_bitfield uint32 // 0x20 Locked bit 0, Count bits 4..10
    	_         [4]byte
    	Name_     [8]byte // 0x28 uint64, unaligned
    }

    // Device_0: the anonymous union at Device+0x08, 0x8 bytes; read it
    // through its accessors.
    type Device_0 struct {
    	raw [1]uint64
    }

    func (s *Device_0) Value() *uint64 { return (*uint64)(unsafe.Pointer(s)) }
    func (s *Device_0) Bytes() *[8]uint8 { return (*[8]uint8)(unsafe.Pointer(s)) }
    func (s *Device) Count() int32 { return int32(s._bitfield<<21) >> 25 }
    func (s *Device) Name() *uint64 { return (*uint64)(unsafe.Pointer(&s.Name_)) }

    type Mode int32

    const (
    	Off Mode = 0
    	On  Mode = 0x10
    )

    // Packed: 0x5 (5) bytes, packed to 1.
    type Packed struct {
    	Tag    uint8
    	Value_ [4]byte // 0x1 uint32, unaligned
    }

    func (s *Packed) Value() *uint32 { return (*uint32)(unsafe.Pointer(&s.Value_)) }

    var _ = [1]int{}[unsafe.Sizeof(Packed{})-0x5]
  )",
                                     Options);
  const RecordDef &Device = recordOf(Library, "Device");
  EXPECT_EQ(shapeOf(Device),
            "struct{Kind@0 union@8{Value@0 Bytes@0} Next@16 Dispatch@24 "
            "Locked@256b:1 Count@260b:7 Name@40}");
  EXPECT_EQ(Library.find("Device_0"), nullptr);
  EXPECT_EQ(Device.Size, 48u);
  // The accessor reads Count signed.
  EXPECT_EQ(Device.Members[5].Type->TheBuiltin, Builtin::Int);
  EXPECT_EQ(Library.find("Mode")->Enum->Values[1].Value, 0x10u);
  const RecordDef &Packed = recordOf(Library, "Packed");
  EXPECT_EQ(Packed.Size, 5u);
  EXPECT_EQ(shapeOf(Packed), "struct{Tag@0 Value@1}");
}

TEST(TypeParse, ReadsBackTheFixtureAsRendered) {
  auto Library = importPDBTypes(NEVERD_TYPES_FIXTURES "/local_types.pdb");
  ASSERT_TRUE(bool(Library)) << llvm::toString(Library.takeError());
  for (BuiltinSpelling Spelling :
       {BuiltinSpelling::Portable, BuiltinSpelling::Windows,
        BuiltinSpelling::Keywords})
    expectRoundTrip(**Library, Language::C, Spelling);
  expectRoundTrip(**Library, Language::Cpp);
  expectRoundTrip(**Library, Language::Rust);
  expectRoundTrip(**Library, Language::Go);
}

namespace {

/// Each named member with the builtin it holds, through records in place
/// and arrays, if it holds one; not the renderers' fillers, which hold no
/// data.
void leafBuiltins(const RecordDef &Record,
                  std::map<std::string, std::optional<Builtin>> &Out) {
  for (const Member &Field : Record.Members) {
    const llvm::StringRef Name = Field.Name;
    if (!Field.Type || Name.starts_with("_filler") ||
        Name.starts_with("__filler") || Name.starts_with("_size") ||
        Name.starts_with("__size"))
      continue;
    if (Field.isAnonymous()) {
      leafBuiltins(*Field.Type->Record, Out);
      continue;
    }
    const TypeExpr *Type = Field.Type.get();
    while (Type->Kind == TypeExprKind::Array && Type->Element)
      Type = Type->Element.get();
    Out[Field.Name] = Type->Kind == TypeExprKind::Builtin
                          ? std::optional<Builtin>(Type->TheBuiltin)
                          : std::nullopt;
  }
}

/// \p Library rendered as \p Lang in Windows' names reads back with every
/// member of a Windows-named builtin of that builtin.
void expectWindowsNamesKept(const TypeLibrary &Library, Language Lang) {
  expectRoundTrip(Library, Lang, BuiltinSpelling::Windows);
  RenderOptions Render;
  Render.Lang = Lang;
  Render.Spelling = BuiltinSpelling::Windows;
  TypeLibrary Back(Library.dataModel());
  ParseOptions Read;
  Read.Lang = Lang;
  ASSERT_TRUE(parseTypes(renderTypes(Library, {}, Render), Back, Read).ok());
  unsigned Compared = 0;
  for (const LocalType *Type : Library.types()) {
    if (!Type->Record || !Type->Record->Defined)
      continue;
    std::map<std::string, std::optional<Builtin>> Original, ReadBack;
    leafBuiltins(*Type->Record, Original);
    leafBuiltins(recordOf(Back, Type->Name), ReadBack);
    std::string Expected, Actual;
    for (const auto &[Name, Held] : Original) {
      if (!Held || !windowsTypedef(*Held))
        continue;
      Expected += Name + ":" + builtinInfo(*Held).Windows + " ";
      const auto Found = ReadBack.find(Name);
      Actual += Name + ":" +
                (Found != ReadBack.end() && Found->second
                     ? builtinInfo(*Found->second).Windows
                     : "?") +
                " ";
      ++Compared;
    }
    EXPECT_EQ(Actual, Expected) << Type->Name;
  }
  EXPECT_GT(Compared, 0u);
}

} // namespace

TEST(TypeParse, RustAndGoInWindowsNamesKeepEachBuiltin) {
  // LONG and ULONG come back as themselves, where i32 and u32 would read
  // as int and unsigned int.
  auto Library = importPDBTypes(NEVERD_TYPES_FIXTURES "/local_types.pdb");
  ASSERT_TRUE(bool(Library)) << llvm::toString(Library.takeError());
  expectWindowsNamesKept(**Library, Language::Rust);
  expectWindowsNamesKept(**Library, Language::Go);

  // Without their aliases too, as a type is edited alone; and an alias that
  // would make one another width fails where it stands.
  ParseOptions Read;
  Read.Lang = Language::Rust;
  const TypeLibrary Alone = parsed(
      "#[repr(C)]\npub struct S { pub a: ULONG, pub b: UINT, pub c: CHAR }\n",
      Read);
  const RecordDef &S = recordOf(Alone, "S");
  ASSERT_EQ(S.Members.size(), 3u);
  EXPECT_EQ(S.Members[0].Type->TheBuiltin, Builtin::ULong);
  EXPECT_EQ(S.Members[1].Type->TheBuiltin, Builtin::UInt);
  EXPECT_EQ(S.Members[2].Type->TheBuiltin, Builtin::Char);
  EXPECT_EQ(Alone.find("ULONG"), nullptr);
  TypeLibrary Wrong;
  const ParseResult Result =
      parseTypes("#[repr(C)]\npub struct T { pub a: ULONG }\n"
                 "pub type ULONG = u64;\n",
                 Wrong, Read);
  ASSERT_EQ(Result.Problems.size(), 1u);
  EXPECT_EQ(Result.Problems[0].Line, 3u);
  EXPECT_EQ(Result.Problems[0].Column, 10u);
  EXPECT_EQ(Result.Problems[0].Message,
            "'ULONG' is the builtin of 4 bytes that Windows names so; an alias "
            "cannot make it another");

  // Go reads them so too, through an alias or a defined type.
  ParseOptions ReadGo;
  ReadGo.Lang = Language::Go;
  const TypeLibrary GoAlone = parsed(
      "package types\n\ntype S struct {\n\tA ULONG\n\tB UINT\n}\n", ReadGo);
  const RecordDef &GoS = recordOf(GoAlone, "S");
  ASSERT_EQ(GoS.Members.size(), 2u);
  EXPECT_EQ(GoS.Members[0].Type->TheBuiltin, Builtin::ULong);
  EXPECT_EQ(GoS.Members[1].Type->TheBuiltin, Builtin::UInt);
  TypeLibrary GoWrong;
  const ParseResult GoResult =
      parseTypes("package types\n\ntype ULONG uint64\n", GoWrong, ReadGo);
  ASSERT_EQ(GoResult.Problems.size(), 1u);
  EXPECT_EQ(GoResult.Problems[0].Line, 3u);
  EXPECT_EQ(GoResult.Problems[0].Message,
            "'ULONG' is the builtin of 4 bytes that Windows names so; a type "
            "cannot make it another");
}

TEST(TypeParseKernel, ReadsBackEveryKernelTypeAsRendered) {
  const char *Path = std::getenv("NEVERD_TYPES_NTKRNLMP");
  if (!Path)
    GTEST_SKIP() << "NEVERD_TYPES_NTKRNLMP is not set";
  auto Library = importPDBTypes(Path);
  ASSERT_TRUE(bool(Library)) << llvm::toString(Library.takeError());
  expectRoundTrip(**Library, Language::C);
  expectRoundTrip(**Library, Language::Rust);
  expectWindowsNamesKept(**Library, Language::Rust);
  expectRoundTrip(**Library, Language::Go);
  expectWindowsNamesKept(**Library, Language::Go);
}
