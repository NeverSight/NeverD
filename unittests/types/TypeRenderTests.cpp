//===- TypeRenderTests.cpp - Local types as C, C++, Rust and Go -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/types/PDBTypeImport.h"
#include "neverd/types/TypeLayout.h"
#include "neverd/types/TypeParse.h"
#include "neverd/types/TypeRender.h"

#include <gtest/gtest.h>

using namespace neverd::types;

TEST(TypeRender, ForeignWidthPointersAreAddressesInOtherLanguages) {
  for (const auto Model : {DataModel::ILP32, DataModel::LLP64}) {
    TypeLibrary Library(Model);
    ASSERT_TRUE(parseCTypes("struct Mixed { void * __ptr32 Small; "
                            "void * __ptr64 Large; };",
                            Library)
                    .ok());
    const auto *Type = Library.find("Mixed");
    ASSERT_NE(Type, nullptr);
    for (const auto Lang : {Language::Rust, Language::Go, Language::Python}) {
      RenderOptions Options;
      Options.Lang = Lang;
      const std::string Text = renderType(Library, *Type, Options);
      const bool Wide = Model == DataModel::LLP64;
      const char *Expected =
          Lang == Language::Rust ? (Wide ? "u32" : "u64")
          : Lang == Language::Go
              ? (Wide ? "uint32" : "uint64")
              : (Wide ? "ctypes.c_uint32" : "ctypes.c_uint64");
      EXPECT_NE(Text.find(Expected), std::string::npos) << Text;
    }
  }
}

namespace {

std::unique_ptr<TypeLibrary> loadFixture() {
  auto Library = importPDBTypes(NEVERD_TYPES_FIXTURES "/local_types.pdb");
  if (!Library) {
    ADD_FAILURE() << llvm::toString(Library.takeError());
    return nullptr;
  }
  return std::move(*Library);
}

RenderOptions in(Language Lang) {
  RenderOptions Options;
  Options.Lang = Lang;
  return Options;
}

class TypeRenderFixture : public ::testing::Test {
protected:
  void SetUp() override {
    Library = loadFixture();
    ASSERT_NE(Library, nullptr);
  }

  std::string render(llvm::StringRef Name, RenderOptions Options) const {
    const LocalType *Type = Library->find(Name);
    EXPECT_NE(Type, nullptr) << Name.str();
    return Type ? renderType(*Library, *Type, Options) : std::string();
  }

  std::string header(llvm::StringRef Name, RenderOptions Options) const {
    const LocalType *Type = Library->find(Name);
    EXPECT_NE(Type, nullptr) << Name.str();
    if (!Type)
      return std::string();
    const LocalType *Roots[] = {Type};
    return renderTypes(*Library, Roots, Options);
  }

  std::unique_ptr<TypeLibrary> Library;
};

Member member(std::string Name, TypeExprRef Type, uint64_t Byte) {
  Member Field;
  Field.Name = std::move(Name);
  Field.Type = std::move(Type);
  Field.BitOffset = Byte * 8;
  return Field;
}

Member bitfield(std::string Name, Builtin Type, uint64_t Byte,
                uint32_t Position, uint32_t Width) {
  Member Field = member(std::move(Name), makeBuiltin(Type), Byte);
  Field.StorageBitOffset = Field.BitOffset;
  Field.BitOffset += Position;
  Field.BitWidth = Width;
  return Field;
}

LocalType structure(std::string Name, uint64_t Size,
                    std::vector<Member> Members) {
  LocalType Type;
  Type.Name = std::move(Name);
  Type.Kind = TypeKind::Struct;
  auto Record = std::make_shared<RecordDef>();
  Record->Size = Size;
  Record->Members = std::move(Members);
  Type.Record = std::move(Record);
  return Type;
}

bool contains(const std::string &Text, llvm::StringRef Part) {
  return llvm::StringRef(Text).contains(Part);
}

} // namespace

TEST(TypeRender, NamesItsLanguages) {
  for (Language Lang :
       {Language::C, Language::Cpp, Language::Rust, Language::Go})
    EXPECT_EQ(languageNamed(languageId(Lang)), Lang);
  EXPECT_EQ(languageNamed("RUST"), Language::Rust);
  EXPECT_EQ(languageNamed("pascal"), std::nullopt);
}

TEST_F(TypeRenderFixture, CShowsEveryOffsetAndBit) {
  EXPECT_EQ(render("_THREAD_LIKE", in(Language::C)),
            R"(// _THREAD_LIKE: 0x18 (24) bytes
struct _THREAD_LIKE
{
    uint64_t Header;                                            // 0x00
    union                                                       // 0x08
    {
        struct NEVERD_MS_STRUCT                                 // 0x08
        {
            uint32_t AutoBoostActive : 1;                       // 0x08 bit 0
            uint32_t ReadyTransition : 1;                       // 0x08 bit 1
            uint32_t MiscSpare : 30;                            // 0x08 bits 2..31
        };
        int32_t MiscFlags;                                      // 0x08
    };
    union                                                       // 0x0C
    {
        struct NEVERD_MS_STRUCT                                 // 0x0C
        {
            uint32_t ThreadFlagsSpare : 2;                      // 0x0C bits 0..1
            uint32_t AutoAlignment : 1;                         // 0x0C bit 2
            uint32_t DisableBoost : 1;                          // 0x0C bit 3
            uint32_t ThreadReserved : 28;                       // 0x0C bits 4..31
        };
        int32_t ThreadFlags;                                    // 0x0C
    };
    uint8_t Tail[3];                                            // 0x10
    // 0x13: 5 bytes of padding
};
)");
}

TEST_F(TypeRenderFixture, CSpellsBuiltinsAsAsked) {
  // Exact widths unless asked.
  RenderOptions Options = in(Language::C);
  EXPECT_TRUE(contains(render("_THREAD_LIKE", Options), "uint64_t Header;"));
  EXPECT_TRUE(contains(render("_THREAD_LIKE", Options),
                       "uint32_t AutoBoostActive : 1;"));
  Options.Spelling = BuiltinSpelling::Windows;
  EXPECT_TRUE(
      contains(render("_THREAD_LIKE", Options), "ULONG AutoBoostActive : 1;"));
  Options.Spelling = BuiltinSpelling::Keywords;
  EXPECT_TRUE(
      contains(render("_THREAD_LIKE", Options), "unsigned long long Header;"));
  Options.HexOffsets = false;
  EXPECT_TRUE(contains(render("_THREAD_LIKE", Options), "// +16"));
}

TEST_F(TypeRenderFixture, AHeaderDefinesWhatATypeHoldsFirst) {
  RenderOptions Options = in(Language::C);
  Options.Asserts = true;
  const std::string Text = header("_DEVICE", Options);
  // Records it points to are declared; what it holds is defined before it.
  EXPECT_TRUE(contains(Text, "struct _DEVICE;\n"));
  const size_t Device = Text.find("struct _DEVICE\n{");
  for (const char *Held : {"struct _LIST_ENTRY\n{", "union _LARGE_INTEGER\n{",
                           "struct _PUSH_LOCK\n{", "enum _POOL_KIND\n{"}) {
    const size_t At = Text.find(Held);
    ASSERT_NE(At, std::string::npos) << Held;
    EXPECT_LT(At, Device) << Held;
  }
  EXPECT_FALSE(contains(Text, "_THREAD_LIKE"));
  // Exact widths need no declaring; Windows' names do, where the Windows
  // headers are not.
  EXPECT_FALSE(contains(Text, "typedef uint32_t ULONG;"));
  Options.Spelling = BuiltinSpelling::Windows;
  EXPECT_TRUE(contains(header("_DEVICE", Options), "typedef uint32_t ULONG;"));
  // The header compiles as C++ too: C names its check as C++ does.
  EXPECT_TRUE(contains(Text, "#if !defined(__cplusplus) && "
                             "!defined(static_assert)\n#define static_assert "
                             "_Static_assert\n#endif\n"));
  EXPECT_TRUE(
      contains(Text, "\nstatic_assert(offsetof(struct _DEVICE, Pool) == 0x50, "
                     "\"_DEVICE.Pool\");"));
}

TEST_F(TypeRenderFixture, CppNamesTypesBare) {
  RenderOptions Options = in(Language::Cpp);
  Options.Asserts = true;
  const std::string Text = header("_DEVICE", Options);
  EXPECT_TRUE(contains(Text, "struct _DEVICE;\n"));
  EXPECT_TRUE(contains(Text, "    _LIST_ENTRY Links;"));
  EXPECT_TRUE(contains(Text, "static_assert(sizeof(_DEVICE) == 0x58"));
}

TEST_F(TypeRenderFixture, CKeepsPacking) {
  const std::string Text = render("_PACKED", in(Language::C));
  EXPECT_TRUE(contains(Text, "#pragma pack(push, 1)\nstruct _PACKED\n"));
  EXPECT_TRUE(contains(Text, "    uint32_t Value;"));
  EXPECT_TRUE(contains(Text, "};\n#pragma pack(pop)\n"));
}

TEST_F(TypeRenderFixture, RustNamesAnonymousRecordsAsWindowsRsDoes) {
  const std::string Text = render("_THREAD_LIKE", in(Language::Rust));
  EXPECT_TRUE(contains(Text, "#[repr(C)]\n#[derive(Clone, Copy)]\npub struct "
                             "_THREAD_LIKE {\n"));
  EXPECT_TRUE(contains(Text, "    pub Anonymous1: _THREAD_LIKE_0,"));
  EXPECT_TRUE(contains(Text, "    pub Anonymous2: _THREAD_LIKE_1,"));
  EXPECT_TRUE(contains(Text, "pub union _THREAD_LIKE_0 {\n"));
  EXPECT_TRUE(contains(Text, "    pub MiscFlags: i32,"));
  EXPECT_TRUE(contains(Text, "    pub _bitfield: u32,"));
  EXPECT_TRUE(contains(Text, "    pub const fn ReadyTransition(&self) -> u32 "
                             "{\n        (self._bitfield >> 1) & 0x1\n    }"));
}

TEST_F(TypeRenderFixture, RustTakesWindowsNamesAsAliases) {
  // Exact widths unless asked, as windows-rs spells them.
  EXPECT_TRUE(contains(render("_THREAD_LIKE", in(Language::Rust)),
                       "    pub MiscFlags: i32,"));
  RenderOptions Options = in(Language::Rust);
  Options.Spelling = BuiltinSpelling::Windows;
  const std::string Text = header("_THREAD_LIKE", Options);
  // The names the types use, declared as wdk-sys declares them, at the
  // program's widths.
  EXPECT_TRUE(contains(Text, "\n/// The Windows headers' names of builtin "
                             "types.\npub type UCHAR = u8;\npub type LONG = "
                             "i32;\npub type ULONG = u32;\npub type "
                             "ULONGLONG = u64;\n"))
      << Text;
  EXPECT_TRUE(contains(Text, "    pub Header: ULONGLONG,"));
  EXPECT_TRUE(contains(Text, "    pub MiscFlags: LONG,"));
  EXPECT_TRUE(contains(Text, "    pub Tail: [UCHAR; 3],"));
  // A bitfield reads as its own type; its storage stays a width.
  EXPECT_TRUE(contains(Text, "    pub _bitfield: u32,"));
  EXPECT_TRUE(
      contains(Text, "    pub const fn ReadyTransition(&self) -> ULONG {"));
  // A spelling Rust does not take renders as its default.
  Options.Spelling = BuiltinSpelling::Keywords;
  EXPECT_TRUE(
      contains(header("_THREAD_LIKE", Options), "    pub MiscFlags: i32,"));
  EXPECT_EQ(spellingOf(Options), BuiltinSpelling::Portable);
  // Each language lists its default, exact widths, first.
  EXPECT_EQ(spellingsText(Language::Rust), "portable or windows");
  EXPECT_EQ(spellingsText(Language::Go), "portable or windows");
  EXPECT_EQ(spellingsText(Language::C), "portable, windows or keywords");
  for (Language Lang :
       {Language::C, Language::Cpp, Language::Rust, Language::Go,
        Language::Python, Language::JavaScript, Language::Lua})
    EXPECT_EQ(defaultSpelling(Lang), BuiltinSpelling::Portable);
  EXPECT_FALSE(spellingNamed("ida"));
}

TEST_F(TypeRenderFixture, GoTakesWindowsNamesAsAliases) {
  // Exact widths unless asked, as golang.org/x/sys/windows spells them.
  EXPECT_TRUE(
      contains(render("_THREAD_LIKE", in(Language::Go)), "\tHeader uint64"));
  RenderOptions Options = in(Language::Go);
  Options.Spelling = BuiltinSpelling::Windows;
  const std::string Text = header("_THREAD_LIKE", Options);
  EXPECT_TRUE(contains(Text, "\n// The Windows headers' names of builtin "
                             "types.\ntype UCHAR = uint8\ntype LONG = "
                             "int32\ntype ULONG = uint32\ntype ULONGLONG = "
                             "uint64\n"))
      << Text;
  EXPECT_TRUE(contains(Text, "\tHeader ULONGLONG"));
  EXPECT_TRUE(contains(Text, "\tTail [3]UCHAR"));
  EXPECT_TRUE(contains(Text, ") MiscFlags() *LONG {"));
  // A bitfield reads as its own type; its storage stays a width.
  EXPECT_TRUE(contains(Text, "\t_bitfield uint32"));
  EXPECT_TRUE(contains(Text, ") ReadyTransition() ULONG {"));
  EXPECT_TRUE(contains(Text, ") SetReadyTransition(value ULONG) {"));
}

TEST_F(TypeRenderFixture, PythonDeclaresRecordsAsCtypesDoes) {
  RenderOptions Options = in(Language::Python);
  Options.Asserts = true;
  const std::string Text = header("_THREAD_LIKE", Options);
  // Declared first and laid out after, so that a pointer may name a record
  // laid out later.
  EXPECT_TRUE(contains(Text, "class _THREAD_LIKE(ctypes.Structure):\n    "
                             "\"\"\"_THREAD_LIKE: 0x18 (24) bytes.\"\"\"\n"))
      << Text;
  EXPECT_LT(Text.find("class _THREAD_LIKE("),
            Text.find("_THREAD_LIKE._fields_ = ["));
  // Records in place are classes of their own, which _anonymous_ promotes.
  EXPECT_TRUE(contains(Text, "class _THREAD_LIKE_0(ctypes.Union):"));
  EXPECT_TRUE(contains(
      Text, "_THREAD_LIKE._anonymous_ = (\"Anonymous1\", \"Anonymous2\")\n"));
  // A run of bitfields is storage read through properties, each run's
  // storage named apart from the others the record promotes.
  EXPECT_TRUE(contains(Text, "(\"_bitfield\", ctypes.c_uint32),"));
  EXPECT_TRUE(contains(Text, "(\"_bitfield1\", ctypes.c_uint32),"));
  EXPECT_TRUE(
      contains(Text, "    @property\n    def ReadyTransition(self) -> int:\n"));
  EXPECT_TRUE(contains(Text, "        return (self._bitfield >> 1) & 0x1\n"));
  EXPECT_TRUE(contains(Text, "        return (self._bitfield1 >> 2) & 0x1\n"));
  EXPECT_TRUE(contains(
      Text, "assert ctypes.sizeof(_THREAD_LIKE) == 0x18, \"_THREAD_LIKE\"\n"));
  EXPECT_TRUE(contains(Text, "assert _THREAD_LIKE.Tail.offset == 0x10, "
                             "\"_THREAD_LIKE.Tail\"\n"));

  // A pack lays out as MSVC does, which Python 3.14 asks for by name.
  EXPECT_TRUE(contains(header("_PACKED", in(Language::Python)),
                       "_PACKED._pack_ = 1\n_PACKED._layout_ = \"ms\"\n"
                       "_PACKED._fields_ = [\n"));
  // Alone, a record's layout is in its class.
  EXPECT_TRUE(
      contains(render("_PACKED", in(Language::Python)),
               "class _PACKED(ctypes.Structure):\n    \"\"\"_PACKED: "
               "0x7 (7) bytes, packed to 1.\"\"\"\n\n    _pack_ = 1\n"));

  // An enumeration is an IntEnum, its field the underlying integer; a
  // function's address is ctypes.c_void_p, its C type in the comment.
  const std::string Device = header("_DEVICE", in(Language::Python));
  EXPECT_TRUE(contains(Device, "class _POOL_KIND(enum.IntEnum):"));
  EXPECT_TRUE(contains(Device, "(\"Pool\", ctypes.c_int32),"));
  EXPECT_TRUE(contains(Device, "enum _POOL_KIND\n"));
  EXPECT_TRUE(contains(Device, "(\"MajorFunction\", ctypes.c_void_p * 4),"));
  EXPECT_TRUE(contains(Device, "(*[4])(struct _DEVICE *, "));

  // Windows' names are aliases of exact widths at the top.
  Options = in(Language::Python);
  Options.Spelling = BuiltinSpelling::Windows;
  const std::string Windows = header("_PACKED", Options);
  EXPECT_TRUE(contains(Windows, "UCHAR = ctypes.c_uint8\n")) << Windows;
  EXPECT_TRUE(contains(Windows, "(\"Value\", ULONG),"));
}

TEST(TypeRender, PythonBindsTypedefsAndLaysOutInOrder) {
  // A typedef is bound before a layout names it; one of an array after the
  // layout of what it holds, since ctypes sizes an array type once.
  TypeLibrary Library;
  ASSERT_TRUE(parseCTypes(R"(
    typedef struct _NODE NODE;
    struct _NODE { NODE *Next; unsigned long Value; };
    typedef struct _PAIR { unsigned char A; unsigned long B; } PAIR;
    typedef PAIR PAIRS[2];
    struct _HOLDER { PAIRS Items; NODE Head; };
  )",
                          Library)
                  .ok());
  const std::string Text = renderTypes(Library, {}, in(Language::Python));
  const auto at = [&](llvm::StringRef Part) {
    const size_t Found = Text.find(Part.str());
    EXPECT_NE(Found, std::string::npos) << Part.str() << "\n" << Text;
    return Found;
  };
  EXPECT_LT(at("NODE = _NODE\n"), at("_NODE._fields_ = ["));
  EXPECT_LT(at("_PAIR._fields_ = ["), at("PAIRS = PAIR * 2\n"));
  EXPECT_LT(at("PAIRS = PAIR * 2\n"), at("_HOLDER._fields_ = ["));
  EXPECT_LT(at("_NODE._fields_ = ["), at("_HOLDER._fields_ = ["));
  EXPECT_TRUE(contains(Text, "(\"Next\", ctypes.POINTER(NODE)),"));
}

TEST_F(TypeRenderFixture, JavaScriptReadsRecordsThroughDataView) {
  RenderOptions Options = in(Language::JavaScript);
  Options.Asserts = true;
  const std::string Thread = header("_THREAD_LIKE", Options);
  // TypeScript checks the module by its JSDoc.
  EXPECT_TRUE(Thread.starts_with("// @ts-check\n")) << Thread;
  EXPECT_TRUE(contains(Thread, "export class _THREAD_LIKE {\n"));
  EXPECT_TRUE(contains(Thread, "  static size = 0x18;\n"));
  // The caller gives the bytes, where the record begins and their order.
  EXPECT_TRUE(contains(
      Thread, "  constructor(view, offset = 0, littleEndian = true) {"));
  // An anonymous record's members are the record's own, as C reaches them;
  // a bitfield reads its unit in 32-bit arithmetic.
  EXPECT_TRUE(contains(Thread, "  get ReadyTransition() { return "
                               "(this.#view.getUint32(this.#offset + 0x08, "
                               "this.#little) >>> 1) & 0x1; }\n"));
  EXPECT_TRUE(contains(Thread, "(unit & 0xFFFFFFFD) | ((value << 1) & 0x2)"));
  EXPECT_FALSE(contains(Thread, "class _THREAD_LIKE_0"));
  // An array's elements by index, which the module checks.
  EXPECT_TRUE(contains(Thread, "  Tail(i) { return this.#view.getUint8("
                               "this.#offset + 0x10 + 1 * index(i, 3)); }\n"));
  EXPECT_TRUE(contains(Thread, "function index(i, count) {"));
  // Each accessor read and written back within a buffer of the record's
  // size, which throws past its end.
  EXPECT_TRUE(contains(Thread, "check(_THREAD_LIKE, (r) => {\n"
                               "  r.Header = r.Header;\n"));
  EXPECT_TRUE(contains(Thread, "  r.setTail(2, r.Tail(2));\n"));

  const std::string Device = header("_DEVICE", Options);
  // A record held in place is a view of its bytes; an eight-byte value,
  // a pointer's address among them, is a bigint.
  EXPECT_TRUE(contains(Device, "  get Links() { return new _LIST_ENTRY("
                               "this.#view, this.#offset + 0x08, "
                               "this.#little); }\n"));
  EXPECT_TRUE(contains(Device, "[0x08, _LIST_ENTRY.size]"));
  EXPECT_TRUE(contains(Device, "  MajorFunction(i) { return "
                               "this.#view.getBigUint64(this.#offset + 0x18 + "
                               "8 * index(i, 4), this.#little); }\n"));
  // An enumeration is a frozen object, its field the underlying integer.
  EXPECT_TRUE(contains(Device, "export const _POOL_KIND = Object.freeze({\n"));
  EXPECT_TRUE(contains(Device, "  MustSucceed: -2147483648,\n"));
  EXPECT_TRUE(contains(Device, "/** @typedef {number} _POOL_KIND */\n"));
  EXPECT_TRUE(contains(Device, "/** 0x50 enum _POOL_KIND Pool @returns "
                               "{_POOL_KIND} */\n  get Pool() { return "
                               "this.#view.getInt32("));

  // A bitfield of an eight-byte unit reads as a bigint.
  EXPECT_TRUE(contains(header("_PUSH_LOCK", in(Language::JavaScript)),
                       "  get Shared() { return BigInt.asUintN(60, "
                       "this.#view.getBigUint64(this.#offset + 0x0, "
                       "this.#little) >> 4n); }\n"));

  // Windows' names are typedefs of the JavaScript types.
  Options = in(Language::JavaScript);
  Options.Spelling = BuiltinSpelling::Windows;
  const std::string Windows = header("_PACKED", Options);
  EXPECT_TRUE(contains(Windows, "/** @typedef {number} ULONG */\n")) << Windows;
  EXPECT_TRUE(contains(Windows, "@returns {ULONG} */"));
}

TEST(TypeRender, JavaScriptExtendsSignsAndNamesWhatItDeclares) {
  TypeLibrary Library;
  ASSERT_TRUE(parseCTypes(R"(
    typedef struct _NODE NODE, *PNODE;
    struct _NODE { PNODE Next; int Low : 3; int High : 29; };
    struct DataView { int constructor; __int128 Wide; };
  )",
                          Library)
                  .ok());
  const std::string Text = renderTypes(Library, {}, in(Language::JavaScript));
  // A signed field's top bit is the 32nd and back.
  EXPECT_TRUE(contains(Text, "  get Low() { return (this.#view.getUint32("
                             "this.#offset + 0x08, this.#little) << 29) >> "
                             "29; }\n"))
      << Text;
  EXPECT_TRUE(contains(Text, "  get High() { return (this.#view.getUint32("
                             "this.#offset + 0x08, this.#little) << 0) >> "
                             "3; }\n"));
  // A typedef of a record names its class; one of a pointer, its address.
  EXPECT_TRUE(contains(Text, "/** @typedef {_NODE} NODE */\n"));
  EXPECT_TRUE(contains(Text, "export const NODE = _NODE;\n"));
  EXPECT_TRUE(contains(Text, "/** @typedef {bigint} PNODE The address of "));
  EXPECT_TRUE(contains(Text, "@returns {PNODE} */\n  get Next() {"));
  // What the module itself names, and a class's own words, step aside.
  EXPECT_TRUE(contains(Text, "export class DataView_ {"));
  EXPECT_TRUE(contains(Text, "  get constructor_() {"));
  // Sixteen bytes read as one bigint from two halves.
  EXPECT_TRUE(contains(Text, "  get Wide() { return this.#wide("));
  EXPECT_TRUE(contains(Text, "  #wide(at, signed) {"));

  // A type TypeScript names itself, as the Windows headers' `boolean`, an
  // unsigned char, takes a name of its own in JSDoc.
  TypeLibrary Windows;
  ASSERT_TRUE(parseCTypes("typedef unsigned char boolean;\n"
                          "typedef boolean BOOLEAN;\n"
                          "struct FLAGS { BOOLEAN On; };\n",
                          Windows)
                  .ok());
  const std::string Flags = renderTypes(Windows, {}, in(Language::JavaScript));
  EXPECT_TRUE(contains(Flags, "/** @typedef {number} boolean_ */\n")) << Flags;
  EXPECT_TRUE(contains(Flags, "/** @typedef {boolean_} BOOLEAN */\n")) << Flags;
}

TEST_F(TypeRenderFixture, LuaReadsRecordsThroughStringUnpack) {
  RenderOptions Options = in(Language::Lua);
  Options.Asserts = true;
  const std::string Thread = header("_THREAD_LIKE", Options);
  // A module of record types, each in a block of its own.
  EXPECT_TRUE(Thread.starts_with("-- ")) << Thread;
  EXPECT_TRUE(contains(Thread, "local M = {}\n"));
  EXPECT_TRUE(contains(Thread, "do\n  local T = record(\"_THREAD_LIKE\", "
                               "0x18)\n  M._THREAD_LIKE = T\n"));
  EXPECT_TRUE(Thread.ends_with("return M\n"));
  // An anonymous record's members are the record's own; a bitfield masks
  // its unit, which string.pack takes whole.
  EXPECT_TRUE(contains(Thread, "  function T.get.ReadyTransition(r) return "
                               "(get(r, 0x08, \"I4\") >> 1) & 0x1 end\n"));
  EXPECT_TRUE(contains(Thread, "put(r, 0x08, \"I4\", (get(r, 0x08, \"I4\") & "
                               "0xFFFFFFFD) | ((value << 1) & 0x2))"));
  // An array's elements by index, through methods.
  EXPECT_TRUE(contains(Thread, "  function T.methods.Tail(r, i) return "
                               "get(r, 0x10 + 1 * index(i, 3), \"B\") end\n"));
  EXPECT_TRUE(contains(Thread, "  function T.methods.setTail(r, i, value)"));
  // Each accessor read and written back within bytes of the record's size.
  EXPECT_TRUE(contains(Thread, "check(M._THREAD_LIKE, function(r)\n"
                               "  r.Header = r.Header\n"));
  EXPECT_TRUE(contains(Thread, "  r:setTail(2, r:Tail(2))\n"));

  const std::string Device = header("_DEVICE", Options);
  // A record held in place is a view of the same bytes; a pointer reads
  // as its address.
  EXPECT_TRUE(contains(Device, "  function T.get.Links(r) return "
                               "M._LIST_ENTRY.view(r[BYTES], r[AT] + 0x08, "
                               "r[ORDER] == \">\") end\n"));
  EXPECT_TRUE(contains(Device, "{ 0x08, M._LIST_ENTRY.size }"));
  EXPECT_TRUE(contains(Device, "-- 0x38 struct _DEVICE *Next: address\n"));
  EXPECT_TRUE(contains(Device, "M._POOL_KIND = {\n"));
  EXPECT_TRUE(contains(Device, "  MustSucceed = -2147483648,\n"));
  EXPECT_TRUE(contains(Device, "  function T.get.Pool(r) return get(r, "
                               "0x50, \"i4\") end\n"));

  // A signed bitfield extends its top bit; an eight-byte unit is one of
  // Lua's integers.
  EXPECT_TRUE(contains(header("_PUSH_LOCK", in(Language::Lua)),
                       "  function T.get.Shared(r) return (get(r, 0x0, "
                       "\"I8\") >> 4) & 0xFFFFFFFFFFFFFFF end\n"));
}

TEST(TypeRender, LuaExtendsSignsAndNamesWhatItDeclares) {
  TypeLibrary Library;
  ASSERT_TRUE(parseCTypes(R"(
    typedef struct _NODE NODE, *PNODE;
    struct _NODE { PNODE Next; int Low : 3; int end; __int128 Wide; };
  )",
                          Library)
                  .ok());
  const std::string Text = renderTypes(Library, {}, in(Language::Lua));
  EXPECT_TRUE(contains(Text, "  function T.get.Low(r) local v = get(r, "
                             "0x08, \"I4\") & 0x7; return (v ~ 0x4) - 0x4 "
                             "end\n"))
      << Text;
  // A typedef of a record names its type; one of a pointer, an address.
  EXPECT_TRUE(contains(Text, "M.NODE = M._NODE\n"));
  EXPECT_TRUE(contains(Text, "-- PNODE is struct _NODE *, which reads as an "
                             "address.\n"));
  // Lua's words step aside; sixteen bytes read as the string they are.
  EXPECT_TRUE(contains(Text, "  function T.get.end_(r)"));
  EXPECT_TRUE(contains(Text, "  function T.get.Wide(r) return raw(r, 0x10, "
                             "16) end\n"));
}

TEST_F(TypeRenderFixture, AnEmbeddedCRendererRendersAsEachCallDoes) {
  // One renderer spells the library's names once for every unit; each unit
  // reads as a call of its own makes it, whatever it rendered before.
  RenderOptions Options = in(Language::C);
  Options.Spelling = BuiltinSpelling::Portable;
  Options.Asserts = true;
  const EmbeddedCRenderer Renderer(*Library, Options);
  std::vector<std::vector<const LocalType *>> Units;
  for (const char *Name :
       {"_DEVICE", "_USES_FORWARD", "_POOL_KIND", "_LARGE_INTEGER"})
    if (const LocalType *Type = Library->find(Name))
      Units.push_back({Type});
  ASSERT_EQ(Units.size(), 4u);
  Units.push_back({Units[0][0], Units[1][0]});
  for (int Pass = 0; Pass < 2; ++Pass)
    for (const auto &Roots : Units) {
      const EmbeddedC Shared = Renderer.render(Roots);
      const EmbeddedC Own = renderEmbeddedC(*Library, Roots, Options);
      EXPECT_EQ(Shared.Text, Own.Text);
      EXPECT_EQ(Shared.Records, Own.Records);
      EXPECT_EQ(Shared.Declared, Own.Declared);
      EXPECT_EQ(Shared.Identifiers, Own.Identifiers);
      EXPECT_EQ(Shared.Types, Own.Types);
      EXPECT_EQ(Shared.DeclaredTypes, Own.DeclaredTypes);
    }
}

TEST_F(TypeRenderFixture, EmbeddedCDefinesWhatAUnitNames) {
  // A decompiled unit holds the records it names, what they hold by value,
  // and declarations of what they only point to, each under a guard.
  const LocalType *Device = Library->find("_DEVICE");
  const LocalType *Uses = Library->find("_USES_FORWARD");
  ASSERT_TRUE(Device && Uses);
  RenderOptions Options = in(Language::C);
  Options.Spelling = BuiltinSpelling::Portable;
  Options.Asserts = true;
  const LocalType *Roots[] = {Device, Uses};
  const EmbeddedC Embedded = renderEmbeddedC(*Library, Roots, Options);
  const std::string &Text = Embedded.Text;
  EXPECT_FALSE(contains(Text, "#pragma once")) << Text;
  EXPECT_TRUE(contains(Text, "#ifndef NEVERD_TYPE__DEVICE\n"
                             "#define NEVERD_TYPE__DEVICE\n"));
  EXPECT_TRUE(contains(Text, "struct _DEVICE\n{\n"));
  EXPECT_TRUE(contains(Text, "static_assert(sizeof(struct _DEVICE) == 0x58"));
  // By value: defined before the record that holds them.
  EXPECT_LT(Text.find("union _LARGE_INTEGER\n{"),
            Text.find("struct _DEVICE\n{"));
  EXPECT_TRUE(contains(Text, "enum _POOL_KIND"));
  // Only pointed to: declared.
  EXPECT_TRUE(contains(Text, "struct _FORWARD_ONLY;\n"));
  EXPECT_FALSE(contains(Text, "struct _FORWARD_ONLY\n{"));
  EXPECT_TRUE(llvm::is_contained(
      Embedded.Records, std::pair<std::string, bool>("_DEVICE", false)));
  EXPECT_TRUE(llvm::is_contained(
      Embedded.Records, std::pair<std::string, bool>("_LARGE_INTEGER", true)));
  EXPECT_TRUE(llvm::is_contained(Embedded.Identifiers, "MustSucceed"));
  EXPECT_TRUE(llvm::is_contained(Embedded.Types, "_PUSH_LOCK"));
  EXPECT_FALSE(llvm::is_contained(Embedded.Types, "_FORWARD_ONLY"));
}

TEST_F(TypeRenderFixture, EmbeddedSourceSaysHowCodeReachesEachMember) {
  // The types a C unit holds, as Rust and Go pseudocode holds them beside
  // the code, and how that code reaches the members C names on their own.
  const LocalType *Thread = Library->find("_THREAD_LIKE");
  const LocalType *Packed = Library->find("_PACKED");
  const LocalType *Integer = Library->find("_LARGE_INTEGER");
  const LocalType *Forward = Library->find("_FORWARD_ONLY");
  ASSERT_TRUE(Thread && Packed && Integer && Forward);
  const LocalType *Defined[] = {Thread, Packed, Integer};
  const LocalType *Declared[] = {Forward};
  const auto hops = [](const EmbeddedSource &Source, llvm::StringRef Record,
                       llvm::StringRef Member) {
    std::string Text;
    const auto Members = Source.Members.find(Record.str());
    if (Members == Source.Members.end())
      return Text;
    const auto Path = Members->second.find(Member.str());
    if (Path == Members->second.end())
      return Text;
    for (const MemberPath::Hop &Hop : Path->second.Hops)
      Text += (Text.empty() ? "" : ".") + Hop.Name + (Hop.Accessor ? "()" : "");
    // A bit-field's methods, after the hops to the record that has them.
    if (!Path->second.Getter.empty())
      Text += " " + Path->second.Getter + "/" + Path->second.Setter;
    return Text;
  };
  const EmbeddedSource Rust =
      renderEmbeddedSource(*Library, Defined, Declared, in(Language::Rust));
  // No module header: the text sits beside the code.
  EXPECT_FALSE(contains(Rust.Text, "#![allow")) << Rust.Text;
  EXPECT_FALSE(contains(Rust.Text, "use core::ffi::c_void;"));
  EXPECT_TRUE(contains(Rust.Text, "pub struct _FORWARD_ONLY {\n    "
                                  "_opaque: [u8; 0],\n}"));
  EXPECT_TRUE(contains(Rust.Text, "pub union _THREAD_LIKE_0 {"));
  EXPECT_EQ(hops(Rust, "_THREAD_LIKE", "Header"), "Header");
  EXPECT_EQ(hops(Rust, "_THREAD_LIKE", "MiscFlags"), "Anonymous1.MiscFlags");
  EXPECT_EQ(hops(Rust, "_PACKED", "Value"), "Value");
  EXPECT_EQ(hops(Rust, "_LARGE_INTEGER", "QuadPart"), "QuadPart");
  // A bit-field reads and writes through methods of the record that holds
  // its storage, here a structure in an anonymous union.
  EXPECT_EQ(hops(Rust, "_THREAD_LIKE", "AutoBoostActive"),
            "Anonymous1.Anonymous AutoBoostActive/set_AutoBoostActive");
  // Go reads a union's members, and what it cannot align, through
  // methods that return pointers to them.
  const EmbeddedSource Go =
      renderEmbeddedSource(*Library, Defined, Declared, in(Language::Go));
  EXPECT_FALSE(contains(Go.Text, "package ")) << Go.Text;
  EXPECT_FALSE(contains(Go.Text, "import "));
  EXPECT_TRUE(contains(Go.Text, "type _FORWARD_ONLY struct{}"));
  EXPECT_EQ(hops(Go, "_THREAD_LIKE", "MiscFlags"), "Anonymous1.MiscFlags()");
  EXPECT_EQ(hops(Go, "_PACKED", "Tag"), "Tag");
  EXPECT_EQ(hops(Go, "_PACKED", "Value"), "Value()");
  EXPECT_EQ(hops(Go, "_LARGE_INTEGER", "QuadPart"), "QuadPart()");
  EXPECT_EQ(hops(Go, "_THREAD_LIKE", "AutoBoostActive"),
            "Anonymous1.Anonymous() AutoBoostActive/SetAutoBoostActive");
  EXPECT_TRUE(renderEmbeddedSource(*Library, Defined, Declared, in(Language::C))
                  .Text.empty());
}

TEST_F(TypeRenderFixture, RustAndGoKeepPacking) {
  EXPECT_TRUE(
      contains(render("_PACKED", in(Language::Rust)), "#[repr(C, packed(1))]"));
  // Go cannot align a member below its alignment: bytes and an accessor.
  const std::string Go = render("_PACKED", in(Language::Go));
  EXPECT_TRUE(contains(Go, "\tValue_ [4]byte"));
  EXPECT_TRUE(contains(Go, "func (s *_PACKED) Value() *uint32 {\n\treturn "
                           "(*uint32)(unsafe.Pointer(&s.Value_))\n}"));
}

TEST_F(TypeRenderFixture, GoReadsAUnionThroughAccessors) {
  const std::string Text = render("_LARGE_INTEGER", in(Language::Go));
  EXPECT_TRUE(contains(Text, "type _LARGE_INTEGER struct {\n\traw [1]uint64"));
  EXPECT_TRUE(contains(Text, "func (s *_LARGE_INTEGER) QuadPart() *int64 {"));
  EXPECT_TRUE(contains(Text, "func (s *_LARGE_INTEGER) u() *_LARGE_INTEGER_1"));
}

TEST_F(TypeRenderFixture, EnumerationsKeepTheirValues) {
  EXPECT_TRUE(contains(render("_SIGNED_KIND", in(Language::C)),
                       "    Negative = -1,\n"));
  EXPECT_TRUE(contains(render("_SIGNED_KIND", in(Language::Rust)),
                       "pub type _SIGNED_KIND = i32;\npub const Negative: "
                       "_SIGNED_KIND = -1;"));
  EXPECT_TRUE(contains(render("_SIGNED_KIND", in(Language::Go)),
                       "\tNegative _SIGNED_KIND = -1\n"));
}

TEST(TypeRender, SignedBitfieldsReadTheirSign) {
  TypeLibrary Library;
  Library.define(structure("_FLAGS", 4,
                           {bitfield("Low", Builtin::Long, 0, 0, 4),
                            bitfield("Delta", Builtin::Long, 0, 4, 4)}));
  const LocalType &Flags = *Library.find("_FLAGS");
  // C extends a signed field's top bit, so the accessors do too.
  EXPECT_TRUE(contains(renderType(Library, Flags, in(Language::Rust)),
                       "pub const fn Delta(&self) -> i32 {\n        "
                       "((self._bitfield << 24) as i32) >> 28\n    }"));
  EXPECT_TRUE(contains(renderType(Library, Flags, in(Language::Go)),
                       "func (s *_FLAGS) Delta() int32 {\n\treturn "
                       "int32(s._bitfield<<24) >> 28\n}"));
}

TEST(TypeRender, UnnamedBitsAndClosedRunsStay) {
  // Bits 0..3 of the first unit are unnamed, and the second unit begins
  // although the first has room.
  TypeLibrary Library;
  Library.define(structure("_BITS", 8,
                           {bitfield("A", Builtin::ULong, 0, 4, 4),
                            bitfield("B", Builtin::ULong, 4, 0, 4)}));
  const std::string Text =
      renderType(Library, *Library.find("_BITS"), in(Language::C));
  EXPECT_TRUE(contains(Text, "    uint32_t : 4;"));
  EXPECT_TRUE(contains(Text, "    uint32_t A : 4;"));
  EXPECT_TRUE(contains(Text, "    uint32_t : 0;\n    uint32_t B : 4;"));
}

TEST(TypeRender, AMemberPastItsPlaceTakesAFiller) {
  // MSVC put Wide at 0x10, as __declspec(align(16)) would.
  TypeLibrary Library;
  Library.define(
      structure("_ALIGNED", 0x20,
                {member("Head", makeBuiltin(Builtin::ULong), 0),
                 member("Wide", makeBuiltin(Builtin::ULongLong), 0x10)}));
  const LocalType &Aligned = *Library.find("_ALIGNED");
  EXPECT_TRUE(contains(renderType(Library, Aligned, in(Language::C)),
                       "uint8_t __filler0[12];"));
  EXPECT_TRUE(contains(renderType(Library, Aligned, in(Language::Rust)),
                       "pub _filler1: [u8; 12],"));
  EXPECT_TRUE(
      contains(renderType(Library, Aligned, in(Language::Go)), "\t_ [12]byte"));
}

TEST(TypeRender, GoReadsAFlexibleArrayThroughAnAccessor) {
  TypeLibrary Library;
  Library.define(structure(
      "_LIST", 8,
      {member("Count", makeBuiltin(Builtin::ULong), 0),
       member("Items", makeArray(makeBuiltin(Builtin::UShort), 0), 4)}));
  const std::string Text =
      renderType(Library, *Library.find("_LIST"), in(Language::Go));
  EXPECT_FALSE(contains(Text, "\tItems [0]uint16"));
  EXPECT_TRUE(contains(Text, "func (s *_LIST) Items(n int) []uint16 {"));
}

TEST(TypeRender, CCharacterTypesLeaveCppItsKeywords) {
  // C declares the character types C++ has keywords for, so that its header
  // compiles as either; char8_t is a keyword from C++20.
  TypeLibrary Library;
  Library.define(structure("_TEXT", 4,
                           {member("Narrow", makeBuiltin(Builtin::Char8), 0),
                            member("Wide", makeBuiltin(Builtin::Char16), 2)}));
  for (BuiltinSpelling Spelling :
       {BuiltinSpelling::Windows, BuiltinSpelling::Keywords}) {
    RenderOptions Options = in(Language::C);
    Options.Spelling = Spelling;
    const std::string Text = renderTypes(Library, {}, Options);
    EXPECT_TRUE(contains(Text, "#if !defined(__cplusplus) || "
                               "!defined(__cpp_char8_t)\ntypedef uint8_t "
                               "char8_t;\n#endif\n"))
        << Text;
    EXPECT_TRUE(contains(Text, "#ifndef __cplusplus\ntypedef uint16_t "
                               "char16_t;\n#endif\n"))
        << Text;
  }
  EXPECT_FALSE(contains(renderTypes(Library, {}, in(Language::Cpp)),
                        "typedef uint16_t char16_t;"));
}

TEST(TypeRender, RecordLongDoublesAreAsLargeOnEveryCompiler) {
  // A record's long double of 16 bytes is the compiler's own where it is as
  // large, and its bytes elsewhere, as MSVC's long double is a double; a
  // signature passes the program's long double, and the keywords spelling
  // keeps it.
  TypeLibrary Library(DataModel::LP64);
  ASSERT_TRUE(parseCTypes("typedef long double real;\n"
                          "struct Wide { char c; long double d;\n"
                          "  long double (*scale)(long double); real r; };\n"
                          "long double half(long double x);\n",
                          Library)
                  .ok());
  RenderOptions Options = in(Language::C);
  Options.Prototypes = true;
  const std::string Portable = renderTypes(Library, {}, Options);
  EXPECT_TRUE(contains(Portable, "neverd_long_double_16 d;")) << Portable;
  // A typedef the record holds is as large.
  EXPECT_TRUE(contains(Portable, "typedef neverd_long_double_16 real;"))
      << Portable;
  EXPECT_TRUE(contains(Portable, "#if (defined(__SIZEOF_LONG_DOUBLE__) && "
                                 "__SIZEOF_LONG_DOUBLE__ == 16)\ntypedef long "
                                 "double neverd_long_double_16;\n#else\n"
                                 "typedef struct DECLSPEC_ALIGN(16)\n{\n    "
                                 "uint8_t bytes[16];\n} "
                                 "neverd_long_double_16;\n#endif\n"))
      << Portable;
  EXPECT_TRUE(contains(Portable, "long double (*scale)(long double);"))
      << Portable;
  EXPECT_TRUE(contains(Portable, "long double half(long double x);"))
      << Portable;
  // A type shown alone, without the prelude, keeps the program's name.
  const std::string Alone = renderType(Library, *Library.find("Wide"), Options);
  EXPECT_TRUE(contains(Alone, "long double d;")) << Alone;
  EXPECT_FALSE(contains(Alone, "neverd_long_double")) << Alone;
  Options.Spelling = BuiltinSpelling::Keywords;
  const std::string Keywords = renderTypes(Library, {}, Options);
  EXPECT_TRUE(contains(Keywords, "long double d;")) << Keywords;
  EXPECT_FALSE(contains(Keywords, "neverd_long_double")) << Keywords;

  // Windows' spelling of MSVC's long double, which MinGW's compilers make
  // sixteen bytes: a double for them.
  TypeLibrary Windows(DataModel::LLP64);
  ASSERT_TRUE(
      parseCTypes("struct Narrow { char c; long double d; };\n", Windows).ok());
  Options.Spelling = BuiltinSpelling::Windows;
  const std::string Named = renderTypes(Windows, {}, Options);
  EXPECT_TRUE(contains(Named, "neverd_long_double_8 d;")) << Named;
  EXPECT_TRUE(contains(Named, "#if defined(_MSC_VER) || "
                              "(defined(__SIZEOF_LONG_DOUBLE__) && "
                              "__SIZEOF_LONG_DOUBLE__ == 8)\ntypedef long "
                              "double neverd_long_double_8;\n#else\ntypedef "
                              "double neverd_long_double_8;\n#endif\n"))
      << Named;
  Options.Spelling = BuiltinSpelling::Portable;
  EXPECT_TRUE(contains(renderTypes(Windows, {}, Options), "double d;"));
}

TEST(TypeRender, RecordsATypedefNamesGoByThatName) {
  // `typedef struct { ... } X` names the record by X alone, as a system's
  // headers declare it: C names it so, ahead and after.
  TypeLibrary Library(DataModel::LP64);
  ASSERT_TRUE(parseCTypes("typedef struct { int v[2]; } fsid_like;\n"
                          "struct holder { fsid_like id; fsid_like *next; };\n",
                          Library)
                  .ok());
  const std::string C = renderTypes(Library, {}, in(Language::C));
  EXPECT_TRUE(contains(C, "typedef struct fsid_like fsid_like;\n")) << C;
  EXPECT_TRUE(contains(C, "    fsid_like id;")) << C;
  EXPECT_TRUE(contains(C, "    fsid_like *next;")) << C;
  EXPECT_FALSE(contains(C, "struct fsid_like id")) << C;
  const std::string Alone =
      renderType(Library, *Library.find("fsid_like"), in(Language::C));
  EXPECT_TRUE(contains(Alone, "};\ntypedef struct fsid_like fsid_like;\n"))
      << Alone;
  TypeLibrary Back(DataModel::LP64);
  ASSERT_TRUE(parseCTypes(C, Back).ok());
  EXPECT_TRUE(Back.find("fsid_like")->TypedefNamed);
}

TEST(TypeRender, TheCLibrarysTypesComeFromItsHeadersOnItsSystem) {
  // A program's record holding glibc's: on x64 glibc they come from its
  // headers, and elsewhere from definitions whose names the compiler's own
  // headers keep.
  TypeLibrary Library(DataModel::LP64);
  const TypeLibrary *Glibc = headerLibrary(HeaderLibrary::LinuxX64);
  ASSERT_NE(Glibc, nullptr);
  mergeHeaderTypes(Library, *Glibc);
  ASSERT_TRUE(parseCTypes("struct record { FILE *log; struct stat info; "
                          "sigset_t mask; long count; };\n",
                          Library)
                  .ok());
  RenderOptions Options = in(Language::C);
  Options.SystemLibrary = HeaderLibrary::LinuxX64;
  Options.Asserts = true;
  const std::string Text =
      renderTypes(Library, {Library.find("record")}, Options);
  for (const char *Expected :
       {"#if !defined(NEVERD_OWN_LIBC_TYPES) && defined(__linux__) && \\\n"
        "    defined(__x86_64__) && !defined(__ILP32__)\n#ifndef _GNU_SOURCE\n"
        "#define _GNU_SOURCE 1\n#endif\n",
        "#if defined(__GLIBC__) && !defined(__UCLIBC__)\n#if "
        "!(defined(__USE_GNU))\n",
        "#define NEVERD_LIBC_FROM_SYSTEM 1\n", "#include <stdio.h>\n",
        "#define _IO_FILE neverd__IO_FILE\n",
        "#if !defined(NEVERD_LIBC_FROM_SYSTEM) || !defined(__cplusplus)\n"
        "static_assert(",
        "static_assert(sizeof(struct stat) == 0x90",
        "#pragma pop_macro(\"_IO_FILE\")\n"})
    EXPECT_TRUE(contains(Text, Expected)) << Expected << "\n" << Text;
  // The program's record stands after the branches, on every system.
  const size_t Branch = Text.find("#if defined(NEVERD_LIBC_FROM_SYSTEM)\n");
  const size_t Fallback = Text.find("#else\n", Branch);
  const size_t End = Text.find("#endif\n", Fallback);
  ASSERT_NE(End, std::string::npos) << Text;
  EXPECT_LT(Fallback, Text.find("\nstruct stat\n"));
  EXPECT_GT(End, Text.find("\nstruct stat\n"));
  EXPECT_GT(Text.find("\nstruct record\n"), End);
  // The prelude's standard names stay the compiler's.
  EXPECT_FALSE(contains(Text, "typedef uint64_t size_t;")) << Text;
  Options.SystemLibrary.reset();
  EXPECT_FALSE(contains(renderTypes(Library, {Library.find("record")}, Options),
                        "NEVERD_LIBC_FROM_SYSTEM"));
}

TEST(TypeRender, I386RecordsLayOutAsI386OnEveryCompiler) {
  // The i386 System V ABI aligns long long to four, which pack(4) keeps on
  // compilers that align it to eight; GNU's bit-fields ask MinGW's
  // compilers for GNU's layout.
  TypeLibrary Library(DataModel::ILP32);
  Library.setRecordABI(RecordABI::I386SystemV);
  ASSERT_TRUE(parseCTypes("struct loose { char c; long long q; };\n"
                          "struct ints { char c; int i; };\n"
                          "struct bits { char c : 3; int i : 5; };\n",
                          Library)
                  .ok());
  const std::string Text = renderTypes(Library, {}, in(Language::C));
  EXPECT_TRUE(contains(Text, "packed to 4 as the i386 System V ABI aligns its "
                             "eight-byte scalars\n#pragma pack(push, "
                             "4)\nstruct loose\n"))
      << Text;
  EXPECT_FALSE(contains(Text, "#pragma pack(push, 4)\nstruct ints\n"));
  EXPECT_TRUE(contains(Text, "struct NEVERD_GNU_STRUCT bits\n")) << Text;
  EXPECT_TRUE(contains(Text, "#define NEVERD_GNU_STRUCT "
                             "__attribute__((gcc_struct))\n"))
      << Text;
}

TEST(TypeRender, ChecksLeaveTheFillersOut) {
  // A filler of the bytes after the last member is no member a system's own
  // declaration has, read back or not.
  TypeLibrary Library(DataModel::LP64);
  Library.define(
      structure("Tail", 12, {member("x", makeBuiltin(Builtin::Int), 0)}));
  RenderOptions Options = in(Language::C);
  Options.Asserts = true;
  const std::string Text = renderTypes(Library, {}, Options);
  EXPECT_TRUE(contains(Text, "__filler0[8];")) << Text;
  EXPECT_FALSE(contains(Text, "__filler0) ==")) << Text;
  TypeLibrary Back(DataModel::LP64);
  ASSERT_TRUE(parseCTypes(Text, Back).ok());
  EXPECT_FALSE(contains(renderTypes(Back, {}, Options), "__filler0) =="));
}

TEST(TypeRender, TheWindowsRuntimesRecordsStayMinGWsUnderMinGW) {
  // A Windows program's records of the C runtime, which MinGW's <stdint.h>
  // declares too: under MinGW the header's own take other names, which
  // leave MinGW's after it.
  TypeLibrary Library(DataModel::LLP64);
  ASSERT_TRUE(parseCTypes("struct tagLC_ID { unsigned short wLanguage; "
                          "unsigned short wCountry; unsigned short "
                          "wCodePage; };\n"
                          "typedef long long time_t;\n"
                          "struct uses { struct tagLC_ID id; time_t when; };\n",
                          Library)
                  .ok());
  const std::string Text = renderTypes(Library, {}, in(Language::C));
  for (const char *Expected :
       {"#if defined(__MINGW32__)\n#pragma push_macro(\"tagLC_ID\")\n#define "
        "tagLC_ID neverd_tagLC_ID\n#endif\n",
        "#if defined(_WIN32)\n#pragma push_macro(\"time_t\")\n#define time_t "
        "neverd_time_t\n#endif\n",
        "#if defined(__MINGW32__)\n#pragma pop_macro(\"tagLC_ID\")\n#endif\n"})
    EXPECT_TRUE(contains(Text, Expected)) << Expected << "\n" << Text;
  // A name no compiler's headers declare keeps its own.
  EXPECT_FALSE(contains(Text, "push_macro(\"uses\")")) << Text;
  // Read back, the types keep their names.
  TypeLibrary Back(DataModel::LLP64);
  ASSERT_TRUE(parseCTypes(Text, Back).ok());
  EXPECT_NE(Back.find("tagLC_ID"), nullptr);
  EXPECT_EQ(Back.find("neverd_tagLC_ID"), nullptr);
}

TEST(TypeRender, KeywordBitfieldsAndPartlyNamedParametersRender) {
  // A bit-field named type, Rust's r#type, takes the setter set_type; Go
  // names every parameter of a function or none.
  TypeLibrary Library(DataModel::LP64);
  ASSERT_TRUE(parseCTypes("struct flags { unsigned type : 3; unsigned rest : "
                          "5; };\n"
                          "typedef void recorder(int, void *, unsigned type, "
                          "int);\n"
                          "typedef void printer(char *fmt, ...);\n",
                          Library)
                  .ok());
  const std::string Rust = renderTypes(Library, {}, in(Language::Rust));
  EXPECT_TRUE(contains(Rust, "pub fn set_type(&mut self")) << Rust;
  EXPECT_FALSE(contains(Rust, "set_r#")) << Rust;
  const std::string Go = renderTypes(Library, {}, in(Language::Go));
  EXPECT_TRUE(
      contains(Go, "func(_ int32, _ unsafe.Pointer, type_ uint32, _ int32)"))
      << Go;
  EXPECT_TRUE(contains(Go, "func(fmt *int8, _ ...uintptr)")) << Go;
}

TEST(TypeRender, RustImportsCoreVoidWhereItIsSpelled) {
  // An import nothing uses warns, which a check of warnings fails on.
  TypeLibrary Library;
  Library.define(
      structure("Plain", 4, {member("x", makeBuiltin(Builtin::Int), 0)}));
  EXPECT_FALSE(contains(renderTypes(Library, {}, in(Language::Rust)),
                        "use core::ffi::c_void;"));
  Library.define(structure(
      "Holder", 8,
      {member("data", makePointer(makeBuiltin(Builtin::Void), 8), 0)}));
  EXPECT_TRUE(contains(renderTypes(Library, {}, in(Language::Rust)),
                       "use core::ffi::c_void;"));
}

TEST(TypeRender, NamesStepAroundKeywords) {
  TypeLibrary Library;
  Library.define(structure("_WORDS", 8,
                           {member("type", makeBuiltin(Builtin::ULong), 0),
                            member("class", makeBuiltin(Builtin::ULong), 4)}));
  const LocalType &Words = *Library.find("_WORDS");
  // The C a C/C++ view shows reads as C++ too, and steps around C++'s
  // keywords; the C a decompiled unit embeds keeps C's names, as its code
  // spells them.
  const std::string C = renderType(Library, Words, in(Language::C));
  EXPECT_TRUE(contains(C, "uint32_t type;"));
  EXPECT_TRUE(contains(C, "uint32_t class_;")) << C;
  EXPECT_TRUE(contains(renderType(Library, Words, in(Language::Cpp)),
                       "uint32_t class_;"));
  const LocalType *Root = &Words;
  const EmbeddedC Embedded = renderEmbeddedC(Library, {Root}, in(Language::C));
  EXPECT_TRUE(contains(Embedded.Text, "uint32_t class;")) << Embedded.Text;
  EXPECT_TRUE(contains(renderType(Library, Words, in(Language::Rust)),
                       "pub r#type: u32,"));
  EXPECT_TRUE(
      contains(renderType(Library, Words, in(Language::Go)), "\ttype_ uint32"));
}

TEST(TypeRender, KeywordNamespacesKeepTheirDistinctSpellings) {
  TypeLibrary Library;
  for (const char *Name :
       {"auto", "class", "func", "end", "return", "yield", "self", "Self",
        "crate", "super", "_", "neverd_keyword"})
    Library.define(
        structure(Name, 4, {member("value", makeBuiltin(Builtin::Int), 0)}));
  using Names = std::map<std::string, std::string>;
  const Names Cpp = {
      {"auto", "auto_"}, {"class", "class_"}, {"return", "return_"}};
  const std::pair<Language, Names> Cases[] = {
      // Library views of C also escape C++ keywords.
      {Language::C, Cpp},
      {Language::Cpp, Cpp},
      {Language::Rust,
       {{"_", "__"},
        {"crate", "crate_"},
        {"return", "r#return"},
        {"self", "self_"},
        {"Self", "Self_"},
        {"super", "super_"},
        {"yield", "r#yield"}}},
      {Language::Go, {{"func", "func_"}, {"return", "return_"}}},
      {Language::Python,
       {{"class", "class_"}, {"return", "return_"}, {"yield", "yield_"}}},
      {Language::JavaScript,
       {{"class", "class_"},
        {"return", "return_"},
        {"super", "super_"},
        {"yield", "yield_"}}},
      {Language::Lua, {{"end", "end_"}, {"return", "return_"}}}};
  for (const auto &[Lang, Expected] : Cases) {
    SCOPED_TRACE(languageId(Lang).str());
    const auto Renamed = renamedNames(Library, Lang);
    const Names Actual(Renamed.begin(), Renamed.end());
    EXPECT_EQ(Actual, Expected);
  }
}

TEST(TypeRender, UnitsSpellReservedTypedefsInExactWidths) {
  // Darwin's headers define __darwin_size_t as unsigned long, which
  // uint64_t, an unsigned long long there, is not: a decompiled unit that
  // defined it would not compile beside them, and spells what it stands
  // for instead, as wide whatever system reads it.
  TypeLibrary Library(DataModel::LP64);
  ASSERT_TRUE(parseCTypes("typedef unsigned long __darwin_size_t;\n"
                          "typedef struct _opaque *__darwin_handle_t;\n"
                          "typedef __darwin_size_t length_t;\n"
                          "struct buffer { __darwin_size_t size;\n"
                          "  __darwin_handle_t owner; length_t used; };\n",
                          Library)
                  .ok());
  const LocalType *Root = Library.find("buffer");
  const EmbeddedC Embedded = renderEmbeddedC(Library, {Root}, in(Language::C));
  EXPECT_FALSE(contains(Embedded.Text, "__darwin")) << Embedded.Text;
  EXPECT_TRUE(contains(Embedded.Text, "uint64_t size;")) << Embedded.Text;
  EXPECT_TRUE(contains(Embedded.Text, "typedef uint64_t length_t;"))
      << Embedded.Text;
  // The record the reserved typedef points to is declared, as one a
  // definition points to is.
  EXPECT_TRUE(contains(Embedded.Text, "struct _opaque;")) << Embedded.Text;
  EXPECT_TRUE(llvm::is_contained(Embedded.DeclaredTypes, "_opaque"));
  EXPECT_FALSE(llvm::is_contained(Embedded.Types, "__darwin_size_t"));
  // The code beside the definitions spells it so too.
  EXPECT_EQ(spellType(Library, *makeNamed("__darwin_size_t", TypeKind::Typedef),
                      "n", in(Language::C)),
            "uint64_t n");
  // A view of the library shows the typedef as the library has it.
  EXPECT_TRUE(contains(
      renderType(Library, *Library.find("__darwin_size_t"), in(Language::C)),
      "typedef uint64_t __darwin_size_t;"));
}

TEST(TypeRender, NamesThatAreNoIdentifiersCompile) {
  TypeLibrary Library;
  Library.define(structure("Outer::Inner<int>", 4,
                           {member("x", makeBuiltin(Builtin::Int), 0)}));
  Library.define(structure(
      "Holder", 4,
      {member("Inner", makeNamed("Outer::Inner<int>", TypeKind::Struct), 0)}));
  for (const char *Name : {"_FIRST", "_SECOND"}) {
    LocalType Kind;
    Kind.Name = Name;
    Kind.Kind = TypeKind::Enum;
    auto Enum = std::make_shared<EnumDef>();
    Enum->Values = {{"None", 0, ""}, {std::string(Name) + "_One", 1, ""}};
    Kind.Enum = std::move(Enum);
    Library.define(std::move(Kind));
  }
  Library.define(
      structure("u32", 4, {member("x", makeBuiltin(Builtin::Int), 0)}));

  const std::string C =
      renderType(Library, *Library.find("Holder"), in(Language::C));
  EXPECT_TRUE(contains(C, "struct Outer__Inner_int_ Inner;"));
  // The comment keeps the name as the program spells it.
  EXPECT_TRUE(contains(
      renderType(Library, *Library.find("Outer::Inner<int>"), in(Language::C)),
      "// Outer::Inner<int>: 0x4 (4) bytes\nstruct "
      "Outer__Inner_int_\n"));
  // The second enumeration's None takes its name as a prefix.
  EXPECT_TRUE(
      contains(renderType(Library, *Library.find("_SECOND"), in(Language::C)),
               "    _SECOND_None = 0,\n"));
  EXPECT_TRUE(
      contains(renderType(Library, *Library.find("u32"), in(Language::Rust)),
               "pub struct u32_ {"));
  const auto Renamed = renamedNames(Library, Language::C);
  ASSERT_EQ(Renamed.size(), 2u);
  EXPECT_EQ(Renamed[0].first, "Outer::Inner<int>");
  EXPECT_EQ(Renamed[0].second, "Outer__Inner_int_");
  EXPECT_EQ(Renamed[1].first, "_SECOND::None");
  EXPECT_EQ(Renamed[1].second, "_SECOND_None");
}

TEST(TypeRender, ThirtyTwoBitRecordsAlignTheirEightByteMembers) {
  // Microsoft's 32-bit compilers align an eight-byte scalar to eight, which
  // the 32-bit library holds; GNU's i386 ABI aligns it to four inside a
  // record, so the source asks GNU's for it wherever no pack decides it.
  // Microsoft's are not asked: a pack around a record holding the member
  // could not reduce an alignment they were told.
  TypeLibrary Library(DataModel::ILP32);
  ASSERT_TRUE(parseCTypes(R"(
    typedef struct _STRING64 {
      USHORT Length;
      USHORT MaximumLength;
      ULONGLONG Buffer;
    } STRING64;
    typedef union _QUAD { ULONGLONG Quad; double Real[2]; } QUAD;
    #pragma pack(push, 4)
    typedef struct _PACKED4 { ULONG Low; ULONGLONG High; } PACKED4;
    #pragma pack(pop)
    #pragma pack(push, 8)
    typedef struct _PACKED8 { ULONG Low; ULONGLONG Wide; } PACKED8;
    #pragma pack(pop)
  )",
                          Library)
                  .ok());
  RenderOptions C = in(Language::C);
  C.Asserts = true;
  const std::string Text = renderTypes(Library, {}, C);
  EXPECT_TRUE(contains(Text, "#define NEVERD_ALIGN8 __attribute__")) << Text;
  EXPECT_TRUE(contains(Text, "NEVERD_ALIGN8 uint64_t Buffer;"));
  EXPECT_TRUE(contains(Text, "NEVERD_ALIGN8 uint64_t Quad;"));
  EXPECT_TRUE(contains(Text, "NEVERD_ALIGN8 double Real[2];"));
  EXPECT_TRUE(contains(Text, "    uint16_t Length;"));
  // A pack below eight decides the alignment itself; pack(8) only lowers
  // an alignment, so GNU's would stay four under it.
  EXPECT_TRUE(contains(Text, "    uint64_t High;"));
  EXPECT_TRUE(contains(Text, "NEVERD_ALIGN8 uint64_t Wide;"));
  const std::string Cpp = renderTypes(Library, {}, in(Language::Cpp));
  EXPECT_TRUE(contains(Cpp, "NEVERD_ALIGN8 uint64_t Buffer;")) << Cpp;
  // A 64-bit library's own alignment holds on every compiler.
  TypeLibrary Wide(DataModel::LLP64);
  ASSERT_TRUE(parseCTypes("typedef struct _STRING64 { USHORT Length; USHORT "
                          "MaximumLength; ULONGLONG Buffer; } STRING64;",
                          Wide)
                  .ok());
  EXPECT_FALSE(contains(renderTypes(Wide, {}, C), "NEVERD_ALIGN8"));
}

TEST(TypeRender, GnuBitFieldsThatShareBytesTakeStorageOfTheirOwn) {
  // GCC packs Ready at the bit after Kind, in the int whose first byte Kind
  // is, and Wide after it in the long long that starts there too: Rust, Go
  // and Python, which cannot overlap fields, store the run in the bytes it
  // takes, and the record's alignment, which those bytes no longer give,
  // is declared or filled.
  TypeLibrary Library(DataModel::LP64);
  ASSERT_TRUE(parseCTypes(R"(
    struct _SHARED {
      unsigned char Kind;
      unsigned int Ready : 1;
      unsigned int Level : 3;
      int Delta : 4;
      unsigned long long Wide : 40;
      unsigned short Tail;
    };
    struct _NATURAL {
      unsigned int Low : 8;
      unsigned int High : 24;
      unsigned int Next;
    };
  )",
                          Library)
                  .ok());
  const LocalType *Shared = Library.find("_SHARED");
  ASSERT_NE(Shared, nullptr);
  EXPECT_EQ(Shared->Record->Members[1].BitOffset, 8u);

  const std::string Rust = renderTypes(Library, {}, in(Language::Rust));
  EXPECT_TRUE(contains(Rust, "#[repr(C, align(8))]\n#[derive(Clone, Copy)]\n"
                             "pub struct _SHARED {"))
      << Rust;
  EXPECT_TRUE(contains(Rust, "pub _bitfield: [u8; 6],")) << Rust;
  // Wide, bits 16..55 of the record, in the run's bytes from its second.
  EXPECT_TRUE(contains(Rust, "let unit = self._bitfield[1] as u64 | "
                             "(self._bitfield[2] as u64) << 8"))
      << Rust;
  // Delta keeps its own type and sign.
  EXPECT_TRUE(contains(Rust, "pub const fn Delta(&self) -> i32 {")) << Rust;
  // A run its type's unit holds, without sharing a byte, stays in it.
  EXPECT_TRUE(contains(Rust, "pub _bitfield: u32,")) << Rust;

  const std::string Go = renderTypes(Library, {}, in(Language::Go));
  EXPECT_TRUE(contains(Go, "_bitfield [6]byte")) << Go;
  EXPECT_TRUE(contains(Go, "func (s *_SHARED) SetWide(value uint64) {")) << Go;

  const std::string Python = renderTypes(Library, {}, in(Language::Python));
  EXPECT_TRUE(contains(Python, "(\"_bitfield\", ctypes.c_uint8 * 6),"))
      << Python;
  EXPECT_TRUE(contains(Python, "_align_ = 8")) << Python;
  EXPECT_TRUE(contains(Python, "int.from_bytes(bytes(self._bitfield[0:1]), "
                               "\"little\")"))
      << Python;
}

TEST(TypeRender, RustAndGoProgramsTypesCompileAsC) {
  // A Rust type's name can hold `extern "C"`, which a check's message
  // quotes; a unit structure has no bytes, which C defines no record of;
  // a pointer to an array of records needs the record complete first.
  TypeLibrary Library(DataModel::LP64);
  const std::string Quoted = "Holder<unsafe extern \"C\" fn(*mut u8)>";
  Library.define(structure(
      "Cell", 8,
      {member("rows",
              makePointer(makeArray(makeNamed("Row", TypeKind::Struct), 4), 8),
              0)}));
  Library.define(
      structure("Row", 4, {member("x", makeBuiltin(Builtin::Int), 0)}));
  Library.define(structure(
      Quoted, 8, {member("cell", makeNamed("Cell", TypeKind::Struct), 0)}));
  Library.define(structure("core::marker::PhantomData<u8>", 0, {}));
  RenderOptions Options = in(Language::C);
  Options.Asserts = true;
  const std::string C = renderTypes(Library, {}, Options);
  EXPECT_TRUE(contains(C, "\"Holder<unsafe extern \\\"C\\\" fn(*mut u8)>\""))
      << C;
  EXPECT_TRUE(contains(C, "// core::marker::PhantomData<u8>: 0 bytes, which C "
                          "defines no record of\nstruct "
                          "core__marker__PhantomData_u8_;\n"))
      << C;
  EXPECT_FALSE(contains(C, "sizeof(struct core__marker__PhantomData_u8_)"))
      << C;
  const size_t Row = C.find("struct Row\n{");
  const size_t Cell = C.find("struct Cell\n{");
  ASSERT_NE(Row, std::string::npos) << C;
  ASSERT_NE(Cell, std::string::npos) << C;
  EXPECT_LT(Row, Cell) << C;

  // A blank field, as Go's own types hold, is no field Go's checks reach.
  TypeLibrary Go(DataModel::LP64);
  Go.define(structure("Padded", 8,
                      {member("_", makeBuiltin(Builtin::UInt), 0),
                       member("x", makeBuiltin(Builtin::UInt), 4)}));
  RenderOptions GoOptions = in(Language::Go);
  GoOptions.Asserts = true;
  const std::string Padded = renderTypes(Go, {}, GoOptions);
  EXPECT_TRUE(contains(Padded, "\t_ uint32")) << Padded;
  EXPECT_TRUE(contains(Padded, "unsafe.Offsetof(Padded{}.x)")) << Padded;
  EXPECT_FALSE(contains(Padded, "unsafe.Offsetof(Padded{}._)")) << Padded;
}

TEST(TypeRender, UnitsSpellHeaderTypedefRecordsByTheirNames) {
  // glibc names fd_set by a typedef alone (`typedef struct {...} fd_set;`),
  // which leaves it no tag: a unit that takes it from its header spells it
  // by that name, in its checks and in the records that hold it, and lists
  // it, so that the unit declares no typedef of its own, which would
  // contradict the header's.
  TypeLibrary Library(DataModel::LP64);
  const TypeLibrary *Glibc = headerLibrary(HeaderLibrary::LinuxX64);
  ASSERT_NE(Glibc, nullptr);
  const LocalType *Theirs = Glibc->find("fd_set");
  ASSERT_NE(Theirs, nullptr);
  ASSERT_TRUE(Theirs->TypedefNamed);
  ASSERT_FALSE(Theirs->Header.empty());
  mergeHeaderTypes(Library, *Glibc);
  ASSERT_TRUE(parseCTypes("struct waiter { fd_set readable; fd_set *more; "
                          "int count; };\n",
                          Library)
                  .ok());
  RenderOptions Options = in(Language::C);
  Options.SystemLibrary = HeaderLibrary::LinuxX64;
  Options.Asserts = true;
  const EmbeddedC Embedded =
      renderEmbeddedC(Library, {Library.find("waiter")}, Options);
  EXPECT_TRUE(contains(Embedded.Text, "#include <" + Theirs->Header + ">\n"))
      << Embedded.Text;
  EXPECT_TRUE(contains(Embedded.Text, "static_assert(sizeof(fd_set) == "))
      << Embedded.Text;
  EXPECT_TRUE(contains(Embedded.Text, "fd_set readable;")) << Embedded.Text;
  EXPECT_FALSE(contains(Embedded.Text, "struct fd_set")) << Embedded.Text;
  EXPECT_TRUE(llvm::is_contained(Embedded.HeaderTypedefs, "fd_set"));
  EXPECT_TRUE(llvm::is_contained(Embedded.HeaderTypes, "fd_set"));
}

TEST(TypeRender, HeaderRecordsNamedLikeTheirFunctionsKeepTheirTags) {
  // glibc's <sys/sysinfo.h> declares struct sysinfo and the function that
  // fills it, sysinfo: the name is an ordinary identifier of the text, so a
  // unit that takes the record from the header declares no typedef of it,
  // which would make the header's function a redefinition.
  TypeLibrary Library(DataModel::LP64);
  const TypeLibrary *Glibc = headerLibrary(HeaderLibrary::LinuxX64);
  ASSERT_NE(Glibc, nullptr);
  ASSERT_NE(Glibc->find("sysinfo"), nullptr);
  ASSERT_NE(Glibc->findPrototype("sysinfo"), nullptr);
  mergeHeaderTypes(Library, *Glibc);
  ASSERT_TRUE(parseCTypes("struct probe { struct sysinfo info; int ready; };\n",
                          Library)
                  .ok());
  RenderOptions Options = in(Language::C);
  Options.SystemLibrary = HeaderLibrary::LinuxX64;
  Options.Asserts = true;
  const EmbeddedC Embedded =
      renderEmbeddedC(Library, {Library.find("probe")}, Options);
  EXPECT_TRUE(contains(Embedded.Text, "#include <sys/sysinfo.h>\n"))
      << Embedded.Text;
  EXPECT_TRUE(llvm::is_contained(Embedded.Identifiers, "sysinfo"));
  EXPECT_TRUE(contains(Embedded.Text, "struct sysinfo info;")) << Embedded.Text;
}

TEST(TypeRender, UnitsSpellHeaderTypedefEnumerationsByTheirNames) {
  // glibc's <search.h> names twalk's VISIT by a typedef alone (`typedef
  // enum {...} VISIT;`), which leaves it no tag: a unit spells it by that
  // name in its checks and records, and lists it, so that it declares no
  // `typedef enum VISIT VISIT;` of its own, which would contradict the
  // header's.
  TypeLibrary Library(DataModel::LP64);
  const TypeLibrary *Glibc = headerLibrary(HeaderLibrary::LinuxX64);
  ASSERT_NE(Glibc, nullptr);
  const LocalType *Theirs = Glibc->find("VISIT");
  ASSERT_NE(Theirs, nullptr);
  ASSERT_EQ(Theirs->Kind, TypeKind::Enum);
  ASSERT_TRUE(Theirs->TypedefNamed);
  mergeHeaderTypes(Library, *Glibc);
  ASSERT_TRUE(
      parseCTypes("struct walker { VISIT order; int depth; };\n", Library)
          .ok());
  RenderOptions Options = in(Language::C);
  Options.SystemLibrary = HeaderLibrary::LinuxX64;
  Options.Asserts = true;
  const EmbeddedC Embedded =
      renderEmbeddedC(Library, {Library.find("walker")}, Options);
  EXPECT_TRUE(contains(Embedded.Text, "#include <" + Theirs->Header + ">\n"))
      << Embedded.Text;
  EXPECT_TRUE(contains(Embedded.Text, "static_assert(sizeof(VISIT) == 4"))
      << Embedded.Text;
  EXPECT_TRUE(contains(Embedded.Text, "VISIT order;")) << Embedded.Text;
  EXPECT_FALSE(contains(Embedded.Text, "enum VISIT")) << Embedded.Text;
  EXPECT_TRUE(llvm::is_contained(Embedded.HeaderTypedefs, "VISIT"));
}

TEST(TypeRender, UnitsTakeTheSystemsOwnTypesFromItsHeaders) {
  // A decompiled unit includes the C library's headers, which define what a
  // program's record holds, struct stat: defined again, it would not
  // compile beside them.  The unit includes its header instead and checks
  // that the header lays it out as the program does.  A record of glibc's
  // name that the program lays out its own way stays the program's.
  TypeLibrary Library(DataModel::LP64);
  const TypeLibrary *Glibc = headerLibrary(HeaderLibrary::LinuxX64);
  ASSERT_NE(Glibc, nullptr);
  ASSERT_NE(Glibc->find("stat"), nullptr);
  const std::string Header = Glibc->find("stat")->Header;
  ASSERT_FALSE(Header.empty());
  mergeHeaderTypes(Library, *Glibc);
  ASSERT_TRUE(
      parseCTypes("struct record { struct stat info; long count; };\n", Library)
          .ok());
  RenderOptions Options = in(Language::C);
  Options.SystemLibrary = HeaderLibrary::LinuxX64;
  Options.Asserts = true;
  const LocalType *Record = Library.find("record");
  const EmbeddedC Embedded = renderEmbeddedC(Library, {Record}, Options);
  EXPECT_TRUE(contains(Embedded.Text, "#include <" + Header + ">\n"))
      << Embedded.Text;
  EXPECT_FALSE(contains(Embedded.Text, " st_ino;")) << Embedded.Text;
  EXPECT_TRUE(contains(Embedded.Text, "count;")) << Embedded.Text;
  EXPECT_TRUE(
      contains(Embedded.Text, "static_assert(sizeof(struct stat) == 0x90"))
      << Embedded.Text;
  EXPECT_TRUE(contains(Embedded.Text, "offsetof(struct stat, st_ino)"))
      << Embedded.Text;
  EXPECT_TRUE(llvm::is_contained(Embedded.Types, "stat"));

  TypeLibrary Own(DataModel::LP64);
  ASSERT_TRUE(parseCTypes("struct stat { int mine; };\n", Own).ok());
  const LocalType *Mine = Own.find("stat");
  const EmbeddedC Kept = renderEmbeddedC(Own, {Mine}, Options);
  EXPECT_FALSE(contains(Kept.Text, "#include <" + Header + ">")) << Kept.Text;
  EXPECT_TRUE(contains(Kept.Text, "mine;")) << Kept.Text;
}
