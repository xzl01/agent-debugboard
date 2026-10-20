/* SPDX-License-Identifier: LGPL-3.0-or-later */
#ifndef ZEPHYR_SHELL_SHELL_H_
#define ZEPHYR_SHELL_SHELL_H_

#include <stddef.h>

#ifndef ARG_UNUSED
#define ARG_UNUSED(value) ((void)(value))
#endif

struct shell {
	int unused;
};

void shell_print(const struct shell *sh, const char *format, ...);
void shell_error(const struct shell *sh, const char *format, ...);
void shell_fprintf(const struct shell *sh, int level, const char *format, ...);

#define SHELL_NORMAL 0

/* Keep every command function referenced so -Wunused-function stays clean
 * without registering real Zephyr shell entries in the host test. */
#define SHELL_CMD(name, subcmd, help, func) (const void *)(func)
#define SHELL_CMD_ARG(name, subcmd, help, func, mandatory, optional) \
	(const void *)(func)
#define SHELL_STATIC_SUBCMD_SET_CREATE(name, ...) \
	static const void *const name[] = { __VA_ARGS__ }
#define SHELL_SUBCMD_SET_END NULL
#define SHELL_CMD_REGISTER(name, subcmd, help, func) \
	static const void *const __attribute__((unused)) shell_register_##name = \
		(const void *)(func)
#define SHELL_CMD_ARG_REGISTER(name, subcmd, help, func, mandatory, optional) \
	static const void *const __attribute__((unused)) shell_register_##name = \
		(const void *)(func)

#endif /* ZEPHYR_SHELL_SHELL_H_ */
