
#ifndef _LIBPOP_UTIL_H_
#define _LIBPOP_UTIL_H_

#include <stdio.h>

extern int libpop_verbose;

/* print success (green) */
#define pr_s(fmt, ...) \
	fprintf(stderr,							\
		"\x1b[1m\x1b[32m" PROGNAME ":%d:%s(): " fmt		\
		"\x1b[0m\n",						\
		__LINE__, __func__, ##__VA_ARGS__)

/* print error (red) */
#define pr_e(fmt, ...) \
	fprintf(stderr,							\
		"\x1b[1m\x1b[31m" PROGNAME ":%d:%s(): " fmt		\
		"\x1b[0m\n",						\
		__LINE__, __func__, ##__VA_ARGS__)

#define pr_vs(fmt, ...)					\
	if (libpop_verbose) { pr_s(fmt, ##__VA_ARGS__); }

#define pr_ve(fmt, ...)					\
	if (libpop_verbose) { pr_e(fmt, ##__VA_ARGS__); }


#endif /* _LIBPOP_UTIL_H_ */
