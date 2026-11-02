# Android Bionic error text

The error-number/text catalog in
`lib/emulation/os/linux/android/AndroidErrorStrings.def` is transcribed from
Android NDK 26.2.11394342's ARM64 `libc.a(strerror.o)`,
`__sys_error_strings`, and converted to a macro catalog on 2026-10-22.
The complete original sysroot `NOTICE` is retained beside this file.

Archive SHA-256: `ad1ba5e133e694130529b0f4cf9e3e147ff0f83912215c35aa5cf45ae92b0732`.
Object SHA-256: `888d3baa554aa9d406e5e3929f8c2dfc91ea1ff028df2113b4f4bb314243b7c5`.

NeverD's guest-memory placement and error-text model are independently authored.
This catalog choice is not a claim of exact text equivalence across Android
device releases. The model does not call the host's libc or locale services.
