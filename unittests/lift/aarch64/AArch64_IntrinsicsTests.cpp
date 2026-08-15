#include "NeverDLiftFixture.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/ir/intrinsics/Intrinsics.h"

#include "llvm/Support/raw_ostream.h"

class AArch64_Intrinsics : public NeverDLiftTest {
protected:
    std::string readDecompiledFile(const std::string& fname) {
        auto f = tmpFile(fname);
        std::ifstream ifs(f);
        std::ostringstream ss;
        ss << ifs.rdbuf();
        return ss.str();
    }
};

static fs::path obj(const char* name) {
    return fs::path(TEST_OBJ_DIR) / name;
}

static neverd::HighFunc fixedConvertHighFunc(const char* name,
                                             neverd::Intrinsic iid,
                                             uint16_t inputBytes,
                                             uint16_t outputBytes,
                                             uint64_t fbits) {
    using namespace neverd;
    HighFunc func;
    func.Name = name;
    func.ReturnType = NdType::makeInt(outputBytes, false);
    func.Params.push_back({"arg0", NdType::makeInt(inputBytes, false)});

    MedVar input;
    input.Kind = MedVar::Param;
    input.Id = 0;
    input.Size = inputBytes;
    input.TheArch = Arch::AArch64;

    auto call = HighExpr::makeCall(
        "", 0, {HighExpr::makeVar(input), HighExpr::makeConst(fbits, 4)});
    call->IntrinsicId = iid;
    call->Type = func.ReturnType;

    HighStmt ret;
    ret.Kind = StmtKind::Return;
    ret.RetVal = std::move(call);
    func.Body.push_back(std::move(ret));
    return func;
}

TEST(AArch64_HighCIntrinsics, FixedFP16ConversionsUseNeverCBuiltins) {
    using namespace neverd;
    std::vector<HighFunc> funcs;
    funcs.push_back(fixedConvertHighFunc("scvtf_w", Intrinsic::A64_ScvtfFixed,
                                         4, 2, 16));
    funcs.push_back(fixedConvertHighFunc("ucvtf_x", Intrinsic::A64_UcvtfFixed,
                                         8, 2, 64));
    funcs.push_back(fixedConvertHighFunc("fcvtzs_w", Intrinsic::A64_FcvtzsFixed,
                                         2, 4, 16));
    funcs.push_back(fixedConvertHighFunc("fcvtzu_x", Intrinsic::A64_FcvtzuFixed,
                                         2, 8, 64));

    std::string c;
    llvm::raw_string_ostream os(c);
    CEmitterOptions opts;
    opts.TheArch = Arch::AArch64;
    ASSERT_TRUE(HighCEmitter().emit(funcs, os, opts));
    os.flush();

    EXPECT_NE(c.find("__neverd_a64_scvtf_fixed(arg0, 16, 0)"),
              std::string::npos) << c;
    EXPECT_NE(c.find("__neverd_a64_ucvtf_fixed(arg0, 64, 1)"),
              std::string::npos) << c;
    EXPECT_NE(c.find("__neverd_a64_fcvtzs_fixed(arg0, 16, 0)"),
              std::string::npos) << c;
    EXPECT_NE(c.find("__neverd_a64_fcvtzu_fixed(arg0, 64, 1)"),
              std::string::npos) << c;
}

TEST(AArch64_HighCIntrinsics, FPSRUsesClangSystemRegisterBuiltins) {
    using namespace neverd;

    HighFunc read;
    read.Name = "read_fpsr";
    read.ReturnType = NdType::makeInt(8, false);
    auto get = HighExpr::makeCall(intrinsicCName(Intrinsic::A64_GetFPSR), 0,
                                  {});
    get->IntrinsicId = Intrinsic::A64_GetFPSR;
    get->Type = read.ReturnType;
    HighStmt readRet;
    readRet.Kind = StmtKind::Return;
    readRet.RetVal = std::move(get);
    read.Body.push_back(std::move(readRet));

    HighFunc write;
    write.Name = "write_fpsr";
    write.ReturnType = NdType::makeVoid();
    write.Params.push_back({"arg0", NdType::makeInt(8, false)});
    MedVar input;
    input.Kind = MedVar::Param;
    input.Id = 0;
    input.Size = 8;
    input.TheArch = Arch::AArch64;
    auto set = HighExpr::makeCall(intrinsicCName(Intrinsic::A64_SetFPSR), 0,
                                  {HighExpr::makeVar(input)});
    set->IntrinsicId = Intrinsic::A64_SetFPSR;
    HighStmt setStmt;
    setStmt.Kind = StmtKind::Call;
    setStmt.CallExpr = std::move(set);
    write.Body.push_back(std::move(setStmt));

    std::string c;
    llvm::raw_string_ostream os(c);
    CEmitterOptions opts;
    opts.TheArch = Arch::AArch64;
    ASSERT_TRUE(HighCEmitter().emit({read, write}, os, opts));
    os.flush();

    EXPECT_NE(c.find("__builtin_arm_rsr64(\"FPSR\")"), std::string::npos)
        << c;
    EXPECT_NE(c.find("__builtin_arm_wsr64(\"FPSR\", arg0)"),
              std::string::npos)
        << c;
}

TEST_F(AArch64_Intrinsics, AllStages) {
    verifyAllModesSucceed(obj("test_intrinsics_a64.o"));
}

TEST_F(AArch64_Intrinsics, OnlyBrkIsUnreachable) {
    auto r = liftToLLVMIR(obj("test_intrinsics_a64.o"));
    ASSERT_EQ(r.exitCode, 0) << "LLVM IR lift failed: " << r.err;

    auto functionStart = r.out.find("@test_brk_intrinsic");
    ASSERT_NE(functionStart, std::string::npos) << r.out.substr(0, 3000);
    functionStart = r.out.rfind("define ", functionStart);
    ASSERT_NE(functionStart, std::string::npos);
    auto functionEnd = r.out.find("\n}", functionStart);
    ASSERT_NE(functionEnd, std::string::npos);
    functionEnd += 2;

    const std::string brkFunction =
        r.out.substr(functionStart, functionEnd - functionStart);
    EXPECT_NE(brkFunction.find("call void @llvm.trap()"), std::string::npos)
        << brkFunction;
    EXPECT_NE(brkFunction.find("unreachable"), std::string::npos)
        << brkFunction;

    std::string otherFunctions = r.out;
    otherFunctions.erase(functionStart, functionEnd - functionStart);
    EXPECT_EQ(otherFunctions.find("unreachable"), std::string::npos)
        << "Only architectural traps may be unreachable:\n"
        << otherFunctions.substr(0, 3000);
}

TEST_F(AArch64_Intrinsics, LLVM_NoNdStubs) {
    auto r = liftToLLVMIR(obj("test_intrinsics_a64.o"));
    ASSERT_EQ(r.exitCode, 0) << "LLVM IR lift failed: " << r.err;
    EXPECT_TRUE(r.out.find("__nd_") == std::string::npos)
        << "Found __nd_ extern stub in LLVM IR:\n"
        << r.out.substr(0, 3000);
}

TEST_F(AArch64_Intrinsics, Decompile_NoNdStubs) {
    auto r = decompileToHighC(obj("test_intrinsics_a64.o"));
    ASSERT_EQ(r.exitCode, 0) << "Decompile failed: " << r.err;
    auto cfile = tmpFile("decompiled_high.c");
    auto content = std::string{};
    { std::ifstream ifs(cfile); std::ostringstream ss; ss << ifs.rdbuf(); content = ss.str(); }
    EXPECT_FALSE(content.empty()) << "Decompiled C is empty";
    EXPECT_TRUE(content.find("__nd_") == std::string::npos)
        << "Found __nd_ in decompiled C:\n"
        << content.substr(0, 3000);
}

TEST_F(AArch64_Intrinsics, Decompile_NoMsvcAsm) {
    auto r = decompileToHighC(obj("test_intrinsics_a64.o"));
    ASSERT_EQ(r.exitCode, 0) << "Decompile failed: " << r.err;
    auto cfile = tmpFile("decompiled_high.c");
    auto content = std::string{};
    { std::ifstream ifs(cfile); std::ostringstream ss; ss << ifs.rdbuf(); content = ss.str(); }
    EXPECT_TRUE(content.find("__asm {") == std::string::npos)
        << "Found MSVC __asm{} in AArch64 decompile — should use ACLE or GNU __asm__ volatile:\n"
        << content.substr(0, 3000);
}

TEST_F(AArch64_Intrinsics, LlvmC_NoNdStubs) {
    auto r = decompileToC(obj("test_intrinsics_a64.o"));
    ASSERT_EQ(r.exitCode, 0) << "LLVM C decompile failed: " << r.err;
    auto cfile = tmpFile("decompiled.c");
    auto content = std::string{};
    { std::ifstream ifs(cfile); std::ostringstream ss; ss << ifs.rdbuf(); content = ss.str(); }
    EXPECT_FALSE(content.empty()) << "LLVM C decompiled output is empty";
    EXPECT_TRUE(content.find("__nd_") == std::string::npos)
        << "Found __nd_ in LLVM C decompile:\n"
        << content.substr(0, 3000);
}

TEST_F(AArch64_Intrinsics, LlvmC_NoMsvcAsm) {
    auto r = decompileToC(obj("test_intrinsics_a64.o"));
    ASSERT_EQ(r.exitCode, 0) << "LLVM C decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled.c");
    EXPECT_TRUE(content.find("__asm {") == std::string::npos)
        << "Found MSVC __asm{} in AArch64 LLVM C decompile — should use GNU __asm__ volatile:\n"
        << content.substr(0, 3000);
}

TEST_F(AArch64_Intrinsics, Decompile_DmbUsesACLE) {
    auto r = decompileToHighC(obj("test_intrinsics_a64.o"));
    ASSERT_EQ(r.exitCode, 0) << "Decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled_high.c");
    EXPECT_TRUE(content.find("__dmb(") != std::string::npos)
        << "Expected __dmb() ACLE intrinsic in AArch64 decompile:\n"
        << content.substr(0, 3000);
}

TEST_F(AArch64_Intrinsics, LlvmC_BarriersUseACLE) {
    auto r = decompileToC(obj("test_intrinsics_a64.o"));
    ASSERT_EQ(r.exitCode, 0) << "LLVM C decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled.c");
    EXPECT_TRUE(content.find("__dmb(") != std::string::npos)
        << "Expected __dmb() in LLVM C:\n" << content.substr(0, 3000);
    EXPECT_TRUE(content.find("__dsb(") != std::string::npos)
        << "Expected __dsb() in LLVM C:\n" << content.substr(0, 3000);
    EXPECT_TRUE(content.find("__isb(") != std::string::npos)
        << "Expected __isb() in LLVM C:\n" << content.substr(0, 3000);
}

TEST_F(AArch64_Intrinsics, LlvmC_HintsUseACLE) {
    auto r = decompileToC(obj("test_intrinsics_a64.o"));
    ASSERT_EQ(r.exitCode, 0) << "LLVM C decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled.c");
    EXPECT_TRUE(content.find("__yield()") != std::string::npos)
        << "Expected __yield() in LLVM C:\n" << content.substr(0, 3000);
    EXPECT_TRUE(content.find("__clrex()") != std::string::npos)
        << "Expected __clrex() in LLVM C:\n" << content.substr(0, 3000);
}

TEST_F(AArch64_Intrinsics, LlvmC_CleanOutput) {
    auto r = decompileToC(obj("test_intrinsics_a64.o"));
    ASSERT_EQ(r.exitCode, 0) << "LLVM C decompile failed: " << r.err;
    auto content = readDecompiledFile("decompiled.c");
    size_t frame_count = 0;
    size_t pos = 0;
    while ((pos = content.find("frame", pos)) != std::string::npos) {
        ++frame_count; pos += 5;
    }
    EXPECT_EQ(frame_count, 0u)
        << "Found frame variables in clean ARM intrinsic functions:\n"
        << content.substr(0, 3000);
}
