//===- X64_IntegerSemanticTests.cpp - x64 integer ALU tests ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Migrated from scripts/lift_verifier.py — x64 integer instruction categories:
//   Core, Ext, Shift, Rotate, BMI, MulDiv, SetccCmov, CoreExtra, Op32, Op16,
//   BSwapBT, ShldShrd, MiscExt, FlagMisc, Fence, CoreMissing, BMIExtended
//
//===----------------------------------------------------------------------===//

#include "UnicornSemanticFixture.h"

// ============================================================================
// TEST_P body — shared by all instantiations
// ============================================================================

TEST_P(X64Semantic, IntegerVerify) { runX64(GetParam()); }

// ============================================================================
// Core: MOV, ADD, SUB, AND, OR, XOR, NOT, NEG, INC, DEC, LEA, etc.
// ============================================================================
// clang-format off
static const std::vector<SemTC> kX64Core = {
  {"mov_imm",       "mov rax, 42",                              {},                                           {"rax"},           "Core", {}},
  {"mov_reg",       "mov rax, rbx",                             {{"rbx", 99}},                                {"rax"},           "Core", {}},
  {"add_imm",       "add rax, 42",                              {{"rax", 100}},                               {"rax"},           "Core", {}},
  {"add_reg",       "add rax, rbx",                             {{"rax", 50}, {"rbx", 30}},                   {"rax"},           "Core", {}},
  {"sub_imm",       "sub rax, 10",                              {{"rax", 100}},                               {"rax"},           "Core", {}},
  {"sub_reg",       "sub rax, rbx",                             {{"rax", 100}, {"rbx", 30}},                  {"rax"},           "Core", {}},
  {"and_imm",       "and rax, 0xFF",                            {{"rax", 0x12345678}},                        {"rax"},           "Core", {}},
  {"and_reg",       "and rax, rbx",                             {{"rax", 0xFF00}, {"rbx", 0x0FF0}},           {"rax"},           "Core", {}},
  {"or_imm",        "or rax, 0xFF",                             {{"rax", 0x100}},                             {"rax"},           "Core", {}},
  {"or_reg",        "or rax, rbx",                              {{"rax", 0xF0}, {"rbx", 0x0F}},               {"rax"},           "Core", {}},
  {"xor_imm",       "xor rax, 0xFF",                            {{"rax", 0xAA}},                              {"rax"},           "Core", {}},
  {"xor_reg",       "xor rax, rbx",                             {{"rax", 0xFF}, {"rbx", 0x55}},               {"rax"},           "Core", {}},
  {"xor_self",      "xor rax, rax",                             {{"rax", 42}},                                {"rax"},           "Core", {}},
  {"not",           "not rax",                                  {{"rax", 0}},                                 {"rax"},           "Core", {}},
  {"neg",           "neg rax",                                  {{"rax", 42}},                                {"rax"},           "Core", {}},
  {"inc",           "inc rax",                                  {{"rax", 99}},                                {"rax"},           "Core", {}},
  {"dec",           "dec rax",                                  {{"rax", 100}},                               {"rax"},           "Core", {}},
  {"nop",           "nop; mov rax, 42",                         {},                                           {"rax"},           "Core", {}},
  {"lea_simple",    "lea rax, [rbx + 8]",                       {{"rbx", 100}},                               {"rax"},           "Core", {}},
  {"lea_complex",   "lea rax, [rbx + rcx*4 + 8]",              {{"rbx", 100}, {"rcx", 10}},                  {"rax"},           "Core", {}},
  {"lea_scale",     "lea rax, [rcx*8]",                         {{"rcx", 5}},                                 {"rax"},           "Core", {}},
  {"cmp_flags",     "cmp rax, rbx; setz cl",                    {{"rax", 42}, {"rbx", 42}},                   {"rcx"},           "Core", {}},
  {"test_flags",    "test rax, rax; setz cl",                   {{"rax", 0}},                                 {"rcx"},           "Core", {}},
  {"adc_simple",    "stc; adc rax, rbx",                        {{"rax", 10}, {"rbx", 20}},                   {"rax"},           "Core", {}},
  {"sbb_simple",    "stc; sbb rax, rbx",                        {{"rax", 100}, {"rbx", 30}},                  {"rax"},           "Core", {}},
  {"push_pop",      "push rax; pop rbx",                        {{"rax", 0x42}},                              {"rbx"},           "Core", {}},
  {"xchg",          "xchg rax, rbx",                            {{"rax", 1}, {"rbx", 2}},                     {"rax", "rbx"},    "Core", {}},
};

// ============================================================================
// Ext: MOVZX, MOVSX, IMUL, BSWAP, BSF, BSR, BT, POPCNT, LZCNT, TZCNT
// ============================================================================
static const std::vector<SemTC> kX64Ext = {
  {"movzx_byte",    "movzx eax, bl",                            {{"rbx", 0xFF42}},                            {"rax"},           "Ext", {}},
  {"movzx_word",    "movzx eax, bx",                            {{"rbx", 0xFF42}},                            {"rax"},           "Ext", {}},
  {"movsx_byte",    "movsx rax, bl",                             {{"rbx", 0x80}},                              {"rax"},           "Ext", {}},
  {"movsx_word",    "movsx rax, bx",                             {{"rbx", 0x8000}},                            {"rax"},           "Ext", {}},
  {"movsxd",        "movsxd rax, ebx",                           {{"rbx", 0xFFFFFFFF}},                        {"rax"},           "Ext", {}},
  {"imul_2op",      "imul rax, rbx",                             {{"rax", 6}, {"rbx", 7}},                     {"rax"},           "Ext", {}},
  {"imul_3op",      "imul rax, rbx, 7",                          {{"rbx", 6}},                                 {"rax"},           "Ext", {}},
  {"mul_reg",       "mul rbx",                                   {{"rax", 6}, {"rbx", 7}},                     {"rax", "rdx"},    "Ext", {}},
  {"div_reg",       "xor rdx, rdx; mov rax, 100; mov rcx, 7; div rcx", {},                                    {"rax", "rdx"},    "Ext", {}},
  {"idiv_reg",      "xor rdx, rdx; mov rax, 100; mov rcx, 7; idiv rcx", {},                                   {"rax", "rdx"},    "Ext", {}},
  {"cdqe",          "mov eax, -1; cdqe",                         {},                                           {"rax"},           "Ext", {}},
  {"cqo",           "mov rax, -1; cqo",                          {},                                           {"rdx"},           "Ext", {}},
  {"bswap",         "bswap rax",                                 {{"rax", 0x0102030405060708ULL}},              {"rax"},           "Ext", {}},
  {"bsf",           "bsf rax, rbx",                              {{"rbx", 0x80}},                              {"rax"},           "Ext", {}},
  {"bsr",           "bsr rax, rbx",                              {{"rbx", 0x80}},                              {"rax"},           "Ext", {}},
  {"bt_imm",        "bt rax, 3; setc cl",                        {{"rax", 0x8}},                               {"rcx"},           "Ext", {}},
  {"bts",           "bts rax, 5",                                {{"rax", 0}},                                 {"rax"},           "Ext", {}},
  {"btr",           "btr rax, 3",                                {{"rax", 0xFF}},                              {"rax"},           "Ext", {}},
  {"btc",           "btc rax, 3",                                {{"rax", 0}},                                 {"rax"},           "Ext", {}},
  {"popcnt",        "popcnt rax, rbx",                           {{"rbx", 0xFF}},                              {"rax"},           "Ext", {}},
  {"lzcnt",         "lzcnt rax, rbx",                            {{"rbx", 0x100}},                             {"rax"},           "Ext", {}},
  {"tzcnt",         "tzcnt rax, rbx",                            {{"rbx", 0x100}},                             {"rax"},           "Ext", {}},
};

// ============================================================================
// Shift: SHL, SHR, SAR, ROL, ROR, SHLD, SHRD
// ============================================================================
static const std::vector<SemTC> kX64Shift = {
  {"shl_imm",       "shl rax, 3",                               {{"rax", 5}},                                 {"rax"},           "Shift", {}},
  {"shr_imm",       "shr rax, 2",                               {{"rax", 100}},                               {"rax"},           "Shift", {}},
  {"sar_imm",       "sar rax, 2",                               {{"rax", 0xFFFFFFFFFFFFFF00ULL}},              {"rax"},           "Shift", {}},
  {"shl_cl",        "shl rax, cl",                               {{"rax", 1}, {"rcx", 10}},                    {"rax"},           "Shift", {}},
  {"shr_cl",        "shr rax, cl",                               {{"rax", 1024}, {"rcx", 2}},                  {"rax"},           "Shift", {}},
  {"rol_imm",       "rol rax, 4",                                {{"rax", 0xF}},                               {"rax"},           "Shift", {}},
  {"ror_imm",       "ror rax, 4",                                {{"rax", 0xF0}},                              {"rax"},           "Shift", {}},
  {"shld",          "shld rax, rbx, 4",                          {{"rax", 0xF0}, {"rbx", 0x0F}},               {"rax"},           "Shift", {}},
  {"shrd",          "shrd rax, rbx, 4",                          {{"rax", 0xF0}, {"rbx", 0x0F00000000000000ULL}}, {"rax"},         "Shift", {}},
};

// ============================================================================
// Control: SETcc, CMOVcc
// ============================================================================
static const std::vector<SemTC> kX64Control = {
  {"setz",          "cmp rax, rbx; setz cl",                     {{"rax", 5}, {"rbx", 5}},                     {"rcx"},           "Control", {}},
  {"setnz",         "cmp rax, rbx; setnz cl",                    {{"rax", 5}, {"rbx", 6}},                     {"rcx"},           "Control", {}},
  {"setl",          "cmp rax, rbx; setl cl",                     {{"rax", 3}, {"rbx", 5}},                     {"rcx"},           "Control", {}},
  {"setg",          "cmp rax, rbx; setg cl",                     {{"rax", 10}, {"rbx", 5}},                    {"rcx"},           "Control", {}},
  {"setle",         "cmp rax, rbx; setle cl",                    {{"rax", 5}, {"rbx", 5}},                     {"rcx"},           "Control", {}},
  {"setge",         "cmp rax, rbx; setge cl",                    {{"rax", 5}, {"rbx", 5}},                     {"rcx"},           "Control", {}},
  {"seta",          "cmp rax, rbx; seta cl",                     {{"rax", 10}, {"rbx", 5}},                    {"rcx"},           "Control", {}},
  {"setb",          "cmp rax, rbx; setb cl",                     {{"rax", 3}, {"rbx", 5}},                     {"rcx"},           "Control", {}},
  {"cmovz",         "cmp rax, rax; cmovz rbx, rcx",              {{"rax", 42}, {"rbx", 0}, {"rcx", 99}},      {"rbx"},           "Control", {}},
  {"cmovnz",        "cmp rax, rbx; cmovnz rcx, rdx",             {{"rax", 1}, {"rbx", 2}, {"rcx", 0}, {"rdx", 99}}, {"rcx"},     "Control", {}},
  {"cmovl",         "cmp rax, rbx; cmovl rcx, rdx",              {{"rax", 1}, {"rbx", 5}, {"rcx", 0}, {"rdx", 99}}, {"rcx"},     "Control", {}},
  {"cmovg",         "cmp rax, rbx; cmovg rcx, rdx",              {{"rax", 10}, {"rbx", 5}, {"rcx", 0}, {"rdx", 99}}, {"rcx"},    "Control", {}},
  {"jz_taken",      "cmp rax, rbx; jz .L1; mov rcx, 0; .L1: mov rcx, 1",
                                                                  {{"rax", 5}, {"rbx", 5}, {"rcx", 0}},       {"rcx"},           "Control", {}},
};

// ============================================================================
// MulDiv: mul, imul, div, idiv variants
// ============================================================================
static const std::vector<SemTC> kX64MulDiv = {
  {"mul_reg",       "mov rax, 7; mov rbx, 6; mul rbx",           {},                                           {"rax", "rdx"},    "MulDiv", {}},
  {"imul_reg",      "mov rax, -3; mov rbx, 4; imul rbx",         {},                                           {"rax", "rdx"},    "MulDiv", {}},
  {"imul_reg_imm",  "mov rbx, 10; imul rax, rbx, 5",             {},                                           {"rax"},           "MulDiv", {}},
  {"imul_two_op",   "mov rax, 7; mov rbx, 8; imul rax, rbx",     {},                                           {"rax"},           "MulDiv", {}},
  {"div_reg",       "mov rax, 100; xor rdx, rdx; mov rbx, 7; div rbx", {},                                     {"rax", "rdx"},    "MulDiv", {}},
  {"idiv_reg",      "mov rax, -100; cqo; mov rbx, 7; idiv rbx",  {},                                           {"rax", "rdx"},    "MulDiv", {}},
  {"mul_32",        "mov eax, 1000; mov ebx, 1000; mul ebx",     {},                                           {"rax", "rdx"},    "MulDiv", {}},
  {"imul_32_imm",   "mov ebx, 100; imul eax, ebx, -3",           {},                                           {"rax"},           "MulDiv", {}},
};

// ============================================================================
// Rotate: ROL, ROR, RCL, RCR
// ============================================================================
static const std::vector<SemTC> kX64Rotate = {
  {"rol_imm",       "mov rax, 0x8000000000000001; rol rax, 1",   {},                                           {"rax"},           "Rotate", {}},
  {"ror_imm",       "mov rax, 0x0000000000000003; ror rax, 1",   {},                                           {"rax"},           "Rotate", {}},
  {"rol_cl",        "mov rax, 0x8000000000000001; mov rcx, 4; rol rax, cl", {},                                 {"rax"},           "Rotate", {}},
  {"ror_cl",        "mov rax, 0x0000000000000003; mov rcx, 4; ror rax, cl", {},                                 {"rax"},           "Rotate", {}},
  {"rcl",           "stc; mov rax, 0x8000000000000000; rcl rax, 1", {},                                         {"rax"},           "Rotate", {}},
  {"rcr",           "stc; mov rax, 1; rcr rax, 1",               {},                                           {"rax"},           "Rotate", {}},
  {"rol_32",        "mov eax, 0x80000001; rol eax, 1",           {},                                           {"rax"},           "Rotate", {}},
  {"ror_32",        "mov eax, 0x00000003; ror eax, 1",           {},                                           {"rax"},           "Rotate", {}},
};

// ============================================================================
// BMI: BLSI, BLSMSK, BLSR, ANDN, BEXTR, BZHI, RORX, SARX, etc.
// ============================================================================
static const std::vector<SemTC> kX64BMI = {
  {"blsi",          "blsi rax, rbx",                              {{"rbx", 0x30}},                              {"rax"},           "BMI", {}},
  {"blsmsk",        "blsmsk rax, rbx",                            {{"rbx", 0x30}},                              {"rax"},           "BMI", {}},
  {"blsr",          "blsr rax, rbx",                              {{"rbx", 0x30}},                              {"rax"},           "BMI", {}},
  {"andn",          "andn rax, rbx, rcx",                         {{"rbx", 0xFF}, {"rcx", 0x1234}},             {"rax"},           "BMI", {}},
  {"bextr",         "bextr rax, rbx, rcx",                        {{"rbx", 0xABCD}, {"rcx", 0x0804}},           {"rax"},           "BMI", {}},
  {"bzhi",          "bzhi rax, rbx, rcx",                         {{"rbx", 0xFFFFFFFF}, {"rcx", 16}},           {"rax"},           "BMI", {}},
  {"rorx",          "rorx rax, rbx, 4",                           {{"rbx", 0xF0}},                              {"rax"},           "BMI", {}},
  {"sarx",          "sarx rax, rbx, rcx",                         {{"rbx", 0xFFFFFFFFFFFFFF00ULL}, {"rcx", 4}}, {"rax"},           "BMI", {}},
  {"shlx",          "shlx rax, rbx, rcx",                         {{"rbx", 0x1}, {"rcx", 10}},                  {"rax"},           "BMI", {}},
  {"shrx",          "shrx rax, rbx, rcx",                         {{"rbx", 0x10000}, {"rcx", 4}},               {"rax"},           "BMI", {}},
  {"pdep",          "pdep rax, rbx, rcx",                         {{"rbx", 0xFF}, {"rcx", 0x5555}},             {"rax"},           "BMI", {}},
  {"pext",          "pext rax, rbx, rcx",                         {{"rbx", 0xAAAA}, {"rcx", 0x5555}},           {"rax"},           "BMI", {}},
  {"mulx",          "mulx rcx, rdx, rbx",                         {{"rbx", 7}, {"rdx", 6}},                     {"rcx", "rdx"},    "BMI", {}},
};

// ============================================================================
// SetccCmov: SETcc / CMOVcc full set
// ============================================================================
static const std::vector<SemTC> kX64SetccCmov = {
  {"sete",          "xor eax,eax; cmp rbx, rcx; sete al",        {{"rbx", 42}, {"rcx", 42}},                   {"rax"},           "SetccCmov", {}},
  {"setne",         "xor eax,eax; cmp rbx, rcx; setne al",       {{"rbx", 42}, {"rcx", 99}},                   {"rax"},           "SetccCmov", {}},
  {"setl",          "xor eax,eax; cmp rbx, rcx; setl al",        {{"rbx", 10}, {"rcx", 20}},                   {"rax"},           "SetccCmov", {}},
  {"setge",         "xor eax,eax; cmp rbx, rcx; setge al",       {{"rbx", 20}, {"rcx", 10}},                   {"rax"},           "SetccCmov", {}},
  {"setb",          "xor eax,eax; cmp rbx, rcx; setb al",        {{"rbx", 5}, {"rcx", 10}},                    {"rax"},           "SetccCmov", {}},
  {"setae",         "xor eax,eax; cmp rbx, rcx; setae al",       {{"rbx", 10}, {"rcx", 5}},                    {"rax"},           "SetccCmov", {}},
  {"cmove",         "mov rax, 100; cmp rbx, rcx; cmove rax, rdx", {{"rbx", 5}, {"rcx", 5}, {"rdx", 200}},      {"rax"},           "SetccCmov", {}},
  {"cmovne",        "mov rax, 100; cmp rbx, rcx; cmovne rax, rdx", {{"rbx", 5}, {"rcx", 10}, {"rdx", 200}},    {"rax"},           "SetccCmov", {}},
  {"cmovl",         "mov rax, 100; cmp rbx, rcx; cmovl rax, rdx", {{"rbx", 5}, {"rcx", 10}, {"rdx", 200}},     {"rax"},           "SetccCmov", {}},
  {"cmovg",         "mov rax, 100; cmp rbx, rcx; cmovg rax, rdx", {{"rbx", 10}, {"rcx", 5}, {"rdx", 200}},     {"rax"},           "SetccCmov", {}},
  {"sets",          "xor eax,eax; mov rbx, 0x8000000000000000; test rbx,rbx; sets al", {},                      {"rax"},           "SetccCmov", {}},
  {"seto",          "xor eax,eax; mov rbx, 0x7FFFFFFFFFFFFFFF; add rbx, 1; seto al",  {},                       {"rax"},           "SetccCmov", {}},
};

// ============================================================================
// CoreExtra: more ALU, XADD, CMPXCHG, CQO, CDQ, CBW, CWDE
// ============================================================================
static const std::vector<SemTC> kX64CoreExtra = {
  {"neg_pos",       "neg rax",                                    {{"rax", 5}},                                 {"rax"},           "CoreExt", {}},
  {"neg_neg",       "neg rax",                                    {{"rax", 0xFFFFFFFFFFFFFFFBULL}},             {"rax"},           "CoreExt", {}},
  {"neg_zero",      "neg rax",                                    {{"rax", 0}},                                 {"rax"},           "CoreExt", {}},
  {"not_val",       "not rax",                                    {{"rax", 0xFF00FF00FF00FF00ULL}},             {"rax"},           "CoreExt", {}},
  {"inc_reg",       "inc rax",                                    {{"rax", 41}},                                {"rax"},           "CoreExt", {}},
  {"dec_reg",       "dec rax",                                    {{"rax", 43}},                                {"rax"},           "CoreExt", {}},
  {"xadd_regs",     "xadd rbx, rax",                              {{"rax", 10}, {"rbx", 20}},                   {"rax", "rbx"},    "CoreExt", {}},
  {"cmpxchg_reg_eq","mov rax, 42; cmpxchg rbx, rcx",              {{"rbx", 42}, {"rcx", 99}},                   {"rax", "rbx"},    "CoreExt", {}},
  {"cmpxchg_reg_neq","mov rax, 10; cmpxchg rbx, rcx",             {{"rbx", 42}, {"rcx", 99}},                   {"rax", "rbx"},    "CoreExt", {}},
  {"cdqe",          "mov eax, 0xFFFFFF00; cdqe",                  {},                                           {"rax"},           "CoreExt", {}},
  {"cqo",           "mov rax, 0xFFFFFFFF80000000; cqo",           {},                                           {"rdx"},           "CoreExt", {}},
  {"cdq",           "mov eax, 0x80000000; cdq",                   {},                                           {"rdx"},           "CoreExt", {}},
  {"cbw",           "mov al, 0x80; cbw",                          {},                                           {"rax"},           "CoreExt", {}},
  {"cwde",          "mov ax, 0x8000; cwde",                       {},                                           {"rax"},           "CoreExt", {}},
};

// ============================================================================
// Op32: 32-bit operands on x64 (verify zero-extension)
// ============================================================================
static const std::vector<SemTC> kX64Op32 = {
  {"add_32",        "add eax, ebx",                               {{"rax", 0xFFFFFFFF00000001ULL}, {"rbx", 2}}, {"rax"},           "Op32", {}},
  {"sub_32",        "sub eax, ebx",                               {{"rax", 0xFFFFFFFF0000000AULL}, {"rbx", 3}}, {"rax"},           "Op32", {}},
  {"and_32",        "and eax, 0xFF",                              {{"rax", 0xFFFFFFFF000000FFULL}},             {"rax"},           "Op32", {}},
  {"or_32",         "or eax, 0xFF00",                             {{"rax", 0xFFFFFFFF000000FFULL}},             {"rax"},           "Op32", {}},
  {"xor_32",        "xor eax, eax",                               {{"rax", 0xDEADBEEF}},                       {"rax"},           "Op32", {}},
  {"mov_32",        "mov eax, 42",                                {{"rax", 0xDEADBEEF}},                       {"rax"},           "Op32", {}},
  {"shl_32",        "shl eax, 4",                                 {{"rax", 0xFFFFFFFF00000001ULL}},             {"rax"},           "Op32", {}},
  {"shr_32",        "shr eax, 4",                                 {{"rax", 0xFFFFFFFF000000F0ULL}},             {"rax"},           "Op32", {}},
  {"imul_32",       "imul eax, ebx",                              {{"rax", 6}, {"rbx", 7}},                     {"rax"},           "Op32", {}},
  {"neg_32",        "neg eax",                                    {{"rax", 0xFFFFFFFF00000001ULL}},             {"rax"},           "Op32", {}},
  {"not_32",        "not eax",                                    {{"rax", 0xFFFFFFFF000000FFULL}},             {"rax"},           "Op32", {}},
  {"inc_32",        "inc eax",                                    {{"rax", 0xFFFFFFFF00000001ULL}},             {"rax"},           "Op32", {}},
  {"dec_32",        "dec eax",                                    {{"rax", 0xFFFFFFFF00000002ULL}},             {"rax"},           "Op32", {}},
  {"bswap_32",      "bswap eax",                                  {{"rax", 0xFFFFFFFF01020304ULL}},             {"rax"},           "Op32", {}},
};

// ============================================================================
// Op16: 16-bit / 8-bit operands
// ============================================================================
static const std::vector<SemTC> kX64Op16 = {
  {"add_16",        "add ax, bx",                                 {{"rax", 0xDEAD0001}, {"rbx", 2}},            {"rax"},           "Op16", {}},
  {"sub_16",        "sub ax, bx",                                 {{"rax", 0xDEAD000A}, {"rbx", 3}},            {"rax"},           "Op16", {}},
  {"add_8",         "add al, bl",                                 {{"rax", 0xDEAD0001}, {"rbx", 2}},            {"rax"},           "Op16", {}},
  {"sub_8",         "sub al, bl",                                 {{"rax", 0xDEAD000A}, {"rbx", 3}},            {"rax"},           "Op16", {}},
  {"xor_8",         "xor al, al",                                 {{"rax", 0xDEAD00FF}},                       {"rax"},           "Op16", {}},
  {"movzx_al",      "movzx eax, bl",                              {{"rax", 0xDEAD}, {"rbx", 0x80}},             {"rax"},           "Op16", {}},
  {"movsx_al",      "movsx eax, bl",                              {{"rax", 0xDEAD}, {"rbx", 0x80}},             {"rax"},           "Op16", {}},
  {"xchg_16",       "xchg ax, bx",                                {{"rax", 0xDEAD1111}, {"rbx", 0xBEEF2222}},   {"rax", "rbx"},    "Op16", {}},
};

// ============================================================================
// ControlExt: more SETcc/CMOVcc/LEA variants
// ============================================================================
static const std::vector<SemTC> kX64ControlExt = {
  {"setae",         "cmp rax, rbx; setae cl",                     {{"rax", 10}, {"rbx", 5}},                    {"rcx"},           "ControlExt", {}},
  {"setbe",         "cmp rax, rbx; setbe cl",                     {{"rax", 3}, {"rbx", 5}},                     {"rcx"},           "ControlExt", {}},
  {"sets",          "mov rax, -1; test rax, rax; sets cl",        {{"rax", 0}},                                 {"rcx"},           "ControlExt", {}},
  {"setns",         "mov rax, 1; test rax, rax; setns cl",        {{"rax", 0}},                                 {"rcx"},           "ControlExt", {}},
  {"seto",          "mov rax, 0x7FFFFFFFFFFFFFFF; add rax, 1; seto cl", {},                                     {"rcx"},           "ControlExt", {}},
  {"setno",         "mov rax, 5; add rax, 1; setno cl",           {},                                           {"rcx"},           "ControlExt", {}},
  {"setp",          "mov al, 3; test al, al; setp cl",            {},                                           {"rcx"},           "ControlExt", {}},
  {"setnp",         "mov al, 3; test al, al; setnp cl",           {},                                           {"rcx"},           "ControlExt", {}},
  {"cmova",         "cmp rax, rbx; cmova rcx, rdx",               {{"rax", 10}, {"rbx", 5}, {"rcx", 0}, {"rdx", 99}}, {"rcx"},    "ControlExt", {}},
  {"cmovb",         "cmp rax, rbx; cmovb rcx, rdx",               {{"rax", 3}, {"rbx", 5}, {"rcx", 0}, {"rdx", 99}},  {"rcx"},    "ControlExt", {}},
  {"cmovae",        "cmp rax, rbx; cmovae rcx, rdx",              {{"rax", 5}, {"rbx", 5}, {"rcx", 0}, {"rdx", 99}},  {"rcx"},    "ControlExt", {}},
  {"cmovbe",        "cmp rax, rbx; cmovbe rcx, rdx",              {{"rax", 3}, {"rbx", 5}, {"rcx", 0}, {"rdx", 99}},  {"rcx"},    "ControlExt", {}},
  {"cmovs",         "mov rax, -1; test rax, rax; cmovs rcx, rdx", {{"rcx", 0}, {"rdx", 99}},                   {"rcx"},           "ControlExt", {}},
  {"cmovns",        "mov rax, 1; test rax, rax; cmovns rcx, rdx", {{"rcx", 0}, {"rdx", 99}},                   {"rcx"},           "ControlExt", {}},
  {"lea_base_disp", "lea rax, [rbx + 0x10]",                      {{"rbx", 0x100}},                             {"rax"},           "ControlExt", {}},
  {"lea_index_scale","lea rax, [rbx + rcx*4]",                    {{"rbx", 0x100}, {"rcx", 5}},                 {"rax"},           "ControlExt", {}},
  {"lea_sib_full",  "lea rax, [rbx + rcx*8 + 0x20]",             {{"rbx", 0x100}, {"rcx", 3}},                 {"rax"},           "ControlExt", {}},
};

// ============================================================================
// Mem: memory load/store patterns
// ============================================================================
static const std::vector<SemTC> kX64Mem = {
  {"mov_load64",    "mov rax, [rsi]",                             {{"rsi", DATA_BASE}},                         {"rax"},           "Mem",
   {{DATA_BASE, packU64(0xDEADBEEF)}}},
  {"mov_store64",   "mov [rsi], rax; mov rbx, [rsi]",             {{"rsi", DATA_BASE}, {"rax", 0x42}},          {"rbx"},           "Mem", {}},
  {"mov_load32",    "mov eax, [rsi]",                             {{"rsi", DATA_BASE}},                         {"rax"},           "Mem",
   {{DATA_BASE, packU32(0xCAFEBABE)}}},
  {"mov_load16",    "movzx eax, word ptr [rsi]",                  {{"rsi", DATA_BASE}},                         {"rax"},           "Mem",
   {{DATA_BASE, packU16(0x1234)}}},
  {"mov_load8",     "movzx eax, byte ptr [rsi]",                  {{"rsi", DATA_BASE}},                         {"rax"},           "Mem",
   {{DATA_BASE, {0x42}}}},
  {"lea_rip",       "lea rax, [rip + 0]",                         {},                                           {"rax"},           "Mem", {}},
  {"cmovz_mem",     "cmp rax, rax; cmovz rbx, [rsi]",             {{"rax", 42}, {"rsi", DATA_BASE}},            {"rbx"},           "Mem",
   {{DATA_BASE, packU64(99)}}},
};

// ============================================================================
// Flags: CLC, STC, CMC, LAHF, SAHF, CLD, STD
// ============================================================================
static const std::vector<SemTC> kX64Flags = {
  {"stc_setc",      "stc; setc al",                               {},                                           {"rax"},           "Flags", {}},
  {"clc_setc",      "clc; setc al",                               {},                                           {"rax"},           "Flags", {}},
  {"cmc_after_clc", "clc; cmc; setc al",                          {},                                           {"rax"},           "Flags", {}},
  {"cmc_after_stc", "stc; cmc; setc al",                          {},                                           {"rax"},           "Flags", {}},
  {"lahf_sahf",     "stc; lahf; clc; sahf; setc al",              {},                                           {"rax"},           "Flags", {}},
  {"cld_std_flags", "cld; mov rax, 0; std; mov rax, 1",           {},                                           {"rax"},           "Flags", {}},
};

// ============================================================================
// String: STOS, LODS, MOVS, CMPS, SCAS (single iteration)
// ============================================================================
static const std::vector<SemTC> kX64String = {
  {"stosb",         "cld; mov al, 0x42; stosb",                   {{"rdi", DATA_BASE}},                         {"rdi"},           "String", {}},
  {"lodsb",         "cld; lodsb",                                 {{"rsi", DATA_BASE}},                         {"rax", "rsi"},    "String", {}},
  {"movsb",         "cld; movsb",                                 {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 0x100}}, {"rsi", "rdi"}, "String", {}},
};

// ============================================================================
// StringExt: additional string ops
// ============================================================================
static const std::vector<SemTC> kX64StringExt = {
  {"stosw",         "cld; mov ax, 0x1234; stosw",                 {{"rdi", DATA_BASE}},                         {"rdi"},           "StringExt", {}},
  {"stosd",         "cld; mov eax, 0x12345678; stosd",            {{"rdi", DATA_BASE}},                         {"rdi"},           "StringExt", {}},
  {"stosq",         "cld; mov rax, 0x1234567890ABCDEF; stosq",    {{"rdi", DATA_BASE}},                         {"rdi"},           "StringExt", {}},
  {"lodsw",         "cld; lodsw",                                 {{"rsi", DATA_BASE}},                         {"rax", "rsi"},    "StringExt",
   {{DATA_BASE, packU16(0x1234)}}},
  {"lodsd",         "cld; lodsd",                                 {{"rsi", DATA_BASE}},                         {"rax", "rsi"},    "StringExt",
   {{DATA_BASE, packU32(0x12345678)}}},
  {"lodsq",         "cld; lodsq",                                 {{"rsi", DATA_BASE}},                         {"rax", "rsi"},    "StringExt",
   {{DATA_BASE, packU64(0x1234567890ABCDEFULL)}}},
  {"movsw",         "cld; movsw",                                 {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 0x100}}, {"rsi", "rdi"}, "StringExt",
   {{DATA_BASE, packU16(0xABCD)}}},
  {"movsq",         "cld; movsq",                                 {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 0x100}}, {"rsi", "rdi"}, "StringExt",
   {{DATA_BASE, packU64(0xCAFEBABEDEADBEEFULL)}}},
  {"cmpsb_eq",      "cld; cmpsb; setz al",                        {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 0x100}}, {"rax", "rsi", "rdi"}, "StringExt",
   {{DATA_BASE, {0x42}}, {DATA_BASE + 0x100, {0x42}}}},
  {"cmpsb_neq",     "cld; cmpsb; setz al",                        {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 0x100}}, {"rax", "rsi", "rdi"}, "StringExt",
   {{DATA_BASE, {0x42}}, {DATA_BASE + 0x100, {0x43}}}},
  {"scasb_found",   "cld; mov al, 0x42; scasb; setz cl",          {{"rdi", DATA_BASE}},                         {"rcx", "rdi"},    "StringExt",
   {{DATA_BASE, {0x42}}}},
  {"scasb_notfound","cld; mov al, 0x42; scasb; setz cl",          {{"rdi", DATA_BASE}},                         {"rcx", "rdi"},    "StringExt",
   {{DATA_BASE, {0x43}}}},
};

// ============================================================================
// Atomic: XCHG, CMPXCHG, XADD (memory variants)
// ============================================================================
static const std::vector<SemTC> kX64Atomic = {
  {"xchg_reg",      "xchg rax, rbx",                              {{"rax", 1}, {"rbx", 2}},                     {"rax", "rbx"},    "Atomic", {}},
  {"cmpxchg_eq",    "mov rax, 42; mov rcx, 99; cmpxchg [rsi], rcx",
                                                                   {{"rsi", DATA_BASE}},                        {"rax"},           "Atomic",
   {{DATA_BASE, packU64(42)}}},
  {"cmpxchg_neq",   "mov rax, 10; mov rcx, 99; cmpxchg [rsi], rcx",
                                                                   {{"rsi", DATA_BASE}},                        {"rax"},           "Atomic",
   {{DATA_BASE, packU64(42)}}},
  {"xadd_reg",      "xadd [rsi], rax",                             {{"rsi", DATA_BASE}, {"rax", 10}},            {"rax"},           "Atomic",
   {{DATA_BASE, packU64(5)}}},
};
// clang-format on

// ============================================================================
// Test suite instantiations
// ============================================================================
INSTANTIATE_TEST_SUITE_P(Core, X64Semantic, ::testing::ValuesIn(kX64Core), semTCName);
INSTANTIATE_TEST_SUITE_P(Ext, X64Semantic, ::testing::ValuesIn(kX64Ext), semTCName);
INSTANTIATE_TEST_SUITE_P(Shift, X64Semantic, ::testing::ValuesIn(kX64Shift), semTCName);
INSTANTIATE_TEST_SUITE_P(Control, X64Semantic, ::testing::ValuesIn(kX64Control), semTCName);
INSTANTIATE_TEST_SUITE_P(MulDiv, X64Semantic, ::testing::ValuesIn(kX64MulDiv), semTCName);
INSTANTIATE_TEST_SUITE_P(Rotate, X64Semantic, ::testing::ValuesIn(kX64Rotate), semTCName);
INSTANTIATE_TEST_SUITE_P(BMI, X64Semantic, ::testing::ValuesIn(kX64BMI), semTCName);
INSTANTIATE_TEST_SUITE_P(SetccCmov, X64Semantic, ::testing::ValuesIn(kX64SetccCmov), semTCName);
INSTANTIATE_TEST_SUITE_P(CoreExtra, X64Semantic, ::testing::ValuesIn(kX64CoreExtra), semTCName);
INSTANTIATE_TEST_SUITE_P(Op32, X64Semantic, ::testing::ValuesIn(kX64Op32), semTCName);
INSTANTIATE_TEST_SUITE_P(Op16, X64Semantic, ::testing::ValuesIn(kX64Op16), semTCName);
INSTANTIATE_TEST_SUITE_P(ControlExt, X64Semantic, ::testing::ValuesIn(kX64ControlExt), semTCName);
INSTANTIATE_TEST_SUITE_P(Mem, X64Semantic, ::testing::ValuesIn(kX64Mem), semTCName);
INSTANTIATE_TEST_SUITE_P(Flags, X64Semantic, ::testing::ValuesIn(kX64Flags), semTCName);
INSTANTIATE_TEST_SUITE_P(String, X64Semantic, ::testing::ValuesIn(kX64String), semTCName);
INSTANTIATE_TEST_SUITE_P(StringExt, X64Semantic, ::testing::ValuesIn(kX64StringExt), semTCName);
INSTANTIATE_TEST_SUITE_P(Atomic, X64Semantic, ::testing::ValuesIn(kX64Atomic), semTCName);
