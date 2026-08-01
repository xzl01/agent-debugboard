#ifndef ZEPHYR_SHELL_SHELL_H_
#define ZEPHYR_SHELL_SHELL_H_

struct shell {
	int unused;
};

void shell_print(const struct shell *sh, const char *format, ...);
void shell_error(const struct shell *sh, const char *format, ...);

#endif
