/* SPDX-License-Identifier: LGPL-3.0-or-later */
void linkr_pd_test_log_warning(const char *format, ...);

#define LOG_MODULE_REGISTER(...)
#define LOG_INF(...)
#define LOG_WRN(...) linkr_pd_test_log_warning(__VA_ARGS__)
