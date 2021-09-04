/*
 * Configuration parser and main loop to manage multiple concurrent
 * synchronisation sets, providing an interface to the continual
 * synchronisation function continual_sync in sync.c.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <getopt.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <wordexp.h>
#include <fnmatch.h>
#include <syslog.h>
#include "sync.h"


/* List of config sections. */
static struct sync_set_s config_sections[MAX_CONFIG_SECTIONS];
static int config_sections_count = 0;

/* List of config sections chosen on the command line. */
static char **config_sections_selected = NULL;
static int config_sections_selected_count = 0;

static char *pidfile = NULL;		 /* PID file if in daemon mode */
flag_t sync_exit_now = 0;		 /* exit-now flag (on signal) */


/*
 * Return the index of the config section with the given name, or -1 on
 * failure.
 */
static int find_config_section(const char *name)
{
	int idx;

	for (idx = 0; idx < config_sections_count; idx++) {
		if (NULL == config_sections[idx].name)
			continue;
		if (strcmp(config_sections[idx].name, name) == 0)
			return idx;
	}

	return -1;
}


/*
 * Expand %s, %h etc in the string pointed to by strptr, reallocating the
 * string if necessary, returning nonzero on error (and reporting the
 * error).
 */
static int expand_config_sequences(struct sync_set_s *cf, char **strptr,
				   char *parameter)
{
	int orig_string_len = 0;
	int new_string_len = 0;
	char *origstr;
	char *newstr;
	int idx;

	if (NULL == strptr)
		return 0;
	if (NULL == *strptr)
		return 0;

	origstr = *strptr;
	orig_string_len = strlen(origstr);
	newstr = NULL;

	idx = 0;
	while (idx < orig_string_len) {
		int section_length;
		char *to_append;
		int to_append_len;

		section_length = strcspn(&(origstr[idx]), "%");
		if (0 < section_length) {
			newstr =
			    realloc(newstr,
				    new_string_len + section_length + 1);
			if (NULL == newstr) {
				error("%s: %s", "realloc",
				      strerror(errno));
				return 1;
			}
			strncpy(&(newstr[new_string_len]), &(origstr[idx]),
				section_length);
			new_string_len += section_length;
			newstr[new_string_len] = 0;
		}
		idx += section_length;
		if (idx >= orig_string_len)
			break;

		idx++;

		switch (origstr[idx]) {
		case '%':
			to_append = "%";
			to_append_len = 1;
			break;
		case 'n':
			to_append = cf->name;
			to_append_len = strlen(cf->name);
			break;
		case 's':
			to_append = cf->source;
			to_append_len = strlen(cf->source);
			break;
		case 'd':
			to_append = strrchr(cf->destination, ':');
			if (NULL != to_append) {
				to_append++;
			} else {
				to_append = cf->destination;
			}
			to_append_len = strlen(to_append);
			break;
		case 'h':
			to_append = cf->destination;
			to_append_len = strcspn(to_append, ":");
			if (to_append_len == strlen(to_append)) {
				to_append = "localhost";
				to_append_len = strlen(to_append);
			}
			break;
		default:
			error("%s: %s: %s: %%%c", cf->name, parameter,
			      _("invalid variable substitution"),
			      origstr[idx]);
			if (NULL != newstr)
				free(newstr);
			return 1;
		}

		idx++;

		if (0 < to_append_len) {
			newstr =
			    realloc(newstr,
				    new_string_len + to_append_len + 1);
			if (NULL == newstr) {
				error("%s: %s", "realloc",
				      strerror(errno));
				return 1;
			}
			strncpy(&(newstr[new_string_len]), to_append,
				to_append_len);
			new_string_len += to_append_len;
			newstr[new_string_len] = 0;
		}
	}

	if (NULL == newstr)
		return 0;

	if (strcmp(newstr, origstr) == 0) {
		free(newstr);
		return 0;
	}

	debug("(cf) %s: %s: [%s] -> [%s]", cf->name, parameter, origstr,
	      newstr);

	free(origstr);
	*strptr = newstr;

	return 0;
}


/*
 * Return nonzero if the configuration section with the given index is not
 * valid, and report the error.
 *
 * As a side effect, fills in the defaults from DEFAULTS_SECTION and expands
 * the parameters, if this isn't the DEFAULTS_SECTION section.
 */
static int validate_config_section(int idx, int defaults_idx)
{
	int rc = 0;

	if (strcmp(config_sections[idx].name, DEFAULTS_SECTION) == 0) {
		if (NULL != config_sections[idx].source) {
			error("%s: %s", config_sections[idx].name,
			      _("default source directory not allowed"));
			rc = 1;
		} else if (NULL != config_sections[idx].destination) {
			error("%s: %s", config_sections[idx].name,
			      _
			      ("default destination directory not allowed"));
			rc = 1;
		}
		debug("(cf valid) %d %s: %s", idx,
		      config_sections[idx].name,
		      rc == 0 ? "OK" : "FAILED");
		return rc;
	} else {
		if (NULL == config_sections[idx].source) {
			error("%s: %s", config_sections[idx].name,
			      _("no source directory defined"));
			rc = 1;
		} else if (NULL == config_sections[idx].destination) {
			error("%s: %s", config_sections[idx].name,
			      _("no destination directory defined"));
			rc = 1;
		}
	}

	/*
	 * Fill in the defaults from DEFAULT_SECTION, if there are any.
	 */
	if (0 <= defaults_idx) {
#define dup_default_string(x) if ((NULL == config_sections[idx].x) && (NULL != config_sections[defaults_idx].x)) { \
config_sections[idx].x = xstrdup(config_sections[defaults_idx].x); \
debug("(cf) %s: %s: %s -> %s", config_sections[idx].name, #x, "using default", config_sections[defaults_idx].x); \
}
		dup_default_string(source_validation);
		dup_default_string(destination_validation);
		dup_default_string(full_marker);
		dup_default_string(partial_marker);
		dup_default_string(change_queue);
		dup_default_string(transfer_list);
		dup_default_string(tempdir);
		dup_default_string(sync_lock);
		dup_default_string(full_rsync_opts);
		dup_default_string(partial_rsync_opts);
		dup_default_string(log_file);
		dup_default_string(status_file);
#define copy_default_ulong(x) if ((0 == config_sections[idx].set.x) && (0 != config_sections[defaults_idx].set.x)) { \
config_sections[idx].x = config_sections[defaults_idx].x; \
debug("(cf) %s: %s: %s -> %lu", config_sections[idx].name, #x, "using default", config_sections[defaults_idx].x); \
}
		copy_default_ulong(full_interval);
		copy_default_ulong(full_retry);
		copy_default_ulong(partial_interval);
		copy_default_ulong(partial_retry);
		copy_default_ulong(recursion_depth);

		if ((0 == config_sections[idx].exclude_count)
		    && (0 != config_sections[defaults_idx].exclude_count)) {
			int eidx;
			debug("(cf) %s: %s", config_sections[idx].name,
			      "using excludes from defaults section");
			config_sections[idx].exclude_count =
			    config_sections[defaults_idx].exclude_count;
			for (eidx = 0;
			     eidx <
			     config_sections[defaults_idx].exclude_count;
			     eidx++) {
				config_sections[idx].excludes[eidx] =
				    xstrdup(config_sections
					    [defaults_idx].excludes[eidx]);
			}
		}
	}

	/*
	 * Expand all %s, %h, etc special sequences before continuing.
	 */
#define expand_sequences(x) if (expand_config_sequences(&(config_sections[idx]), &(config_sections[idx].x), #x) != 0) rc=1;
	expand_sequences(source_validation);
	expand_sequences(destination_validation);
	expand_sequences(full_marker);
	expand_sequences(partial_marker);
	expand_sequences(change_queue);
	expand_sequences(transfer_list);
	expand_sequences(tempdir);
	expand_sequences(sync_lock);
	expand_sequences(full_rsync_opts);
	expand_sequences(partial_rsync_opts);
	expand_sequences(log_file);
	expand_sequences(status_file);

	if (NULL != config_sections[idx].change_queue) {
		struct stat sb;
		if (lstat(config_sections[idx].change_queue, &sb) != 0) {
			error("%s: %s: %s", config_sections[idx].name,
			      config_sections[idx].change_queue,
			      strerror(errno));
			rc = 1;
		} else if (!S_ISDIR(sb.st_mode)) {
			error("%s: %s: %s", config_sections[idx].name,
			      config_sections[idx].change_queue,
			      _("not a directory"));
			rc = 1;
		}
	}

	if (NULL != config_sections[idx].tempdir) {
		struct stat sb;
		if (lstat(config_sections[idx].tempdir, &sb) != 0) {
			error("%s: %s: %s", config_sections[idx].name,
			      config_sections[idx].tempdir,
			      strerror(errno));
			rc = 1;
		} else if (!S_ISDIR(sb.st_mode)) {
			error("%s: %s: %s", config_sections[idx].name,
			      config_sections[idx].tempdir,
			      _("not a directory"));
			rc = 1;
		}
	}

	if ((0 == config_sections[idx].full_interval)
	    && (0 == config_sections[idx].partial_interval)) {
		error("%s: %s", config_sections[idx].name,
		      _
		      ("both full and partial intervals are 0 - section would do nothing"));
		rc = 1;
	}
#define blank_if_none(x) if ((NULL != config_sections[idx].x) && (strcmp(config_sections[idx].x, "none") == 0)) { \
        free(config_sections[idx].x); \
        config_sections[idx].x = NULL; \
}
	blank_if_none(source_validation);
	blank_if_none(destination_validation);
	blank_if_none(full_marker);
	blank_if_none(partial_marker);
	blank_if_none(change_queue);
	blank_if_none(transfer_list);
	blank_if_none(tempdir);
	blank_if_none(sync_lock);
	blank_if_none(log_file);
	blank_if_none(status_file);

	debug("(cf valid) %d %s: %s", idx, config_sections[idx].name,
	      rc == 0 ? "OK" : "FAILED");

	return rc;
}


/*
 * Parse the given configuration file, returning nonzero on error.
 */
static int parse_config(const char *filename, int depth)
{
	char linebuf[4096] = { 0, };
	int lineno;
	FILE *fptr;
	struct sync_set_s *section;

	if (depth > 3) {
		debug("(cf) %s: %s", filename,
		      "max recursion depth reached - ignoring file");
		return 0;
	}

	debug("(cf) %s: %s", filename, "opening file");

	fptr = fopen(filename, "r");
	if (NULL == fptr) {
		error("%s: %s", filename, strerror(errno));
		return 1;
	}

	lineno = 0;
	section = NULL;

	while ((!feof(fptr)) && (!ferror(fptr))
	       && (NULL != fgets(linebuf, sizeof(linebuf) - 1, fptr))) {
		unsigned long param_ulong;
		char param_str[4096];
		int idx, len;

		lineno++;

		if (sscanf(linebuf, " [%999[0-9A-Za-z_.-]]", param_str) ==
		    1) {

			/* New section */
			debug("(cf) %s: %d: %s: %s", filename, lineno,
			      "section", param_str);

			if (find_config_section(param_str) >= 0) {
				error("%s: %d: %s: %s", filename, lineno,
				      param_str,
				      _("section already defined"));
				fclose(fptr);
				return 1;
			}

			if (config_sections_count >=
			    (MAX_CONFIG_SECTIONS - 1)) {
				error("%s: %s: %s", filename, param_str,
				      _
				      ("maximum number of sections reached"));
				fclose(fptr);
				return 1;
			}

			section =
			    &(config_sections[config_sections_count]);
			config_sections_count++;

			memset(section, 0, sizeof(config_sections[0]));
			section->name = xstrdup(param_str);
			section->full_interval = 86400;
			section->full_retry = 3600;
			section->partial_interval = 30;
			section->partial_retry = 300;
			section->recursion_depth = 20;

			continue;

		} else
		    if (sscanf(linebuf, " include = %4095[^\n]", param_str)
			== 1) {
			wordexp_t p;
			int word_idx;
			int cwdfd;
			char *resolved_path;
			char *ptr;

			/* Include another config file */
			debug("(cf) %s: %d: %s: %s", filename, lineno,
			      "include", param_str);

			/*
			 * Temporarily change directory to the location of
			 * the current configuration file, so that include
			 * paths are relative to the current file.
			 */
			cwdfd = open(".", O_RDONLY | O_DIRECTORY);
			resolved_path = realpath(filename, NULL);
			if (NULL == resolved_path) {
				debug("(cf) %s: %s: %s", filename,
				      "realpath", strerror(errno));
			} else {
				ptr = strrchr(resolved_path, '/');
				if (NULL != ptr)
					ptr[0] = 0;
				if (0 <= cwdfd) {
					int rc;
					rc = chdir(resolved_path);
					debug("(cf) chdir: %s = %d",
					      resolved_path, rc);
				}
				free(resolved_path);
			}

			if (wordexp(param_str, &p, WRDE_NOCMD) != 0) {
				error("%s: %d: %s: %s", filename, lineno,
				      _("failed to parse include line"),
				      strerror(errno));
				fclose(fptr);
				if (0 <= cwdfd) {
					int ignored_rc;
					ignored_rc = fchdir(cwdfd);
					close(cwdfd);
				}
				return 1;
			}

			if (0 <= cwdfd) {
				int ignored_rc;
				ignored_rc = fchdir(cwdfd);
				close(cwdfd);
			}

			for (word_idx = 0; word_idx < p.we_wordc;
			     word_idx++) {
				if (access(p.we_wordv[word_idx], F_OK) !=
				    0) {
					debug("(cf) %s: %s: %s",
					      p.we_wordv[word_idx],
					      "skipping", strerror(errno));
					continue;
				}
				if ((fnmatch
				     ("*~", p.we_wordv[word_idx],
				      FNM_NOESCAPE) == 0)
				    ||
				    (fnmatch
				     ("*.rpmsave", p.we_wordv[word_idx],
				      FNM_NOESCAPE) == 0)
				    ||
				    (fnmatch
				     ("*.rpmorig", p.we_wordv[word_idx],
				      FNM_NOESCAPE) == 0)
				    ||
				    (fnmatch
				     ("*.rpmnew", p.we_wordv[word_idx],
				      FNM_NOESCAPE) == 0)
				    ) {
					debug("(cf) %s: %s: %s",
					      p.we_wordv[word_idx],
					      "skipping", "ignored");
					continue;
				}
				if (parse_config
				    (p.we_wordv[word_idx],
				     depth + 1) != 0) {
					wordfree(&p);
					return 1;
				}
			}

			wordfree(&p);

			continue;

		} else if (NULL == section) {

			/* Not in a section at the moment */
			int idx = 0, len = strlen(linebuf);
			while ((idx < len) && isspace(linebuf[idx]))
				idx++;
			if (NULL == strchr("#\r\n", linebuf[idx])) {
				error("%s: %d: %s", filename, lineno,
				      _
				      ("must start a section declaration first"));
				fclose(fptr);
				return 1;
			}
			continue;
		}

		/*
		 * If we get here, then we're in a section declaration.
		 */

		/*
		 * Strip comments - a hash that's either at the start of the
		 * line or preceded by whitespace.
		 */
		len = strlen(linebuf);
		for (idx = 0; idx < len; idx++) {
			if ('#' != linebuf[idx])
				continue;
			if ((idx > 0) && (!isspace(linebuf[idx - 1])))
				continue;
			linebuf[idx] = '\0';
			break;
		}

		/*
		 * Strip trailing whitespace.
		 */
		len = strlen(linebuf);
		for (idx = len - 1; idx > 0; idx--) {
			if (!isspace(linebuf[idx]))
				break;
			linebuf[idx] = '\0';
		}

#define cf_string(X, Y) if (sscanf(linebuf, " " X, param_str) == 1) { \
debug("(cf) %s: %d: %s = [%s]", filename, lineno, #Y, param_str); \
section->Y = xstrdup(param_str); \
continue; \
}
#define cf_ulong(X, Y) if (sscanf(linebuf, X, &param_ulong) == 1) { \
debug("(cf) %s: %d: %s = [%lu]", filename, lineno, #Y, param_ulong); \
section->Y = param_ulong; \
section->set.Y = 1; \
continue; \
}
		cf_string("source = %4095[^\n]", source);
		cf_string("destination = %4095[^\n]", destination);
		cf_string("source validation command = %4095[^\n]",
			  source_validation);
		cf_string("destination validation command = %4095[^\n]",
			  destination_validation);
		cf_ulong("full sync interval = %lu", full_interval);
		cf_ulong("full sync retry = %lu", full_retry);
		cf_ulong("partial sync interval = %lu", partial_interval);
		cf_ulong("partial sync retry = %lu", partial_retry);
		cf_ulong("recursion depth = %lu", recursion_depth);
		cf_string("full sync marker file = %4095[^\n]",
			  full_marker);
		cf_string("partial sync marker file = %4095[^\n]",
			  partial_marker);
		cf_string("change queue = %4095[^\n]", change_queue);
		cf_string("transfer list = %4095[^\n]", transfer_list);
		cf_string("temporary directory = %4095[^\n]", tempdir);
		cf_string("sync lock = %4095[^\n]", sync_lock);
		cf_string("full rsync options = %4095[^\n]",
			  full_rsync_opts);
		cf_string("partial rsync options = %4095[^\n]",
			  partial_rsync_opts);
		cf_string("log file = %4095[^\n]", log_file);
		cf_string("status file = %4095[^\n]", status_file);

		if (sscanf(linebuf, " exclude = %4095[^\n]", param_str) ==
		    1) {
			debug("(cf) %s: %d: %s = [%s]", filename, lineno,
			      "exclude", param_str);
			if (section->exclude_count >= (MAX_EXCLUDES - 1)) {
				error("%s: %d: %s", filename, lineno,
				      _
				      ("maximum number of excludes reached"));
				fclose(fptr);
				return 1;
			}
			section->excludes[section->exclude_count++] =
			    xstrdup(param_str);
			continue;
		}

		/*
		 * If we get here, it's either a blank line, a comment, or
		 * an invalid directive.
		 */
		idx = 0;
		len = strlen(linebuf);
		while ((idx < len) && isspace(linebuf[idx]))
			idx++;
		if (NULL == strchr("#\r\n", linebuf[idx])) {
			error("%s: %d: %s", filename, lineno,
			      _("invalid configuration directive"));
			fclose(fptr);
			return 1;
		}
	}

	fclose(fptr);
	return 0;
}


/*
 * Free up memory allocated by parse_options and parse_config.
 */
static void free_options(void)
{
	int cf_idx;
	for (cf_idx = 0; cf_idx < config_sections_count; cf_idx++) {
		int excl_idx;
#define free_and_clear(X) if (NULL != config_sections[cf_idx].X) { \
free(config_sections[cf_idx].X); \
config_sections[cf_idx].X = NULL; \
}
		free_and_clear(name);
		free_and_clear(source);
		free_and_clear(destination);
		free_and_clear(source_validation);
		free_and_clear(destination_validation);
		free_and_clear(full_marker);
		free_and_clear(partial_marker);
		free_and_clear(change_queue);
		free_and_clear(transfer_list);
		free_and_clear(tempdir);
		free_and_clear(sync_lock);
		free_and_clear(full_rsync_opts);
		free_and_clear(partial_rsync_opts);
		free_and_clear(log_file);
		free_and_clear(status_file);
		for (excl_idx = 0;
		     excl_idx < config_sections[cf_idx].exclude_count;
		     excl_idx++) {
			if (NULL !=
			    config_sections[cf_idx].excludes[excl_idx]) {
				free(config_sections[cf_idx].excludes
				     [excl_idx]);
				config_sections[cf_idx].excludes[excl_idx]
				    = NULL;
			}
		}
	}
	if (NULL != config_sections_selected)
		free(config_sections_selected);
	config_sections_selected = NULL;
	config_sections_selected_count = 0;
	if (NULL != pidfile) {
		free(pidfile);
		pidfile = NULL;
	}
}


/*
 * Parse the command line arguments, and read the configuration files. 
 * Returns 0 on success, -1 if the program should exit immediately without
 * an error, or 1 if the program should exit with an error.
 */
static int parse_options(int argc, char **argv)
{
	struct option long_options[] = {
		{"help", 0, 0, 'h'},
		{"version", 0, 0, 'V'},
		{"config", 1, 0, 'c'},
		{"daemon", 1, 0, 'D'},
#if ENABLE_DEBUGGING
		{"debug", 0, 0, 'd'},
#endif
		{0, 0, 0, 0}
	};
	int option_index = 0;
	char *short_options = "hVc:D:"
#if ENABLE_DEBUGGING
	    "d"
#endif
	    ;
	int c, numopts;
	flag_t config_specified = 0;

	config_sections_selected_count = 0;
	config_sections_selected = calloc(argc + 1, sizeof(char *));
	if (NULL == config_sections_selected) {
		die("%s", strerror(errno));
		return 1;
	}

	numopts = 0;

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
			printf("%s: %s %s\n", _("Usage"),
			       common_program_name,
			       _("[OPTIONS] [SECTIONS]"));
			printf("%s\n",
			       _
			       ("Synchronise the directories specified in the given SECTIONS of the\nconfiguration file(s), or all sections if nothing is specified."));
			printf("\n");
			printf("  -c, --config %s   %s\n", _("FILE"),
			       _("read configuration FILE"));
			printf("  -D, --daemon %s   %s\n", _("FILE"),
			       _("run as daemon, write PID to FILE"));
			printf("\n");
			printf("  -h, --help    %s\n",
			       _("display this help"));
			printf("  -V, --version %s\n",
			       _("display program version"));
#if ENABLE_DEBUGGING
			printf("  -d, --debug   %s\n",
			       _("enable debugging"));
#endif
			printf("\n");
			printf("%s%s\n",
			       _
			       ("If no configuration file is specified, the default is\nused: "),
			       DEFAULT_CONFIG_FILE);
			free_options();
			return -1;
			break;
		case 'V':
			printf("%s %s\n", common_program_name, VERSION);
			free_options();
			return -1;
			break;
		case 'c':
			if (parse_config(optarg, 0) != 0) {
				free_options();
				return 1;
			}
			config_specified = 1;
			break;
		case 'D':
			pidfile = xstrdup(optarg);
			break;
#if ENABLE_DEBUGGING
		case 'd':
			debugging_enabled = 1;
			break;
#endif
		default:
			fprintf(stderr,
				_("Try `%s --help' for more information."),
				common_program_name);
			fprintf(stderr, "\n");
			free_options();
			return 1;
			break;
		}

	} while (c != -1);

	/*
	 * Read default config file if none was specified.
	 */
	if (!config_specified) {
		if (parse_config(DEFAULT_CONFIG_FILE, 0) != 0) {
			free_options();
			return 1;
		}
	}

	/*
	 * Store remaining command-line arguments.
	 */
	while (optind < argc) {
		config_sections_selected[config_sections_selected_count++]
		    = argv[optind++];
	}

	return 0;
}


/*
 * Become a daemon, i.e. detach from the controlling terminal and run in the
 * background.  Exits in the parent, returns in the child.
 */
static void daemonise(const char *pidfile)
{
	pid_t child;
	int fd;

	child = fork();

	if (child < 0) {
		/*
		 * Fork failed - abort.
		 */
		die("%s: %s", "fork", strerror(errno));
		exit(EXIT_FAILURE);
	}

	if (child > 0) {
		FILE *pidfptr;
		/*
		 * Fork succeeded and we're the parent process - write
		 * child's PID to the PID file and exit successfully.
		 */
		pidfptr = fopen(pidfile, "w");
		if (pidfptr == NULL) {
			error("%s: %s", pidfile, strerror(errno));
			kill(child, SIGTERM);
			exit(EXIT_FAILURE);
		}
		fprintf(pidfptr, "%d\n", child);
		fclose(pidfptr);
		exit(EXIT_SUCCESS);
	}

	/*
	 * We're the background child process - cut our ties with the parent
	 * environment.
	 */
	fd = open("/dev/null", O_RDONLY);
	if (0 <= fd) {
		if (dup2(fd, 0) < 0)
			close(0);
		close(fd);
	} else {
		close(0);
	}
	fd = open("/dev/null", O_WRONLY);
	if (0 <= fd) {
		if (dup2(fd, 1) < 0)
			close(1);
		close(fd);
	} else {
		close(1);
	}
#if ENABLE_DEBUGGING
	if (!debugging_enabled) {
		if (dup2(1, 2) < 0)
			close(2);
	}
#else				/* ENABLE_DEBUGGING */
	if (dup2(1, 2) < 0)
		close(2);
#endif				/* ENABLE_DEBUGGING */

	setsid();
}


/*
 * Handler for an exit signal such as SIGTERM - set a flag to trigger an
 * exit.
 */
static void sync_main_exitsignal(int signum)
{
	sync_exit_now = 1;
}


/*
 * Handler for a signal we do nothing with, such as SIGCHLD or SIGALRM.
 */
static void sync_main_nullsignal(int signum)
{
	/* Do nothing. */
}


/*
 * Set up the signal handlers.
 */
static void set_signal_handlers(void)
{
	struct sigaction sa;

	sa.sa_handler = sync_main_exitsignal;
	sigemptyset(&(sa.sa_mask));
	sa.sa_flags = 0;
	sigaction(SIGTERM, &sa, NULL);

	sa.sa_handler = sync_main_exitsignal;
	sigemptyset(&(sa.sa_mask));
	sa.sa_flags = 0;
	sigaction(SIGINT, &sa, NULL);

	sa.sa_handler = sync_main_nullsignal;
	sigemptyset(&(sa.sa_mask));
	sa.sa_flags = 0;
	sigaction(SIGALRM, &sa, NULL);

	sa.sa_handler = sync_main_nullsignal;
	sigemptyset(&(sa.sa_mask));
	sa.sa_flags = 0;
	sigaction(SIGCHLD, &sa, NULL);
}


/*
 * Main entry point.  Read the command line arguments, parse the
 * configuration file(s), and start the main sync loop(s).
 */
int main(int argc, char **argv)
{
	int rc, sel_idx, cf_idx, defaults_idx;
	flag_t any_sections_chosen;
	char *env_path;

	common_program_name = ds_leafname(argv[0]);

	rc = parse_options(argc, argv);
	if (rc < 0)
		return EXIT_SUCCESS;
	else if (rc > 0)
		return EXIT_FAILURE;

	/*
	 * Check we have some configuration sections.
	 */
	if (0 == config_sections_count) {
		error("%s", _("no configuration sections defined"));
		free_options();
		return EXIT_FAILURE;
	}

	/*
	 * Find the defaults section so we can use it later, and validate
	 * it.
	 */
	defaults_idx = find_config_section(DEFAULTS_SECTION);
	if (0 <= defaults_idx) {
		if (validate_config_section(defaults_idx, -1) != 0) {
			free_options();
			return EXIT_FAILURE;
		}
	}

	any_sections_chosen = 0;

	/*
	 * Check that if we've chosen sections, they all exist and are
	 * valid, and mark them as selected.
	 */
	for (sel_idx = 0; sel_idx < config_sections_selected_count;
	     sel_idx++) {
		cf_idx =
		    find_config_section(config_sections_selected[sel_idx]);
		if (0 > cf_idx) {
			error("%s: %s", config_sections_selected[sel_idx],
			      _("configuration section not found"));
			free_options();
			return EXIT_FAILURE;
		}
		if (strcmp(config_sections[cf_idx].name, DEFAULTS_SECTION)
		    == 0) {
			error("%s",
			      _("cannot choose the defaults section"));
			free_options();
			return EXIT_FAILURE;
		}
		if (validate_config_section(cf_idx, defaults_idx) != 0) {
			free_options();
			return EXIT_FAILURE;
		}
		config_sections[cf_idx].selected = 1;
		any_sections_chosen = 1;
	}

	/*
	 * If we chose no sections, we chose them all except
	 * DEFAULTS_SECTION, so check they all are valid in that case, and
	 * mark them all as selected.
	 */
	if (0 == config_sections_selected_count) {
		for (cf_idx = 0; cf_idx < config_sections_count; cf_idx++) {
			if (strcmp
			    (config_sections[cf_idx].name,
			     DEFAULTS_SECTION) == 0)
				continue;
			if (validate_config_section(cf_idx, defaults_idx)
			    != 0) {
				free_options();
				return EXIT_FAILURE;
			}
			config_sections[cf_idx].selected = 1;
			any_sections_chosen = 1;
		}
	}

	/*
	 * If there were no sections chosen, we cannot do anything.
	 */
	if (!any_sections_chosen) {
		error("%s", _("no sections to synchronise"));
		free_options();
		exit(EXIT_FAILURE);
	}

	/*
	 * Set a default PATH environment variable if we don't have one.
	 */
	env_path = getenv("PATH");
	if ((NULL == env_path) || ('\0' == env_path[0]))
		putenv
		    ("PATH=/usr/bin:/bin:/usr/local/bin:/usr/sbin:/sbin:/usr/local/sbin");

	/*
	 * Become a daemon if necessary.
	 */
	if (NULL != pidfile) {
		daemonise(pidfile);
		openlog(common_program_name, LOG_PID, LOG_DAEMON);
		using_syslog = 1;
	}

	common_program_name = xstrdup(common_program_name);

	initproctitle(argc, argv);

	setproctitle("%s", common_program_name);

	/*
	 * Set up signal handling.
	 */
	set_signal_handlers();

	/*
	 * Main loop: maintain a child process for each selected section.
	 */
	while (!sync_exit_now) {
		/*
		 * Spawn any sync processes that need starting.
		 */
		for (cf_idx = 0; cf_idx < config_sections_count; cf_idx++) {
			pid_t child;

			if (!config_sections[cf_idx].selected)
				continue;
			if (0 < config_sections[cf_idx].pid)
				continue;

			child = fork();

			if (0 == child) {
				/* Child - run sync for this section */
				setproctitle("%s [%s]",
					     common_program_name,
					     config_sections[cf_idx].name);
				set_signal_handlers();
				continual_sync(&(config_sections[cf_idx]));
				free_options();
				free(common_program_name);
				exit(EXIT_SUCCESS);
			} else if (child < 0) {
				/* Error - output a warning */
				error("%s: %s", "fork", strerror(errno));
			} else {
				/* Parent - store PID */
				config_sections[cf_idx].pid = child;
				debug("(master) pid %d spawned [%s]",
				      child, config_sections[cf_idx].name);
			}
		}
		/*
		 * Clean up any child processes that have exited.
		 */
		for (cf_idx = 0; cf_idx < config_sections_count; cf_idx++) {
			if (!config_sections[cf_idx].selected)
				continue;
			if (0 >= config_sections[cf_idx].pid)
				continue;
			if (waitpid
			    (config_sections[cf_idx].pid, NULL,
			     WNOHANG) != 0) {
				debug("(master) pid %d exited [%s]",
				      config_sections[cf_idx].pid,
				      config_sections[cf_idx].name);
				config_sections[cf_idx].pid = 0;
			}
		}
		usleep(100000);
	}

	/*
	 * Kill any still-running sync processes.
	 */
	for (cf_idx = 0; cf_idx < config_sections_count; cf_idx++) {
		if (!config_sections[cf_idx].selected)
			continue;
		if (0 >= config_sections[cf_idx].pid)
			continue;
		kill(config_sections[cf_idx].pid, SIGTERM);
	}

	if (NULL != pidfile) {
		remove(pidfile);
		closelog();
	}
	free_options();
	free(common_program_name);

	return EXIT_SUCCESS;
}

/* EOF */
