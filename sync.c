/*
 * Continuously synchronise the contents of a source directory to a given
 * destination, reading the details from a configuration file.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <wordexp.h>
#include <fnmatch.h>
#include <dirent.h>
#include <utime.h>
#include <search.h>
#include "sync.h"

#define ACTION_WAITING "-"
#define ACTION_VALIDATION_SRC "VALIDATE-SOURCE"
#define ACTION_VALIDATION_DST "VALIDATE-DESTINATION"
#define ACTION_SYNC_FULL_WAIT "SYNC-FULL-AWAITING-LOCK"
#define ACTION_SYNC_FULL "SYNC-FULL"
#define ACTION_SYNC_PARTIAL_WAIT "SYNC-PARTIAL-AWAITING-LOCK"
#define ACTION_SYNC_PARTIAL "SYNC-PARTIAL"


struct sync_status_s {
	const char *action;
	pid_t watcher;
	pid_t pid;
	time_t next_full_sync;
	time_t next_partial_sync;
	time_t last_full_sync;
	time_t last_partial_sync;
	time_t last_failed_full_sync;
	time_t last_failed_partial_sync;
	char *last_full_sync_status;
	char *last_partial_sync_status;
	int full_sync_failures;
	int partial_sync_failures;
	char *workdir;
	char *excludes_file;
	char *rsync_error_file;
};

extern int watch_dir(const char *toplevel_path,
		     const char *changedpath_dir,
		     unsigned long full_scan_interval,
		     unsigned long queue_run_interval,
		     unsigned long queue_run_max_seconds,
		     unsigned long changedpath_dump_interval,
		     unsigned int max_dir_depth, char **excludes,
		     unsigned int exclude_count);

static int run_validation(struct sync_set_s *, const char *, const char *,
			  struct sync_status_s *, const char *);
static void run_watcher(struct sync_set_s *);
static void update_timestamp_file(struct sync_set_s *cf, const char *);
static int sync_full(struct sync_set_s *, struct sync_status_s *);
static int sync_partial(struct sync_set_s *, struct sync_status_s *);
static void log_message(const char *, const char *, ...);
static void recursively_delete(const char *, int);


/*
 * Return a pointer to a static buffer describing the given epoch time as
 * YYYY-MM-DD HH:MM:SS in the local time zone, or "-" if the time is 0.
 */
static char *dump_time(time_t t)
{
	static char tbuf[256];
	struct tm *tm;

	if (0 == t)
		return "-";

	tm = localtime(&t);
	strftime(tbuf, sizeof(tbuf) - 1, "%Y-%m-%d %H:%M:%S", tm);

	return tbuf;
}


/*
 * Update the status file, if we have one.
 */
static void update_status_file(struct sync_set_s *cf,
			       struct sync_status_s *st)
{
	int tmpfd;
	char *temp_filename;
	FILE *status_fptr;

	if (sync_exit_now)
		return;
	if (NULL == cf->status_file)
		return;

	tmpfd = ds_tmpfile(cf->status_file, &temp_filename);
	if (0 > tmpfd)
		return;

	status_fptr = fdopen(tmpfd, "w");
	if (NULL == status_fptr) {
		error("%s: %s(%d): %s", temp_filename,
		      "fdopen", tmpfd, strerror(errno));
		close(tmpfd);
		remove(temp_filename);
		free(temp_filename);
		return;
	}

	fprintf(status_fptr, "section                  : %s\n", cf->name);
	fprintf(status_fptr,
		"current action           : %s\n", st->action);
	fprintf(status_fptr, "sync process             : %d\n", st->pid);
	if (0 == st->watcher) {
		fprintf(status_fptr, "watcher process          : -\n");
	} else {
		fprintf(status_fptr,
			"watcher process          : %d\n", st->watcher);
	}
	fprintf(status_fptr,
		"last full sync status    : %s\n",
		st->last_full_sync_status);
	fprintf(status_fptr,
		"last partial sync status : %s\n",
		st->last_partial_sync_status);
	fprintf(status_fptr,
		"last full sync           : %s\n",
		dump_time(st->last_full_sync));
	fprintf(status_fptr,
		"last partial sync        : %s\n",
		dump_time(st->last_partial_sync));
	fprintf(status_fptr,
		"next full sync           : %s\n",
		dump_time(st->next_full_sync));
	fprintf(status_fptr,
		"next partial sync        : %s\n",
		dump_time(st->next_partial_sync));
	fprintf(status_fptr,
		"failed full sync         : %s\n",
		dump_time(st->last_failed_full_sync));
	fprintf(status_fptr,
		"failed partial sync      : %s\n",
		dump_time(st->last_failed_partial_sync));
	fprintf(status_fptr,
		"partial sync failures    : %d\n",
		st->partial_sync_failures);
	fprintf(status_fptr,
		"full sync failures       : %d\n", st->full_sync_failures);
	fprintf(status_fptr,
		"working directory        : %s\n", st->workdir);

	/*
	 * Trailing blank line so you can cat everything in
	 * /var/run/continual-sync/ and it is tidy.
	 */

	fprintf(status_fptr, "\n");

	fchmod(tmpfd, 0644);

	fclose(status_fptr);

	if (rename(temp_filename, cf->status_file) != 0) {
		error("%s: %s", cf->status_file, strerror(errno));
	}
	remove(temp_filename);
	free(temp_filename);
}


/*
 * Run a continual sync as defined by the given configuration.  Assumes that
 * the "sync_exit_now" flag will be set by a signal handler when a SIGTERM
 * is received.
 */
void continual_sync(struct sync_set_s *cf)
{
	char workdir[4096] = { 0, };
	struct sync_status_s status;
	struct stat sb;
	FILE *fptr;

	status.action = ACTION_WAITING;
	status.watcher = 0;
	status.pid = getpid();
	status.next_full_sync = 0;
	status.next_partial_sync = 0;
	status.last_full_sync = 0;
	status.last_partial_sync = 0;
	status.last_failed_full_sync = 0;
	status.last_failed_partial_sync = 0;
	status.last_full_sync_status = "-";
	status.last_partial_sync_status = "-";
	status.full_sync_failures = 0;
	status.partial_sync_failures = 0;
	status.workdir = workdir;
	status.excludes_file = NULL;
	status.rsync_error_file = NULL;

	/*
	 * Create a temporary working directory.
	 */
	snprintf(workdir, sizeof(workdir) - 1, "%s/%s",
		 NULL == cf->tempdir ? "/tmp" : cf->tempdir, "syncXXXXXX");
	if (mkdtemp(workdir) == NULL) {
		error("%s: %s: %s", "mkdtemp", workdir, strerror(errno));
		return;
	}
	debug("%s: %s", "temporary working directory", workdir);

	/*
	 * Create the file that rsync will write stderr to.
	 */
	if (asprintf
	    (&(status.rsync_error_file), "%s/%s", workdir,
	     "rsync-stderr") < 0) {
		error("%s: %s", "asprintf", strerror(errno));
		rmdir(workdir);
		return;
	}

	/*
	 * Create the file that rsync will use with --excludes-from.
	 */
	if (asprintf(&(status.excludes_file), "%s/%s", workdir, "excludes")
	    < 0) {
		error("%s: %s", "asprintf", strerror(errno));
		free(status.rsync_error_file);
		rmdir(workdir);
		return;
	}
	fptr = fopen(status.excludes_file, "w");
	if (NULL == fptr) {
		error("%s: %s: %s", status.excludes_file, "fopen",
		      strerror(errno));
		free(status.rsync_error_file);
		free(status.excludes_file);
		recursively_delete(workdir, 0);
		return;
	}
	if (0 < cf->exclude_count) {
		int eidx;
		for (eidx = 0; eidx < cf->exclude_count; eidx++) {
			fprintf(fptr, "%s\n", cf->excludes[eidx]);
		}
	} else {
		fprintf(fptr, "*.tmp\n*~\n");
	}
	fclose(fptr);

	/*
	 * If no transfer list file was defined, define one under the
	 * temporary working directory.
	 */
	if (NULL == cf->transfer_list) {
		if (asprintf
		    (&(cf->transfer_list), "%s/%s", workdir,
		     "transfer") < 0) {
			error("%s: %s", "asprintf", strerror(errno));
			free(status.rsync_error_file);
			free(status.excludes_file);
			cf->transfer_list = NULL;
			recursively_delete(workdir, 0);
			return;
		}
		debug("%s: %s", "automatically set transfer list",
		      cf->transfer_list);
	}

	/*
	 * If no change queue directory was defined, create one under the
	 * temporary working directory.
	 */
	if (NULL == cf->change_queue) {
		if (asprintf
		    (&(cf->change_queue), "%s/%s", workdir,
		     "changes") < 0) {
			error("%s: %s", "asprintf", strerror(errno));
			free(status.rsync_error_file);
			free(status.excludes_file);
			cf->change_queue = NULL;
			recursively_delete(workdir, 0);
			return;
		}
		if (mkdir(cf->change_queue, 0700) != 0) {
			error("%s: %s: %s", cf->change_queue, "mkdir",
			      strerror(errno));
			free(status.rsync_error_file);
			free(status.excludes_file);
			free(cf->change_queue);
			cf->change_queue = NULL;
			recursively_delete(workdir, 0);
			return;
		}
		debug("%s: %s", "automatically set change queue",
		      cf->change_queue);
	}

	log_message(cf->log_file, "[%s] %s", cf->name,
		    _("process started"));

	/*
	 * If we're using a full sync marker file, use its timestamp to
	 * determine the next full sync time.
	 */
	if ((NULL != cf->full_marker) && (stat(cf->full_marker, &sb) == 0)) {
		status.next_full_sync = sb.st_mtime + cf->full_interval;
		log_message(cf->log_file, "[%s] %s: %s", cf->name,
			    _
			    ("used full sync marker file - next full sync"),
			    dump_time(status.next_full_sync));
	}

	/*
	 * If we're using a partial sync marker file, use its timestamp to
	 * determine the next partial sync time.
	 */
	if ((NULL != cf->partial_marker)
	    && (stat(cf->partial_marker, &sb) == 0)) {
		status.next_partial_sync =
		    sb.st_mtime + cf->partial_interval;
		log_message(cf->log_file, "[%s] %s: %s", cf->name,
			    _
			    ("used partial sync marker file - next partial sync"),
			    dump_time(status.next_partial_sync));
	}

	/*
	 * Update our status file, if we have one.
	 */
	update_status_file(cf, &status);

	/*
	 * Main loop.
	 */
	while (!sync_exit_now) {
		flag_t check_workdir = 0;

		/*
		 * If there is no watcher and there should be one, start
		 * one.
		 */
		if ((0 == status.watcher) && (0 < cf->partial_interval)) {
			pid_t child;

			/*
			 * If we have a source validation command, run that
			 * first, and skip to the end of the loop on
			 * failure.
			 */
			if (run_validation
			    (cf, cf->source_validation,
			     _("source"), &status,
			     ACTION_VALIDATION_SRC) != 0) {
				status.action = ACTION_WAITING;
				update_status_file(cf, &status);
				sleep(5);
			} else {
				child = fork();
				if (0 == child) {
					/* Child - run watcher */
					run_watcher(cf);
					/*
					 * We return here instead of exiting
					 * so that the main sync program can
					 * do its usual cleanup, freeing up
					 * memory etc.
					 */
					free(status.rsync_error_file);
					free(status.excludes_file);
					return;
				} else if (child < 0) {
					/* Error - output a warning */
					error("%s: %s", "fork",
					      strerror(errno));
				} else {
					/* Parent */
					status.watcher = child;
					log_message(cf->log_file,
						    "[%s] %s: %d",
						    cf->name,
						    _
						    ("started new watcher"),
						    status.watcher);
				}
			}
		}

		/*
		 * If it's time for a full sync, run one.
		 */
		if ((time(NULL) >= status.next_full_sync)
		    && (0 < cf->full_interval)) {

			check_workdir = 1;

			if ((run_validation
			     (cf, cf->source_validation, _("source"),
			      &status, ACTION_VALIDATION_SRC) != 0)
			    ||
			    (run_validation
			     (cf, cf->destination_validation,
			      _("destination"), &status,
			      ACTION_VALIDATION_DST) != 0)) {
				/*
				 * Validation failed - just retry later
				 */
				status.next_full_sync =
				    time(NULL) + cf->full_retry;
			} else {
				flag_t sync_failed = 0;
				/*
				 * Validation succeeded - attempt to run
				 * sync
				 */
				sync_failed = sync_full(cf, &status);
				if (0 == sync_failed) {
					/* sync succeeded */
					/*
					 * No need to update last_full_sync
					 * etc, as sync_full() did all that.
					 */
					status.next_full_sync =
					    time(NULL) + cf->full_interval;
				} else {
					/* sync failed */
					status.next_full_sync =
					    time(NULL) + cf->full_retry;
					status.last_failed_full_sync =
					    time(NULL);
					status.full_sync_failures++;
					status.last_full_sync_status =
					    "FAILED";
				}
			}
			/*
			 * Update our status after the attempt.
			 */
			status.action = ACTION_WAITING;
			update_status_file(cf, &status);
		}

		/*
		 * If it's time for a partial sync and we have a watcher
		 * process, run a partial sync.
		 */
		if ((0 != status.watcher)
		    && (time(NULL) >= status.next_partial_sync)) {

			check_workdir = 1;

			if ((run_validation
			     (cf, cf->source_validation, _("source"),
			      &status, ACTION_VALIDATION_SRC) != 0)
			    ||
			    (run_validation
			     (cf, cf->destination_validation,
			      _("destination"), &status,
			      ACTION_VALIDATION_DST) != 0)) {
				/*
				 * Validation failed - just retry later
				 */
				status.next_partial_sync =
				    time(NULL) + cf->partial_retry;
			} else {
				flag_t sync_failed = 0;
				/*
				 * Validation succeeded - attempt to run
				 * sync
				 */
				sync_failed = sync_partial(cf, &status);
				if (0 == sync_failed) {
					/* sync succeeded OR not run */
					status.next_partial_sync =
					    time(NULL) +
					    cf->partial_interval;
					/*
					 * No need to update
					 * last_partial_sync etc, as
					 * sync_partial() did all that.
					 */
				} else {
					/* sync run AND failed */
					status.next_partial_sync =
					    time(NULL) + cf->partial_retry;
					status.last_failed_partial_sync =
					    time(NULL);
					status.partial_sync_failures++;
					status.last_partial_sync_status =
					    "FAILED";
				}
			}
			/*
			 * Update our status after the attempt.
			 */
			status.action = ACTION_WAITING;
			update_status_file(cf, &status);
		}

		/*
		 * Clean up our child process if it has exited.
		 */
		if ((0 != status.watcher)
		    && (waitpid(status.watcher, NULL, WNOHANG) != 0)) {
			check_workdir = 1;
			log_message(cf->log_file, "[%s] %s", cf->name,
				    _("watcher process ended"));
			status.watcher = 0;
		}

		if (check_workdir) {
			/*
			 * Check whether our work directory disappeared, and
			 * exit immediately if so.
			 *
			 * We only check this after a full or partial sync
			 * attempt, or when a watcher process exits, to
			 * avoid too many stat calls.
			 */
			check_workdir = 0;
			if (stat(workdir, &sb) != 0) {
				log_message(cf->log_file, "[%s] %s",
					    cf->name,
					    _
					    ("working directory disappeared - exiting"));
				sync_exit_now = 1;
			}
		}

		if (!sync_exit_now)
			usleep(100000);
	}

	/*
	 * Free the rsync_error_file string we made.
	 */
	free(status.rsync_error_file);

	/*
	 * Free the excludes_file string we made.
	 */
	free(status.excludes_file);

	/*
	 * Kill our watcher process, if we have one.
	 */
	if (0 != status.watcher)
		kill(status.watcher, SIGTERM);

	/*
	 * Remove our temporary working directory.
	 */
	recursively_delete(workdir, 0);

	/*
	 * Remove our status file, if we have one.
	 */
	if (NULL != cf->status_file)
		remove(cf->status_file);

	log_message(cf->log_file, "[%s] %s", cf->name, _("process ended"));
}


/*
 * Run the given command, if there is one (returns zero if not).  Returns
 * nonzero if the command was run and it failed, and logs the error.
 *
 * Before the command starts, st->action is set to "action" and the
 * status file is updated.
 */
static int run_validation(struct sync_set_s *cf, const char *command,
			  const char *name, struct sync_status_s *st,
			  const char *action)
{
	int ret, exit_status;

	if (NULL == command)
		return 0;

	debug("(sync) [%s] running %s validation: [%s]", cf->name, name,
	      command);

	st->action = action;
	update_status_file(cf, st);

	ret = system(command);

	if (WIFSIGNALED(ret)) {
		log_message(cf->log_file, "[%s] %s: %s: %d",
			    cf->name, name,
			    _
			    ("validation command received a signal"),
			    WTERMSIG(ret));
		sync_exit_now = 1;
		return 1;
	}

	exit_status = WEXITSTATUS(ret);
	if (exit_status == 0)
		return 0;

	log_message(cf->log_file,
		    "[%s] %s: %s: %d",
		    cf->name, name,
		    _("validation command gave non-zero exit status"),
		    exit_status);

	return 1;
}


/*
 * Run the watcher on the source directory.
 */
static void run_watcher(struct sync_set_s *cf)
{
	int rc;
	setproctitle("%s %s [%s]", common_program_name, _("watcher"),
		     cf->name);
	rc = watch_dir(cf->source, cf->change_queue, cf->full_interval, 2,
		       5, cf->partial_interval, cf->recursion_depth,
		       cf->excludes, cf->exclude_count);
}


/*
 * Run rsync with the given parameters, returning the exit status.
 */
static int run_rsync(const char *log_file, const char *section,
		     const char *source, const char *destination,
		     const char *excludes_file, const char *options,
		     const char *transfer_list,
		     const char *rsync_error_file)
{
	char **rsync_argv;
	int rsync_argc, optidx;
	wordexp_t p;
	int rc = -1;
	pid_t rsync_pid;
	struct stat sb;

	if (wordexp(options, &p, WRDE_NOCMD) != 0) {
		error("%s: %s", "wordexp", strerror(errno));
		return -1;
	}

	rsync_argc = 1;			    /* "rsync" */
	rsync_argc += p.we_wordc;	    /* options */
	if (NULL != transfer_list)
		rsync_argc += 2;	    /* --files-from */
	if (NULL != excludes_file)
		rsync_argc += 2;	    /* --exclude-from */
	rsync_argc += 2;		    /* source, dest */

	rsync_argv = calloc(rsync_argc + 1, sizeof(char *));
	if (NULL == rsync_argv) {
		error("%s: %s", "calloc", strerror(errno));
		return -1;
	}

	rsync_argv[0] = "rsync";
	for (optidx = 1; optidx <= p.we_wordc; optidx++) {
		rsync_argv[optidx] = p.we_wordv[optidx - 1];
	}
	if (NULL != transfer_list) {
		rsync_argv[optidx++] = "--files-from";
		rsync_argv[optidx++] = (char *) transfer_list;
	}
	if (NULL != excludes_file) {
		rsync_argv[optidx++] = "--exclude-from";
		rsync_argv[optidx++] = (char *) excludes_file;
	}
	rsync_argv[optidx++] = (char *) source;
	rsync_argv[optidx++] = (char *) destination;

	remove(rsync_error_file);

	rsync_pid = fork();

	if (0 == rsync_pid) {
		int fd;
		/* Child - run rsync */
		fd = open(rsync_error_file, O_CREAT | O_WRONLY, 0600);
		if (0 <= fd) {
			dup2(fd, 2);
			close(fd);
		}
		execvp("rsync", rsync_argv);
		exit(EXIT_FAILURE);
	} else if (rsync_pid < 0) {
		/* Error - output a warning */
		error("%s: %s", "fork", strerror(errno));
		rc = -1;
	} else {
		/* Parent */
		debug("(rsync) %s: %d", "process spawned", rsync_pid);
		while ((!sync_exit_now) && (0 != rsync_pid)) {
			int wait_status;
			pid_t waited_for;
			waited_for = waitpid(rsync_pid, &wait_status, 0);
			if (0 > waited_for) {
				if ((errno == EINTR) || (errno == EAGAIN))
					continue;
				log_message(log_file, "[%s] %s: %s: %s",
					    section,
					    _("failed to wait for rsync"),
					    "waitpid", strerror(errno));
				rc = -1;
				break;
			} else {
				rsync_pid = 0;
				rc = WEXITSTATUS(wait_status);
				debug("(rsync) %s: %d",
				      "process ended, exit status", rc);
			}
		}
		if (0 != rsync_pid) {
			debug("(rsync) %s: %d", "killing rsync process",
			      rsync_pid);
			kill(rsync_pid, SIGTERM);
		}
	}

	free(rsync_argv);
	wordfree(&p);

	if ((stat(rsync_error_file, &sb) == 0) && (0 < sb.st_size)) {
		char linebuf[1024];
		FILE *err_fptr;

		err_fptr = fopen(rsync_error_file, "r");
		if (NULL == err_fptr) {
			error("%s: %s", rsync_error_file, strerror(errno));
			return rc;
		}

		while ((!feof(err_fptr))
		       && (NULL !=
			   fgets(linebuf, sizeof(linebuf) - 1,
				 err_fptr))) {
			char *nlptr;

			nlptr = strrchr(linebuf, '\n');
			if (NULL != nlptr)
				nlptr[0] = '\0';

			log_message(log_file, "[%s] %s: %s", section,
				    "rsync", linebuf);
		}

		fclose(err_fptr);

		log_message(log_file, "[%s] %s: %d", section,
			    "rsync failed with exit status", rc);
	}

	return rc;
}


/*
 * Update the given timestamp file if it's not NULL, creating it if it
 * doesn't exist, and setting its last-modification time to now.
 */
static void update_timestamp_file(struct sync_set_s *cf, const char *file)
{
	FILE *fptr;
	if (NULL == file)
		return;
	fptr = fopen(file, "a");
	if (NULL != fptr) {
		fclose(fptr);
	} else {
		log_message(cf->log_file, "[%s] %s: %s", cf->name, file,
			    strerror(errno));
	}
	if (utime(file, NULL) != 0) {
		log_message(cf->log_file, "[%s] %s: %s", cf->name, file,
			    strerror(errno));
	}
}


/*
 * Run a full sync, returning nonzero on failure.
 *
 * If a sync succeeded, the full marker file's timestamp is updated, if one
 * is defined; st->last_full_sync is set to the current time;
 * st->full_sync_failures is set to 0; and st->last_full_sync_status is set
 * to point to "OK".
 *
 * The status file is updated before the sync begins.
 */
static int sync_full(struct sync_set_s *cf, struct sync_status_s *st)
{
	int lockfd = -1;
	int rc = 0;

	if (NULL != cf->sync_lock) {
		st->action = ACTION_SYNC_FULL_WAIT;
		update_status_file(cf, st);
		lockfd =
		    open(cf->sync_lock, O_CREAT | O_WRONLY | O_APPEND,
			 0600);
		if (0 > lockfd) {
			debug("(lock) %s: %s", cf->sync_lock,
			      strerror(errno));
		} else {
			log_message(cf->log_file, "[%s] %s: %s", cf->name,
				    _("full sync"),
				    _("acquiring sync lock"));
			lockf(lockfd, F_LOCK, 0);
			log_message(cf->log_file, "[%s] %s: %s", cf->name,
				    _("full sync"),
				    _("sync lock acquired"));
		}
	}

	st->action = ACTION_SYNC_FULL;
	update_status_file(cf, st);

	log_message(cf->log_file, "[%s] %s: %s", cf->name, _("full sync"),
		    _("sync starting"));

	rc = run_rsync(cf->log_file, cf->name, cf->source, cf->destination,
		       st->excludes_file,
		       NULL ==
		       cf->full_rsync_opts ? "--delete -axH" :
		       cf->full_rsync_opts, NULL, st->rsync_error_file);

	log_message(cf->log_file, "[%s] %s: %s: %s", cf->name,
		    _("full sync"), _("sync ended"),
		    rc == 0 ? _("OK") : _("FAILED"));

	if (0 <= lockfd) {
		lockf(lockfd, F_ULOCK, 0);
		close(lockfd);
	}

	if (rc == 0) {
		update_timestamp_file(cf, cf->full_marker);
		st->last_full_sync = time(NULL);
		st->full_sync_failures = 0;
		st->last_full_sync_status = "OK";
	}

	return rc;
}


/*
 * Collate a transfer list from the change queue: remove the change queue
 * entries, appending those that refer to items that still exist to the
 * transfer list.
 */
static void collate_transfer_list(struct sync_set_s *cf)
{
	struct dirent **namelist;
	int namelist_length, idx;
	char path[4096] = { 0, };
	FILE *list_fptr;
	FILE *changefile_fptr;
	void *tree_root = NULL;

	list_fptr = fopen(cf->transfer_list, "a");
	if (NULL == list_fptr) {
		error("%s: %s: %s", cf->name, cf->transfer_list,
		      strerror(errno));
		return;
	}

	namelist_length =
	    scandir(cf->change_queue, &namelist, NULL, alphasort);
	if (0 > namelist_length) {
		error("%s: %s: %s", "scandir", cf->change_queue,
		      strerror(errno));
		fclose(list_fptr);
		return;
	}

	for (idx = 0; idx < namelist_length; idx++) {
		struct stat sb;
		char linebuf[4096] = { 0, };

		if ('.' == namelist[idx]->d_name[0])
			continue;

		snprintf(path, sizeof(path) - 1, "%s/%s", cf->change_queue,
			 namelist[idx]->d_name);

		if (lstat(path, &sb) != 0)
			continue;
		if (!S_ISREG(sb.st_mode))
			continue;

		changefile_fptr = fopen(path, "r");
		if (NULL == changefile_fptr) {
			debug("%s: %s", path, strerror(errno));
			remove(path);
			continue;
		}

		while ((!feof(changefile_fptr))
		       && (NULL !=
			   fgets(linebuf, sizeof(linebuf) - 1,
				 changefile_fptr))) {
			char *nlptr;
			char *changedpath;

			nlptr = strrchr(linebuf, '\n');
			if (NULL != nlptr)
				nlptr[0] = '\0';

			/*
			 * Use a binary tree to keep track of lines we've
			 * seen before, so we can strip duplicates.
			 */
			if ((NULL != tree_root)
			    && (NULL !=
				tfind(linebuf, &tree_root,
				      (comparison_fn_t) strcmp))) {
				debug("%s: %s",
				      "skipping duplicate change line",
				      linebuf);
				continue;
			}
			tsearch(xstrdup(linebuf), &tree_root,
				(comparison_fn_t) strcmp);

			if (asprintf
			    (&changedpath, "%s/%s", cf->source,
			     linebuf) < 0) {
				error("%s: %s", "asprintf",
				      strerror(errno));
				fclose(changefile_fptr);
				break;
			}
			if (lstat(changedpath, &sb) == 0)
				fprintf(list_fptr, "%s\n", linebuf);
			free(changedpath);
		}

		fclose(changefile_fptr);

		remove(path);
	}

	if (NULL != tree_root)
		tdestroy(tree_root, free);

	for (idx = 0; idx < namelist_length; idx++) {
		free(namelist[idx]);
	}
	free(namelist);

	fclose(list_fptr);
}


/*
 * Run a partial sync, returning nonzero on failure.  Returns zero if there
 * is nothing to sync, or if there was a sync and it succeeded.
 *
 * If a sync was run, and it succeeded, the partial marker file's timestamp
 * is updated, if one is defined; st->last_partial_sync is set to the
 * current time; st->partial_sync_failures is set to 0; and
 * st->last_partial_sync_status is set to point to "OK".
 *
 * The status file is updated before an action is performed.
 */
static int sync_partial(struct sync_set_s *cf, struct sync_status_s *st)
{
	struct stat sb;
	int lockfd = -1;
	int rc = 0;
	FILE *list_fptr;

	collate_transfer_list(cf);

	if ((stat(cf->transfer_list, &sb) != 0) || (0 == sb.st_size)) {
		/*
		 * If there is no transfer list, there is nothing to sync.
		 */
		return 0;
	}

	if (NULL != cf->sync_lock) {
		st->action = ACTION_SYNC_PARTIAL_WAIT;
		update_status_file(cf, st);
		lockfd =
		    open(cf->sync_lock, O_CREAT | O_WRONLY | O_APPEND,
			 0600);
		if (0 > lockfd) {
			debug("(lock) %s: %s", cf->sync_lock,
			      strerror(errno));
		} else {
			log_message(cf->log_file, "[%s] %s: %s", cf->name,
				    _("partial sync"),
				    _("acquiring sync lock"));
			lockf(lockfd, F_LOCK, 0);
			log_message(cf->log_file, "[%s] %s: %s", cf->name,
				    _("partial sync"),
				    _("sync lock acquired"));
		}
	}

	st->action = ACTION_SYNC_PARTIAL;
	update_status_file(cf, st);

	log_message(cf->log_file, "[%s] %s: %s", cf->name,
		    _("partial sync"), _("sync starting"));

	/*
	 * Write a copy of the transfer list to the log file (stopping at
	 * 100 lines so the log doesn't grow too much).
	 */
	list_fptr = fopen(cf->transfer_list, "r");
	if (NULL != list_fptr) {
		char linebuf[4096];
		int lineno = 0;
		while ((!feof(list_fptr))
		       && (NULL !=
			   fgets(linebuf, sizeof(linebuf) - 1,
				 list_fptr))) {
			char *nlptr;

			lineno++;
			if (lineno > 100) {
				log_message(cf->log_file, "[%s]   %s",
					    cf->name, "...");
				break;
			}

			nlptr = strrchr(linebuf, '\n');
			if (NULL != nlptr)
				nlptr[0] = '\0';

			log_message(cf->log_file, "[%s]   %s", cf->name,
				    linebuf);
		}
		fclose(list_fptr);
	}

	rc = run_rsync(cf->log_file, cf->name, cf->source, cf->destination,
		       st->excludes_file,
		       NULL ==
		       cf->partial_rsync_opts ? "--delete -dlptgoDH" :
		       cf->partial_rsync_opts, cf->transfer_list,
		       st->rsync_error_file);

	log_message(cf->log_file, "[%s] %s: %s: %s", cf->name,
		    _("partial sync"), _("sync ended"),
		    rc == 0 ? _("OK") : _("FAILED"));

	if (0 <= lockfd) {
		lockf(lockfd, F_ULOCK, 0);
		close(lockfd);
	}

	remove(cf->transfer_list);

	if (rc == 0) {
		update_timestamp_file(cf, cf->partial_marker);
		st->last_partial_sync = time(NULL);
		st->partial_sync_failures = 0;
		st->last_partial_sync_status = "OK";
	}

	return rc;
}


/*
 * Log the given message to the given file, with a timestamp.
 */
static void log_message(const char *file, const char *format, ...)
{
	FILE *fptr;
	va_list ap;
	time_t t;
	struct tm *tm;
	char tbuf[128];

	time(&t);
	tm = localtime(&t);
	tbuf[0] = 0;
	strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", tm);

#if ENABLE_DEBUGGING
	va_start(ap, format);
	if (debugging_enabled) {
		fprintf(stderr, "[%s] (log) ", tbuf);
		vfprintf(stderr, format, ap);
		fprintf(stderr, "\n");
	}
	va_end(ap);
#endif				/* ENABLE_DEBUGGING */

	if (NULL == file)
		return;

	fptr = fopen(file, "a");
	if (NULL == fptr) {
		debug("(log) %s: %s", file, strerror(errno));
		return;
	}

	lockf(fileno(fptr), F_LOCK, 0);
	fseek(fptr, 0, SEEK_END);

	fprintf(fptr, "[%s] ", tbuf);
	va_start(ap, format);
	vfprintf(fptr, format, ap);
	va_end(ap);
	fprintf(fptr, "\n");

	lockf(fileno(fptr), F_ULOCK, 0);
	fclose(fptr);
}


/*
 * Delete the given directory and everything in it.
 */
static void recursively_delete(const char *dir, int depth)
{
	struct dirent **namelist;
	int namelist_length, idx;
	char path[4096] = { 0, };

	depth++;
	if (depth > 10)
		return;

	namelist_length = scandir(dir, &namelist, NULL, alphasort);
	if (0 > namelist_length) {
		error("%s: %s: %s", "scandir", dir, strerror(errno));
		return;
	}

	for (idx = 0; idx < namelist_length; idx++) {
		struct stat sb;
		if (strcmp(namelist[idx]->d_name, ".") == 0)
			continue;
		if (strcmp(namelist[idx]->d_name, "..") == 0)
			continue;
		snprintf(path, sizeof(path) - 1, "%s/%s", dir,
			 namelist[idx]->d_name);
		if (lstat(path, &sb) != 0) {
			error("%s: %s: %s", "lstat", path,
			      strerror(errno));
			continue;
		}
		if (S_ISDIR(sb.st_mode)) {
			recursively_delete(path, depth);
		} else {
			debug("%s: %s", "removing", path);
			remove(path);
		}
	}

	for (idx = 0; idx < namelist_length; idx++) {
		free(namelist[idx]);
	}
	free(namelist);

	debug("%s: %s", "removing", dir);
	rmdir(dir);
}

/* EOF */
