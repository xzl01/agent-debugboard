#ifndef RADXA_LINKR_DEBUGGER_CONFIG_SHELL_H_
#define RADXA_LINKR_DEBUGGER_CONFIG_SHELL_H_

#include <stddef.h>

struct shell;

int linkr_debugger_config_shell_show(const struct shell *sh, size_t argc,
				     char **argv);
int linkr_debugger_config_shell_save(const struct shell *sh, size_t argc,
				     char **argv);
int linkr_debugger_config_shell_apply(const struct shell *sh, size_t argc,
				      char **argv);
int linkr_debugger_config_shell_clear(const struct shell *sh, size_t argc,
				      char **argv);

#endif
