
The `--unary-owner` variant independently compiles `Core<T>` implementing
`View<T>`. Its private target constructor retains the complete nested
`Function1<? super Core<T>, Boolean>` Signature. The genuine compiler access
constructor has the same raw ABI and one `DefaultConstructorMarker` argument.
Original/frozen callback observers check the exact receiver, partial fields,
wide bits, return casts and exceptions. The only source metadata difference is
the access constructor's reported synthetic flag.
