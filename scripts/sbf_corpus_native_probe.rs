// NeverD Decompiler: compile-only instantiation of generated Rust source.
//
// These unresolved Rust-ABI symbols keep host effects and outcomes opaque.
// This adapter is never executed and supplies no fabricated VM responses.
// A concrete environment forces native code generation for the program body;
// compiling an uninstantiated public generic would only check its metadata.

unsafe extern "Rust" {
    fn neverd_qualify_load(context: usize, address: u64, width: u8) -> Result<u64, SbfErrorV2>;
    fn neverd_qualify_store(
        context: usize,
        address: u64,
        width: u8,
        value: u64,
    ) -> Result<(), SbfErrorV2>;
    fn neverd_qualify_syscall(
        context: usize,
        invocation: SbfSyscallInvocation,
    ) -> SbfSyscallOutcomeV2;
    fn neverd_qualify_features(context: usize) -> Option<SbfRuntimeFeatures>;
}

struct NativeQualificationEnvironment {
    context: usize,
}

impl SbfEnvironmentV2 for NativeQualificationEnvironment {
    fn load(&mut self, address: u64, width: u8) -> Result<u64, SbfErrorV2> {
        unsafe { neverd_qualify_load(self.context, address, width) }
    }

    fn store(&mut self, address: u64, width: u8, value: u64) -> Result<(), SbfErrorV2> {
        unsafe { neverd_qualify_store(self.context, address, width, value) }
    }

    fn syscall_with_features(&mut self, invocation: SbfSyscallInvocation) -> SbfSyscallOutcomeV2 {
        unsafe { neverd_qualify_syscall(self.context, invocation) }
    }

    fn runtime_features(&self) -> Option<SbfRuntimeFeatures> {
        unsafe { neverd_qualify_features(self.context) }
    }
}

#[unsafe(no_mangle)]
pub extern "Rust" fn neverd_qualified_entry(
    context: usize,
    input: u64,
    instruction_data: u64,
) -> Result<u64, SbfErrorV2> {
    let mut environment = NativeQualificationEnvironment { context };
    neverd_sbf_program_v2(&mut environment, input, instruction_data)
}
