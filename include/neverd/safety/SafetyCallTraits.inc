// Call-return traits used by the argument prefilter and the implicit-copy
// length capture.  Included with SAFETY_CALL_TRAIT(Name, IsLength, IsBounded)
// defined.  Names are compared after stripLeadingUnderscores().
//
// IsLength: the return is a string/object length (strlen family).
// IsBounded: the return is no larger than an existing object, so a copy
// bounded by it does not depend on attacker-controlled size.  Every length
// call is also bounded.

SAFETY_CALL_TRAIT("strlen", 1, 1)
SAFETY_CALL_TRAIT("strnlen", 1, 1)
SAFETY_CALL_TRAIT("strnlen_s", 1, 1)
SAFETY_CALL_TRAIT("wcslen", 1, 1)
SAFETY_CALL_TRAIT("wcsnlen", 1, 1)
SAFETY_CALL_TRAIT("stat", 0, 1)
SAFETY_CALL_TRAIT("fstat", 0, 1)
SAFETY_CALL_TRAIT("lstat", 0, 1)
SAFETY_CALL_TRAIT("min", 0, 1)
SAFETY_CALL_TRAIT("umin", 0, 1)
