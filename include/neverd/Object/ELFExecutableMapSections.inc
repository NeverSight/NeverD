// ELF output sections that carry executable symbols in linker map files.
// Included from SectionNames.h with ELF_EXECUTABLE_MAP_SECTION(Section) defined.
// StringSwitch::Case requires string literals; values must match
// section_names::elf::{Init,Fini,Plt,PltGot}.

ELF_EXECUTABLE_MAP_SECTION(".init")
ELF_EXECUTABLE_MAP_SECTION(".fini")
ELF_EXECUTABLE_MAP_SECTION(".plt")
ELF_EXECUTABLE_MAP_SECTION(".plt.got")
