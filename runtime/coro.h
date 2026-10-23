/* Minimal coroutines: one native stack per game thread.
 * Windows uses Fibers; everything else uses ucontext. */
#pragma once
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
typedef struct { LPVOID fiber; } Coro;
static inline void coro_init_main(Coro* c) { c->fiber = ConvertThreadToFiber(NULL); if (!c->fiber) c->fiber = GetCurrentFiber(); }
static inline void coro_create(Coro* c, void (*fn)(void), size_t stack) {
    c->fiber = CreateFiber(stack, (LPFIBER_START_ROUTINE)(void*)fn, NULL);
}
static inline void coro_switch(Coro* from, Coro* to) { (void)from; SwitchToFiber(to->fiber); }
#else
#include <ucontext.h>
typedef struct { ucontext_t ctx; void* stack; } Coro;
static inline void coro_init_main(Coro* c) { c->stack = NULL; }
static inline void coro_create(Coro* c, void (*fn)(void), size_t stack) {
    getcontext(&c->ctx);
    c->stack = malloc(stack);
    c->ctx.uc_stack.ss_sp = c->stack;
    c->ctx.uc_stack.ss_size = stack;
    c->ctx.uc_link = NULL;
    makecontext(&c->ctx, fn, 0);
}
static inline void coro_switch(Coro* from, Coro* to) { swapcontext(&from->ctx, &to->ctx); }
#endif
