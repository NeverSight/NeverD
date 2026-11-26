$group_increment = comdat any

define linkonce_odr i32 @group_increment(i32 %x) noinline nounwind comdat {
  %value = add i32 %x, 1
  ret i32 %value
}

define weak i32 @group_choice() noinline nounwind {
  ret i32 CHOICE
}

define i32 @SIDE(i32 %x) nounwind {
  %value = notail call i32 @group_increment(i32 %x)
  ret i32 %value
}
