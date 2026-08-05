//===- PatchFullRepro.cpp - fast standalone obfuscation repro -------------===//
//
// NeverD Decompiler — throwaway debugging harness (NOT a unit test).
//
// Compiles in seconds (one tiny TU) so an obfuscation/codegen bug found by the
// 22k-line PatchFullSubstRTTests.cpp can be isolated without the ~33min rebuild
// of that translation unit.  Builds the `farloop` sample IR, applies a
// configurable subset of the L1 passes (argv), compiles through the exact
// rewrite backend, and emulates under Unicorn with a per-instruction trace ring
// so the faulting PC and the path that reached it are visible.
//
//   ./PatchFullRepro <isa> <passes>
//     isa    : aarch64-elf | aarch64-coff | aarch64-macho | x64-elf | ...
//     passes : comma list of subst,constenc,opaque,bogus,flatten,indirect,
//              indcall,mba,indgv,launder,constpool,bitmask  | all | none
//
//===----------------------------------------------------------------------===//

#include "neverd/backend/codegen/CodeGen.h"
#include "neverd/pipeline/Pipeline.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <unicorn/unicorn.h>

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/MC/BinaryRewrite.h"
#include "llvm/Support/TargetSelect.h"

using namespace neverd;

namespace {

constexpr uint64_t CODE_VA = 0x400000;
constexpr uint64_t CODE_MAX = 0x4000000; // 64MB code ceiling
constexpr uint64_t DATA_VA = 0x8000000;
constexpr uint64_t RODATA_VA = 0x9000000;
constexpr uint64_t RO_SLOT = 0x8000;
constexpr uint64_t MISC_VA = 0xA000000;
constexpr uint64_t STK_BASE = 0xC000000;
constexpr uint64_t STK_SIZE = 0x2000000; // 32MB stack
constexpr uint64_t RET_BASE = 0x3F0000;
constexpr uint64_t RET_SIZE = 0x1000;
constexpr uint64_t RET_ADDR = RET_BASE;

struct EmuISA {
  const char *Label;
  Arch A;
  BinaryFormat F;
  const char *Triple;
  uc_arch UA;
  uc_mode UM;
};

const EmuISA kISAs[] = {
    {"aarch64-elf", Arch::AArch64, BinaryFormat::ELF, "aarch64-unknown-linux-elf",
     UC_ARCH_ARM64, static_cast<uc_mode>(0)},
    {"aarch64-coff", Arch::AArch64, BinaryFormat::COFF, "aarch64-pc-windows-msvc",
     UC_ARCH_ARM64, static_cast<uc_mode>(0)},
    {"aarch64-macho", Arch::AArch64, BinaryFormat::MachO, "arm64-apple-macos14.0",
     UC_ARCH_ARM64, static_cast<uc_mode>(0)},
    {"x64-elf", Arch::X64, BinaryFormat::ELF, "x86_64-unknown-linux-elf",
     UC_ARCH_X86, UC_MODE_64},
    {"arm32-elf", Arch::ARM, BinaryFormat::ELF, "arm-unknown-linux-gnueabihf",
     UC_ARCH_ARM, UC_MODE_ARM},
};

void ensureTargets() {
  static bool Done = false;
  if (Done)
    return;
  llvm::InitializeAllTargetInfos();
  llvm::InitializeAllTargets();
  llvm::InitializeAllTargetMCs();
  llvm::InitializeAllAsmParsers();
  llvm::InitializeAllAsmPrinters();
  Done = true;
}

const llvm::mc_rewrite::RewriteSection *
findText(const llvm::mc_rewrite::RewriteResult &R) {
  for (auto &S : R.Sections) {
    llvm::StringRef N(S.Name);
    if (N.contains("text") || N.contains("TEXT"))
      return &S;
  }
  return nullptr;
}

uint64_t findSym(const llvm::mc_rewrite::RewriteResult &R, const char *Name,
                 uint64_t Default) {
  if (R.SymbolAddrs.count(Name))
    return R.SymbolAddrs.at(Name);
  std::string Prefixed = std::string("_") + Name;
  if (R.SymbolAddrs.count(Prefixed))
    return R.SymbolAddrs.at(Prefixed);
  return Default;
}

llvm::mc_rewrite::RewriteResult compileRW(llvm::Module &M, const EmuISA &E) {
  ensureTargets();
  llvm::mc_rewrite::RewriteOptions Opts;
  Opts.Model.TextVA = CODE_VA;
  auto RoVAByName = std::make_shared<std::map<std::string, uint64_t>>();
  auto RwVAByName = std::make_shared<std::map<std::string, uint64_t>>();
  Opts.Model.getSectionVA = [RoVAByName,
                             RwVAByName](llvm::StringRef Name) -> uint64_t {
    if (Name.contains("text") || Name.contains("TEXT"))
      return CODE_VA;
    if (Name.contains("pdata") || Name.contains("xdata"))
      return MISC_VA;
    bool ReadOnly = Name.contains("rodata") || Name.contains("rdata") ||
                    Name.contains("rel.ro") || Name.contains("const") ||
                    Name.contains("literal") || Name.contains("cst") ||
                    Name.contains("cstring");
    if (ReadOnly) {
      auto It = RoVAByName->find(Name.str());
      if (It != RoVAByName->end())
        return It->second;
      uint64_t VA = RODATA_VA + RoVAByName->size() * RO_SLOT;
      (*RoVAByName)[Name.str()] = VA;
      return VA;
    }
    if (Name.contains("data") || Name.contains("DATA") || Name.contains("bss") ||
        Name.contains("BSS") || Name.contains("common") ||
        Name.contains("COMMON")) {
      auto It = RwVAByName->find(Name.str());
      if (It != RwVAByName->end())
        return It->second;
      uint64_t VA = DATA_VA + RwVAByName->size() * RO_SLOT;
      (*RwVAByName)[Name.str()] = VA;
      return VA;
    }
    return MISC_VA;
  };
  Opts.Model.resolve = [](llvm::StringRef,
                          uint32_t) -> std::optional<uint64_t> {
    return std::nullopt;
  };
  Codegen CG;
  return CG.compileForRewrite(M, E.A, Opts, E.F);
}

std::unique_ptr<llvm::Module> buildFarLoopIR(llvm::LLVMContext &Ctx,
                                             const char *Triple) {
  auto M = std::make_unique<llvm::Module>("substtest", Ctx);
  M->setTargetTriple(llvm::Triple(Triple));
  auto *I32 = llvm::Type::getInt32Ty(Ctx);
  auto *FTy = llvm::FunctionType::get(I32, {I32, I32}, false);
  auto *F = llvm::Function::Create(FTy, llvm::Function::ExternalLinkage,
                                   "farloop", M.get());
  auto *Entry = llvm::BasicBlock::Create(Ctx, "entry", F);
  auto *Header = llvm::BasicBlock::Create(Ctx, "header", F);
  auto *Body = llvm::BasicBlock::Create(Ctx, "body", F);
  auto *Exit = llvm::BasicBlock::Create(Ctx, "exit", F);
  auto *A = F->getArg(0), *Bb = F->getArg(1);

  llvm::IRBuilder<> B(Entry);
  auto *N = B.CreateAdd(B.CreateAnd(Bb, B.getInt32(3)), B.getInt32(1));
  B.CreateBr(Header);

  llvm::IRBuilder<> HB(Header);
  auto *I = HB.CreatePHI(I32, 2, "i");
  auto *Acc = HB.CreatePHI(I32, 2, "acc");
  I->addIncoming(HB.getInt32(0), Entry);
  Acc->addIncoming(A, Entry);
  auto *Cond = HB.CreateICmpSLT(I, N);
  HB.CreateCondBr(Cond, Body, Exit);

  llvm::IRBuilder<> BB(Body);
  llvm::Value *acc = Acc;
  for (int k = 0; k < 800; ++k) {
    uint32_t K = 0x9E3779B9u * (uint32_t)(k + 1) + 0x7F4A7C15u;
    switch (k % 5) {
    case 0:
      acc = BB.CreateAdd(acc, BB.getInt32(K));
      break;
    case 1:
      acc = BB.CreateSub(acc, BB.getInt32(K));
      break;
    case 2:
      acc = BB.CreateXor(acc, BB.getInt32(K));
      break;
    case 3:
      acc = BB.CreateOr(acc, BB.getInt32(K | 0x101u));
      break;
    default:
      acc = BB.CreateAnd(acc, BB.getInt32(K | 0xFFFFu));
      break;
    }
  }
  auto *INext = BB.CreateAdd(I, BB.getInt32(1));
  BB.CreateBr(Header);
  I->addIncoming(INext, Body);
  Acc->addIncoming(acc, Body);

  llvm::IRBuilder<> EB(Exit);
  EB.CreateRet(EB.CreateXor(Acc, EB.CreateAdd(A, Bb)));
  return M;
}

// --- per-instruction trace ring -------------------------------------------
uint64_t g_ring[512];
size_t g_pos = 0;
uint64_t g_count = 0;
void hookCode(uc_engine *, uint64_t addr, uint32_t, void *) {
  g_ring[g_pos] = addr;
  g_pos = (g_pos + 1) % 512;
  ++g_count;
}

bool emulateCall(const llvm::mc_rewrite::RewriteResult &RR, const EmuISA &E,
                 uint64_t Entry, const std::vector<uint32_t> &Args,
                 uint32_t &OutRet, std::string &Err) {
  g_pos = 0;
  g_count = 0;
  uc_engine *uc = nullptr;
  uc_err e = uc_open(E.UA, E.UM, &uc);
  if (e != UC_ERR_OK) {
    Err = std::string("uc_open: ") + uc_strerror(e);
    return false;
  }
  struct Closer {
    uc_engine *U;
    ~Closer() {
      if (U)
        uc_close(U);
    }
  } Guard{uc};

  if (E.A == Arch::ARM)
    uc_ctl_set_cpu_model(uc, UC_CPU_ARM_MAX);

  uc_mem_map(uc, CODE_VA, CODE_MAX, UC_PROT_ALL);
  uc_mem_map(uc, DATA_VA, 0x1000000, UC_PROT_ALL);
  uc_mem_map(uc, RODATA_VA, 0x1000000, UC_PROT_ALL);
  uc_mem_map(uc, MISC_VA, 0x1000000, UC_PROT_ALL);
  uc_mem_map(uc, STK_BASE, STK_SIZE, UC_PROT_ALL);
  uc_mem_map(uc, RET_BASE, RET_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uc_hook H;
  uc_hook_add(uc, &H, UC_HOOK_CODE, (void *)hookCode, nullptr, 1, 0);

  const uint64_t SP = STK_BASE + STK_SIZE - 0x200;
  const uint64_t Ret = RET_ADDR;

  switch (E.A) {
  case Arch::X64: {
    static const int AR[] = {UC_X86_REG_RDI, UC_X86_REG_RSI, UC_X86_REG_RDX,
                             UC_X86_REG_RCX, UC_X86_REG_R8, UC_X86_REG_R9};
    for (size_t i = 0; i < Args.size() && i < 6; ++i) {
      uint64_t V = Args[i];
      uc_reg_write(uc, AR[i], &V);
    }
    uc_mem_write(uc, SP, &Ret, 8);
    uint64_t S = SP;
    uc_reg_write(uc, UC_X86_REG_RSP, &S);
    break;
  }
  case Arch::AArch64: {
    static const int AR[] = {UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2,
                             UC_ARM64_REG_X3, UC_ARM64_REG_X4, UC_ARM64_REG_X5,
                             UC_ARM64_REG_X6, UC_ARM64_REG_X7};
    for (size_t i = 0; i < Args.size() && i < 8; ++i) {
      uint64_t V = Args[i];
      uc_reg_write(uc, AR[i], &V);
    }
    uint64_t R = Ret;
    uc_reg_write(uc, UC_ARM64_REG_LR, &R);
    uint64_t S = SP;
    uc_reg_write(uc, UC_ARM64_REG_SP, &S);
    break;
  }
  case Arch::ARM: {
    uint32_t CPACR = 0x00F00000;
    uc_reg_write(uc, UC_ARM_REG_C1_C0_2, &CPACR);
    uint32_t FPEXC = 0x40000000;
    uc_reg_write(uc, UC_ARM_REG_FPEXC, &FPEXC);
    static const int AR[] = {UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
                             UC_ARM_REG_R3};
    for (size_t i = 0; i < Args.size() && i < 4; ++i) {
      uint32_t V = Args[i];
      uc_reg_write(uc, AR[i], &V);
    }
    uint32_t R = static_cast<uint32_t>(Ret);
    uc_reg_write(uc, UC_ARM_REG_LR, &R);
    uint32_t S = static_cast<uint32_t>(SP);
    uc_reg_write(uc, UC_ARM_REG_SP, &S);
    break;
  }
  default:
    Err = "unsupported arch";
    return false;
  }

  int PCReg = (E.A == Arch::X64)       ? int(UC_X86_REG_RIP)
              : (E.A == Arch::X86)     ? int(UC_X86_REG_EIP)
              : (E.A == Arch::AArch64) ? int(UC_ARM64_REG_PC)
                                       : int(UC_ARM_REG_PC);
  constexpr uint64_t kInsnBudget = 200000000;
  e = uc_emu_start(uc, Entry, Ret, 0, kInsnBudget);
  if (e != UC_ERR_OK) {
    uint64_t PC = 0;
    uc_reg_read(uc, PCReg, &PC);
    uint64_t SP = 0;
    if (E.A == Arch::AArch64)
      uc_reg_read(uc, UC_ARM64_REG_SP, &SP);
    char Buf[128];
    snprintf(Buf, sizeof(Buf), " (pc=0x%llx sp=0x%llx insns=%llu)",
             (unsigned long long)PC, (unsigned long long)SP,
             (unsigned long long)g_count);
    Err = std::string("uc_emu_start: ") + uc_strerror(e) + Buf;
    return false;
  }
  uint64_t EndPC = 0;
  uc_reg_read(uc, PCReg, &EndPC);
  if ((EndPC & ~uint64_t(1)) != (Ret & ~uint64_t(1))) {
    char Buf[96];
    snprintf(Buf, sizeof(Buf), "did not return pc=0x%llx insns=%llu",
             (unsigned long long)EndPC, (unsigned long long)g_count);
    Err = Buf;
    return false;
  }
  int RetReg = (E.A == Arch::X64)       ? int(UC_X86_REG_RAX)
               : (E.A == Arch::X86)     ? int(UC_X86_REG_EAX)
               : (E.A == Arch::AArch64) ? int(UC_ARM64_REG_X0)
                                        : int(UC_ARM_REG_R0);
  uint64_t RV = 0;
  uc_reg_read(uc, RetReg, &RV);
  OutRet = static_cast<uint32_t>(RV);
  return true;
}

void dumpTrace(const llvm::mc_rewrite::RewriteResult &RR) {
  const auto *T = findText(RR);
  uint64_t TVA = T ? T->VA : CODE_VA;
  uint64_t TEnd = T ? T->VA + T->Bytes.size() : 0;
  fprintf(stderr, ".text VA=0x%llx size=%zu end=0x%llx\n",
          (unsigned long long)TVA, T ? T->Bytes.size() : 0,
          (unsigned long long)TEnd);
  size_t n = g_count < 512 ? (size_t)g_count : 512;
  fprintf(stderr, "last %zu PCs (oldest->newest):\n", n);
  for (size_t i = 0; i < n; ++i) {
    size_t idx = (g_pos + 512 - n + i) % 512;
    uint64_t pc = g_ring[idx];
    const char *where = (pc >= TVA && pc < TEnd) ? "text"
                        : (pc == RET_ADDR)       ? "RET"
                                                 : "??";
    fprintf(stderr, "  0x%llx [%s+0x%llx]\n", (unsigned long long)pc, where,
            (unsigned long long)(pc >= TVA ? pc - TVA : pc));
  }
}

void setCfg(Pipeline::ObfuscationConfig &C, const std::string &name) {
  if (name == "subst")
    C.InstSubstitution = true;
  else if (name == "constenc")
    C.ConstantEncryption = true;
  else if (name == "opaque")
    C.OpaquePredicate = true;
  else if (name == "bogus")
    C.BogusControlFlow = true;
  else if (name == "flatten")
    C.ControlFlowFlattening = true;
  else if (name == "indirect")
    C.IndirectBranch = true;
  else if (name == "indcall")
    C.IndirectCall = true;
  else if (name == "mba")
    C.MBA = true;
  else if (name == "indgv")
    C.IndirectGlobal = true;
  else if (name == "launder")
    C.ValueLaunder = true;
  else if (name == "constpool")
    C.ConstantPooling = true;
  else if (name == "bitmask")
    C.BitMasking = true;
  else if (name == "all") {
    C.InstSubstitution = C.ConstantEncryption = C.OpaquePredicate =
        C.BogusControlFlow = C.ControlFlowFlattening = C.IndirectBranch =
            C.IndirectCall = C.MBA = C.IndirectGlobal = C.ValueLaunder =
                C.ConstantPooling = C.BitMasking = true;
  } else
    fprintf(stderr, "unknown pass: %s\n", name.c_str());
}

} // namespace

int main(int argc, char **argv) {
  std::string isa = argc > 1 ? argv[1] : "aarch64-elf";
  std::string passes = argc > 2 ? argv[2] : "all";

  const EmuISA *E = nullptr;
  for (auto &c : kISAs)
    if (isa == c.Label)
      E = &c;
  if (!E) {
    fprintf(stderr, "unknown isa %s\n", isa.c_str());
    return 2;
  }

  Pipeline::ObfuscationConfig Cfg;
  if (passes != "none") {
    std::string cur;
    std::string s = passes + ",";
    for (char ch : s) {
      if (ch == ',') {
        if (!cur.empty())
          setCfg(Cfg, cur);
        cur.clear();
      } else
        cur += ch;
    }
  }

  ensureTargets();
  llvm::LLVMContext Ctx;

  // Baseline.
  auto MB = buildFarLoopIR(Ctx, E->Triple);
  auto RRb = compileRW(*MB, *E);
  uint64_t EntryB = findSym(RRb, "farloop", findText(RRb)->VA);

  // Obfuscated.
  auto MS = buildFarLoopIR(Ctx, E->Triple);
  auto Counts = Pipeline::runObfuscationPasses(*MS, Cfg);
  fprintf(stderr, "[%s passes=%s] applied total=%u\n", isa.c_str(),
          passes.c_str(), Counts.total());
  if (llvm::verifyModule(*MS, &llvm::errs())) {
    fprintf(stderr, "INVALID IR after passes\n");
    return 3;
  }
  auto RRs = compileRW(*MS, *E);
  const auto *Ts = findText(RRs);
  fprintf(stderr, "subst .text size=%zu\n", Ts ? Ts->Bytes.size() : 0);
  uint64_t EntryS = findSym(RRs, "farloop", findText(RRs)->VA);
  if (Ts) {
    uint64_t off = EntryS - Ts->VA;
    fprintf(stderr, "subst prologue @0x%llx (entry off=0x%llx):",
            (unsigned long long)EntryS, (unsigned long long)off);
    for (size_t i = 0; i < 48 && off + i < Ts->Bytes.size(); ++i)
      fprintf(stderr, " %02x", Ts->Bytes[off + i]);
    fprintf(stderr, "\n");
  }

  const std::vector<std::vector<uint32_t>> kPairs = {
      {0, 0},        {1, 0},          {0, 1},
      {1, 1},        {17, 25},        {0xFFFFFFFFu, 1},
      {123456u, 654321u}, {0x7FFFFFFFu, 0x7FFFFFFFu}};

  int rc = 0;
  for (auto &In : kPairs) {
    uint32_t Rb = 0, Rs = 0;
    std::string Eb, Es;
    bool okB = emulateCall(RRb, *E, EntryB, In, Rb, Eb);
    bool okS = emulateCall(RRs, *E, EntryS, In, Rs, Es);
    if (!okB) {
      fprintf(stderr, "BASELINE FAIL in=(%u,%u): %s\n", In[0], In[1],
              Eb.c_str());
      dumpTrace(RRb);
      rc = 1;
      break;
    }
    if (!okS) {
      fprintf(stderr, "SUBST FAIL in=(%u,%u): %s\n", In[0], In[1], Es.c_str());
      dumpTrace(RRs);
      rc = 1;
      break;
    }
    if (Rb != Rs) {
      fprintf(stderr, "MISMATCH in=(%u,%u) orig=%u subst=%u\n", In[0], In[1],
              Rb, Rs);
      rc = 1;
      break;
    }
    fprintf(stderr, "ok in=(%u,%u) ret=%u\n", In[0], In[1], Rb);
  }
  if (rc == 0)
    fprintf(stderr, "ALL OK\n");
  return rc;
}
