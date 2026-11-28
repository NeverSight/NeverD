declare void @finish(i32) noreturn

define void @calc() nounwind noreturn section ".text.calc" align 4 {
  notail call void @finish(i32 STATUS)
  unreachable
}

@_start = alias void (), ptr @calc
