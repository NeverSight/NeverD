; ModuleID = '/Users/gmh/.codex/worktrees/native-source-patching/NeverD/unittests/Inputs/source-patch/neverc-hosted/scalar.c'
source_filename = "/Users/gmh/.codex/worktrees/native-source-patching/NeverD/unittests/Inputs/source-patch/neverc-hosted/scalar.c"
target datalayout = "e-m:w-p:64:64-i32:32-i64:64-i128:128-n32:64-S128"
target triple = "aarch64-pc-windows-msvc19.33.0"

; Function Attrs: nounwind null_pointer_is_valid uwtable
define dso_local i32 @replacement(i32 noundef %x) local_unnamed_addr #0 {
entry:
  %add = shl i32 %x, 1
  %add1 = add i32 %add, 11
  ret i32 %add1
}

attributes #0 = { nounwind null_pointer_is_valid uwtable "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="generic" "target-features"="+fp-armv8,+neon,+v8a,-fmv" }

!llvm.module.flags = !{!0, !1}
!llvm.ident = !{!2}

!0 = !{i32 1, !"wchar_size", i32 2}
!1 = !{i32 7, !"uwtable", i32 2}
!2 = !{!"neverc version 3389.1.5"}
