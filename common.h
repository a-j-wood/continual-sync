/*
 * Header for functions common to all programs.
 */

#ifndef COMMON_H
#define COMMON_H 1

#ifndef _STDINT_H
#include <stdint.h>
#endif	/* _STDINT_H */

#ifndef VERSION
#define VERSION "0.0.1"
#endif

#define ENABLE_DEBUGGING 1
#define ENABLE_SETPROCTITLE 1

#define _(x) x

typedef int_fast8_t flag_t;

extern flag_t debugging_enabled;	 /* global flag to enable debugging */
extern flag_t using_syslog;		 /* global flag to report to syslog */
extern char *common_program_name;	 /* set this to program leafname */
extern int error_count;			 /* global error counter from error() */

#if ENABLE_DEBUGGING
void debug(const char *, ...);
#else				/* ENABLE_DEBUGGING */
#define debug(x,...)
#endif				/* ENABLE_DEBUGGING */
void error(const char *, ...);
void die(const char *, ...);

int ds_leafname_pos(char *pathname);
char *ds_leafname(char *pathname);
int ds_tmpfile(char *pathname, char **tmpnameptr);


#if ENABLE_SETPROCTITLE
void initproctitle(int, char **);
void setproctitle(const char *, ...);
#else				/* ENABLE_SETPROCTITLE */
#define initproctitle(x,y)
#define setproctitle(x,...)
#endif				/* ENABLE_SETPROCTITLE */
char *xstrdup(const char *);

#endif	/* COMMON_H */

/* EOF */
