declare i32 @left(i32)
declare i32 @right(i32)
declare i32 @group_choice()

define i32 @calc(i32 %x) nounwind section ".text.calc" {
  %a = notail call i32 @left(i32 %x)
  %b = notail call i32 @right(i32 %x)
  %choice = notail call i32 @group_choice()
  %sum = add i32 %a, %b
  %selected = add i32 %sum, %choice
  %value = add i32 %selected, ADJUST
  ret i32 %value
}
