/*
 *  Minimal stand-in for <orc/orc.h>.
 *
 *  schroedinger is built with -DDISABLE_ORC, so every orc program in
 *  schroorc-dist.c takes its plain-C branch and the orc runtime is never
 *  entered. What is still needed from this header is four names: two integer
 *  typedefs, and the two memory helpers orc exposes as thin wrappers.
 */
#ifndef SCHRO_ORC_SHIM_H
#define SCHRO_ORC_SHIM_H

#include <stdint.h>
#include <string.h>

typedef int16_t orc_int16;
typedef int32_t orc_int32;
typedef int64_t orc_int64;
typedef uint8_t orc_uint8;
typedef uint16_t orc_uint16;
typedef uint32_t orc_uint32;
typedef uint64_t orc_uint64;


/* Opaque: schromotion.h keeps OrcProgram* fields in a struct unconditionally,
 * but with DISABLE_ORC nothing ever dereferences them. */
typedef struct _OrcProgram OrcProgram;
typedef struct _OrcExecutor OrcExecutor;

/* schro.c calls orc_init() from schro_init(); with DISABLE_ORC there is
 * nothing to initialise. */
static inline void orc_init(void) { }

static inline void orc_memcpy(void *dst, const void *src, int n) { memcpy(dst, src, (size_t)n); }
static inline void orc_memset(void *dst, int c, int n) { memset(dst, c, (size_t)n); }

#endif
