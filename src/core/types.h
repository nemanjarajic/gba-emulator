#ifndef GBA_CORE_TYPES_H
#define GBA_CORE_TYPES_H

// The dual-compilation macro layer.
//
// Every file in src/core/ is compiled TWICE: once as C++20 for the host
// reference core, and once as GLSL compute for the GPU. GBA_GLSL is defined by
// the shader build; the C++ build defines nothing.
//
// This exists so we get a correctness oracle. Running the same source on the
// CPU and the GPU and diffing register state after every instruction turns
// "the shader renders garbage" into "instruction 0x1234 at PC 0x08000f20
// disagrees", which is the difference between a tractable project and an
// intractable one.
//
// Rules for everything in src/core/ (all are things GLSL cannot express):
//   - no pointers, no references except via INOUT()/OUT()
//   - no recursion, no function pointers, no virtual dispatch
//   - no union, no memcpy, no unaligned access
//   - no 8- or 16-bit types: all memory is uint[] plus shift-and-mask
//   - no struct member initialisers (GLSL forbids them) -- zero with {}
//   - no default arguments, no overloading
//   - integer literals always carry a 'u' suffix; `1 << 31` is UB in both
//     languages, `1u << 31` is defined in both

#ifdef GBA_GLSL

#define U32 uint
#define I32 int
#define INOUT(T) inout T
#define OUT(T) out T
#define CORE_FN
#define KCONST const
#define STATIC_ASSERT(cond, msg)

#else  // ---- C++ ----

#include <cstdint>

#define U32 uint32_t
#define I32 int32_t
#define INOUT(T) T&
#define OUT(T) T&
#define CORE_FN inline
#define KCONST constexpr
#define STATIC_ASSERT(cond, msg) static_assert(cond, msg)

#endif

// Rotate right. Neither language has a builtin, and ARM needs this constantly
// (barrel shifter ROR, and the rotation LDR applies on an unaligned address).
// The r == 0 guard matters: a shift by 32 is undefined in both languages.
CORE_FN U32 ror32(U32 x, U32 r) {
    r = r & 31u;
    if (r == 0u) return x;
    return (x >> r) | (x << (32u - r));
}

// Sign-extend the low `bits` of x to 32 bits.
CORE_FN U32 sign_extend(U32 x, U32 bits) {
    U32 shift = 32u - bits;
    return U32(I32(x << shift) >> I32(shift));
}

#endif  // GBA_CORE_TYPES_H
