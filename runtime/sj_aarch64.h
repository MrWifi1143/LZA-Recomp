// AArch64 Linux: LLVM has no __builtin_longjmp there, so the game code's setjmp points (tools/gen.py: a host
// __builtin_setjmp at every guest setjmp call) use the runtime's own pair instead (runtime/dispatch.c rt_sj_set /
// rt_sj_long), as on Windows (cpu.h). Force-included into the game code on AArch64 (Makefile GENFLAGS) - outside cpu.h,
// so the x86-64 / Windows game libraries' GAMELIB_ID stays the same - and included by dispatch.c.
#pragma once
#if defined(__aarch64__) && !defined(_WIN32)
int rt_sj_set(void **buf) __attribute__((returns_twice));
void rt_sj_long(void **buf, int v) __attribute__((noreturn));
#define __builtin_setjmp(b) rt_sj_set(b)
#define __builtin_longjmp(b, v) rt_sj_long(b, v)
#endif
