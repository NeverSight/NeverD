// The types phnt takes from Microsoft's SDK that mingw-w64's headers do not
// declare, for scripts/generate_windows_types.py.

// winnt.h: which datum RtlGetSystemGlobalData reads, an enumeration whose
// 32-bit value phnt passes.
typedef ULONG RTL_SYSTEM_GLOBAL_DATA_ID;

// winioctl.h: the storage reserve areas of a volume, as Microsoft documents
// them.
typedef enum _STORAGE_RESERVE_ID {
  StorageReserveIdNone = 0,
  StorageReserveIdHard,
  StorageReserveIdSoft,
  StorageReserveIdUpdateScratch,
  StorageReserveIdMax
} STORAGE_RESERVE_ID, *PSTORAGE_RESERVE_ID;
