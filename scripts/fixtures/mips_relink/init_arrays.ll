; Each hook appends an octal digit. The linker orders priorities within each
; array. The freestanding startup visits the finalization array backwards.
source_filename = "init_arrays"

@trace = internal global i32 0
@result = global i32 0
@preinit_slot = constant ptr @preinit, section ".preinit_array"
@llvm.global_ctors = appending global [2 x { i32, ptr, ptr }] [
  { i32, ptr, ptr } { i32 101, ptr @first, ptr null },
  { i32, ptr, ptr } { i32 300, ptr @last, ptr null }]
@llvm.global_dtors = appending global [2 x { i32, ptr, ptr }] [
  { i32, ptr, ptr } { i32 101, ptr @finish_last, ptr null },
  { i32, ptr, ptr } { i32 200, ptr @finish_first, ptr null }]

@__preinit_array_start = external global ptr
@__preinit_array_end = external global ptr
@__init_array_start = external global ptr
@__init_array_end = external global ptr
@__fini_array_start = external global ptr
@__fini_array_end = external global ptr

define void @record(i32 %digit) {
  %old = load volatile i32, ptr @trace
  %shifted = shl i32 %old, 3
  %next = or i32 %shifted, %digit
  store volatile i32 %next, ptr @trace
  ret void
}

define void @preinit() {
  call void @record(i32 1)
  ret void
}
define void @first() {
  call void @record(i32 2)
  ret void
}
define void @last() {
  call void @record(i32 4)
  ret void
}
define void @finish_first() {
  call void @record(i32 5)
  ret void
}
define void @finish_last() {
  call void @record(i32 6)
  ret void
}

define internal void @forward(ptr %begin, ptr %end) {
entry:
  br label %test
test:
  %slot = phi ptr [ %begin, %entry ], [ %next, %call ]
  %done = icmp eq ptr %slot, %end
  br i1 %done, label %return, label %call
call:
  %fn = load ptr, ptr %slot
  call void %fn()
  %next = getelementptr ptr, ptr %slot, i32 1
  br label %test
return:
  ret void
}

define i32 @run_calls() {
entry:
  call void @forward(ptr @__preinit_array_start, ptr @__preinit_array_end)
  call void @forward(ptr @__init_array_start, ptr @__init_array_end)
  br label %test
test:
  %end = phi ptr [ @__fini_array_end, %entry ], [ %slot, %call ]
  %done = icmp eq ptr %end, @__fini_array_start
  br i1 %done, label %return, label %call
call:
  %slot = getelementptr ptr, ptr %end, i32 -1
  %fn = load ptr, ptr %slot
  call void %fn()
  br label %test
return:
  %events = load volatile i32, ptr @trace
  %ordered = icmp eq i32 %events, 42798 ; octal 123456
  %observed = load volatile i32, ptr @result
  %status = select i1 %ordered, i32 %observed, i32 199
  ret i32 %status
}
