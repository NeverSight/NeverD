# Owned ELF fixture exposing a Python extension initializer and a producer marker.
.text
.globl PyInit_owned
.type PyInit_owned,@function
PyInit_owned:
    ret
.size PyInit_owned, .-PyInit_owned
.globl __pyx_owned_marker
.type __pyx_owned_marker,@function
__pyx_owned_marker:
    ret
.size __pyx_owned_marker, .-__pyx_owned_marker
