; ModuleID = '/Users/gmh/.codex/worktrees/native-source-patching/NeverD/unittests/Inputs/source-patch/neverc-linux/replacement.c'
source_filename = "/Users/gmh/.codex/worktrees/native-source-patching/NeverD/unittests/Inputs/source-patch/neverc-linux/replacement.c"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

; Function Attrs: nounwind null_pointer_is_valid uwtable
define dso_local i32 @replacement(i32 noundef %x) local_unnamed_addr #0 {
entry:
  %add = shl i32 %x, 1
  %add1 = add i32 %add, 11
  ret i32 %add1
}

attributes #0 = { nounwind null_pointer_is_valid uwtable "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }

!llvm.module.flags = !{!0, !1, !2}
!llvm.ident = !{!3}

!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 7, !"PIE Level", i32 2}
!2 = !{i32 7, !"uwtable", i32 2}
!3 = !{!"neverc version 3389.1.5"}
