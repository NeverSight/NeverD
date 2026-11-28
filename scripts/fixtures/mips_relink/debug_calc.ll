source_filename = "calc.c"

define i32 @calc(i32 %x) section ".text.calc" !dbg !5 {
  %twice = shl i32 %x, 1, !dbg !8
  %result = add i32 %twice, ADJUST, !dbg !8
  ret i32 %result, !dbg !8
}

@neighbor = constant i32 12345

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!3, !4}
!0 = distinct !DICompileUnit(language: DW_LANG_C99, file: !1, producer: "NeverD relink qualification", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug)
!1 = !DIFile(filename: "calc.c", directory: "/src/math")
!2 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!3 = !{i32 2, !"Dwarf Version", i32 DWARF_VERSION}
!4 = !{i32 2, !"Debug Info Version", i32 3}
!5 = distinct !DISubprogram(name: "calc", scope: !1, file: !1, line: SOURCE_LINE, type: !6, scopeLine: SOURCE_LINE, spFlags: DISPFlagDefinition, unit: !0)
!6 = !DISubroutineType(types: !7)
!7 = !{!2, !2}
!8 = !DILocation(line: SOURCE_LINE, column: 7, scope: !5)
