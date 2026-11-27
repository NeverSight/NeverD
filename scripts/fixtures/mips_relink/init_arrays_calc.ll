source_filename = "init_arrays_calc"

@result = external global i32
@llvm.global_ctors = appending global [1 x { i32, ptr, ptr }] [
  { i32, ptr, ptr } { i32 200, ptr @calc, ptr null }]
declare void @record(i32)

define void @calc() section ".text.calc" {
  call void @record(i32 3)
  store volatile i32 RESULT, ptr @result
  ret void
}
