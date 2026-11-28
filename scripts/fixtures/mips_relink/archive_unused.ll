declare i32 @unselected_dependency()

define i32 @unused_member() nounwind {
  %value = notail call i32 @unselected_dependency()
  ret i32 %value
}
