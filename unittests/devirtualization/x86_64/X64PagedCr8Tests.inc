//===- X64PagedCr8Tests.inc - Paged CR8 commit and fault proofs -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compile the emitted instruction bodies and schedulers against independently
/// supplied fetch, exception-entry and instruction-debug outcomes.
//===----------------------------------------------------------------------===//

// The body and entry validation below are the ones emitted by the SDK. The
// boundary supplies staged state and explicit outcomes. Its progress reads
// count calls as well as exposing the receipt, so double accounting is visible.
const char PagedCr8Boundary[] = R"C(
typedef struct {
    uint64_t progress, before_status, before_progress, after_status, fault_status;
    uint64_t saved_pc, vector, shadow;
    unsigned bodies, faults, receipt_reads, observed_byte, bad;
} paged_outcomes;

static uint64_t fault_access(void *context, uint64_t address, uint64_t size,
                             uint64_t kind) {
    paged_outcomes *m = context;
    ++m->faults;
    m->bad |= kind != 3 || size != 0 || address != 13;
    m->vector = address;
    return 1;
}
#if DEBUG_CR8
#define ACCESS neverd_x64_paged_debug_access_v1
#define VALIDATE neverd_x64_paged_debug_validate_v2
#define STEP neverd_x64_paged_debug_step_v2
#define SHADOW_ARG , uint64_t shadow
#else
#define ACCESS neverd_x64_paged_access_v1
#define VALIDATE neverd_x64_paged_validate_v1
#define STEP neverd_x64_paged_step_v1
#define SHADOW_ARG
#endif

uint64_t ACCESS(void *context, uint64_t address, uint64_t size, uint64_t kind) {
    return fault_access(context, address, size, kind);
}
uint64_t VALIDATE(uint8_t *context, uint8_t *raw, uint64_t address,
                  neverd_x64_paged_body_v1 validate, uint64_t paging) {
    paged_outcomes *m = (paged_outcomes *)context;
    m->progress = 0;
    return validate(raw, 0);
}
uint64_t STEP(uint8_t *context, uint8_t *raw, uint64_t address,
              const uint8_t *bytes, uint64_t size, neverd_x64_paged_body_v1 body,
              neverd_x64_paged_body_v1 validate, uint64_t mask, uint64_t paging
              SHADOW_ARG) {
    paged_outcomes *m = (paged_outcomes *)context;
    neverd_x64_state_v6 *state = (neverd_x64_state_v6 *)raw;
#if DEBUG_CR8
    m->shadow = shadow;
#endif
    uint64_t status = VALIDATE(context, raw, address, validate, paging);
    if (status) return status;
    if (m->before_status) {
        m->progress = m->before_progress;
        m->saved_pc = state->PC;
        return m->before_status;
    }
    neverd_x64_state_v6 staged;
    __builtin_memcpy(&staged, state, sizeof(staged));
    const uint8_t *observation = (uint8_t *)&staged + m->observed_byte % sizeof(staged);
    uint8_t prior = *observation;
    ++m->bodies;
    status = body(&staged, m);
    if (status == 17) {
        m->bad |= !m->faults || *observation != prior;
        m->progress = 1;
        m->saved_pc = state->PC;
        return m->fault_status;
    }
    if (status != 0) return status;
    *state = staged;
    m->progress = 5;
    if (m->after_status) {
        m->saved_pc = state->PC;
        if (m->after_status == 7) state->PC = 0x9000;
        return m->after_status;
    }
    return 0;
}
uint64_t neverd_x64_paged_pending_v1(uint8_t *context) { return 0; }
uint64_t neverd_x64_paged_progress_v1(uint8_t *context) {
    paged_outcomes *m = (paged_outcomes *)context;
    ++m->receipt_reads;
    return m->progress;
}
uint64_t neverd_x64_paged_event_v1(uint8_t *context, uint8_t *raw,
                                   const uint8_t *event, uint64_t mask) {
    paged_outcomes *m = (paged_outcomes *)context;
    m->progress = 2;
    ((neverd_x64_state_v6 *)raw)->PC = 0x9000;
    return 7;
}
#define PREPARE() neverd_x64_state_v6 state = seed(); \
    neverd_x64_interruptibility_v1 shadows = {0}; \
    uint64_t budget = 1, poll_budget = 8; \
    producer events = {.tpr = 0x3b}; \
    paged_outcomes memory = {.progress = 5, .fault_status = 7}
#define RUN() neverd_x64_continue_paged_events_v6( \
    &state, (uint8_t *)&memory, &budget, EVENTS)
#define GP_AT(PC) (memory.faults == 1 && memory.vector == 13 && \
    memory.saved_pc == (PC) && memory.progress == 1 && memory.bodies == 1 && \
    memory.receipt_reads == 1 && !memory.bad)
)C";

const char PagedCr8Probes[] = R"C(
int proof_same_value_last_instruction(void) {
    PREPARE();
    return RUN() != 9 || budget || poll_budget != 7 || state.PC != 0x1004 ||
           state.CR8 != 3 || BAD(1,1,0,0x30) || memory.receipt_reads != 1;
}
int proof_legal_write(uint64_t value) {
    PREPARE();
    state.GPR[0] = value & 15;
    return RUN() != 9 || budget || state.PC != 0x1004 || state.CR8 != (value & 15) ||
           BAD(1,1,0,(value & 15) << 4) || memory.receipt_reads != 1;
}
int proof_reserved_write(uint64_t value, unsigned byte) {
    if (value <= 15) return 0;
    PREPARE();
    memory.observed_byte = byte;
    state.GPR[0] = value;
    return RUN() != 7 || budget || !GP_AT(0x1000) || BAD(1,0,0,0x3b);
}
int proof_fault_entry_backing_stop(void) {
    PREPARE();
    state.GPR[0] = 16;
    memory.fault_status = 18;
    return RUN() != 18 || budget || !GP_AT(0x1000) || BAD(1,0,0,0x3b);
}
int proof_unprivileged(unsigned cpl, unsigned read, unsigned byte) {
    if (cpl < 1 || cpl > 3) return 0;
    PREPARE();
    memory.observed_byte = byte;
    state.CS.Selector |= cpl;
    state.CS.Attributes |= cpl << 5;
    state.SS.Selector |= cpl;
    state.SS.Attributes |= cpl << 5;
    state.PC = read ? 0x1004 : 0x1000;
    uint64_t pc = state.PC;
    return RUN() != 7 || budget || !GP_AT(pc) || BAD(1,0,0,0x3b);
}
int proof_conforming_code_keeps_cpl(unsigned byte) {
    PREPARE();
    memory.observed_byte = byte;
    state.PC = 0x1004;
    state.CS.Selector |= 3;
    state.CS.Attributes |= 4;
    state.SS.Selector |= 3;
    state.SS.Attributes |= 3u << 5;
    return RUN() != 7 || budget || !GP_AT(0x1004) || BAD(1,0,0,0x3b);
}
static uint64_t fresh_flags(neverd_x64_state_v6 *s, uint64_t choices) {
    const unsigned bits[] = {11, 7, 6, 4, 2, 0};
    for (unsigned i = 0; i < 6; ++i)
        s->Undefined[i] = choices >> bits[i];
    return (s->RFlags & ~0x8d5ULL) | (choices & 0x8d5ULL);
}
int proof_write_flags(uint64_t choices) {
    PREPARE();
    uint64_t expected = fresh_flags(&state, choices);
    return RUN() != 9 || state.RFlags != expected || BAD(1,1,0,0x30);
}
int proof_read_preserves_tpr(uint64_t choices) {
    PREPARE();
    state.PC = 0x1004;
    uint64_t expected = fresh_flags(&state, choices);
    return RUN() != 9 || budget || state.PC != 0x1008 || state.GPR[3] != 3 ||
           state.RFlags != expected || BAD(1,0,0,0x3b) || memory.receipt_reads != 1;
}
int proof_ordinary_step(void) {
    PREPARE();
    state.PC = 0x100c;
    return RUN() != 9 || budget || state.PC != 0x100d ||
           BAD(1,0,0,0x3b) || memory.receipt_reads != 1;
}
int proof_notification_before_next_poll(void) {
    PREPARE();
    budget = 3;
    state.GPR[1] = 5;
    return RUN() != 9 || budget || poll_budget != 5 || state.PC != 0x100c ||
           state.CR8 != 5 || events.seen_tpr != 0x30 || BAD(3,2,0,0x50) ||
           memory.receipt_reads != 3;
}
int proof_no_replay(void) {
    PREPARE();
    if (RUN() != 9 || BAD(1,1,0,0x30)) return 1;
    budget = 1;
    return RUN() != 9 || budget || state.PC != 0x1008 ||
           BAD(2,1,0,0x30) || memory.receipt_reads != 2;
}
int proof_fetch_fault_precedes_reserved_check(void) {
    PREPARE();
    state.GPR[0] = 16;
    memory.before_status = 7;
    memory.before_progress = 1;
    return RUN() != 7 || budget || memory.bodies || memory.faults ||
           memory.saved_pc != 0x1000 || BAD(1,0,0,0x3b) || memory.receipt_reads != 1;
}
int proof_fetch_backing_and_changed_code(unsigned changed) {
    PREPARE();
    memory.before_status = changed ? 20 : 18;
    return RUN() != memory.before_status || budget != 1 || memory.bodies ||
           memory.faults || BAD(1,0,0,0x3b) || memory.receipt_reads != 1;
}
int proof_preempted_write(void) {
    PREPARE();
    events.ready = 1;
    return RUN() != 7 || budget != 1 || memory.bodies || state.PC != 0x9000 ||
           BAD(1,0,1,0x3b) || memory.receipt_reads != 1;
}
int proof_preflight_and_budget(unsigned invalid) {
    PREPARE();
    if (invalid) state.CR8 = 16;
    else budget = 0;
    return RUN() != (invalid ? 5u : 9u) || poll_budget != 8 || memory.bodies ||
           memory.receipt_reads || BAD(0,0,0,0x3b);
}
#if FUNCTION_CONTEXT
int proof_checked_function_retains_completion(void) {
    PREPARE();
    state.GPR[4] = 0x14008;
#if DEBUG_CR8
    neverd_x64_function_context_v2 function = {0};
    if (!neverd_x64_begin_debug_function_v6(&state, &function)) return 1;
    uint64_t status = neverd_x64_continue_paged_debug_events_function_v6(
#else
    neverd_x64_function_context_v1 function = {0};
    if (!neverd_x64_begin_function_v6(&state, &function)) return 1;
    uint64_t status = neverd_x64_continue_paged_events_function_v6(
#endif
        &state, (uint8_t *)&memory, &budget, EVENTS, &function);
    return status != 9 || budget || state.PC != 0x1004 || function.Phase != 1 ||
           BAD(1,1,0,0x30) || memory.receipt_reads != 1;
}
#endif
#if DEBUG_CR8
int proof_breakpoint_before_write(void) {
    PREPARE();
    memory.before_status = 7;
    memory.before_progress = 1;
    return RUN() != 7 || budget || memory.bodies || memory.saved_pc != 0x1000 ||
           state.CR8 != 3 || BAD(1,0,0,0x3b) || memory.receipt_reads != 1;
}
int proof_debug_stop_after_write(unsigned stop) {
    PREPARE();
    state.RFlags |= 0x100;
    state.GPR[0] = 6;
    memory.after_status = stop == 0 ? 7 : stop == 1 ? 18 : 5;
    return RUN() != memory.after_status || budget || memory.saved_pc != 0x1004 ||
           state.PC != (stop ? 0x1004u : 0x9000u) || state.CR8 != 6 ||
           BAD(1,1,0,0x60) || memory.receipt_reads != 1;
}
int proof_debug_bridge_keeps_ss_shadow(void) {
    PREPARE();
    shadows.SSBlocked = 1;
    return RUN() != 9 || budget || memory.shadow != 1 || shadows.SSBlocked ||
           BAD(1,1,0,0x30) || memory.receipt_reads != 1;
}
#endif
)C";

TEST_F(X64Cr8WriteNotificationTest, PagedCompletionSurvivesPostCommitStops) {
  if (!neverd::test::compilerReadsCurrentIR(NEVERD_TEST_CLANG))
    GTEST_SKIP() << "this Clang cannot read the linked LLVM's IR";
  loadPriority();
  for (bool LLVM : {false, true})
    for (bool Debug : {false, true})
      for (bool FunctionContext : {false, true}) {
        const std::string Name = std::string("paged-cr8-") +
                                 (LLVM ? "llvm-" : "high-") +
                                 (Debug ? "debug-" : "plain-") +
                                 (FunctionContext ? "function" : "region");
        SCOPED_TRACE(Name);
        const auto [Source, Report] = source(
            request(
                (llvm::Twine(
                     FunctionContext
                         ? R"("entries":[4096],"follow_internal_calls":true,"function_context":true)"
                         : R"("instructions":[4096,4100,4104,4108])") +
                 R"(,"paged_memory":true,"scheduled_events":true,"notify_cr8_writes":true,"paged_debug":)" +
                 (Debug ? "true" : "false"))
                    .str()),
            LLVM, 6, /*Paged=*/true);
        auto Manifest = llvm::json::parse(Report);
        ASSERT_TRUE(bool(Manifest));
        const auto *Contract =
            Manifest->getAsObject()->getObject("cr8_write_notification");
        ASSERT_NE(Contract, nullptr);
        const auto *Sites = Contract->getArray("instructions");
        ASSERT_NE(Sites, nullptr);
        ASSERT_EQ(Sites->size(), 2u);
        EXPECT_EQ((*Sites)[0].getAsString(), "0x1000");
        EXPECT_EQ((*Sites)[1].getAsString(), "0x1008");
        provePredicates(true, Name, Source,
                        "\n#define DEBUG_CR8 " + std::to_string(Debug) +
                            "\n#define FUNCTION_CONTEXT " +
                            std::to_string(FunctionContext) + "\n" +
                            Cr8NotificationProducer + PagedCr8Boundary +
                            PagedCr8Probes,
                        (Debug ? 18u : 15u) + unsigned(FunctionContext), 0, 1,
                        /*CloseHelpers=*/true);
      }
}

TEST_F(X64Cr8WriteNotificationTest, PagedWidthPrefixesKeepExtendedRegisters) {
  if (!neverd::test::compilerReadsCurrentIR(NEVERD_TEST_CLANG))
    GTEST_SKIP() << "this Clang cannot read the linked LLVM's IR";
  load({0x66, 0x4d, 0x0f, 0x22, 0xc1,   // MOV CR8,R9 with 66H and REX.W.
        0x66, 0x45, 0x0f, 0x20, 0xc2}); // MOV R10,CR8 with 66H.
  const char Probes[] = R"C(
int proof_width_override_and_rex_b(uint64_t value) {
    PREPARE();
    state.GPR[1] = 11;
    state.GPR[2] = 0x12345678;
    state.GPR[9] = value & 15;
    state.GPR[10] = ~0ULL;
    budget = 2;
    return RUN() != 9 || budget || state.PC != 0x100a || state.CR8 != (value & 15) ||
           state.GPR[1] != 11 || state.GPR[2] != 0x12345678 ||
           state.GPR[9] != (value & 15) || state.GPR[10] != (value & 15) ||
           BAD(2,1,0,(value & 15) << 4) || memory.receipt_reads != 2;
}
int proof_ignored_width_preserves_reserved_bits(uint64_t value, unsigned byte) {
    if (value <= 15) return 0;
    PREPARE();
    memory.observed_byte = byte;
    state.GPR[9] = value;
    return RUN() != 7 || budget || !GP_AT(0x1000) || BAD(1,0,0,0x3b);
}
)C";
  const auto Selection = request(
      R"("instructions":[4096,4101],"paged_memory":true,"scheduled_events":true,"notify_cr8_writes":true)");
  for (bool LLVM : {false, true}) {
    const auto [Source, Report] = source(Selection, LLVM, 6, /*Paged=*/true);
    provePredicates(
        true, LLVM ? "paged-cr8-width-llvm" : "paged-cr8-width-high", Source,
        std::string(Cr8NotificationProducer) + PagedCr8Boundary + Probes, 2, 0,
        1, /*CloseHelpers=*/true);
  }
}
