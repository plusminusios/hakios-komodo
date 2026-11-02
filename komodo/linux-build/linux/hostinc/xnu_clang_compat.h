/*
 * Force-included into every XNU translation unit when building with stock
 * (non-Apple) clang. Apple's clang has builtins that describe the pointer/data
 * layout of a type for kalloc_type; upstream clang lacks them.
 *
 * The summary says "padding and data, no pointers": 8796.141.3 asserts in OSData.h
 * that a value type is data-only unless it is inserted with kValueNoCopy, and a
 * summary with pointer granules would trip that assert for every type. Data-only is
 * the permissive answer; the serialized signature ("12") is not used for anything
 * but keeping the type system's records well formed.
 */
#ifndef XNU_CLANG_COMPAT_H
#define XNU_CLANG_COMPAT_H
#if !defined(ASSEMBLER) && !defined(__ASSEMBLER__)
#if !__has_builtin(__builtin_xnu_type_signature)
#define XNU_NO_TYPE_BUILTINS                     1
#define __builtin_xnu_type_signature(type)       "12"
#define __builtin_xnu_type_summary(type)         ((1U << 0) | (1U << 2))
#ifdef __cplusplus
#define __builtin_xnu_types_compatible(a, b)     __is_same(a, b)
#else
#define __builtin_xnu_types_compatible(a, b)     __builtin_types_compatible_p(a, b)
#endif
#endif
#endif
#endif
