// Classic x86 listing spellings: mnemonics, operand forms, size keywords,
// immediates, displacements and frame variables; the user's number formats in
// x86, ARM-family, SBF and EVM operands.
#include "OperandFormat.h"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace neverd::worker;

namespace {
void check(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

std::string hex(std::int64_t value) {
  char text[24];
  std::snprintf(text, sizeof(text), "%llX",
                static_cast<unsigned long long>(value));
  return text;
}

void expectInstruction(std::string_view mnemonic, std::string_view operands,
                       std::string_view bytes, std::string_view classic,
                       std::string_view classicOperands) {
  const auto spelled =
      classicInstruction(OperandDialect::X86, mnemonic, operands, bytes);
  check(spelled.mnemonic == classic && spelled.operands == classicOperands,
        std::string(mnemonic) + " " + std::string(operands) + " became " +
            spelled.mnemonic + " " + spelled.operands);
}

struct Format {
  std::string_view mnemonic, operands, bytes;
  std::string_view expected;
  bool wide = true;
  /// The user's number formats, by operand index.
  std::vector<NumberFormat> formats = {};
  OperandDialect dialect = OperandDialect::X86;
};

/// The local type `_T`'s members: a structure offset lands in the one that
/// holds its byte.
std::optional<StructLanding> landing(std::string_view type, std::int64_t offset,
                                     unsigned) {
  struct Member {
    std::int64_t start, size;
    const char *path;
  };
  static const Member members[] = {{0, 1, "Type"},
                                   {2, 1, "Size"},
                                   {0x10, 8, "List.Flink"},
                                   {0x18, 8, "List.Blink"},
                                   {0x220, 8, "Process"}};
  if (type != "_T")
    return std::nullopt;
  for (const Member &member : members)
    if (offset >= member.start && offset < member.start + member.size)
      return StructLanding{member.path, member.start};
  return std::nullopt;
}

std::string format(const Format &input, const FrameNamer *frame = nullptr,
                   std::optional<std::int64_t> stackDepth = std::nullopt) {
  static const StructNamer structs = landing;
  OperandFacts facts;
  facts.structs = &structs;
  facts.dialect = input.dialect;
  facts.mnemonic = input.mnemonic;
  facts.address = 0x401000;
  facts.size = 4;
  facts.wide = input.wide;
  facts.bytes = input.bytes;
  if (!input.formats.empty())
    facts.numberFormats = &input.formats;
  if (frame) {
    facts.frameRegister = "rbp";
    facts.frame = frame;
    facts.stackRegister = "rsp";
    facts.stackDepth = stackDepth;
  }
  // Every address but the rest of the first page has a name.
  return formatOperands(input.operands, facts,
                        [](std::uint64_t address, NameUse, std::string_view) {
                          return address == 0 || address >= 0x1000
                                     ? LocationName{"unk_" + hex(address)}
                                     : LocationName{};
                        })
      .text();
}

void expectFormat(const Format &input, const FrameNamer *frame = nullptr,
                  std::optional<std::int64_t> stackDepth = std::nullopt) {
  const auto text = format(input, frame, stackDepth);
  check(text == input.expected,
        std::string(input.mnemonic) + " " + std::string(input.operands) +
            " became " + text + ", not " + std::string(input.expected));
}
} // namespace

int main() {
  // Mnemonics name the flag a condition tests; hidden prefixes disappear.
  expectInstruction("je", "0x401020", "", "jz", "0x401020");
  expectInstruction("jae", "0x401020", "", "jnb", "0x401020");
  expectInstruction("ret", "", "", "retn", "");
  expectInstruction("seta", "al", "", "setnbe", "al");
  expectInstruction("cmovne", "eax, ecx", "", "cmovnz", "eax, ecx");
  expectInstruction("movabs", "rax, 0x1122334455667788", "", "mov",
                    "rax, 0x1122334455667788");
  expectInstruction("notrack jmp", "rax", "", "jmp", "rax");
  expectInstruction("lock cmpxchg", "qword ptr [rdi], rcx", "", "lock cmpxchg",
                    "qword ptr [rdi], rcx");
  // String instructions drop their implicit operands; SSE movsd keeps them.
  expectInstruction("rep movsb", "byte ptr [rdi], byte ptr [rsi]", "",
                    "rep movsb", "");
  expectInstruction("stosq", "qword ptr [rdi], rax", "", "stosq", "");
  expectInstruction("movsd", "xmm0, qword ptr [rax]", "", "movsd",
                    "xmm0, qword ptr [rax]");
  // Folded and reordered operand forms, and the two-byte no-op.
  expectInstruction("imul", "rdx, rdx, 7", "", "imul", "rdx, 7");
  expectInstruction("imul", "rdx, r13, 7", "", "imul", "rdx, r13, 7");
  expectInstruction("xchg", "rbx, rdi", "", "xchg", "rdi, rbx");
  expectInstruction("xchg", "qword ptr [rax], rdi", "", "xchg",
                    "qword ptr [rax], rdi");
  expectInstruction("nop", "", "6690", "xchg", "ax, ax");

  // A register of the same size states the size; vectors keep theirs.
  expectFormat({"mov", "qword ptr [rcx + 8], rdx", "", "[rcx+8], rdx"});
  expectFormat({"mov", "eax, dword ptr [rdi + 0x40]", "", "eax, [rdi+40h]"});
  expectFormat({"movzx", "eax, byte ptr [rdi]", "", "eax, byte ptr [rdi]"});
  expectFormat({"cmp", "byte ptr [rdi + 0xb], 0", "", "byte ptr [rdi+0Bh], 0"});
  expectFormat({"movups", "xmmword ptr [rdi + rax], xmm0", "",
                "xmmword ptr [rdi+rax], xmm0"});
  expectFormat({"call", "qword ptr [rax + 0x58]", "", "qword ptr [rax+58h]"});
  // Negative immediates are unsigned in the operation width, except where a
  // signed byte stays signed.
  expectFormat({"add", "r12, -0x180", "", "r12, 0FFFFFFFFFFFFFE80h"});
  expectFormat({"cmp", "eax, -1", "", "eax, 0FFFFFFFFh"});
  expectFormat({"cmp", "byte ptr [rax], -1", "", "byte ptr [rax], 0FFh"});
  expectFormat({"push", "-0x18", "", "0FFFFFFFFFFFFFFE8h"});
  expectFormat({"push", "-1", "", "0FFFFFFFFh", false});
  expectFormat({"imul", "rax, -0x38", "", "rax, -38h"});
  // The user's number formats: a base, a changed sign, inverted bits.
  const auto user = [](NumberBase base, bool negate = false,
                       bool invert = false) {
    return std::vector<NumberFormat>{{}, {base, negate, invert}};
  };
  expectFormat(
      {"mov", "eax, 0x1a", "", "eax, 26", true, user(NumberBase::Decimal)});
  expectFormat({"mov", "eax, 26", "", "eax, 1Ah", true, user(NumberBase::Hex)});
  expectFormat(
      {"mov", "eax, 0x1a", "", "eax, 11010b", true, user(NumberBase::Binary)});
  expectFormat(
      {"mov", "eax, 0x41", "", "eax, 'A'", true, user(NumberBase::Char)});
  expectFormat({"cmp", "eax, 0x41424344", "", "eax, 'ABCD'", true,
                user(NumberBase::Char)});
  expectFormat(
      {"cmp", "eax, -1", "", "eax, -1", true, user(NumberBase::Number, true)});
  expectFormat(
      {"cmp", "eax, -1", "", "eax, -1", true, user(NumberBase::Decimal, true)});
  expectFormat({"and", "eax, 0xfffffff0", "", "eax, ~0Fh", true,
                user(NumberBase::Number, false, true)});
  expectFormat({"mov", "eax, 0x401000", "", "eax, offset unk_401000", true,
                user(NumberBase::Offset)});
  // A format that cannot show the number leaves the listing's spelling: a
  // control character, an offset nothing names.
  expectFormat({"mov", "eax, 7", "", "eax, 7", true, user(NumberBase::Char)});
  expectFormat(
      {"mov", "eax, 0x10", "", "eax, 10h", true, user(NumberBase::Offset)});
  // Structure offsets: a displacement from a register, a number, the
  // register pointing into the type, and a number no member holds.
  const auto member = [](std::size_t operand, std::int64_t delta = 0,
                         std::string type = "_T") {
    std::vector<NumberFormat> formats(operand + 1);
    formats[operand] = {NumberBase::Struct, false, false, std::move(type),
                        delta};
    return formats;
  };
  expectFormat({"mov", "rdi, qword ptr [rcx + 0x220]", "",
                "rdi, [rcx+_T.Process]", true, member(1)});
  expectFormat({"mov", "byte ptr [rcx + 2], 0x58", "",
                "byte ptr [rcx+_T.Size], 58h", true, member(0)});
  expectFormat({"mov", "eax, dword ptr [rcx + rdx*4 + 0x224]", "",
                "eax, [rcx+rdx*4+(_T.Process+4)]", true, member(1)});
  expectFormat({"mov", "al, byte ptr [rcx - 0x10]", "",
                "al, [rcx+(_T.Type-10h)]", true, member(1, 0x10)});
  expectFormat({"add", "rcx, 0x18", "", "rcx, _T.List.Blink", true, member(1)});
  expectFormat({"mov", "rax, qword ptr [rcx + 0x500]", "", "rax, [rcx+500h]",
                true, member(1)});
  expectFormat({"mov", "rax, qword ptr [rcx + 0x220]", "", "rax, [rcx+220h]",
                true, member(1, 0, "_UNKNOWN")});
  // Only the operand the user formats changes.
  expectFormat({"mov",
                "eax, 0x1a",
                "",
                "eax, 1Ah",
                true,
                {{NumberBase::Decimal, false, false}}});
  // Formats change plain numbers, signed or not, and only x86 operands.
  check(formattableOperands("rsp, 8", OperandDialect::X86) ==
            std::vector<bool>{false, true},
        "a register and a number");
  check(formattableOperands("dword ptr [rbp - 4], -5", OperandDialect::X86) ==
            std::vector<bool>{false, true},
        "a displacement is no plain number");
  check(formattableOperands("w0, #0x20", OperandDialect::AArch64) ==
            std::vector<bool>{false, true},
        "an AArch64 immediate takes a format");
  expectFormat({"imul", "rsi, qword ptr [rax], -0xc8", "",
                "rsi, [rax], 0FFFFFFFFFFFFFF38h"});
  // Thread storage offsets, missing base registers and zero displacements.
  expectFormat({"mov", "rax, qword ptr fs:[0x28]", "", "rax, fs:28h"});
  expectFormat({"lea", "rsi, [rsi*2 + 1]", "", "rsi, ds:1[rsi*2]"});
  expectFormat({"lea", "rax, [rax*4]", "", "rax, ds:0[rax*4]"});
  expectFormat({"mov", "rax, qword ptr [rdx*8 + 0x2000]", "",
                "rax, ds:unk_2000[rdx*8]"});
  expectFormat({"mov", "rax, qword ptr [r13]", "", "rax, [r13+0]"});
  expectFormat({"mov", "rax, qword ptr [rbx + rcx]", "", "rax, [rbx+rcx]"});
  expectFormat(
      {"movzx", "edx, byte ptr [r13 + rbx]", "", "edx, byte ptr [r13+rbx+0]"});
  expectFormat({"nop", "dword ptr [rax]", "0f1f4000", "dword ptr [rax+00h]"});
  expectFormat({"nop", "word ptr cs:[rax + rax]", "662e0f1f840000000000",
                "word ptr cs:[rax+rax+00000000h]"});
  expectFormat({"nop", "dword ptr [rax]", "0f1f00", "dword ptr [rax]"});

  // Frame variables: sized by their widest access; an access inside a
  // larger variable names it with an offset and keeps its own size.
  const std::map<std::int64_t, unsigned> variables = {
      {-0x88, 1}, {-0x78, 16}, {-0x30, 8}, {0x10, 8}};
  const FrameNamer frame =
      [&](std::int64_t offset) -> std::optional<FrameSlot> {
    auto next = variables.upper_bound(offset);
    if (next == variables.begin())
      return std::nullopt;
    const auto &[start, size] = *std::prev(next);
    if (offset >= start + static_cast<std::int64_t>(size))
      return std::nullopt;
    const std::string name =
        start < 0 ? "var_" + hex(-start) : "arg_" + hex(start - 0x10);
    return FrameSlot{name, offset - start, size};
  };
  expectFormat({"mov", "rdi, qword ptr [rbp - 0x30]", "", "rdi, [rbp+var_30]"},
               &frame);
  expectFormat({"lea", "rdx, [rbp - 0x88]", "", "rdx, [rbp+var_88]"}, &frame);
  expectFormat(
      {"mov", "rax, qword ptr [rbp - 0x88]", "", "rax, qword ptr [rbp+var_88]"},
      &frame);
  expectFormat({"mov", "qword ptr [rbp - 0x70], rax", "",
                "qword ptr [rbp+var_78+8], rax"},
               &frame);
  expectFormat({"mov", "qword ptr [rbp - 0x30], 0", "", "[rbp+var_30], 0"},
               &frame);
  expectFormat({"and", "dword ptr [rbp + rax*4 - 0x88], 0", "",
                "dword ptr [rbp+rax*4+var_88], 0"},
               &frame);
  expectFormat(
      {"mov", "rcx, qword ptr [rbp + rax - 0x30]", "", "rcx, [rbp+rax+var_30]"},
      &frame);
  expectFormat({"mov", "rax, qword ptr [rbp + 0x10]", "", "rax, [rbp+arg_0]"},
               &frame);
  // Offsets outside every variable stay numeric.
  expectFormat({"mov", "rax, qword ptr [rbp - 8]", "", "rax, [rbp-8]"}, &frame);
  // Through the stack pointer, with its distance down from the frame's base.
  expectFormat(
      {"mov", "qword ptr [rsp + 0x18], rax", "", "[rsp+48h+var_30], rax"},
      &frame, 0x48);
  expectFormat({"lea", "rdi, [rsp - 0x40]", "", "rdi, [rsp+48h+var_88]"},
               &frame, 0x48);
  expectFormat({"mov", "rcx, qword ptr [rsp + rax*8 + 0x18]", "",
                "rcx, [rsp+rax*8+48h+var_30]"},
               &frame, 0x48);
  // At the frame's base the distance is left out.
  expectFormat({"mov", "rax, qword ptr [rsp + 0x10]", "", "rax, [rsp+arg_0]"},
               &frame, 0);
  // Without a known distance the operand stays numeric.
  expectFormat({"mov", "qword ptr [rsp + 0x18], rax", "", "[rsp+18h], rax"},
               &frame);

  const auto access = x86FrameAccess("dword ptr [rbp - 0x34], edi", "rbp");
  check(access && access->offset == -0x34 && access->size == 4,
        "A dword frame store was not recognized");
  const auto address = x86FrameAccess("rdx, [rbp - 0x88]", "rbp");
  check(address && address->offset == -0x88 && address->size == 0,
        "A frame address was given a size");
  check(!x86FrameAccess("rax, qword ptr [rsp + 8]", "rbp"),
        "A stack pointer access was taken for a frame access");
  check(x86SizeKeyword(16) == "xmmword" && x86SizeKeyword(3).empty(),
        "Size keywords do not follow the size table");
  {
    OperandFacts facts;
    facts.dialect = OperandDialect::X86;
    facts.mnemonic = "mov";
    facts.wide = true;
    std::vector<NumberFormat> formats(2);
    formats[1] = {NumberBase::Enum, false, false, "FLAGS", 1};
    facts.numberFormats = &formats;
    std::vector<unsigned> widths{32, 32};
    facts.operandBits = &widths;
    bool called = false;
    OperandFacts::EnumNamer enums =
        [&](std::string_view type, std::uint64_t value, unsigned bits,
            bool flags) -> std::optional<std::string> {
      called = type == "FLAGS" && value == 11 && bits == 32 && flags;
      return "FLAG_A | FLAG_B | 0x8";
    };
    facts.enums = &enums;
    auto result = formatOperands("eax, 0xb", facts,
                                 [](std::uint64_t, NameUse, std::string_view) {
                                   return LocationName{};
                                 });
    check(called && result.text() == "eax, FLAG_A | FLAG_B | 0x8",
          "Enum operands lost logical width or residual bits");
    facts.mnemonic = "shl";
    widths[1] = 8;
    called = false;
    enums = [&](std::string_view, std::uint64_t value, unsigned bits,
                bool) -> std::optional<std::string> {
      called = value == 255 && bits == 8;
      return "BYTE_NEGATIVE";
    };
    const auto shifted = formatOperands(
        "eax, 0xff", facts, [](std::uint64_t, NameUse, std::string_view) {
          return LocationName{};
        });
    check(called && shifted.text() == "eax, BYTE_NEGATIVE",
          "Enum immediate borrowed the destination register width");
    facts.operandBits = nullptr;
    called = false;
    const auto unknown = formatOperands(
        "eax, 0xff", facts, [](std::uint64_t, NameUse, std::string_view) {
          return LocationName{};
        });
    check(!called && unknown.text().find("BYTE_NEGATIVE") == std::string::npos,
          "Enum rendering invented an absent decoded operand width");
    const auto folded =
        classicInstruction(OperandDialect::X86, "imul", "eax, eax, 0xb", "");
    check(folded.operandIndices == std::vector<std::size_t>{0, 2},
          "Classic folding lost original immediate identity");
  }
  {
    // ARM-family operands keep the decoder's `#` and count an immediate in
    // the first register's width: 32 bits for a `w` register, else 64.
    const auto aarch64 = OperandDialect::AArch64, arm = OperandDialect::ARM;
    const auto at = [](std::size_t operand, NumberFormat format) {
      std::vector<NumberFormat> formats(operand + 1);
      formats[operand] = std::move(format);
      return formats;
    };
    const NumberFormat hexadecimal{NumberBase::Hex},
        decimal{NumberBase::Decimal}, binary{NumberBase::Binary},
        character{NumberBase::Char}, offset{NumberBase::Offset},
        negate{NumberBase::Number, true},
        invert{NumberBase::Number, false, true};
    check(formattableOperands("x0, x1, #0x10, lsl #12", aarch64) ==
              std::vector<bool>{false, false, true, false},
          "a shift is no number");
    check(formattableOperands("x0, [x1, #8]", aarch64) ==
              std::vector<bool>{false, false},
          "a displacement is no plain number");
    check(formattableOperands("d0, #1.00000000", aarch64) ==
              std::vector<bool>{false, false},
          "a floating constant is no number");
    check(formattableOperands("{r4, r5, lr}", arm) == std::vector<bool>{false},
          "a register list is one operand");
    check(structOperands("x29, x30, [sp, #-0x10]!", aarch64) ==
              std::vector<bool>{false, false, true},
          "a pre-indexed displacement takes a structure offset");
    check(structOperands("r0, [pc, #0x10]", arm) ==
              std::vector<bool>{false, false},
          "a PC-relative operand names a location");
    check(structOperands("x0, [x1, x2, lsl #3]", aarch64) ==
              std::vector<bool>{false, false},
          "a register index has no displacement");
    check(operandNumber("w9, #-5", 1, aarch64) == -5 &&
              !operandNumber("w9, #-5", 1, OperandDialect::X86),
          "an ARM immediate's number keeps its sign");
    expectFormat(
        {"mov", "w8, #0x1234", "", "w8, #4660", true, at(1, decimal), aarch64});
    expectFormat(
        {"cmp", "x19, #8", "", "x19, #8", true, at(1, hexadecimal), aarch64});
    expectFormat({"cmp", "x19, #0x65", "", "x19, #0b1100101", true,
                  at(1, binary), aarch64});
    expectFormat(
        {"mov", "w0, #0x41", "", "w0, #'A'", true, at(1, character), aarch64});
    expectFormat({"mov", "w9, #0xfffb", "", "w9, #-0xffff0005", true,
                  at(1, negate), aarch64});
    expectFormat({"and", "x9, x19, #0xff0", "", "x9, x19, #~0xfffffffffffff00f",
                  true, at(2, invert), aarch64});
    expectFormat({"mov", "w0, #-1", "", "w0, #0xffffffff", true,
                  at(1, hexadecimal), aarch64});
    expectFormat({"mov", "x0, #0x1000", "", "x0, unk_1000", true, at(1, offset),
                  aarch64});
    expectFormat({"ld1", "{v0.16b, v1.16b}, [x0], #32", "",
                  "{v0.16b, v1.16b}, [x0], #0x20", true, at(2, hexadecimal),
                  aarch64});
    expectFormat({"fmov", "d0, #1.00000000", "", "d0, #1.00000000", true,
                  at(1, hexadecimal), aarch64});
    expectFormat(
        {"movw", "r0, #0xfffb", "", "r0, #65531", true, at(1, decimal), arm});
    // Structure offsets in displacements and immediates; one no member holds
    // keeps the decoder's number.
    expectFormat({"ldr", "x8, [x20, #0x10]", "", "x8, [x20, #_T.List.Flink]",
                  true, member(1), aarch64});
    expectFormat({"ldrb", "w8, [x20, #2]", "", "w8, [x20, #_T.Size]", true,
                  member(1), aarch64});
    expectFormat({"str", "x8, [x20, #0x18]!", "", "x8, [x20, #_T.List.Blink]!",
                  true, member(1), aarch64});
    expectFormat({"stp", "x29, x30, [sp, #-0x10]!", "",
                  "x29, x30, [sp, #-0x10]!", true, member(2), aarch64});
    expectFormat({"add", "x0, x0, #0x1c", "", "x0, x0, #_T.List.Blink+4", true,
                  member(2), aarch64});
    expectFormat({"ldr", "r0, [r5, #0x220]", "", "r0, [r5, #_T.Process]", true,
                  member(1), arm});
    expectFormat({"ldr", "r0, [pc, #0x10]", "", "r0, [pc, #0x10]", true,
                  member(1), arm});
    // An enumerator without a decoded operand width.
    OperandFacts facts;
    facts.dialect = aarch64;
    facts.mnemonic = "mov";
    std::vector<NumberFormat> formats(2);
    formats[1] = {NumberBase::Enum, false, false, "NUMBERS", 0};
    facts.numberFormats = &formats;
    unsigned width = 0;
    OperandFacts::EnumNamer enums = [&](std::string_view, std::uint64_t value,
                                        unsigned bits,
                                        bool) -> std::optional<std::string> {
      width = value == 7 ? bits : 0;
      return "SEVEN";
    };
    facts.enums = &enums;
    const LocationNamer none = [](std::uint64_t, NameUse, std::string_view) {
      return LocationName{};
    };
    check(formatOperands("w0, #7", facts, none).text() == "w0, #SEVEN" &&
              width == 32,
          "a w register's enumerator lost its width");
    check(formatOperands("x0, #7", facts, none).text() == "x0, #SEVEN" &&
              width == 64,
          "an x register's enumerator lost its width");
    facts.dialect = arm;
    check(formatOperands("r0, #7", facts, none).text() == "r0, #SEVEN" &&
              width == 32,
          "an ARM enumerator lost its width");

    // A 64-bit vector element keeps all of its immediate's bits, and an
    // immediate no width holds counts wider rather than lose bits.
    expectFormat({"vmov.i64", "d16, #0xffffffff00000000", "",
                  "d16, #0xffffffff00000000", true, at(1, hexadecimal), arm});
    expectFormat({"vmov.i64", "d0, #0xff00ff00ff00ff00", "",
                  "d0, #18374966859414961920", true, at(1, decimal), arm});
    expectFormat({"vmov.i64", "d0, #0xff", "", "d0, #~0xffffffffffffff00", true,
                  at(1, invert), arm});
    expectFormat({"vmov.i32", "q0, #0x100000000", "", "q0, #4294967296", true,
                  at(1, decimal), arm});
    // A shifted immediate is a field of a larger number: it takes a base but
    // names no member.
    expectFormat({"add", "x0, x1, #0x18, lsl #12", "", "x0, x1, #24, lsl #12",
                  true, at(2, decimal), aarch64});
    expectFormat({"add", "x0, x1, #0x18, lsl #12", "", "x0, x1, #0x18, lsl #12",
                  true, member(2), aarch64});
    check(structOperands("x0, x1, #0x18, lsl #12", aarch64) ==
              std::vector<bool>{false, false, false, false},
          "a shifted immediate takes a structure offset");

    // A structure offset lands an access as wide as the data a load, store,
    // swap or compare-and-swap moves.
    std::vector<unsigned> asked;
    const StructNamer sizes = [&](std::string_view, std::int64_t,
                                  unsigned bytes) {
      asked.push_back(bytes);
      return std::optional<StructLanding>(StructLanding{"Field", 0});
    };
    const auto access = [&](OperandDialect dialect, std::string_view mnemonic,
                            std::string_view operands, std::size_t operand) {
      OperandFacts accessFacts;
      accessFacts.dialect = dialect;
      accessFacts.mnemonic = mnemonic;
      accessFacts.structs = &sizes;
      const auto formats = member(operand);
      accessFacts.numberFormats = &formats;
      asked.clear();
      formatOperands(operands, accessFacts, none);
      return asked.empty() ? -1 : int(asked.front());
    };
    check(access(aarch64, "ldr", "x8, [x20, #0x10]", 1) == 8,
          "a load lost its register's width");
    check(access(aarch64, "stxr", "w8, x9, [x10]", 2) == 8 &&
              access(aarch64, "stlxp", "w8, x9, x11, [x10]", 3) == 8,
          "an exclusive store took its status register's width");
    check(access(aarch64, "stxrb", "w8, w9, [x10]", 2) == 1,
          "an exclusive byte store lost its size");
    check(access(aarch64, "swp", "x9, x8, [x10]", 2) == 8 &&
              access(aarch64, "casal", "x9, x8, [x10]", 2) == 8 &&
              access(aarch64, "casb", "w9, w8, [x10]", 2) == 1,
          "a swap or compare-and-swap lost its width");
    check(access(aarch64, "ldp", "w0, w1, [x2, #8]", 2) == 4 &&
              access(arm, "ldrd", "r0, r1, [r2, #8]", 2) == 4,
          "a pair counts other than one register");
    check(access(arm, "ldrhs", "r0, [r1, #4]", 1) == 4 &&
              access(arm, "ldrhhs", "r0, [r1, #4]", 1) == 2,
          "a condition was taken for a size");
    check(access(arm, "vld1.8", "{d0, d1}, [r0]", 1) == 0 &&
              access(aarch64, "ld1", "{v0.16b}, [x0]", 1) == 0,
          "a register list was given one register's size");
    check(access(aarch64, "add", "x0, x0, #0x18", 2) == 0,
          "an address computation was taken for an access");
  }
  {
    // SBF and EVM operands keep their decoders' spellings: SBF's signed
    // decimal, lddw's hexadecimal and `[r10 - 8]`, EVM's padded PUSH data.
    const auto sbf = OperandDialect::SBF, evm = OperandDialect::EVM;
    const auto at = [](std::size_t operand, NumberFormat format) {
      std::vector<NumberFormat> formats(operand + 1);
      formats[operand] = std::move(format);
      return formats;
    };
    const NumberFormat hexadecimal{NumberBase::Hex},
        decimal{NumberBase::Decimal}, binary{NumberBase::Binary},
        character{NumberBase::Char}, negate{NumberBase::Number, true},
        invert{NumberBase::Number, false, true};
    // A call target, a byte swap's width and a stack position are numbers,
    // but no constants of the program.
    check(formattableOperands("r1, 5", sbf, "mov64") ==
              std::vector<bool>{false, true},
          "an SBF immediate is no number");
    check(formattableOperands("r1, 65, block_7", sbf, "jeq") ==
              std::vector<bool>{false, true, false},
          "a branch target is a number");
    check(formattableOperands("12345", sbf, "call") == std::vector<bool>{false},
          "a call target is a constant");
    check(formattableOperands("r1, 16", sbf, "le") ==
              std::vector<bool>{false, false},
          "a byte swap's width is a constant");
    check(formattableOperands("r3, [r10 - 8]", sbf, "ldxdw") ==
              std::vector<bool>{false, false},
          "an SBF displacement is a plain number");
    check(structOperands("r3, [r10 - 8]", sbf, "ldxdw") ==
              std::vector<bool>{false, true},
          "an SBF displacement takes no structure offset");
    check(formattableOperands("0x0004", evm, "PUSH2") ==
              std::vector<bool>{true},
          "PUSH data is no number");
    check(formattableOperands("0x01", evm, "DUPN") == std::vector<bool>{false},
          "a stack position is a constant");
    check(structOperands("0x0004", evm, "PUSH2") == std::vector<bool>{true} &&
              structOperands("0xfffffffffffffffffffffffffffffffffffffffffffffff"
                             "fffffffffffffffff",
                             evm, "PUSH32") == std::vector<bool>{false},
          "PUSH data wider than 64 bits takes a structure offset");

    // An SBF immediate counts in its operation's width, a store's in the
    // data's, unless the encoded number needs the field's 32 bits.
    expectFormat(
        {"mov64", "r1, 5", "", "r1, 0x5", true, at(1, hexadecimal), sbf});
    expectFormat({"add64", "r1, -16", "", "r1, 0xfffffffffffffff0", true,
                  at(1, hexadecimal), sbf});
    expectFormat({"add64", "r1, -16", "", "r1, -16", true, at(1, negate), sbf});
    expectFormat({"mov32", "r0, -1", "", "r0, 0xffffffff", true,
                  at(1, hexadecimal), sbf});
    expectFormat({"jeq", "r1, 65, block_7", "", "r1, 'A', block_7", true,
                  at(1, character), sbf});
    expectFormat({"stb", "[r1 + 2], -1", "", "[r1 + 2], 0xff", true,
                  at(1, hexadecimal), sbf});
    expectFormat({"stb", "[r1 + 2], 255", "", "[r1 + 2], 0b11111111", true,
                  at(1, binary), sbf});
    expectFormat({"stb", "[r1 + 2], 300", "", "[r1 + 2], 0x12c", true,
                  at(1, hexadecimal), sbf});
    expectFormat({"lddw", "r2, 0x912345678", "", "r2, 38960125560", true,
                  at(1, decimal), sbf});
    expectFormat({"lddw", "r2, 0x912345678", "", "r2, ~0xfffffff6edcba987",
                  true, at(1, invert), sbf});
    // A structure offset names a displacement's member or an immediate's.
    expectFormat({"ldxdw", "r3, [r1 + 24]", "", "r3, [r1 + _T.List.Blink]",
                  true, member(1), sbf});
    expectFormat({"stxdw", "[r1 + 548], r3", "", "[r1 + _T.Process+4], r3",
                  true, member(0), sbf});
    expectFormat({"ldxb", "r3, [r10 - 8]", "", "r3, [r10 + _T.Type-8]", true,
                  member(1, 8), sbf});
    expectFormat(
        {"ldxdw", "r3, [r10 - 8]", "", "r3, [r10 - 8]", true, member(1), sbf});
    expectFormat(
        {"mov64", "r1, 24", "", "r1, _T.List.Blink", true, member(1), sbf});

    // PUSH data counts in the PUSH's own width, up to the 256-bit word.
    expectFormat({"PUSH2", "0x0004", "", "4", true, at(0, decimal), evm});
    expectFormat({"PUSH2", "0xff00", "", "~0x00ff", true, at(0, invert), evm});
    expectFormat({"PUSH2", "0x0004", "", "-0xfffc", true, at(0, negate), evm});
    expectFormat(
        {"PUSH2", "0x1234", "", "0b1001000110100", true, at(0, binary), evm});
    expectFormat({"PUSH1", "0x41", "", "'A'", true, at(0, character), evm});
    expectFormat(
        {"PUSH32",
         "0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
         "",
         "115792089237316195423570985008687907853269984665640564039457"
         "584007913129639935",
         true, at(0, decimal), evm});
    expectFormat(
        {"PUSH32",
         "0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
         "",
         "~0x0000000000000000000000000000000000000000000000000000000000000000",
         true, at(0, invert), evm});
    expectFormat(
        {"PUSH32",
         "0x45524332303a207472616e7366657220616d6f756e7420657863656564732062",
         "", "'ERC20: transfer amount exceeds b'", true, at(0, character),
         evm});
    expectFormat({"PUSH1", "0x18", "", "_T.List.Blink", true, member(0), evm});
    expectFormat(
        {"PUSH32",
         "0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
         "",
         "0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
         true, member(0), evm});
  }
  return 0;
}
