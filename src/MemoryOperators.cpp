// LeviLamina refuses to load a mod that does not route operator new/delete
// through its unified allocator. Exactly one translation unit must define these.
#define LL_MEMORY_OPERATORS

#include "ll/api/memory/MemoryOperators.h" // IWYU pragma: keep
