//===- SBFOfficialOracle.rs - Pinned Anza sbpf process oracle ------------===//
//
// NeverD Decompiler
//
// This binary is compiled as an example inside a detached worktree of the
// audited anza-xyz/sbpf revision.  It deliberately exposes a tiny stable text
// protocol instead of linking any NeverD code into the oracle process.
//
//===----------------------------------------------------------------------===//

use solana_sbpf::{
    aligned_memory::AlignedMemory,
    declare_builtin_function, ebpf,
    elf::Executable,
    error::{EbpfError, ProgramResult},
    memory_region::{AccessType, MemoryMapping, MemoryRegion},
    program::{BuiltinFunctionDefinition, BuiltinProgram, FunctionRegistry, SBPFVersion},
    verifier::{RequisiteVerifier, Verifier},
    vm::{CallFrame, Config, ContextObject, EbpfVm, ExecutionMode},
};
use std::{
    convert::TryFrom, env, fmt::Write, fs, path::PathBuf, process::ExitCode, ptr, sync::Arc,
};

include!("SBFOfficialOracleProtocol.rs");
include!("SBFOfficialExecutionFaults.rs");
include!("SBFOfficialExecutionConstants.rs");
include!("SBFOfficialOracleVersions.rs");

const REVISION: &str = env!("NEVERD_SBPF_ORACLE_REVISION");
const INFRASTRUCTURE_FAILURE: u8 = 2;

struct OracleContextObject {
    remaining: u64,
    memory_mapping: MemoryMapping,
    syscall_count: u64,
    syscall_digest: u64,
}

impl Default for OracleContextObject {
    fn default() -> Self {
        Self {
            remaining: 0,
            memory_mapping: unsafe {
                MemoryMapping::new(vec![], &Config::default(), SBPFVersion::Reserved)
            }
            .expect("an empty default memory mapping must be valid"),
            syscall_count: 0,
            syscall_digest: SYSCALL_DIGEST_OFFSET_BASIS,
        }
    }
}

impl ContextObject for OracleContextObject {
    fn consume(&mut self, amount: u64) {
        self.remaining = self.remaining.saturating_sub(amount);
    }

    fn get_remaining(&self) -> u64 {
        self.remaining
    }

    fn active_mapping_ptr(&mut self) -> ptr::NonNull<MemoryMapping> {
        ptr::NonNull::from_ref(&mut self.memory_mapping)
    }
}

impl OracleContextObject {
    fn new(remaining: u64) -> Self {
        Self {
            remaining,
            ..Self::default()
        }
    }

    fn record_syscall(&mut self, arguments: [u64; 5]) {
        self.syscall_count = self.syscall_count.saturating_add(1);
        for argument in arguments {
            self.syscall_digest ^= argument;
            self.syscall_digest = self.syscall_digest.wrapping_mul(SYSCALL_DIGEST_PRIME);
        }
    }
}

declare_builtin_function!(
    OracleLogSyscall,
    fn rust(
        context_object: &mut OracleContextObject,
        vm_addr: u64,
        len: u64,
        arg3: u64,
        arg4: u64,
        arg5: u64,
    ) -> Result<u64, Box<dyn std::error::Error>> {
        context_object.record_syscall([vm_addr, len, arg3, arg4, arg5]);
        if len != 0 {
            let host_buffer = Result::from(context_object.memory_mapping.map(
                AccessType::Load,
                vm_addr,
                len,
            ))?;
            unsafe {
                let bytes = host_buffer.ptr().as_ref_unchecked();
                let message_len = bytes
                    .iter()
                    .position(|byte| *byte == 0)
                    .unwrap_or(bytes.len());
                let _ = std::str::from_utf8(&bytes[..message_len]);
            }
        }
        Ok(0)
    }
);

declare_builtin_function!(
    OracleProbeSyscall,
    fn rust(
        context_object: &mut OracleContextObject,
        arg1: u64,
        arg2: u64,
        arg3: u64,
        arg4: u64,
        arg5: u64,
    ) -> Result<u64, Box<dyn std::error::Error>> {
        let arguments = [arg1, arg2, arg3, arg4, arg5];
        context_object.record_syscall(arguments);
        Ok(arguments
            .iter()
            .copied()
            .fold(PROBE_RETURN_BIAS, u64::wrapping_add))
    }
);

#[derive(Clone, Copy)]
enum InputProfile {
    None,
    ByteOne,
}

#[derive(Clone, Copy)]
enum SyscallProfile {
    None,
    Log,
    Probe,
    CollisionProbe,
}

struct CommonOptions {
    input: InputProfile,
    syscalls: SyscallProfile,
    budget: u64,
}

struct ElfOptions {
    path: PathBuf,
    optimize_rodata: bool,
    common: CommonOptions,
}

enum TextSource {
    File(PathBuf),
    Hex(String),
}

struct TextOptions {
    source: TextSource,
    version: SBPFVersion,
    functions: Vec<(u32, usize)>,
    common: CommonOptions,
}

struct VerificationBatchOptions {
    path: PathBuf,
}

struct VerificationRequest {
    index: usize,
    version: SBPFVersion,
    text: Vec<u8>,
}

struct ELFVerificationBatchOptions {
    path: PathBuf,
}

struct ELFVerificationRequest {
    index: usize,
    elf: Vec<u8>,
}

struct ExecutionBatchOptions {
    path: PathBuf,
}

struct ExecutionRequest {
    index: usize,
    version: SBPFVersion,
    budget: u64,
    syscalls: SyscallProfile,
    target_slot: usize,
    functions: Vec<(u32, usize)>,
    input: Vec<u8>,
    text: Vec<u8>,
}

enum Command {
    Version,
    ProbeElf(ElfOptions),
    ExecuteText(TextOptions),
    VerifyBatch(VerificationBatchOptions),
    VerifyELFBatch(ELFVerificationBatchOptions),
    ExecuteBatch(ExecutionBatchOptions),
}

fn parse_u64(value: &str, description: &str) -> Result<u64, String> {
    let parsed = value
        .strip_prefix("0x")
        .map(|digits| u64::from_str_radix(digits, 16))
        .unwrap_or_else(|| value.parse::<u64>());
    parsed.map_err(|error| format!("invalid {description} '{value}': {error}"))
}

fn parse_bool(value: &str) -> Result<bool, String> {
    match value {
        TRUE_VALUE => Ok(true),
        FALSE_VALUE => Ok(false),
        _ => Err(format!("invalid boolean '{value}'")),
    }
}

fn parse_input(value: &str) -> Result<InputProfile, String> {
    match value {
        NONE_PROFILE => Ok(InputProfile::None),
        BYTE_ONE_PROFILE => Ok(InputProfile::ByteOne),
        _ => Err(format!("invalid input profile '{value}'")),
    }
}

fn parse_syscalls(value: &str) -> Result<SyscallProfile, String> {
    match value {
        NONE_PROFILE => Ok(SyscallProfile::None),
        LOG_PROFILE => Ok(SyscallProfile::Log),
        PROBE_PROFILE => Ok(SyscallProfile::Probe),
        COLLISION_PROBE_PROFILE => Ok(SyscallProfile::CollisionProbe),
        _ => Err(format!("invalid syscall profile '{value}'")),
    }
}

fn parse_version(value: &str) -> Result<SBPFVersion, String> {
    parse_generated_version(value).ok_or_else(|| format!("invalid SBPF version '{value}'"))
}

fn next_value(arguments: &[String], index: &mut usize, option: &str) -> Result<String, String> {
    *index += 1;
    arguments
        .get(*index)
        .cloned()
        .ok_or_else(|| format!("missing value for {option}"))
}

fn parse_function(value: &str) -> Result<(u32, usize), String> {
    let (key, pc) = value
        .split_once(':')
        .ok_or_else(|| format!("invalid function '{value}', expected KEY:PC"))?;
    let key = parse_u64(key, "function key")?;
    let pc = parse_u64(pc, "function PC")?;
    Ok((
        u32::try_from(key).map_err(|_| format!("function key '{key}' exceeds u32"))?,
        usize::try_from(pc).map_err(|_| format!("function PC '{pc}' exceeds usize"))?,
    ))
}

fn parse_common(
    input: Option<InputProfile>,
    syscalls: Option<SyscallProfile>,
    budget: Option<u64>,
) -> Result<CommonOptions, String> {
    Ok(CommonOptions {
        input: input.ok_or_else(|| format!("missing {INPUT_ARGUMENT}"))?,
        syscalls: syscalls.ok_or_else(|| format!("missing {SYSCALLS_ARGUMENT}"))?,
        budget: budget.ok_or_else(|| format!("missing {BUDGET_ARGUMENT}"))?,
    })
}

fn parse_elf(arguments: &[String]) -> Result<Command, String> {
    let mut path = None;
    let mut optimize_rodata = None;
    let mut input = None;
    let mut syscalls = None;
    let mut budget = None;
    let mut index = 0;
    while index < arguments.len() {
        match arguments[index].as_str() {
            ELF_ARGUMENT => {
                path = Some(PathBuf::from(next_value(
                    arguments,
                    &mut index,
                    ELF_ARGUMENT,
                )?))
            }
            OPTIMIZE_RODATA_ARGUMENT => {
                optimize_rodata = Some(parse_bool(&next_value(
                    arguments,
                    &mut index,
                    OPTIMIZE_RODATA_ARGUMENT,
                )?)?)
            }
            INPUT_ARGUMENT => {
                input = Some(parse_input(&next_value(
                    arguments,
                    &mut index,
                    INPUT_ARGUMENT,
                )?)?)
            }
            SYSCALLS_ARGUMENT => {
                syscalls = Some(parse_syscalls(&next_value(
                    arguments,
                    &mut index,
                    SYSCALLS_ARGUMENT,
                )?)?)
            }
            BUDGET_ARGUMENT => {
                budget = Some(parse_u64(
                    &next_value(arguments, &mut index, BUDGET_ARGUMENT)?,
                    "budget",
                )?)
            }
            option => return Err(format!("unknown {ELF_COMMAND} option '{option}'")),
        }
        index += 1;
    }
    Ok(Command::ProbeElf(ElfOptions {
        path: path.ok_or_else(|| format!("missing {ELF_ARGUMENT}"))?,
        optimize_rodata: optimize_rodata
            .ok_or_else(|| format!("missing {OPTIMIZE_RODATA_ARGUMENT}"))?,
        common: parse_common(input, syscalls, budget)?,
    }))
}

fn parse_text(arguments: &[String]) -> Result<Command, String> {
    let mut source = None;
    let mut version = None;
    let mut input = None;
    let mut syscalls = None;
    let mut budget = None;
    let mut functions = Vec::new();
    let mut index = 0;
    while index < arguments.len() {
        match arguments[index].as_str() {
            TEXT_ARGUMENT => {
                if source.is_some() {
                    return Err(format!(
                        "{TEXT_ARGUMENT} and {HEX_ARGUMENT} are mutually exclusive"
                    ));
                }
                source = Some(TextSource::File(PathBuf::from(next_value(
                    arguments,
                    &mut index,
                    TEXT_ARGUMENT,
                )?)));
            }
            HEX_ARGUMENT => {
                if source.is_some() {
                    return Err(format!(
                        "{TEXT_ARGUMENT} and {HEX_ARGUMENT} are mutually exclusive"
                    ));
                }
                source = Some(TextSource::Hex(next_value(
                    arguments,
                    &mut index,
                    HEX_ARGUMENT,
                )?));
            }
            SBPF_VERSION_ARGUMENT => {
                version = Some(parse_version(&next_value(
                    arguments,
                    &mut index,
                    SBPF_VERSION_ARGUMENT,
                )?)?)
            }
            INPUT_ARGUMENT => {
                input = Some(parse_input(&next_value(
                    arguments,
                    &mut index,
                    INPUT_ARGUMENT,
                )?)?)
            }
            SYSCALLS_ARGUMENT => {
                syscalls = Some(parse_syscalls(&next_value(
                    arguments,
                    &mut index,
                    SYSCALLS_ARGUMENT,
                )?)?)
            }
            BUDGET_ARGUMENT => {
                budget = Some(parse_u64(
                    &next_value(arguments, &mut index, BUDGET_ARGUMENT)?,
                    "budget",
                )?)
            }
            FUNCTION_ARGUMENT => functions.push(parse_function(&next_value(
                arguments,
                &mut index,
                FUNCTION_ARGUMENT,
            )?)?),
            option => return Err(format!("unknown {TEXT_COMMAND} option '{option}'")),
        }
        index += 1;
    }
    Ok(Command::ExecuteText(TextOptions {
        source: source.ok_or_else(|| format!("missing {TEXT_ARGUMENT} or {HEX_ARGUMENT}"))?,
        version: version.ok_or_else(|| format!("missing {SBPF_VERSION_ARGUMENT}"))?,
        functions,
        common: parse_common(input, syscalls, budget)?,
    }))
}

fn parse_verification_batch(arguments: &[String]) -> Result<Command, String> {
    match arguments {
        [argument, path] if argument == BATCH_INPUT_ARGUMENT => {
            Ok(Command::VerifyBatch(VerificationBatchOptions {
                path: PathBuf::from(path),
            }))
        }
        _ => Err(format!(
            "{VERIFY_BATCH_COMMAND} expects {BATCH_INPUT_ARGUMENT} PATH"
        )),
    }
}

fn parse_elf_verification_batch(arguments: &[String]) -> Result<Command, String> {
    match arguments {
        [argument, path] if argument == BATCH_INPUT_ARGUMENT => {
            Ok(Command::VerifyELFBatch(ELFVerificationBatchOptions {
                path: PathBuf::from(path),
            }))
        }
        _ => Err(format!(
            "{VERIFY_ELF_BATCH_COMMAND} expects {BATCH_INPUT_ARGUMENT} PATH"
        )),
    }
}

fn parse_execution_batch(arguments: &[String]) -> Result<Command, String> {
    match arguments {
        [argument, path] if argument == BATCH_INPUT_ARGUMENT => {
            Ok(Command::ExecuteBatch(ExecutionBatchOptions {
                path: PathBuf::from(path),
            }))
        }
        _ => Err(format!(
            "{EXECUTE_BATCH_COMMAND} expects {BATCH_INPUT_ARGUMENT} PATH"
        )),
    }
}

fn parse_command() -> Result<Command, String> {
    let arguments: Vec<String> = env::args().skip(1).collect();
    let (command, rest) = arguments
        .split_first()
        .ok_or_else(|| "missing command".to_string())?;
    match command.as_str() {
        VERSION_COMMAND if rest.is_empty() => Ok(Command::Version),
        ELF_COMMAND => parse_elf(rest),
        TEXT_COMMAND => parse_text(rest),
        VERIFY_BATCH_COMMAND => parse_verification_batch(rest),
        VERIFY_ELF_BATCH_COMMAND => parse_elf_verification_batch(rest),
        EXECUTE_BATCH_COMMAND => parse_execution_batch(rest),
        _ => Err(format!("unknown command '{command}'")),
    }
}

fn decode_hex(value: &str) -> Result<Vec<u8>, String> {
    if !value.len().is_multiple_of(2) {
        return Err("hex text must contain an even number of digits".to_string());
    }
    value
        .as_bytes()
        .chunks_exact(2)
        .map(|pair| {
            let digits = std::str::from_utf8(pair).map_err(|error| error.to_string())?;
            u8::from_str_radix(digits, 16)
                .map_err(|error| format!("invalid hex byte '{digits}': {error}"))
        })
        .collect()
}

fn read_text(source: TextSource) -> Result<Vec<u8>, String> {
    match source {
        TextSource::File(path) => {
            fs::read(&path).map_err(|error| format!("failed to read '{}': {error}", path.display()))
        }
        TextSource::Hex(value) => decode_hex(&value),
    }
}

fn read_verification_batch(path: &PathBuf) -> Result<Vec<VerificationRequest>, String> {
    let contents = fs::read_to_string(path)
        .map_err(|error| format!("failed to read '{}': {error}", path.display()))?;
    let mut lines = contents.lines();
    let header = lines
        .next()
        .ok_or_else(|| "verification batch is empty".to_string())?;
    if header != BATCH_PROTOCOL {
        return Err(format!(
            "unsupported verification batch protocol '{header}'"
        ));
    }

    let mut requests = Vec::new();
    for (line_offset, line) in lines.enumerate() {
        let line_number = line_offset + 2;
        if line.trim().is_empty() {
            continue;
        }
        let mut fields = line.split_ascii_whitespace();
        let index = fields
            .next()
            .ok_or_else(|| format!("batch line {line_number} is missing an index"))?;
        let version = fields
            .next()
            .ok_or_else(|| format!("batch line {line_number} is missing a version"))?;
        let text = fields
            .next()
            .ok_or_else(|| format!("batch line {line_number} is missing text"))?;
        if fields.next().is_some() {
            return Err(format!("batch line {line_number} has trailing fields"));
        }
        let index = usize::try_from(parse_u64(index, "verification index")?)
            .map_err(|_| format!("verification index on line {line_number} exceeds usize"))?;
        if index != requests.len() {
            return Err(format!(
                "batch line {line_number} has index {index}, expected {}",
                requests.len()
            ));
        }
        requests.push(VerificationRequest {
            index,
            version: parse_version(version)?,
            text: decode_hex(text)?,
        });
    }
    Ok(requests)
}

fn read_elf_verification_batch(path: &PathBuf) -> Result<Vec<ELFVerificationRequest>, String> {
    let contents = fs::read_to_string(path)
        .map_err(|error| format!("failed to read '{}': {error}", path.display()))?;
    let mut lines = contents.lines();
    let header = lines
        .next()
        .ok_or_else(|| "ELF verification batch is empty".to_string())?;
    if header != ELF_BATCH_PROTOCOL {
        return Err(format!(
            "unsupported ELF verification batch protocol '{header}'"
        ));
    }

    let mut requests = Vec::new();
    for (line_offset, line) in lines.enumerate() {
        let line_number = line_offset + 2;
        if line.trim().is_empty() {
            continue;
        }
        let mut fields = line.split_ascii_whitespace();
        let index = fields
            .next()
            .ok_or_else(|| format!("ELF batch line {line_number} is missing an index"))?;
        let elf = fields
            .next()
            .ok_or_else(|| format!("ELF batch line {line_number} is missing bytes"))?;
        if fields.next().is_some() {
            return Err(format!("ELF batch line {line_number} has trailing fields"));
        }
        let index = usize::try_from(parse_u64(index, "ELF verification index")?)
            .map_err(|_| format!("ELF verification index on line {line_number} exceeds usize"))?;
        if index != requests.len() {
            return Err(format!(
                "ELF batch line {line_number} has index {index}, expected {}",
                requests.len()
            ));
        }
        requests.push(ELFVerificationRequest {
            index,
            elf: decode_hex(elf)?,
        });
    }
    Ok(requests)
}

fn parse_execution_functions(value: &str, line_number: usize) -> Result<Vec<(u32, usize)>, String> {
    if value == EMPTY_FIELD {
        return Ok(Vec::new());
    }
    let mut functions = Vec::new();
    for field in value.split(',') {
        if field.is_empty() {
            return Err(format!(
                "execution batch line {line_number} contains an empty function"
            ));
        }
        let function = parse_function(field)?;
        if functions.iter().any(|(key, _)| *key == function.0) {
            return Err(format!(
                "execution batch line {line_number} repeats function key {:#x}",
                function.0
            ));
        }
        functions.push(function);
    }
    Ok(functions)
}

fn parse_execution_bytes(
    value: &str,
    description: &str,
    line_number: usize,
) -> Result<Vec<u8>, String> {
    if value == EMPTY_FIELD {
        return Ok(Vec::new());
    }
    decode_hex(value).map_err(|error| {
        format!("execution batch line {line_number} has invalid {description}: {error}")
    })
}

fn read_execution_batch(path: &PathBuf) -> Result<Vec<ExecutionRequest>, String> {
    let contents = fs::read_to_string(path)
        .map_err(|error| format!("failed to read '{}': {error}", path.display()))?;
    let mut lines = contents.lines();
    let header = lines
        .next()
        .ok_or_else(|| "execution batch is empty".to_string())?;
    if header != EXECUTION_BATCH_PROTOCOL {
        return Err(format!("unsupported execution batch protocol '{header}'"));
    }

    let mut requests = Vec::new();
    for (line_offset, line) in lines.enumerate() {
        let line_number = line_offset + 2;
        if line.trim().is_empty() {
            continue;
        }
        let fields: Vec<&str> = line.split_ascii_whitespace().collect();
        let [index, version, budget, syscalls, target_slot, functions, input, text] =
            fields.as_slice()
        else {
            return Err(format!(
                "execution batch line {line_number} must contain exactly 8 fields"
            ));
        };
        let index = usize::try_from(parse_u64(index, "execution index")?)
            .map_err(|_| format!("execution index on line {line_number} exceeds usize"))?;
        if index != requests.len() {
            return Err(format!(
                "execution batch line {line_number} has index {index}, expected {}",
                requests.len()
            ));
        }
        if requests.len() >= EXECUTION_BATCH_CASE_LIMIT {
            return Err(format!(
                "execution batch exceeds {EXECUTION_BATCH_CASE_LIMIT} cases"
            ));
        }
        let input = parse_execution_bytes(input, "input", line_number)?;
        let text = parse_execution_bytes(text, "text", line_number)?;
        if input.len() > EXECUTION_INPUT_BYTE_LIMIT {
            return Err(format!(
                "execution batch line {line_number} input exceeds {EXECUTION_INPUT_BYTE_LIMIT} bytes"
            ));
        }
        if text.len() > EXECUTION_TEXT_BYTE_LIMIT {
            return Err(format!(
                "execution batch line {line_number} text exceeds {EXECUTION_TEXT_BYTE_LIMIT} bytes"
            ));
        }
        let target_slot = usize::try_from(parse_u64(target_slot, "target slot")?)
            .map_err(|_| format!("target slot on line {line_number} exceeds usize"))?;
        requests.push(ExecutionRequest {
            index,
            version: parse_version(version)?,
            budget: parse_u64(budget, "budget")?,
            syscalls: parse_syscalls(syscalls)?,
            target_slot,
            functions: parse_execution_functions(functions, line_number)?,
            input,
            text,
        });
    }
    if requests.is_empty() {
        return Err("execution batch contains no requests".to_string());
    }
    Ok(requests)
}

fn make_loader(
    common: &CommonOptions,
    mut config: Config,
) -> Result<Arc<BuiltinProgram<OracleContextObject>>, String> {
    config.enable_symbol_and_section_labels = true;
    let mut loader = BuiltinProgram::new_loader(config);
    match common.syscalls {
        SyscallProfile::None => {}
        SyscallProfile::Log => OracleLogSyscall::register(&mut loader, LOG_PROFILE)
            .map_err(|error| format!("failed to register log syscall: {error:?}"))?,
        SyscallProfile::Probe => OracleProbeSyscall::register(&mut loader, PROBE_SYSCALL_NAME)
            .map_err(|error| format!("failed to register probe syscall: {error:?}"))?,
        SyscallProfile::CollisionProbe => {
            let bytes = COLLISION_FUNCTION_SLOT.to_le_bytes();
            let name = std::str::from_utf8(&bytes)
                .map_err(|error| format!("collision syscall name is not UTF-8: {error}"))?;
            OracleProbeSyscall::register(&mut loader, name)
                .map_err(|error| format!("failed to register collision syscall: {error:?}"))?
        }
    }
    Ok(Arc::new(loader))
}

fn emit_rejected(error: impl std::fmt::Debug) {
    eprintln!("official sbpf rejected input: {error:?}");
    println!("{PROTOCOL} {REJECTED}");
}

fn execute(
    executable: &Executable<OracleContextObject>,
    common: &CommonOptions,
) -> Result<(), String> {
    let mut input = match common.input {
        InputProfile::None => Vec::new(),
        InputProfile::ByteOne => vec![1],
    };
    let config = executable.get_config();
    let version = executable.get_sbpf_version();
    let mut stack = AlignedMemory::<{ ebpf::HOST_ALIGN }>::zero_filled(config.stack_size());
    let stack_len = stack.len();
    let mut heap = AlignedMemory::<{ ebpf::HOST_ALIGN }>::zero_filled(0);
    let regions = vec![
        executable.get_ro_region(),
        MemoryRegion::new_gapped(
            &raw mut *stack.as_slice_mut(),
            ebpf::MM_STACK_START,
            if version.stack_frame_gaps() && config.enable_stack_frame_gaps {
                config.stack_frame_size as u64
            } else {
                0
            },
        ),
        MemoryRegion::new(&raw mut *heap.as_slice_mut(), ebpf::MM_HEAP_START),
        MemoryRegion::new(&raw mut input[..], ebpf::MM_INPUT_START),
    ];

    let mut context = OracleContextObject::new(common.budget);
    context.memory_mapping = unsafe { MemoryMapping::new(regions, config, version) }
        .map_err(|error| format!("failed to map VM memory: {error:?}"))?;
    let mut vm = EbpfVm::new(
        executable.get_loader().clone(),
        version,
        &mut context,
        stack_len,
    );
    vm.registers[1] = ebpf::MM_INPUT_START;
    let mut call_frames = vec![CallFrame::default(); config.max_call_depth];
    let (instruction_count, result) = vm.execute_program(
        executable,
        &mut ExecutionMode::Interpreted,
        &mut call_frames,
    );
    match result {
        ProgramResult::Ok(value) => {
            println!("{PROTOCOL} {RETURNED} {value} {instruction_count}");
        }
        ProgramResult::Err(EbpfError::CallDepthExceeded) => {
            println!("{PROTOCOL} {CALL_DEPTH_FAULT} {instruction_count}");
        }
        ProgramResult::Err(error) => {
            eprintln!("official sbpf execution fault: {error:?}");
            println!("{PROTOCOL} {OTHER_FAULT} {instruction_count}");
        }
    }
    Ok(())
}

fn probe_elf(options: ElfOptions) -> Result<(), String> {
    let bytes = fs::read(&options.path)
        .map_err(|error| format!("failed to read '{}': {error}", options.path.display()))?;
    let loader = make_loader(
        &options.common,
        Config {
            optimize_rodata: options.optimize_rodata,
            ..Config::default()
        },
    )?;
    let executable = match Executable::<OracleContextObject>::from_elf(&bytes, loader) {
        Ok(executable) => executable,
        Err(error) => {
            emit_rejected(error);
            return Ok(());
        }
    };
    if let Err(error) = executable.verify::<RequisiteVerifier>() {
        emit_rejected(error);
        return Ok(());
    }
    if options.common.budget == 0 {
        println!("{PROTOCOL} {ACCEPTED}");
        return Ok(());
    }
    execute(&executable, &options.common)
}

fn execute_text(options: TextOptions) -> Result<(), String> {
    let bytes = read_text(options.source)?;
    let loader = make_loader(
        &options.common,
        Config {
            enabled_sbpf_versions: options.version..=options.version,
            ..Config::default()
        },
    )?;
    let mut functions = FunctionRegistry::default();
    for (key, pc) in options.functions {
        functions
            .register_function(key, format!("function_{pc}"), pc)
            .map_err(|error| format!("failed to register function {key:#x}:{pc}: {error:?}"))?;
    }
    let executable = match Executable::<OracleContextObject>::from_text_bytes(
        &bytes,
        loader,
        options.version,
        functions,
    ) {
        Ok(executable) => executable,
        Err(error) => {
            emit_rejected(error);
            return Ok(());
        }
    };
    if let Err(error) = executable.verify::<RequisiteVerifier>() {
        emit_rejected(error);
        return Ok(());
    }
    execute(&executable, &options.common)
}

fn verify_batch(options: VerificationBatchOptions) -> Result<(), String> {
    let requests = read_verification_batch(&options.path)?;
    let function_registry = FunctionRegistry::<usize>::default();
    for request in requests {
        let status = if RequisiteVerifier::verify(
            &request.text,
            request.version,
            &function_registry,
        )
        .is_ok()
        {
            ACCEPTED
        } else {
            REJECTED
        };
        println!(
            "{PROTOCOL} {VERIFICATION_RECORD} {} {status}",
            request.index
        );
    }
    Ok(())
}

fn verify_elf_batch(options: ELFVerificationBatchOptions) -> Result<(), String> {
    let requests = read_elf_verification_batch(&options.path)?;
    let loader = Arc::new(BuiltinProgram::<OracleContextObject>::new_loader(
        Config::default(),
    ));
    for request in requests {
        let accepted =
            match Executable::<OracleContextObject>::from_elf(&request.elf, loader.clone()) {
                Ok(executable) => executable.verify::<RequisiteVerifier>().is_ok(),
                Err(_) => false,
            };
        let status = if accepted { ACCEPTED } else { REJECTED };
        println!(
            "{PROTOCOL} {ELF_VERIFICATION_RECORD} {} {status}",
            request.index
        );
    }
    Ok(())
}

struct ExecutionObservation {
    outcome: &'static str,
    fault: &'static str,
    result: u64,
    instruction_count: u64,
    target_hits: usize,
    input: Vec<u8>,
    syscall_count: u64,
    syscall_digest: u64,
}

fn encode_hex(bytes: &[u8]) -> String {
    if bytes.is_empty() {
        return EMPTY_FIELD.to_string();
    }
    let mut encoded = String::with_capacity(bytes.len().saturating_mul(2));
    for byte in bytes {
        write!(&mut encoded, "{byte:02x}").expect("writing to a String cannot fail");
    }
    encoded
}

fn classify_execution_fault(
    error: &EbpfError,
    text: &[u8],
    register_trace: &[[u64; 12]],
) -> Result<&'static str, String> {
    match error {
        EbpfError::CallDepthExceeded => Ok(FAULT_CALL_DEPTH),
        EbpfError::DivideByZero => Ok(FAULT_DIVIDE_BY_ZERO),
        EbpfError::DivideOverflow => Ok(FAULT_DIVIDE_OVERFLOW),
        EbpfError::ExecutionOverrun => Ok(FAULT_EXECUTION_OVERRUN),
        EbpfError::CallOutsideTextSegment => Ok(FAULT_UNKNOWN_INDIRECT_CALL),
        EbpfError::ExceededMaxInstructions => Ok(FAULT_INSTRUCTION_METER),
        EbpfError::InvalidMemoryRegion(_)
        | EbpfError::AccessViolation(..)
        | EbpfError::StackAccessViolation(..) => Ok(FAULT_MEMORY_ACCESS),
        EbpfError::InvalidInstruction => Ok(FAULT_INVALID_INSTRUCTION),
        EbpfError::UnsupportedInstruction => {
            let fault_pc = register_trace
                .last()
                .map(|registers| registers[11] as usize)
                .ok_or_else(|| "unsupported instruction has no trace entry".to_string())?;
            let byte_offset = fault_pc
                .checked_mul(ebpf::INSN_SIZE)
                .ok_or_else(|| "fault PC overflows a byte offset".to_string())?;
            let opcode = *text.get(byte_offset).ok_or_else(|| {
                format!("fault PC {fault_pc} is outside {} text bytes", text.len())
            })?;
            if opcode == ebpf::CALL_IMM {
                Ok(FAULT_UNRESOLVED_CALL)
            } else {
                Ok(FAULT_INVALID_INSTRUCTION)
            }
        }
        EbpfError::SyscallError(_) => Ok(FAULT_SYSCALL),
        unexpected => Err(format!(
            "unclassified official execution error (fail-closed): {unexpected:?}"
        )),
    }
}

fn execute_observed(
    executable: &Executable<OracleContextObject>,
    request: &ExecutionRequest,
) -> Result<ExecutionObservation, String> {
    let mut input = request.input.clone();
    let config = executable.get_config();
    let version = executable.get_sbpf_version();
    let mut stack = AlignedMemory::<{ ebpf::HOST_ALIGN }>::zero_filled(config.stack_size());
    let stack_len = stack.len();
    let mut heap = AlignedMemory::<{ ebpf::HOST_ALIGN }>::zero_filled(0);
    let regions = vec![
        executable.get_ro_region(),
        MemoryRegion::new_gapped(
            &raw mut *stack.as_slice_mut(),
            ebpf::MM_STACK_START,
            if version.stack_frame_gaps() && config.enable_stack_frame_gaps {
                config.stack_frame_size as u64
            } else {
                0
            },
        ),
        MemoryRegion::new(&raw mut *heap.as_slice_mut(), ebpf::MM_HEAP_START),
        MemoryRegion::new(&raw mut input[..], ebpf::MM_INPUT_START),
    ];

    let mut context = OracleContextObject::new(request.budget);
    context.memory_mapping = unsafe { MemoryMapping::new(regions, config, version) }
        .map_err(|error| format!("failed to map execution batch memory: {error:?}"))?;
    let mut vm = EbpfVm::new(
        executable.get_loader().clone(),
        version,
        &mut context,
        stack_len,
    );
    vm.registers[1] = ebpf::MM_INPUT_START;
    let mut call_frames = vec![CallFrame::default(); config.max_call_depth];
    let (instruction_count, result) = vm.execute_program(
        executable,
        &mut ExecutionMode::Interpreted,
        &mut call_frames,
    );
    let target_hits = vm
        .register_trace
        .iter()
        .filter(|registers| registers[11] as usize == request.target_slot)
        .count();
    let (outcome, fault, result) = match result {
        ProgramResult::Ok(value) => (RETURNED, FAULT_NONE, value),
        ProgramResult::Err(error) => (
            FAULTED,
            classify_execution_fault(&error, &request.text, &vm.register_trace)?,
            0,
        ),
    };
    drop(vm);

    Ok(ExecutionObservation {
        outcome,
        fault,
        result,
        instruction_count,
        target_hits,
        input,
        syscall_count: context.syscall_count,
        syscall_digest: context.syscall_digest,
    })
}

fn execute_batch(options: ExecutionBatchOptions) -> Result<(), String> {
    let requests = read_execution_batch(&options.path)?;
    for request in &requests {
        let instruction_count = request.text.len() / ebpf::INSN_SIZE;
        if request.target_slot >= instruction_count {
            return Err(format!(
                "execution request {} target slot {} is outside {instruction_count} instructions",
                request.index, request.target_slot
            ));
        }
        if let Some((_, target)) = request
            .functions
            .iter()
            .find(|(_, target)| *target >= instruction_count)
        {
            return Err(format!(
                "execution request {} function target {target} is outside {instruction_count} instructions",
                request.index
            ));
        }

        let common = CommonOptions {
            input: InputProfile::None,
            syscalls: request.syscalls,
            budget: request.budget,
        };
        let loader = make_loader(
            &common,
            Config {
                enabled_sbpf_versions: request.version..=request.version,
                enable_register_tracing: true,
                ..Config::default()
            },
        )?;
        let mut functions = FunctionRegistry::default();
        for (key, pc) in &request.functions {
            functions
                .register_function(*key, format!("function_{pc}"), *pc)
                .map_err(|error| {
                    format!(
                        "failed to register execution request {} function {key:#x}:{pc}: {error:?}",
                        request.index
                    )
                })?;
        }
        let executable = match Executable::<OracleContextObject>::from_text_bytes(
            &request.text,
            loader,
            request.version,
            functions,
        ) {
            Ok(executable) => executable,
            Err(error) => {
                eprintln!(
                    "official sbpf rejected execution request {}: {error:?}",
                    request.index
                );
                println!(
                    "{PROTOCOL} {EXECUTION_RECORD} {} {REJECTED} {FAULT_NONE} 0 0 0 {} 0 {SYSCALL_DIGEST_OFFSET_BASIS}",
                    request.index,
                    encode_hex(&request.input)
                );
                continue;
            }
        };
        if let Err(error) = executable.verify::<RequisiteVerifier>() {
            eprintln!(
                "official sbpf verifier rejected execution request {}: {error:?}",
                request.index
            );
            println!(
                "{PROTOCOL} {EXECUTION_RECORD} {} {REJECTED} {FAULT_NONE} 0 0 0 {} 0 {SYSCALL_DIGEST_OFFSET_BASIS}",
                request.index,
                encode_hex(&request.input)
            );
            continue;
        }

        let observation = execute_observed(&executable, request)?;
        println!(
            "{PROTOCOL} {EXECUTION_RECORD} {} {} {} {} {} {} {} {} {}",
            request.index,
            observation.outcome,
            observation.fault,
            observation.result,
            observation.instruction_count,
            observation.target_hits,
            encode_hex(&observation.input),
            observation.syscall_count,
            observation.syscall_digest,
        );
    }
    println!("{PROTOCOL} {EXECUTION_SUMMARY_RECORD} {}", requests.len());
    Ok(())
}

fn run() -> Result<(), String> {
    match parse_command()? {
        Command::Version => {
            println!("{PROTOCOL} {VERSION_RECORD} {REVISION}");
            Ok(())
        }
        Command::ProbeElf(options) => probe_elf(options),
        Command::ExecuteText(options) => execute_text(options),
        Command::VerifyBatch(options) => verify_batch(options),
        Command::VerifyELFBatch(options) => verify_elf_batch(options),
        Command::ExecuteBatch(options) => execute_batch(options),
    }
}

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("official sbpf oracle infrastructure failure: {error}");
            ExitCode::from(INFRASTRUCTURE_FAILURE)
        }
    }
}
