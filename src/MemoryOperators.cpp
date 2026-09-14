// This file will make your mod use LeviLamina's memory operators by default.
// This improves the memory management of your mod and is recommended to use.

// This file will make your mod use LeviLamina's memory operators by default.
// （xmake.lua 里也 add_defines 了同一个宏, 这里加守卫免得全量构建时报 C4005 重定义）
#ifndef LL_MEMORY_OPERATORS
#define LL_MEMORY_OPERATORS
#endif

#include "ll/api/memory/MemoryOperators.h" // IWYU pragma: keep
