//
// circle/types.h - Linux compatibility shim for mt32-pi
//
// Provides the subset of Circle's basic types used by the portable parts of
// mt32-pi so that they can be compiled unchanged on Linux.
//

#ifndef _circle_types_h
#define _circle_types_h

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <sys/types.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

typedef uintptr_t uintptr;

typedef int boolean;
#ifndef FALSE
#define FALSE 0
#endif
#ifndef TRUE
#define TRUE 1
#endif

#ifndef PACKED
#define PACKED __attribute__((packed))
#endif

#ifndef ALIGN
#define ALIGN(n) __attribute__((aligned(n)))
#endif

#ifndef KILOBYTE
#define KILOBYTE 0x400
#define MEGABYTE 0x100000
#define GIGABYTE 0x40000000ULL
#endif

#endif
