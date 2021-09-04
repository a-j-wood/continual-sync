/*
 * Command-line interface to the watch_dir function provided in watch.c.
 */

#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>
#include "common.h"

#define MAX_EXCLUDES 1000

extern int watch_dir(const char *toplevel_path,
		     const char *changedpath_dir,
		     unsigned long full_scan_interval,
		     unsigned long queue_run_interval,
		     unsigned long queue_run_max_seconds,
		     unsigned long changedpath_dump_interval,
		     unsigned int max_dir_depth, char **excludes,
		     unsigned int exclude_count);

/* List of command line parameters after options. */
static char **parameters = NULL;
static int parameter_count = 0;

/* Parameters that can be overridden by command line options. */
static unsigned long full_scan_interval = 7200;
static unsigned long queue_run_interval = 2;
static unsigned long queue_run_max_seconds = 5;
static unsigned long changedpath_dump_interval = 30;
static unsigned int max_dir_depth = 20;
static char *excludes[MAX_EXCLUDES];
static unsigned int exclude_count = 0;


/*
 * Output program usage information.
 */
static void usage(void)
{
	printf("%s: %s %s\n", _("Usage"),
	       common_program_name, _("[OPTIONS] DIRECTORY OUTPUTDIR"));
	printf("%s\n",
	       _
	       ("Watch DIRECTORY for changes, dumping the changed paths to a unique file in\nthe OUTPUTDIR directory every few seconds."));
	printf("\n");
	printf("  -i, --dump-interval %s (%lu)\n",
	       _("SEC       interval between writing change files"),
	       changedpath_dump_interval);
	printf("  -f, --full-scan-interval %s (%lu)\n",
	       _("SEC  do full rescan every SEC seconds"),
	       full_scan_interval);
	printf("  -e, --exclude %s (*.tmp, *~)\n",
	       _("PATTERN         glob pattern to exclude"));
	printf("  -r, --recursion-depth %s (%u)\n",
	       _("NUM     max depth to descend directories"),
	       max_dir_depth);
	printf("  -q, --queue-run-interval %s (%lu)\n",
	       _("SEC  inotify queue processing interval"),
	       queue_run_interval);
	printf("  -m, --queue-run-max %s (%lu)\n",
	       _("SEC       max time to spend processing queue"),
	       queue_run_max_seconds);
	printf("\n");
	printf("  -h, --help     %s\n", _("display this help and exit"));
	printf("  -V, --version  %s\n",
	       _("display program version and exit"));
#if ENABLE_DEBUGGING
	printf("  -d, --debug    %s\n", _("enable debugging"));
#endif
	printf("\n");
	printf("%s\n",
	       _
	       ("The OUTPUTDIR must not be under the DIRECTORY being watched."));
}


/*
 * Parse the command line arguments.  Returns 0 on success, -1 if the
 * program should exit immediately without an error, or 1 if the program
 * should exit with an error.
 */
static int parse_options(int argc, char **argv)
{
	struct option long_options[] = {
		{"help", 0, 0, 'h'},
		{"version", 0, 0, 'V'},
		{"full-scan-interval", 1, 0, 'f'},
		{"full", 1, 0, 'f'},
		{"exclude", 1, 0, 'e'},
		{"recursion-depth", 1, 0, 'r'},
		{"queue-run-interval", 1, 0, 'q'},
		{"queue", 1, 0, 'q'},
		{"queue-run-max", 1, 0, 'm'},
		{"max", 1, 0, 'm'},
		{"dump-interval", 1, 0, 'i'},
		{"interval", 1, 0, 'i'},
		{"depth", 1, 0, 'r'},
#if ENABLE_DEBUGGING
		{"debug", 0, 0, 'd'},
#endif
		{0, 0, 0, 0}
	};
	int option_index = 0;
	char *short_options = "hVf:e:r:q:m:i:"
#if ENABLE_DEBUGGING
	    "d"
#endif
	    ;
	int c;
	unsigned long param;

	parameter_count = 0;
	parameters = calloc(argc + 1, sizeof(char *));
	if (NULL == parameters) {
		die("%s", strerror(errno));
		return 1;
	}

	do {
		c = getopt_long(argc, argv, short_options, long_options,
				&option_index);
		if (c < 0)
			continue;

		/*
		 * Parse each command line option.
		 */
		switch (c) {
		case 'h':
			usage();
			free(parameters);
			parameters = NULL;
			parameter_count = 0;
			return -1;
			break;
		case 'V':
			printf("%s %s\n", common_program_name, VERSION);
			free(parameters);
			parameters = NULL;
			parameter_count = 0;
			return -1;
			break;
#if ENABLE_DEBUGGING
		case 'd':
			debugging_enabled = 1;
			break;
#endif
		case 'e':
			if (exclude_count >= (MAX_EXCLUDES - 1)) {
				error("%s",
				      _
				      ("maximum number of excludes reached"));
				free(parameters);
				parameters = NULL;
				parameter_count = 0;
				return 1;
			}
			excludes[exclude_count++] = xstrdup(optarg);
			break;
		case 'f':
		case 'r':
		case 'q':
		case 'm':
		case 'i':
			errno = 0;
			param = strtoul(optarg, NULL, 10);
			if (0 != errno) {
				error("-%c: %s", c, strerror(errno));
				free(parameters);
				parameters = NULL;
				parameter_count = 0;
				return 1;
			}
			switch (c) {
			case 'f':
				full_scan_interval = param;
				break;
			case 'r':
				max_dir_depth = param;
				break;
			case 'q':
				queue_run_interval = param;
				break;
			case 'm':
				queue_run_max_seconds = param;
				break;
			case 'i':
				changedpath_dump_interval = param;
				break;
			}
			break;
		default:
			fprintf(stderr,
				_("Try `%s --help' for more information."),
				common_program_name);
			fprintf(stderr, "\n");
			free(parameters);
			parameters = NULL;
			parameter_count = 0;
			return 1;
			break;
		}

	} while (c != -1);

	/*
	 * Store remaining command-line arguments.
	 */
	while (optind < argc) {
		parameters[parameter_count++] = argv[optind++];
	}

	if (parameter_count != 2) {
		usage();
		free(parameters);
		parameters = NULL;
		parameter_count = 0;
		return 1;
	}

	return 0;
}


/*
 * Command line entry point: parse command line arguments and start the main
 * watch_dir loop.
 */
int main(int argc, char **argv)
{
	char *toplevel_path;		 /* full path to watched dir */
	char *changedpath_dir;		 /* full path to output queue dir */
	int rc;
	int eidx;

	common_program_name = ds_leafname(argv[0]);

	rc = parse_options(argc, argv);
	if (rc < 0)
		return EXIT_SUCCESS;
	else if (rc > 0)
		return EXIT_FAILURE;

	toplevel_path = realpath(parameters[0], NULL);
	if (NULL == toplevel_path) {
		fprintf(stderr, "%s: %s: %s\n", common_program_name,
			parameters[0], strerror(errno));
		exit(EXIT_FAILURE);
	}

	changedpath_dir = realpath(parameters[1], NULL);
	if (NULL == changedpath_dir) {
		fprintf(stderr, "%s: %s: %s\n", common_program_name,
			parameters[1], strerror(errno));
		free(toplevel_path);
		exit(EXIT_FAILURE);
	}

	rc = watch_dir(toplevel_path, changedpath_dir, full_scan_interval,
		       queue_run_interval, queue_run_max_seconds,
		       changedpath_dump_interval, max_dir_depth, excludes,
		       exclude_count);

	free(toplevel_path);
	free(changedpath_dir);

	for (eidx = 0; eidx < exclude_count; eidx++) {
		if (NULL != excludes[eidx])
			free(excludes[eidx]);
	}
	if (NULL != parameters)
		free(parameters);

	return rc;
}

/* EOF */
