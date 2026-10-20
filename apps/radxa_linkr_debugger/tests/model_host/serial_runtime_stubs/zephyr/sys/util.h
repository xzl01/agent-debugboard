#ifndef LINKR_DEBUGGER_SERIAL_RUNTIME_STUB_UTIL_H_
#define LINKR_DEBUGGER_SERIAL_RUNTIME_STUB_UTIL_H_

#define BIT(n) (1UL << (n))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define IS_ENABLED(...) 0
#define ARG_UNUSED(arg) ((void)(arg))
#define BUILD_ASSERT(condition, message) _Static_assert(condition, message)

#endif
