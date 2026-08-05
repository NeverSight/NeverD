// x86/x64 side-effect intrinsics — included by Intrinsics.cpp.
// Macro: SIDEEFFECT(IntrinsicId)
//
// These intrinsics have observable side effects and must not be
// eliminated as dead even when their result is unused.

// barriers / fences
SIDEEFFECT(Intrinsic::Mfence);
SIDEEFFECT(Intrinsic::Lfence);
SIDEEFFECT(Intrinsic::Sfence);
// hints
SIDEEFFECT(Intrinsic::Pause);
SIDEEFFECT(Intrinsic::Nop);
// cache
SIDEEFFECT(Intrinsic::Clflush);
SIDEEFFECT(Intrinsic::Clflushopt);
SIDEEFFECT(Intrinsic::Clwb);
SIDEEFFECT(Intrinsic::Prefetch);
// traps
SIDEEFFECT(Intrinsic::Int3);
SIDEEFFECT(Intrinsic::Int1);
SIDEEFFECT(Intrinsic::IntN);
SIDEEFFECT(Intrinsic::Into);
SIDEEFFECT(Intrinsic::Ud2);
SIDEEFFECT(Intrinsic::Hlt);
// privileged
SIDEEFFECT(Intrinsic::Cli);
SIDEEFFECT(Intrinsic::Sti);
SIDEEFFECT(Intrinsic::Invlpg);
SIDEEFFECT(Intrinsic::Lgdt);
SIDEEFFECT(Intrinsic::Lidt);
SIDEEFFECT(Intrinsic::Sgdt);
SIDEEFFECT(Intrinsic::Sidt);
SIDEEFFECT(Intrinsic::Lldt);
SIDEEFFECT(Intrinsic::Ltr);
SIDEEFFECT(Intrinsic::Lmsw);
SIDEEFFECT(Intrinsic::Xabort);
SIDEEFFECT(Intrinsic::Bound);
SIDEEFFECT(Intrinsic::Wbinvd);
SIDEEFFECT(Intrinsic::Swapgs);
SIDEEFFECT(Intrinsic::Wrmsr);
SIDEEFFECT(Intrinsic::Wrpkru);
SIDEEFFECT(Intrinsic::Fxsave);
SIDEEFFECT(Intrinsic::Fxrstor);
SIDEEFFECT(Intrinsic::Ldmxcsr);
SIDEEFFECT(Intrinsic::Out);
SIDEEFFECT(Intrinsic::Outsb);
SIDEEFFECT(Intrinsic::Outsw);
SIDEEFFECT(Intrinsic::Outsd);
SIDEEFFECT(Intrinsic::Vmcall);
SIDEEFFECT(Intrinsic::Vmmcall);
// string ops (side-effect)
SIDEEFFECT(Intrinsic::Movsb);
SIDEEFFECT(Intrinsic::Movsw);
SIDEEFFECT(Intrinsic::Movsd);
SIDEEFFECT(Intrinsic::Movsq);
SIDEEFFECT(Intrinsic::Stosb);
SIDEEFFECT(Intrinsic::Stosw);
SIDEEFFECT(Intrinsic::Stosd);
SIDEEFFECT(Intrinsic::Stosq);
// MPX / TSX (deprecated no-ops) + SHA
SIDEEFFECT(Intrinsic::BndMk);
SIDEEFFECT(Intrinsic::BndMov);
SIDEEFFECT(Intrinsic::BndCl);
SIDEEFFECT(Intrinsic::BndCu);
SIDEEFFECT(Intrinsic::BndCn);
SIDEEFFECT(Intrinsic::BndLdx);
SIDEEFFECT(Intrinsic::BndStx);
SIDEEFFECT(Intrinsic::Xacquire);
SIDEEFFECT(Intrinsic::Xrelease);
SIDEEFFECT(Intrinsic::ShaGeneric);
