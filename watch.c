/*
 * Watch the given directory, maintaining an output file containing a list
 * of files changed.
 */

/* File and subdirectory array allocation chunk size */
#define DIRCONTENTS_ALLOC_CHUNK	128

/* Directory index array allocation chunk size */
#define DIR_INDEX_ALLOC_CHUNK 1024

/* Change queue allocation chunk size */
#define CHANGE_QUEUE_ALLOC_CHUNK 1024

/* Changed paths list allocation chunk size */
#define CHANGEDPATH_ALLOC_CHUNK 1024


#define _GNU_SOURCE
#define _ATFILE_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <syslog.h>
#include <limits.h>
#include <sys/inotify.h>
#include <sys/select.h>
#include <poll.h>
#include <fnmatch.h>
#include "common.h"


/*
 * Actions to take on inotify events.
 */
typedef enum {
	IN_ACTION_CREATE,
	IN_ACTION_UPDATE,
	IN_ACTION_DELETE,
	IN_ACTION_NONE
} inotify_action_t;

struct ds_file_s;
typedef struct ds_file_s *ds_file_t;
struct ds_dir_s;
typedef struct ds_dir_s *ds_dir_t;
struct ds_watch_index_s;
typedef struct ds_watch_index_s *ds_watch_index_t;
struct ds_change_queue_s;
typedef struct ds_change_queue_s *ds_change_queue_t;


/*
 * Structure holding information about a file.  An example path would be
 * "0/12/12345/foo.txt"; the leaf would be "foo.txt"; the absolute_path
 * would be "/top/dir/0/12/12345/foo.txt" if the top level directory was
 * "/top/dir".
 */
struct ds_file_s {
	char *absolute_path;		 /* absolute path to file */
	char *path;			 /* path relative to top level */
	char *leaf;			 /* leafname of this file */
	time_t mtime;			 /* file last-modification time */
	off_t size;			 /* file size */
	ds_dir_t parent;		 /* containing directory */
	flag_t seen_in_rescan;		 /* set during dir rescan */
};


/*
 * Structure holding information about a directory.  An example path would
 * be "0/12/12345" with a leaf of "12345".
 */
struct ds_dir_s {
	char *absolute_path;		 /* absolute path to directory */
	char *path;			 /* path relative to top level */
	char *leaf;			 /* leafname of this directory */
	int wd;				 /* inotify watch fd (if dir) */
	int depth;			 /* subdirs deep from top level */
	int file_count;			 /* number of files in directory */
	int subdir_count;		 /* number of immediate subdirs */
	ds_file_t *files;		 /* array of files */
	ds_dir_t *subdirs;		 /* array of subdirectories */
	int file_array_alloced;		 /* file entries allocated */
	int subdir_array_alloced;	 /* subdir entries allocated */
	ds_dir_t parent;		 /* pointer to parent directory */
	ds_dir_t topdir;		 /* pointer to top directory */
	flag_t seen_in_rescan;		 /* set during dir rescan */
	flag_t files_unsorted;		 /* set when files need re-sorting */
	flag_t subdirs_unsorted;	 /* set when dirs need re-sorting */
	/*
	 * Items used only in the top level directory:
	 */
	int fd_inotify;			 /* directory watch file descriptor */
	ds_watch_index_t watch_index;	 /* array of watch descriptors */
	int watch_index_length;		 /* number of entries in array */
	int watch_index_alloced;	 /* number of entries allocated */
	flag_t watch_index_unsorted;	 /* set if array needs sorting */
	ds_change_queue_t change_queue;	 /* array of changes needed */
	int change_queue_length;	 /* number of changes in queue */
	int change_queue_alloced;	 /* array size allocated */
	char **changed_paths;		 /* array of changed paths */
	int changed_paths_length;	 /* number of paths in array */
	int changed_paths_alloced;	 /* array size allocated */
};


/*
 * Structure for indexing directory structures by watch identifier.
 */
struct ds_watch_index_s {
	int wd;
	ds_dir_t dir;
};


/*
 * Structure for a file check or directory scan queue entry.
 */
struct ds_change_queue_s {
	time_t when;
	ds_file_t file;
	ds_dir_t dir;
};


static int ds_filename_valid(const char *name);

static ds_file_t ds_file_add(ds_dir_t dir, const char *name);
static void ds_file_remove(ds_file_t file);
static int ds_file_checkchanged(ds_file_t file);

static ds_dir_t ds_dir_toplevel(int fd_inotify, const char *top_path);
static ds_dir_t ds_dir_add(ds_dir_t dir, const char *name);
static void ds_dir_remove(ds_dir_t dir);
static int ds_dir_scan(ds_dir_t dir, flag_t no_recurse);

static void ds_watch_index_add(ds_dir_t dir, int wd);
static void ds_watch_index_remove(ds_dir_t topdir, int wd);
static ds_dir_t ds_watch_index_lookup(ds_dir_t topdir, int wd);

static void ds_change_queue_file_add(ds_file_t file, time_t when);
static void ds_change_queue_file_remove(ds_file_t file);
static void ds_change_queue_dir_add(ds_dir_t dir, time_t when);
static void ds_change_queue_dir_remove(ds_dir_t dir);

static void ds_change_queue_process(ds_dir_t topdir, time_t work_until);

static void mark_path_changed(ds_dir_t topdir, const char *path,
			      flag_t isdir);
static void dump_changed_paths(ds_dir_t topdir,
			       const char *changedpath_dir);



static unsigned int max_directory_depth = 20;
static flag_t watch_dir_exit_now = 0;
static char **excludes = NULL;
static unsigned int exclude_count = 0;


/*
 * Add the given watch descriptor to the directory index.
 */
static void ds_watch_index_add(ds_dir_t dir, int wd)
{
	if (NULL == dir)
		return;
	if (0 > wd)
		return;
	if (NULL == dir->topdir)
		return;

	/*
	 * Extend the array if we need to.
	 */
	if (dir->topdir->watch_index_length >=
	    dir->topdir->watch_index_alloced) {
		int new_size;
		ds_watch_index_t newptr;
		new_size =
		    dir->topdir->watch_index_alloced +
		    DIR_INDEX_ALLOC_CHUNK;
		newptr =
		    realloc(dir->topdir->watch_index,
			    new_size *
			    sizeof(dir->topdir->watch_index[0]));
		if (NULL == newptr) {
			die("%s: %s", "realloc", strerror(errno));
			return;
		}
		dir->topdir->watch_index = newptr;
		dir->topdir->watch_index_alloced = new_size;
	}

	memset(&
	       (dir->topdir->watch_index[dir->topdir->watch_index_length]),
	       0, sizeof(dir->topdir->watch_index[0]));

	dir->topdir->watch_index[dir->topdir->watch_index_length].wd = wd;
	dir->topdir->watch_index[dir->topdir->watch_index_length].dir =
	    dir;

	dir->topdir->watch_index_length++;
	dir->topdir->watch_index_unsorted = 1;
}


/*
 * Remove the given watch descriptor from the directory index.
 */
static void ds_watch_index_remove(ds_dir_t topdir, int wd)
{
	int readidx, writeidx;

	if (NULL == topdir)
		return;

	for (readidx = 0, writeidx = 0;
	     readidx < topdir->watch_index_length; readidx++) {
		if (topdir->watch_index[readidx].wd == wd) {
			continue;
		}
		if (readidx != writeidx) {
			topdir->watch_index[writeidx] =
			    topdir->watch_index[readidx];
		}
		writeidx++;
	}
	topdir->watch_index_length = writeidx;
	topdir->watch_index_unsorted = 1;
}


/*
 * Comparison function for the directory index.
 */
static int ds_watch_index_compare(const void *a, const void *b)
{
	if (((ds_watch_index_t) a)->wd < ((ds_watch_index_t) b)->wd)
		return -1;
	if (((ds_watch_index_t) a)->wd > ((ds_watch_index_t) b)->wd)
		return 1;
	return 0;
}


/*
 * Return the directory structure associated with the given watch
 * descriptor, or NULL if none.
 */
static ds_dir_t ds_watch_index_lookup(ds_dir_t topdir, int wd)
{
	struct ds_watch_index_s key;
	ds_watch_index_t result;

	if (NULL == topdir)
		return NULL;
	if (NULL == topdir->watch_index)
		return NULL;

	if ((topdir->watch_index_unsorted)
	    && (topdir->watch_index_length > 0)) {
		qsort(topdir->watch_index, topdir->watch_index_length,
		      sizeof(topdir->watch_index[0]),
		      ds_watch_index_compare);
		topdir->watch_index_unsorted = 0;
	}

	key.wd = wd;
	result =
	    bsearch(&key, topdir->watch_index, topdir->watch_index_length,
		    sizeof(topdir->watch_index[0]),
		    ds_watch_index_compare);

	if (NULL == result)
		return NULL;

	return result->dir;
}


/*
 * Add an entry to the change queue.
 */
static void _ds_change_queue_add(ds_dir_t topdir, time_t when,
				 ds_file_t file, ds_dir_t dir)
{
	int idx;

	if (NULL == topdir)
		return;

	if ((NULL == file) && (NULL == dir))
		return;

	/*
	 * Check the change isn't already queued - don't queue it twice.
	 */
	for (idx = 0; idx < topdir->change_queue_length; idx++) {
		if (NULL != file) {
			if (topdir->change_queue[idx].file == file)
				return;
		}
		if (NULL != dir) {
			if (topdir->change_queue[idx].dir == dir)
				return;
		}
	}

	/*
	 * Extend the array if necessary.
	 */
	if (topdir->change_queue_length >= topdir->change_queue_alloced) {
		int new_size;
		ds_change_queue_t newptr;
		new_size =
		    topdir->change_queue_alloced +
		    CHANGE_QUEUE_ALLOC_CHUNK;
		newptr =
		    realloc(topdir->change_queue,
			    new_size * sizeof(topdir->change_queue[0]));
		if (NULL == newptr) {
			die("%s: %s", "realloc", strerror(errno));
			return;
		}
		topdir->change_queue = newptr;
		topdir->change_queue_alloced = new_size;
	}

	debug("%s: %s: %s", "adding to change queue",
	      NULL == file ? "scan directory" : "check file",
	      NULL == file ? dir->path : file->path);

	/*
	 * Add the new entry, and extend the length of the array.
	 */
	topdir->change_queue[topdir->change_queue_length].when = when;
	topdir->change_queue[topdir->change_queue_length].file = file;
	topdir->change_queue[topdir->change_queue_length].dir = dir;

	topdir->change_queue_length++;
}


/*
 * Queue a file check.
 */
static void ds_change_queue_file_add(ds_file_t file, time_t when)
{
	if (NULL == file)
		return;
	if (NULL == file->parent)
		return;
	if (NULL == file->parent->topdir)
		return;

	if (0 == when)
		when = time(NULL) + 2;

	/* TODO: delay scan more if the file is big */
	_ds_change_queue_add(file->parent->topdir, when, file, NULL);
}


/*
 * Remove a file from the change queue.
 */
static void ds_change_queue_file_remove(ds_file_t file)
{
	ds_dir_t topdir;
	int idx;

	if (NULL == file)
		return;
	if (NULL == file->parent)
		return;
	if (NULL == file->parent->topdir)
		return;

	topdir = file->parent->topdir;

	for (idx = 0; idx < topdir->change_queue_length; idx++) {
		if (topdir->change_queue[idx].file == file) {
			topdir->change_queue[idx].file = NULL;
		}
	}
}


/*
 * Queue a directory scan.
 */
static void ds_change_queue_dir_add(ds_dir_t dir, time_t when)
{
	if (NULL == dir)
		return;
	if (NULL == dir->topdir)
		return;
	if (0 == when)
		when = time(NULL);
	_ds_change_queue_add(dir->topdir, when, NULL, dir);
}


/*
 * Remove a directory from the change queue.
 */
static void ds_change_queue_dir_remove(ds_dir_t dir)
{
	int idx;

	if (NULL == dir)
		return;
	if (NULL == dir->topdir)
		return;

	for (idx = 0; idx < dir->topdir->change_queue_length; idx++) {
		if (dir->topdir->change_queue[idx].dir == dir) {
			dir->topdir->change_queue[idx].dir = NULL;
		}
	}
}


/*
 * Add a file to the list of files in the given directory; if the file is
 * already in the list, return the existing file.
 *
 * The "name" string should contain the name of the file relative to the
 * directory (not the full path), e.g. "somefile".
 *
 * Returns the file, or NULL on error.
 */
static ds_file_t ds_file_add(ds_dir_t dir, const char *name)
{
	ds_file_t file;
	int idx;

	if (NULL == dir)
		return NULL;
	if (NULL == name)
		return NULL;
	if (NULL == dir->absolute_path)
		return NULL;


	/*
	 * Check we don't already have this file in this directory - if we
	 * do, return the existing file.
	 */
	for (idx = 0; idx < dir->file_count; idx++) {
		if (strcmp(dir->files[idx]->leaf, name) == 0) {
			return dir->files[idx];
		}
	}

	/*
	 * Extend the file array in the directory structure if we need to.
	 */
	if (dir->file_count >= dir->file_array_alloced) {
		int target_array_alloced;
		void *newptr;

		target_array_alloced = dir->file_array_alloced;
		while (target_array_alloced <= dir->file_count) {
			target_array_alloced += DIRCONTENTS_ALLOC_CHUNK;
		}
		newptr =
		    realloc((void *) (dir->files),
			    target_array_alloced * sizeof(dir->files[0]));
		if (NULL == newptr) {
			die("%s: %s", "realloc", strerror(errno));
			return NULL;
		}
		dir->files = newptr;
		dir->file_array_alloced = target_array_alloced;
	}

	/*
	 * Allocate a new file structure.
	 */
	file = calloc(1, sizeof(*file));
	if (NULL == file) {
		die("%s: %s", "calloc", strerror(errno));
		return NULL;
	}

	/*
	 * Fill in the file structure.
	 */
	if (asprintf
	    (&(file->absolute_path), "%s/%s", dir->absolute_path,
	     name) < 0) {
		die("%s: %s", "asprintf", strerror(errno));
		free(file);
		return NULL;
	}
	if (NULL != dir->topdir) {
		file->path =
		    &(file->absolute_path
		      [strlen(dir->topdir->absolute_path) + 1]);
	} else {
		file->path =
		    &(file->absolute_path[strlen(dir->absolute_path) + 1]);
	}
	file->leaf = ds_leafname(file->absolute_path);
	file->parent = dir;
	file->seen_in_rescan = 0;

	/*
	 * Add the file to the directory structure, and mark the list as
	 * unsorted.
	 */
	dir->files[dir->file_count] = file;
	dir->file_count++;
	dir->files_unsorted = 1;

	return file;
}


/*
 * Free a file information structure and its contents.
 *
 * The parent directory's "files" list is updated to remove this file from
 * it.
 */
static void ds_file_remove(ds_file_t file)
{
	if (NULL == file)
		return;
	if (NULL == file->path)
		return;

	/*
	 * Remove this file from our parent directory's file listing, if we
	 * have a parent.
	 */
	if ((NULL != file->parent) && (NULL != file->leaf)) {
		int readidx, writeidx;
		for (readidx = 0, writeidx = 0;
		     readidx < file->parent->file_count; readidx++) {
			if ((NULL != file->parent->files[readidx]->leaf)
			    &&
			    (strcmp
			     (file->parent->files[readidx]->leaf,
			      file->leaf) == 0)
			    ) {
				continue;
			}
			if (readidx != writeidx) {
				file->parent->files[writeidx] =
				    file->parent->files[readidx];
			}
			writeidx++;
		}
		file->parent->file_count = writeidx;
		file->parent->files_unsorted = 1;
	}

	/* Remove the file from the change queue. */
	ds_change_queue_file_remove(file);

	/*
	 * Free the memory used by the pathname.
	 */
	debug("%s: %s", file->path, "removing from file list");
	free(file->absolute_path);
	file->absolute_path = NULL;
	file->path = NULL;
	file->leaf = NULL;

	/* Free the file structure itself. */
	free(file);
}


/*
 * Check the given file's mtime and size; if either have changed, return 1.
 *
 * Returns 0 if nothing has changed, 1 if it has, or -1 if the file does not
 * exist or is not a regular file.
 *
 * If the file is successfully opened but there is an error while reading
 * it, 0 is returned as if it had not changed.
 */
static int ds_file_checkchanged(ds_file_t file)
{
	struct stat sb;

	if (NULL == file)
		return -1;

	if (NULL == file->absolute_path)
		return -1;

	if (lstat(file->absolute_path, &sb) != 0)
		return -1;

	if (!S_ISREG(sb.st_mode))
		return -1;

	if ((sb.st_mtime == file->mtime) && (sb.st_size == file->size))
		return 0;

	debug("%s: %s", file->path, "file changed");

	file->mtime = sb.st_mtime;
	file->size = sb.st_size;

	return 1;
}


/*
 * Allocate and return a new top-level directory absolutely rooted at
 * "top_path".  All reported paths within the structure will be relative to
 * "top_path".
 *
 * The "fd_inotify" parameter should be the file descriptor to add directory
 * watches to for inoitfy, or -1 if inotify is not being used.
 */
static ds_dir_t ds_dir_toplevel(int fd_inotify, const char *top_path)
{
	ds_dir_t dir;

	dir = calloc(1, sizeof(*dir));
	if (NULL == dir) {
		die("%s: %s", "calloc", strerror(errno));
		return NULL;
	}

	dir->absolute_path = realpath(top_path, NULL);
	if (NULL == dir->absolute_path) {
		die("%s: %s", "realpath", strerror(errno));
		free(dir);
		return NULL;
	}

	dir->path = &(dir->absolute_path[strlen(dir->absolute_path)]);
	dir->leaf = dir->path;
	dir->wd = -1;
	dir->depth = 0;
	dir->parent = NULL;
	dir->topdir = dir;
	dir->seen_in_rescan = 0;

	dir->fd_inotify = fd_inotify;

	return dir;
}


/*
 * Add a subdirectory to the list of subdirectories in the given parent
 * directory; if the subdirectory is already in the list, just return the
 * existing subdirectory.
 *
 * The "name" string should contain the name of the directory relative to
 * the parent directory (not the full path), e.g. "somedir".
 *
 * Returns the subdirectory, or NULL on error.
 */
static ds_dir_t ds_dir_add(ds_dir_t dir, const char *name)
{
	ds_dir_t subdir;
	int idx;

	if (NULL == dir)
		return NULL;
	if (NULL == name)
		return NULL;

	/*
	 * Check that this subdirectory wouldn't be too deep.
	 */
	if (dir->depth >= max_directory_depth) {
		debug("%s/%s: %s", dir->path, name,
		      "too deep - not adding");
		return NULL;
	}

	/*
	 * Check we don't already have this subdirectory in the directory
	 * structure - if we do, return the existing structure.
	 */
	for (idx = 0; idx < dir->subdir_count; idx++) {
		if (strcmp(dir->subdirs[idx]->leaf, name) == 0) {
			return dir->subdirs[idx];
		}
	}

	/*
	 * Extend the subdirectory array in the directory structure if we
	 * need to.
	 */
	if (dir->subdir_count >= dir->subdir_array_alloced) {
		int target_array_alloced;
		void *newptr;

		target_array_alloced = dir->subdir_array_alloced;
		while (target_array_alloced <= dir->subdir_count) {
			target_array_alloced += DIRCONTENTS_ALLOC_CHUNK;
		}
		newptr =
		    realloc((void *) (dir->subdirs),
			    target_array_alloced *
			    sizeof(dir->subdirs[0]));
		if (NULL == newptr) {
			die("%s: %s", "realloc", strerror(errno));
			return NULL;
		}
		dir->subdirs = newptr;
		dir->subdir_array_alloced = target_array_alloced;
	}

	/*
	 * Allocate a new directory structure for the subdirectory.
	 */
	subdir = calloc(1, sizeof(*subdir));
	if (NULL == subdir) {
		die("%s: %s", "calloc", strerror(errno));
		return NULL;
	}

	/*
	 * Fill in the new subdirectory structure.
	 */
	if (asprintf
	    (&(subdir->absolute_path), "%s/%s", dir->absolute_path,
	     name) < 0) {
		die("%s: %s", "asprintf", strerror(errno));
		free(subdir);
		return NULL;
	}
	if (NULL != dir->topdir) {
		subdir->path =
		    &(subdir->absolute_path
		      [strlen(dir->topdir->absolute_path) + 1]);
	} else {
		subdir->path =
		    &(subdir->absolute_path
		      [strlen(dir->absolute_path) + 1]);
	}
	subdir->leaf = ds_leafname(subdir->absolute_path);

	subdir->wd = -1;
	subdir->depth = dir->depth + 1;
	subdir->parent = dir;
	subdir->topdir = dir->topdir;
	subdir->seen_in_rescan = 0;

	/*
	 * Add the subdirectory to the directory structure, and mark the
	 * list as unsorted.
	 */
	dir->subdirs[dir->subdir_count] = subdir;
	dir->subdir_count++;
	dir->subdirs_unsorted = 1;

	return subdir;
}


/*
 * Free a directory information structure.  This includes all files and
 * directories it contains.
 *
 * The parent directory's "subdirs" list is updated to remove this
 * subdirectory from it.
 */
static void ds_dir_remove(ds_dir_t dir)
{
	int item;

	if (NULL == dir)
		return;

	/*
	 * Remove the watch on this directory.
	 */
	if ((0 <= dir->wd) && (NULL != dir->topdir)
	    && (0 <= dir->topdir->fd_inotify)) {
		debug("%s: %s", dir->path, "removing watch");
		if (inotify_rm_watch(dir->topdir->fd_inotify, dir->wd) !=
		    0) {
			/*
			 * We can get EINVAL if the directory was deleted,
			 * so just ignore that.
			 */
			if (errno != EINVAL) {
				error("%s: %s", "inotify_rm_watch",
				      strerror(errno));
			}
		}
		ds_watch_index_remove(dir->topdir, dir->wd);
		dir->wd = -1;
	}

	/*
	 * Remove all files from this directory.
	 */
	if (NULL != dir->files) {
		for (item = 0; item < dir->file_count; item++) {
			/* wipe parent field to avoid wasted work */
			dir->files[item]->parent = NULL;
			ds_file_remove(dir->files[item]);
		}
		free(dir->files);
		dir->files = NULL;
		dir->file_count = 0;
		dir->file_array_alloced = 0;
	}

	/*
	 * Remove all subdirectories from this directory (recursive).
	 */
	if (NULL != dir->subdirs) {
		for (item = 0; item < dir->subdir_count; item++) {
			dir->subdirs[item]->parent = NULL;
			ds_dir_remove(dir->subdirs[item]);
		}
		free(dir->subdirs);
		dir->subdirs = NULL;
		dir->subdir_count = 0;
		dir->subdir_array_alloced = 0;
	}

	/*
	 * Remove this subdirectory from our parent's directory listing, if
	 * we have a parent.  Note that when we call ourselves, above, we've
	 * wiped the parent in the subdirectory, to avoid wasted work.
	 */
	if ((NULL != dir->parent) && (NULL != dir->leaf)) {
		int readidx, writeidx;
		for (readidx = 0, writeidx = 0;
		     readidx < dir->parent->subdir_count; readidx++) {
			if ((NULL != dir->parent->subdirs[readidx]->leaf)
			    &&
			    (strcmp
			     (dir->parent->subdirs[readidx]->leaf,
			      dir->leaf) == 0)
			    ) {
				continue;
			}
			if (readidx != writeidx) {
				dir->parent->subdirs[writeidx] =
				    dir->parent->subdirs[readidx];
			}
			writeidx++;
		}
		dir->parent->subdir_count = writeidx;
		dir->parent->subdirs_unsorted = 1;
	}

	/* Remove the directory from the change queue. */
	ds_change_queue_dir_remove(dir);

	/*
	 * Free the memory used by the pathname.
	 */
	if (NULL != dir->absolute_path) {
		debug("%s: %s", dir->path, "removing from directory list");
		free(dir->absolute_path);
		dir->absolute_path = NULL;
		dir->path = NULL;
		dir->leaf = NULL;
	}

	/*
	 * Free the watch index.
	 */
	if (NULL != dir->watch_index) {
		free(dir->watch_index);
		dir->watch_index = NULL;
	}

	/*
	 * Free the change queue.
	 */
	if (NULL != dir->change_queue) {
		free(dir->change_queue);
		dir->change_queue = NULL;
	}

	/*
	 * Free the changed paths list.
	 */
	if (NULL != dir->changed_paths) {
		int idx;
		for (idx = 0; idx < dir->changed_paths_length; idx++) {
			free(dir->changed_paths[idx]);
		}
		free(dir->changed_paths);
		dir->changed_paths = NULL;
		dir->changed_paths_length = 0;
		dir->changed_paths_alloced = 0;
	}

	/* Free the directory structure itself. */
	free(dir);
}


/*
 * Filter for any filename.
 *
 * Ignore anything ending in .tmp or ~ by default, or if exclude_count is
 * >0, ignore anything matching any of the patterns in excludes[].
 *
 * Returns 1 if the file should be included, 0 if it should be ignored.
 */
static int ds_filename_valid(const char *leafname)
{
	if (leafname[0] == 0)
		return 0;
	if ((leafname[0] == '.') && (leafname[1] == 0))
		return 0;
	if ((leafname[0] == '.') && (leafname[1] == '.')
	    && (leafname[2] == 0))
		return 0;

	if (exclude_count > 0) {
		/*
		 * If given an exclusion list, use it.
		 */
		int eidx;
		for (eidx = 0; eidx < exclude_count; eidx++) {
			if (NULL == excludes[eidx])
				continue;
			if (fnmatch(excludes[eidx], leafname, 0) == 0)
				return 0;
		}
	} else {
		/*
		 * Default is to exclude *~ and *.tmp
		 */
		int namelen;
		namelen = strlen(leafname);
		if ((1 <= namelen) && (leafname[namelen - 1] == '~'))
			return 0;
		if ((namelen > 4)
		    && (strcmp(&(leafname[namelen - 4]), ".tmp") == 0))
			return 0;
	}

	return 1;
}


/*
 * Filter for scanning directories - only include filenames we should be
 * processing.
 */
static int scan_directory_filter(const struct dirent *d)
{
	return ds_filename_valid(d->d_name);
}


/*
 * Recursively scan the given directory.  Also checks files for changes. 
 * Returns nonzero if the scan failed, in which case the directory will have
 * been deleted from the lists.
 *
 * If no_recurse is true, then no subdirectories are scanned, though
 * subdirectories are still added and removed as necessary.
 */
static int ds_dir_scan(ds_dir_t dir, flag_t no_recurse)
{
	struct dirent **namelist;
	int namelist_length;
	int ourpath_length;
	int diridx, fileidx, itemidx;
	struct stat dirsb;

	if (NULL == dir)
		return 1;
	if (NULL == dir->absolute_path)
		return 1;

	if (dir->depth > max_directory_depth) {
		debug("%s: %s: %s", dir->path, "too deep - removing");
		ds_dir_remove(dir);
		return 1;
	}

	if (lstat(dir->absolute_path, &dirsb) != 0) {
		error("%s: %s: %s", dir->path, "lstat", strerror(errno));
		ds_dir_remove(dir);
		return 1;
	}

	ourpath_length = strlen(dir->absolute_path);

	namelist_length =
	    scandir(dir->absolute_path, &namelist, scan_directory_filter,
		    alphasort);
	if (0 > namelist_length) {
		error("%s: %s: %s", dir->absolute_path, "scandir",
		      strerror(errno));
		ds_dir_remove(dir);
		return 1;
	}

	/*
	 * Mark all subdirectories and files as having not been seen, so we
	 * can spot which ones have been removed after we've scanned the
	 * directory.
	 */
	for (diridx = 0; diridx < dir->subdir_count; diridx++) {
		dir->subdirs[diridx]->seen_in_rescan = 0;
	}
	for (fileidx = 0; fileidx < dir->file_count; fileidx++) {
		dir->files[fileidx]->seen_in_rescan = 0;
	}

	/*
	 * Go through the directory scan results, adding new items to the
	 * arrays.
	 */
	for (itemidx = 0; itemidx < namelist_length; itemidx++) {
		char *item_full_path;
		char *item_leaf;
		struct stat sb;

		if (asprintf(&item_full_path, "%s/%s", dir->absolute_path,
			     namelist[itemidx]->d_name) < 0) {
			die("%s: %s", "asprintf", strerror(errno));
			return 1;
		}
		item_leaf = ds_leafname(item_full_path);

		free(namelist[itemidx]);

		if (ds_filename_valid(item_leaf) == 0) {
			free(item_full_path);
			continue;
		}

		if (lstat(item_full_path, &sb) != 0) {
			free(item_full_path);
			continue;
		}

		if (S_ISREG(sb.st_mode)) {
			ds_file_t file;
			file = ds_file_add(dir, item_leaf);
			if (NULL != file)
				file->seen_in_rescan = 1;
		} else if (S_ISDIR(sb.st_mode)) {
			ds_dir_t subdir;
			if (sb.st_dev == dirsb.st_dev) {
				subdir = ds_dir_add(dir, item_leaf);
				if (NULL != subdir)
					subdir->seen_in_rescan = 1;
			} else {
				debug("%s/%s: %s", dir->path, item_leaf,
				      "skipping - different filesystem");
			}
		}

		free(item_full_path);
	}

	free(namelist);

	/*
	 * Delete any subdirectories that we did not see on rescan, and
	 * recursively scan those that we did.
	 */
	for (diridx = 0; diridx < dir->subdir_count; diridx++) {
		if (dir->subdirs[diridx]->seen_in_rescan) {
			if (no_recurse)
				continue;
			if (ds_dir_scan(dir->subdirs[diridx], 0) != 0) {
				/* Go back one, as this diridx has now gone */
				diridx--;
			}
		} else {
			ds_dir_remove(dir->subdirs[diridx]);
			/* Go back one, as this diridx has now gone */
			diridx--;
		}
	}

	/*
	 * Delete any files that we did not see on rescan.
	 */
	for (fileidx = 0; fileidx < dir->file_count; fileidx++) {
		if (dir->files[fileidx]->seen_in_rescan)
			continue;
		ds_file_remove(dir->files[fileidx]);
		/* Go back one, as this fileidx has now gone */
		fileidx--;
	}

	/*
	 * Check all files for changes.
	 */
	for (fileidx = 0; fileidx < dir->file_count; fileidx++) {
		int changed;
		changed = ds_file_checkchanged(dir->files[fileidx]);
		if (0 > changed) {
			ds_file_remove(dir->files[fileidx]);
			/* Go back one, as this fileidx has now gone */
			fileidx--;
		}
	}

	/*
	 * Add an inotify watch to this directory if there isn't one
	 * already.
	 */
	if ((0 > dir->wd) && (NULL != dir->topdir)
	    && (0 <= dir->topdir->fd_inotify)) {
		debug("%s: %s", dir->path, "adding watch");
		dir->wd =
		    inotify_add_watch(dir->topdir->fd_inotify,
				      dir->absolute_path,
				      IN_CREATE | IN_DELETE | IN_MODIFY |
				      IN_DELETE_SELF | IN_MOVED_FROM |
				      IN_MOVED_TO);
		if (0 > dir->wd) {
			error("%s: %s: %s", dir->path, "inotify_add_watch",
			      strerror(errno));
		} else {
			/*
			 * Add this watch descriptor to the directory index,
			 * so that we can find this directory structure when
			 * an inotify event arrives.
			 */
			ds_watch_index_add(dir, dir->wd);
		}
	}

	return 0;
}


/*
 * Process queued changes until the given time or until all queue entries
 * we're ready to process have been done.
 */
static void ds_change_queue_process(ds_dir_t topdir, time_t work_until)
{
	int readidx, writeidx;

	if (NULL == topdir)
		return;

	if (0 >= topdir->change_queue_length)
		return;

	debug("%s: %d", "change queue: starting run, queue length",
	      topdir->change_queue_length);

	for (readidx = 0, writeidx = 0;
	     readidx < topdir->change_queue_length; readidx++) {
		ds_change_queue_t entry;
		time_t now;

		entry = &(topdir->change_queue[readidx]);
		if ((entry->file == NULL) && (entry->dir == NULL))
			continue;

		time(&now);

		/*
		 * Skip if it's not yet time for this item, or if we've
		 * reached our work_until time.
		 */
		if ((entry->when > now) || (now >= work_until)) {
			if (readidx != writeidx) {
				memcpy(&(topdir->change_queue[writeidx]),
				       &(topdir->change_queue[readidx]),
				       sizeof(*entry));
				writeidx++;
			}
			writeidx++;
			continue;
		}

		if (NULL != entry->file) {
			int changed;
			ds_file_t file;

			file = entry->file;
			entry->file = NULL;

			debug("%s: %s", file->path,
			      "checking for changes");
			changed = ds_file_checkchanged(file);

			if (0 > changed) {
				mark_path_changed(file->parent->topdir,
						  file->parent->path, 1);
				ds_file_remove(file);
			} else if (0 < changed) {
				mark_path_changed(file->parent->topdir,
						  file->path, 0);
			}
		} else if (NULL != entry->dir) {
			ds_dir_t dir;

			dir = entry->dir;
			entry->dir = NULL;

			debug("%s: %s", dir->path, "triggering scan");
			ds_dir_scan(dir, 0);
		}
	}

	topdir->change_queue_length = writeidx;

	debug("%s: %d", "change queue: run ended, queue length",
	      topdir->change_queue_length);
}


/*
 * Process a change to a directory inside a watched directory.
 */
static void process_dir_change(struct inotify_event *event, ds_dir_t dir)
{
	ds_dir_t subdir;
	inotify_action_t action;
	int idx;
	char *fullpath;
	struct stat sb;
	ds_dir_t newdir;

	/*
	 * Find the directory structure to which this event refers, if
	 * known.
	 */
	subdir = NULL;
	for (idx = 0; idx < dir->subdir_count; idx++) {
		if (NULL == dir->subdirs[idx]->leaf)
			continue;
		if (strcmp(event->name, dir->subdirs[idx]->leaf) != 0)
			continue;
		subdir = dir->subdirs[idx];
		break;
	}

	/*
	 * Decide what to do: is this a newly created item, an existing item
	 * that has been modified, or an existing item that has been
	 * deleted?
	 */
	action = IN_ACTION_NONE;
	if (event->mask & (IN_ATTRIB | IN_CREATE | IN_MODIFY |
			   IN_MOVED_TO)) {
		action = IN_ACTION_CREATE;
		if (NULL != subdir)
			action = IN_ACTION_UPDATE;
	} else if ((event->mask & (IN_DELETE | IN_MOVED_FROM))
		   && (NULL != subdir)) {
		action = IN_ACTION_DELETE;
	}

	switch (action) {
	case IN_ACTION_NONE:
		break;
	case IN_ACTION_CREATE:
		/*
		 * This a new directory we've not seen before.
		 */

		/*
		 * Ignore the directory if it doesn't pass the filename
		 * filter.
		 */
		if (ds_filename_valid(event->name) == 0) {
			break;
		}

		/*
		 * Make a copy of the full pathname.
		 */
		if (asprintf(&fullpath, "%s/%s", dir->absolute_path,
			     event->name) < 0) {
			die("%s: %s", "asprintf", strerror(errno));
			return;
		}

		/*
		 * Ignore the directory if it doesn't exist.
		 */
		if (lstat(fullpath, &sb) != 0) {
			free(fullpath);
			break;
		}

		/*
		 * Ignore it if it's not a directory.
		 */
		if (!S_ISDIR(sb.st_mode)) {
			free(fullpath);
			break;
		}

		/*
		 * Add the new directory and queue a scan for it.
		 */
		debug("%s: %s", fullpath, "adding new subdirectory");
		newdir = ds_dir_add(dir, event->name);
		free(fullpath);
		ds_change_queue_dir_add(newdir, 0);

		/*
		 * Mark this as a changed path.
		 */
		mark_path_changed(dir->topdir, newdir->path, 1);

		break;
	case IN_ACTION_UPDATE:
		/*
		 * This a directory we've seen before, so queue a rescan for
		 * it.
		 */
		debug("%s: %s", subdir->path, "queueing rescan");
		ds_change_queue_dir_add(subdir, 0);
		break;
	case IN_ACTION_DELETE:
		/*
		 * If we've seen this directory before, delete its
		 * structure.
		 */
		debug("%s: %s", subdir->path, "triggering removal");
		ds_dir_remove(subdir);
		/*
		 * Mark the parent directory as a changed path.
		 */
		mark_path_changed(dir->topdir, dir->path, 1);
		break;
	}
}


/*
 * Process a change to a file inside a watched directory.
 */
static void process_file_change(struct inotify_event *event, ds_dir_t dir)
{
	ds_file_t file;
	inotify_action_t action;
	int idx;
	char *fullpath;
	struct stat sb;
	ds_file_t newfile;

	/*
	 * Find the file structure to which this event refers, if known.
	 */
	file = NULL;
	for (idx = 0; idx < dir->file_count; idx++) {
		if (NULL == dir->files[idx]->leaf)
			continue;
		if (strcmp(event->name, dir->files[idx]->leaf) != 0)
			continue;
		file = dir->files[idx];
		break;
	}

	/*
	 * Decide what to do: is this a newly created item, an existing item
	 * that has been modified, or an existing item that has been
	 * deleted?
	 */
	action = IN_ACTION_NONE;
	if (event->mask & (IN_ATTRIB | IN_CREATE | IN_MODIFY |
			   IN_MOVED_TO)) {
		action = IN_ACTION_CREATE;
		if (NULL != file)
			action = IN_ACTION_UPDATE;
	} else if ((event->mask & (IN_DELETE | IN_MOVED_FROM))
		   && (NULL != file)) {
		action = IN_ACTION_DELETE;
	}

	switch (action) {
	case IN_ACTION_NONE:
		break;
	case IN_ACTION_CREATE:
		/*
		 * This a new file we've not seen before.
		 */

		/*
		 * Ignore the file if it doesn't pass the filename filter.
		 */
		if (ds_filename_valid(event->name) == 0) {
			break;
		}

		/*
		 * Make a copy of the full pathname.
		 */
		if (asprintf(&fullpath, "%s/%s", dir->absolute_path,
			     event->name) < 0) {
			die("%s: %s", "asprintf", strerror(errno));
			return;
		}

		/*
		 * Ignore the file if it doesn't exist or it isn't a regular
		 * file.
		 */
		if (lstat(fullpath, &sb) != 0) {
			free(fullpath);
			break;
		} else if (!S_ISREG(sb.st_mode)) {
			free(fullpath);
			break;
		}

		/*
		 * Add the new file and queue it to be checked.
		 */
		debug("%s: %s", fullpath, "adding new file");
		newfile = ds_file_add(dir, event->name);
		ds_change_queue_file_add(newfile, 0);

		free(fullpath);

		break;
	case IN_ACTION_UPDATE:
		/*
		 * This a file we've seen before, so queue a check for it.
		 */
		ds_change_queue_file_add(file, 0);
		break;
	case IN_ACTION_DELETE:
		/*
		 * If we've seen this file before, delete its structure.
		 */
		debug("%s: %s", file->path, "triggering removal");
		/*
		 * Mark the parent directory as a changed path.
		 */
		mark_path_changed(file->parent->topdir, file->parent->path,
				  1);
		ds_file_remove(file);
		break;
	}
}


/*
 * Process incoming inotify events.
 */
static void process_inotify_events(ds_dir_t topdir)
{
	unsigned char readbuf[8192];
	ssize_t got, pos;

	if (NULL == topdir)
		return;
	if (0 > topdir->fd_inotify)
		return;

	memset(readbuf, 0, sizeof(readbuf));

	/*
	 * Read as many events as we can.
	 */
	got = read(topdir->fd_inotify, readbuf, sizeof(readbuf));
	if (got <= 0) {
		error("%s: (%d): %s", "inotify read event", got,
		      strerror(errno));
		close(topdir->fd_inotify);
		topdir->fd_inotify = -1;
		return;
	}

	/*
	 * Process each event that we've read.
	 */
	for (pos = 0; pos < got;) {
		struct inotify_event *event;
		ds_dir_t dir = NULL;

		event = (struct inotify_event *) &(readbuf[pos]);
		dir = ds_watch_index_lookup(topdir, event->wd);

#if ENABLE_DEBUGGING
		if (debugging_enabled) {
			char flags[1024];
			flags[0] = 0;
			if (event->mask & IN_ACCESS)
				strcat(flags, " IN_ACCESS");
			if (event->mask & IN_ATTRIB)
				strcat(flags, " IN_ATTRIB");
			if (event->mask & IN_CLOSE_WRITE)
				strcat(flags, " IN_CLOSE_WRITE");
			if (event->mask & IN_CLOSE_NOWRITE)
				strcat(flags, " IN_CLOSE_NOWRITE");
			if (event->mask & IN_CREATE)
				strcat(flags, " IN_CREATE");
			if (event->mask & IN_DELETE)
				strcat(flags, " IN_DELETE");
			if (event->mask & IN_DELETE_SELF)
				strcat(flags, " IN_DELETE_SELF");
			if (event->mask & IN_MODIFY)
				strcat(flags, " IN_MODIFY");
			if (event->mask & IN_MOVE_SELF)
				strcat(flags, " IN_MOVE_SELF");
			if (event->mask & IN_MOVED_FROM)
				strcat(flags, " IN_MOVED_FROM");
			if (event->mask & IN_MOVED_TO)
				strcat(flags, " IN_MOVED_TO");
			if (event->mask & IN_OPEN)
				strcat(flags, " IN_OPEN");
			if (event->mask & IN_IGNORED)
				strcat(flags, " IN_IGNORED");
			if (event->mask & IN_ISDIR)
				strcat(flags, " IN_ISDIR");
			if (event->mask & IN_Q_OVERFLOW)
				strcat(flags, " IN_Q_OVERFLOW");
			if (event->mask & IN_UNMOUNT)
				strcat(flags, " IN_UNMOUNT");
			debug("%s: %d: %s: %.*s:%s", "inotify", event->wd,
			      NULL == dir ? "(unknown)" : dir->path,
			      event->len,
			      NULL == event->name
			      && 0 < event->len ? "(none)" : event->name,
			      flags);
		}
#endif				/* ENABLE_DEBUGGING */

		pos += sizeof(*event) + event->len;

		/*
		 * There's nothing we can do if we don't know which
		 * directory it was.
		 */
		if (NULL == dir)
			continue;

		if (event->mask & IN_DELETE_SELF) {
			ds_dir_remove(dir);
			continue;
		}

		/*
		 * If this isn't an event about a named thing in this
		 * directory, we can't do anything.
		 */
		if (NULL == event->name)
			continue;
		if (0 == event->name[0])
			continue;
		if (0 >= event->len)
			continue;

		if (event->mask & IN_ISDIR) {
			process_dir_change(event, dir);
		} else {
			process_file_change(event, dir);
		}
	}
}


/*
 * Add a path to the list of changed paths.
 */
static void mark_path_changed(ds_dir_t topdir, const char *path,
			      flag_t isdir)
{
	char *savepath;
	int idx;

	if (NULL == topdir)
		return;

	if (NULL == path)
		return;

	if (asprintf(&savepath, "%s%s", path, isdir ? "/" : "") < 0) {
		die("%s: %s", "asprintf", strerror(errno));
		return;
	}

	/*
	 * Check the path isn't already listed - don't list it twice.
	 */
	for (idx = 0; idx < topdir->changed_paths_length; idx++) {
		if (strcmp(topdir->changed_paths[idx], savepath) == 0) {
			free(savepath);
			return;
		}
	}

	/*
	 * Extend the array if necessary.
	 */
	if (topdir->changed_paths_length >= topdir->changed_paths_alloced) {
		int new_size;
		char **newptr;
		new_size =
		    topdir->changed_paths_alloced +
		    CHANGEDPATH_ALLOC_CHUNK;
		newptr =
		    realloc(topdir->changed_paths,
			    new_size * sizeof(topdir->changed_paths[0]));
		if (NULL == newptr) {
			die("%s: %s", "realloc", strerror(errno));
			return;
		}
		topdir->changed_paths = newptr;
		topdir->changed_paths_alloced = new_size;
	}

	debug("%s: %s", "adding to changed paths", savepath);

	/*
	 * Add the new entry, and extend the length of the array.
	 */
	topdir->changed_paths[topdir->changed_paths_length] = savepath;
	topdir->changed_paths_length++;
}


/*
 * Write out a new file containing the current changed paths list, and clear
 * the list.
 */
static void dump_changed_paths(ds_dir_t topdir, const char *savedir)
{
	char *savefile;
	char *tmpfile;
	struct tm *tm;
	time_t t;
	int tmpfd;
	FILE *fptr;
	int idx;

	if (NULL == topdir->changed_paths)
		return;
	if (0 >= topdir->changed_paths_length)
		return;

	t = time(NULL);
	tm = localtime(&t);

	if (asprintf
	    (&savefile, "%s/%04d%02d%02d-%02d%02d%02d.%d", savedir,
	     tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour,
	     tm->tm_min, tm->tm_sec, getpid()) < 0) {
		die("%s: %s", "asprintf", strerror(errno));
		return;
	}

	tmpfd = ds_tmpfile(savefile, &tmpfile);
	if (0 > tmpfd) {
		free(savefile);
		return;
	}

	fptr = fdopen(tmpfd, "w");
	if (NULL == fptr) {
		error("%s: %s", tmpfile, strerror(errno));
		close(tmpfd);
		remove(tmpfile);
		free(tmpfile);
		free(savefile);
		return;
	}

	for (idx = 0; idx < topdir->changed_paths_length; idx++) {
		fprintf(fptr, "%s\n", topdir->changed_paths[idx]);
	}

	fclose(fptr);

	if (rename(tmpfile, savefile) != 0) {
		error("%s: %s", savefile, strerror(errno));
		remove(tmpfile);
		free(tmpfile);
		free(savefile);
		return;
	}

	free(tmpfile);
	free(savefile);

	for (idx = 0; idx < topdir->changed_paths_length; idx++) {
		free(topdir->changed_paths[idx]);
	}

	free(topdir->changed_paths);
	topdir->changed_paths = NULL;
	topdir->changed_paths_length = 0;
	topdir->changed_paths_alloced = 0;
}


/*
 * Handler for an exit signal such as SIGTERM - set a flag to trigger an
 * exit.
 */
static void watch_dir_exitsignal(int signum)
{
	watch_dir_exit_now = 1;
}


/*
 * Main entry point.  Set everything up and enter the main loop, which does
 * the following:
 *
 *   - A periodic rescan from the top level directory down.
 *   - Processing of inotify events from all known directories.
 *   - Processing of the change queue generated from the above two.
 *   - Periodic output of a file listing updated paths.
 *
 * Scanned directories are watched using inotify, so that changes to files
 * within it can be noticed immediately.
 *
 * A change queue is maintained, comprising a list of files and directories
 * to re-check, and the time at which to do so.  This is so that when
 * multiple files are changed, or the same file is changed several times, it
 * can be dealt with intelligently - they are pushed on to the change queue,
 * with duplicates being ignored, and then the change queue is processed in
 * chunks to avoid starvation caused by inotify events from one file
 * changing rapidly.
 */
int watch_dir(const char *toplevel_path, const char *changedpath_dir,
	      unsigned long full_scan_interval,
	      unsigned long queue_run_interval,
	      unsigned long queue_run_max_seconds,
	      unsigned long changedpath_dump_interval,
	      unsigned int max_dir_depth, char **excl,
	      unsigned int excl_count)
{
	int fd_inotify;			 /* fd to watch for inotify on */
	time_t next_full_scan;		 /* when to run next full scan */
	ds_dir_t topdir;		 /* top-level directory contents */
	time_t next_change_queue_run;	 /* when to next run changes */
	time_t next_changedpath_dump;	 /* when to next dump changed paths */
	struct sigaction sa;
	flag_t first_run;

	max_directory_depth = max_dir_depth;
	excludes = excl;
	exclude_count = excl_count;

	/*
	 * Set up the signal handlers.
	 */
	sa.sa_handler = watch_dir_exitsignal;
	sigemptyset(&(sa.sa_mask));
	sa.sa_flags = 0;
	sigaction(SIGTERM, &sa, NULL);

	sa.sa_handler = watch_dir_exitsignal;
	sigemptyset(&(sa.sa_mask));
	sa.sa_flags = 0;
	sigaction(SIGINT, &sa, NULL);

	/*
	 * Create the inotify event queue.
	 */
	fd_inotify = inotify_init();
	if (0 > fd_inotify) {
		error("%s: %s", "inotify", strerror(errno));
		return EXIT_FAILURE;
	}

	/*
	 * Create the top-level directory memory structure.
	 */
	topdir = ds_dir_toplevel(fd_inotify, toplevel_path);
	if (NULL == topdir)
		return EXIT_FAILURE;

	/*
	 * Enter the main loop.
	 */

	next_change_queue_run = 0;
	next_full_scan = 0;
	next_changedpath_dump = 0;
	first_run = 1;

	while (!watch_dir_exit_now) {
		time_t now;

		/*
		 * Process new inotify events.
		 */
		if (0 <= fd_inotify) {
			fd_set readfds;
			struct timeval timeout;
			int ready;

			FD_ZERO(&readfds);
			FD_SET(fd_inotify, &readfds);
			timeout.tv_sec = 0;
			timeout.tv_usec = 100000;

			ready =
			    select(1 + fd_inotify, &readfds, NULL,
				   NULL, &timeout);

			if (0 > ready) {
				if (errno != EINTR)
					error("%s: %s", "select",
					      strerror(errno));
				watch_dir_exit_now = 1;
				break;
			} else if ((0 < ready)
				   && FD_ISSET(fd_inotify, &readfds)) {
				process_inotify_events(topdir);
			}
		} else {
			sleep(1);
		}

		time(&now);

		/*
		 * Do a full scan periodically.
		 */
		if (now >= next_full_scan) {
			next_full_scan = now + full_scan_interval;
			ds_change_queue_dir_add(topdir, 0);
		}

		/*
		 * Run our change queue.
		 */
		if (now >= next_change_queue_run) {
			next_change_queue_run = now + queue_run_interval;
			ds_change_queue_process(topdir,
						now +
						queue_run_max_seconds);
		}

		/*
		 * Dump our list of changed paths.
		 */
		if (now >= next_changedpath_dump) {
			next_changedpath_dump =
			    now + changedpath_dump_interval;
			dump_changed_paths(topdir, changedpath_dir);
		}

		first_run = 0;
	}

	ds_dir_remove(topdir);

	if (0 <= fd_inotify)
		close(fd_inotify);

	return EXIT_SUCCESS;
}

/* EOF */
