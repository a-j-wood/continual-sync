/*
 * Functions and variables common to all programs.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <syslog.h>
#include "common.h"

flag_t debugging_enabled = 0;		 /* global flag to enable debugging */
flag_t using_syslog = 0;		 /* global flag to report to syslog */
char *common_program_name = "";		 /* set this to program leafname */
int error_count = 0;			 /* global error counter from error() */

#if ENABLE_DEBUGGING
/*
 * In debug mode, output the given formatted string to stderr.
 */
void debug(const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	if (debugging_enabled) {
		time_t t;
		struct tm *tm;
		char tbuf[128];
		time(&t);
		tm = localtime(&t);
		tbuf[0] = 0;
		strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", tm);
		fprintf(stderr, "[%s] ", tbuf);
		vfprintf(stderr, format, ap);
		fprintf(stderr, "\n");
	}
	va_end(ap);
}
#endif				/* ENABLE_DEBUGGING */


/*
 * Output an error, and increment the error counter.
 */
void error(const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	fprintf(stderr, "%s: ", common_program_name);
	vfprintf(stderr, format, ap);
	fprintf(stderr, "\n");
	va_end(ap);
	if (using_syslog) {
		va_start(ap, format);
		vsyslog(LOG_ERR, format, ap);
		va_end(ap);
	}
	error_count++;
}


/*
 * Output an error and exit the program.
 */
void die(const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	fprintf(stderr, "%s: ", common_program_name);
	vfprintf(stderr, format, ap);
	fprintf(stderr, "\n");
	va_end(ap);
	if (using_syslog) {
		va_start(ap, format);
		vsyslog(LOG_ERR, format, ap);
		va_end(ap);
	}
	exit(1);
}


/*
 * Return the string position of the character just after the last '/' in
 * the given path.
 */
int ds_leafname_pos(char *pathname)
{
	int leafpos;

	if (NULL == pathname)
		return 0;

	leafpos = strlen(pathname);

	while ((leafpos > 0) && (pathname[leafpos] != '/')) {
		leafpos--;
	}

	if (pathname[leafpos] == '/')
		leafpos++;

	return leafpos;
}


/*
 * Utility function to return the leafname of the given path.
 */
char *ds_leafname(char *pathname)
{
	return &(pathname[ds_leafname_pos(pathname)]);
}


/*
 * Open a securely named hidden temporary file based on the given absolute
 * path, and return the file descriptor, filling in the malloced name into
 * tmpnameptr (*tmpnameptr will need to be freed by the caller).
 *
 * Returns -1 on error, in which case nothing is put into tmpnameptr.
 */
int ds_tmpfile(char *pathname, char **tmpnameptr)
{
	int leafpos, fd;
	char *temporary_name;

	if (NULL == pathname)
		return -1;
	if (NULL == tmpnameptr)
		return -1;

	leafpos = ds_leafname_pos(pathname);

	if (asprintf
	    (&temporary_name, "%.*s/.%sXXXXXX", leafpos - 1, pathname,
	     &(pathname[leafpos])) < 0) {
		die("%s: %s", "asprintf", strerror(errno));
		return -1;
	}

	fd = mkstemp(temporary_name);
	if (0 > fd) {
		die("%s: %s: %s", temporary_name, "mkstemp",
		    strerror(errno));
		free(temporary_name);
		return -1;
	}

	*tmpnameptr = temporary_name;
	return fd;
}


#if ENABLE_SETPROCTITLE
/* For setproctitle */
#ifndef SPT_BUFSIZE
#define SPT_BUFSIZE     2048
#endif
#endif				/* ENABLE_SETPROCTITLE */

#if ENABLE_SETPROCTITLE
/* The below is taken from util-linux-ng */

/* proctitle code - we know this to work only on linux... */

/*
**  SETPROCTITLE -- set process title for ps (from sendmail)
**
**      Parameters:
**              fmt -- a printf style format string.
**
**      Returns:
**              none.
**
**      Side Effects:
**              Clobbers argv of our main procedure so ps(1) will
**              display the title.
*/

extern char **environ;

static char **argv0;
static int argv_lth;

void initproctitle(int argc, char **argv)
{
	int i;
	char **envp = environ;

	/*
	 * Move the environment so we can reuse the memory.
	 * (Code borrowed from sendmail.)
	 * WARNING: ugly assumptions on memory layout here;
	 *          if this ever causes problems, #undef DO_PS_FIDDLING
	 */
	for (i = 0; envp[i] != NULL; i++)
		continue;
	environ = (char **) malloc(sizeof(char *) * (i + 1));
	if (environ == NULL)
		return;
	for (i = 0; envp[i] != NULL; i++)
		if ((environ[i] = strdup(envp[i])) == NULL)
			return;
	environ[i] = NULL;

	argv0 = argv;
	if (i > 0)
		argv_lth = envp[i - 1] + strlen(envp[i - 1]) - argv0[0];
	else
		argv_lth =
		    argv0[argc - 1] + strlen(argv0[argc - 1]) - argv0[0];
}

void setproctitle(const char *fmt, ...)
{
	int i;
	char buf[SPT_BUFSIZE];
	va_list ap;

	if (!argv0)
		return;

	va_start(ap, fmt);
	(void) vsnprintf(buf, SPT_BUFSIZE, fmt, ap);
	va_end(ap);

	i = strlen(buf);
	if (i > argv_lth - 2) {
		i = argv_lth - 2;
		buf[i] = '\0';
	}
	memset(argv0[0], '\0', argv_lth);   /* clear the memory area */
	(void) strcpy(argv0[0], buf);

	argv0[1] = NULL;
}
#endif				/* ENABLE_SETPROCTITLE */


/*
 * Like strdup but die() on failure.
 */
char *xstrdup(const char *str)
{
	char *ptr;
	ptr = strdup(str);
	if (NULL == ptr)
		die("%s", strerror(errno));
	return ptr;
}

/* EOF */
