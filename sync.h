/*
 * Header for the sync program.
 */

#ifndef SYNC_H
#define SYNC_H 1

#ifndef COMMON_H
#include "common.h"
#endif

#define DEFAULT_CONFIG_FILE "/etc/continual-sync.conf"
#define DEFAULTS_SECTION "defaults"
#define MAX_CONFIG_SECTIONS 1000
#define MAX_EXCLUDES 1000

/*
 * Structure describing a synchronisation set.
 */
struct sync_set_s {
	char *name;
	char *source;
	char *destination;
	char *excludes[MAX_EXCLUDES];
	int exclude_count;
	char *source_validation;
	char *destination_validation;
	unsigned long full_interval;
	unsigned long full_retry;
	unsigned long partial_interval;
	unsigned long partial_retry;
	unsigned long recursion_depth;
	char *full_marker;
	char *partial_marker;
	char *change_queue;
	char *transfer_list;
	char *tempdir;
	char *sync_lock;
	char *full_rsync_opts;
	char *partial_rsync_opts;
	char *log_file;
	char *status_file;
	flag_t selected;		 /* set if selected on cmd line */
	pid_t pid;			 /* pid of sync process or 0 */
	/*
	 * These flags are set by the config parser if the parameters they
	 * are named for were explicitly set in this section, so we know
	 * which ones to override from the DEFAULTS_SECTION section on
	 * validation.
	 */
	struct {
		flag_t full_interval;
		flag_t full_retry;
		flag_t partial_interval;
		flag_t partial_retry;
		flag_t recursion_depth;
	} set;
};

extern flag_t sync_exit_now;		 /* exit-now flag (on signal) */

void continual_sync(struct sync_set_s *);

#endif	/* SYNC_H */

/* EOF */
