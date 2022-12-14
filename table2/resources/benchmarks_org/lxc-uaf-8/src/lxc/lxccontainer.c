/* liblxcapi
 *
 * Copyright © 2012 Serge Hallyn <serge.hallyn@ubuntu.com>.
 * Copyright © 2012 Canonical Ltd.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.

 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.

 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#define _GNU_SOURCE
#include <stdarg.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <dirent.h>
#include "config.h"
#include "lxc.h"
#include "state.h"
#include <lxc/lxccontainer.h>
#include "conf.h"
#include "confile.h"
#include "console.h"
#include "cgroup.h"
#include "commands.h"
#include "version.h"
#include "log.h"
#include "bdev.h"
#include "utils.h"
#include "attach.h"
#include <lxc/utils.h>
#include <lxc/monitor.h>
#include <lxc/namespace.h>
#include <sched.h>
#include <arpa/inet.h>

#if HAVE_IFADDRS_H
#include <ifaddrs.h>
#else
#include <../include/ifaddrs.h>
#endif

#ifndef HAVE_GETLINE
#ifdef HAVE_FGETLN
#include <../include/getline.h>
#endif
#endif

lxc_log_define(lxc_container, lxc);

static bool file_exists(char *f)
{
	struct stat statbuf;

	return stat(f, &statbuf) == 0;
}

static bool config_file_exists(const char *lxcpath, const char *cname)
{
	/* $lxcpath + '/' + $cname + '/config' + \0 */
	int ret, len = strlen(lxcpath) + strlen(cname) + 9;
	char *fname = alloca(len);

	ret = snprintf(fname, len,  "%s/%s/config", lxcpath, cname);
	if (ret < 0 || ret >= len)
		return false;

	return file_exists(fname);
}

/*
 * A few functions to help detect when a container creation failed.
 * If a container creation was killed partway through, then trying
 * to actually start that container could harm the host.  We detect
 * this by creating a 'partial' file under the container directory,
 * and keeping an advisory lock.  When container creation completes,
 * we remove that file.  When we load or try to start a container, if
 * we find that file, without a flock, we remove the container.
 */
int ongoing_create(struct lxc_container *c)
{
	int len = strlen(c->config_path) + strlen(c->name) + 10;
	char *path = alloca(len);
	int fd, ret;
	struct flock lk;

	ret = snprintf(path, len, "%s/%s/partial", c->config_path, c->name);
	if (ret < 0 || ret >= len) {
		ERROR("Error writing partial pathname");
		return -1;
	}

	if (!file_exists(path))
		return 0;
	process_lock();
	fd = open(path, O_RDWR);
	process_unlock();
	if (fd < 0) {
		// give benefit of the doubt
		SYSERROR("Error opening partial file");
		return 0;
	}
	lk.l_type = F_WRLCK;
	lk.l_whence = SEEK_SET;
	lk.l_start = 0;
	lk.l_len = 0;
	lk.l_pid = -1;
	if (fcntl(fd, F_GETLK, &lk) == 0 && lk.l_pid != -1) {
		// create is still ongoing
		process_lock();
		close(fd);
		process_unlock();
		return 1;
	}
	// create completed but partial is still there.
	process_lock();
	close(fd);
	process_unlock();
	return 2;
}

int create_partial(struct lxc_container *c)
{
	// $lxcpath + '/' + $name + '/partial' + \0
	int len = strlen(c->config_path) + strlen(c->name) + 10;
	char *path = alloca(len);
	int fd, ret;
	struct flock lk;

	ret = snprintf(path, len, "%s/%s/partial", c->config_path, c->name);
	if (ret < 0 || ret >= len) {
		ERROR("Error writing partial pathname");
		return -1;
	}
	process_lock();
	if ((fd=open(path, O_RDWR | O_CREAT | O_EXCL, 0755)) < 0) {
		SYSERROR("Erorr creating partial file");
		process_unlock();
		return -1;
	}
	lk.l_type = F_WRLCK;
	lk.l_whence = SEEK_SET;
	lk.l_start = 0;
	lk.l_len = 0;
	if (fcntl(fd, F_SETLKW, &lk) < 0) {
		SYSERROR("Error locking partial file %s", path);
		close(fd);
		process_unlock();
		return -1;
	}
	process_unlock();

	return fd;
}

void remove_partial(struct lxc_container *c, int fd)
{
	// $lxcpath + '/' + $name + '/partial' + \0
	int len = strlen(c->config_path) + strlen(c->name) + 10;
	char *path = alloca(len);
	int ret;

	process_lock();
	close(fd);
	process_unlock();
	ret = snprintf(path, len, "%s/%s/partial", c->config_path, c->name);
	if (ret < 0 || ret >= len) {
		ERROR("Error writing partial pathname");
		return;
	}
	if (unlink(path) < 0)
		SYSERROR("Error unlink partial file %s", path);
}

/* LOCKING
 * 1. container_mem_lock(c) protects the struct lxc_container from multiple threads.
 * 2. container_disk_lock(c) protects the on-disk container data - in particular the
 *    container configuration file.
 *    The container_disk_lock also takes the container_mem_lock.
 * 3. thread_mutex protects process data (ex: fd table) from multiple threads.
 * NOTHING mutexes two independent programs with their own struct
 * lxc_container for the same c->name, between API calls.  For instance,
 * c->config_read(); c->start();  Between those calls, data on disk
 * could change (which shouldn't bother the caller unless for instance
 * the rootfs get moved).  c->config_read(); update; c->config_write();
 * Two such updaters could race.  The callers should therefore check their
 * results.  Trying to prevent that would necessarily expose us to deadlocks
 * due to hung callers.  So I prefer to keep the locks only within our own
 * functions, not across functions.
 *
 * If you're going to clone while holding a lxccontainer, increment
 * c->numthreads (under privlock) before forking.  When deleting,
 * decrement numthreads under privlock, then if it hits 0 you can delete.
 * Do not ever use a lxccontainer whose numthreads you did not bump.
 */

static void lxc_container_free(struct lxc_container *c)
{
	if (!c)
		return;

	if (c->configfile) {
		free(c->configfile);
		c->configfile = NULL;
	}
	if (c->error_string) {
		free(c->error_string);
		c->error_string = NULL;
	}
	if (c->slock) {
		lxc_putlock(c->slock);
		c->slock = NULL;
	}
	if (c->privlock) {
		lxc_putlock(c->privlock);
		c->privlock = NULL;
	}
	if (c->name) {
		free(c->name);
		c->name = NULL;
	}
	if (c->lxc_conf) {
		lxc_conf_free(c->lxc_conf);
		c->lxc_conf = NULL;
	}
	if (c->config_path) {
		free(c->config_path);
		c->config_path = NULL;
	}
	free(c);
}

/*
 * Consider the following case:
freer                         |    racing get()er
==================================================================
lxc_container_put()           |   lxc_container_get()
\ lxclock(c->privlock)        |   c->numthreads < 1? (no)
\ c->numthreads = 0           |   \ lxclock(c->privlock) -> waits
\ lxcunlock()                 |   \
\ lxc_container_free()        |   \ lxclock() returns
                              |   \ c->numthreads < 1 -> return 0
\ \ (free stuff)              |
\ \ sem_destroy(privlock)     |

 * When the get()er checks numthreads the first time, one of the following
 * is true:
 * 1. freer has set numthreads = 0.  get() returns 0
 * 2. freer is between lxclock and setting numthreads to 0.  get()er will
 *    sem_wait on privlock, get lxclock after freer() drops it, then see
 *    numthreads is 0 and exit without touching lxclock again..
 * 3. freer has not yet locked privlock.  If get()er runs first, then put()er
 *    will see --numthreads = 1 and not call lxc_container_free().
*/

int lxc_container_get(struct lxc_container *c)
{
	if (!c)
		return 0;

	// if someone else has already started freeing the container, don't
	// try to take the lock, which may be invalid
	if (c->numthreads < 1)
		return 0;

	if (container_mem_lock(c))
		return 0;
	if (c->numthreads < 1) {
		// bail without trying to unlock, bc the privlock is now probably
		// in freed memory
		return 0;
	}
	c->numthreads++;
	container_mem_unlock(c);
	return 1;
}

int lxc_container_put(struct lxc_container *c)
{
	if (!c)
		return -1;
	if (container_mem_lock(c))
		return -1;
	if (--c->numthreads < 1) {
		container_mem_unlock(c);
		lxc_container_free(c);
		return 1;
	}
	container_mem_unlock(c);
	return 0;
}

static bool lxcapi_is_defined(struct lxc_container *c)
{
	struct stat statbuf;
	bool ret = false;
	int statret;

	if (!c)
		return false;

	if (container_mem_lock(c))
		return false;
	if (!c->configfile)
		goto out;
	statret = stat(c->configfile, &statbuf);
	if (statret != 0)
		goto out;
	ret = true;

out:
	container_mem_unlock(c);
	return ret;
}

static const char *lxcapi_state(struct lxc_container *c)
{
	lxc_state_t s;

	if (!c)
		return NULL;
	s = lxc_getstate(c->name, c->config_path);
	return lxc_state2str(s);
}

static bool is_stopped(struct lxc_container *c)
{
	lxc_state_t s;
	s = lxc_getstate(c->name, c->config_path);
	return (s == STOPPED);
}

static bool lxcapi_is_running(struct lxc_container *c)
{
	const char *s;

	if (!c)
		return false;
	s = lxcapi_state(c);
	if (!s || strcmp(s, "STOPPED") == 0)
		return false;
	return true;
}

static bool lxcapi_freeze(struct lxc_container *c)
{
	int ret;
	if (!c)
		return false;

	ret = lxc_freeze(c->name, c->config_path);
	if (ret)
		return false;
	return true;
}

static bool lxcapi_unfreeze(struct lxc_container *c)
{
	int ret;
	if (!c)
		return false;

	ret = lxc_unfreeze(c->name, c->config_path);
	if (ret)
		return false;
	return true;
}

static int lxcapi_console_getfd(struct lxc_container *c, int *ttynum, int *masterfd)
{
	int ttyfd;
	if (!c)
		return -1;

	ttyfd = lxc_console_getfd(c, ttynum, masterfd);
	return ttyfd;
}

static int lxcapi_console(struct lxc_container *c, int ttynum, int stdinfd,
			  int stdoutfd, int stderrfd, int escape)
{
	return lxc_console(c, ttynum, stdinfd, stdoutfd, stderrfd, escape);
}

static pid_t lxcapi_init_pid(struct lxc_container *c)
{
	if (!c)
		return -1;

	return lxc_cmd_get_init_pid(c->name, c->config_path);
}

static bool load_config_locked(struct lxc_container *c, const char *fname)
{
	if (!c->lxc_conf)
		c->lxc_conf = lxc_conf_init();
	if (c->lxc_conf && !lxc_config_read(fname, c->lxc_conf))
		return true;
	return false;
}

static bool lxcapi_load_config(struct lxc_container *c, const char *alt_file)
{
	bool ret = false, need_disklock = false;
	int lret;
	const char *fname;
	if (!c)
		return false;

	fname = c->configfile;
	if (alt_file)
		fname = alt_file;
	if (!fname)
		return false;
	/*
	 * If we're reading something other than the container's config,
	 * we only need to lock the in-memory container.  If loading the
	 * container's config file, take the disk lock.
	 */
	if (strcmp(fname, c->configfile) == 0)
		need_disklock = true;

	if (need_disklock)
		lret = container_disk_lock(c);
	else
		lret = container_mem_lock(c);
	if (lret)
		return false;

	ret = load_config_locked(c, fname);

	if (need_disklock)
		container_disk_unlock(c);
	else
		container_mem_unlock(c);
	return ret;
}

static void lxcapi_want_daemonize(struct lxc_container *c)
{
	if (!c)
		return;
	if (container_mem_lock(c)) {
		ERROR("Error getting mem lock");
		return;
	}
	c->daemonize = 1;
	container_mem_unlock(c);
}

static bool lxcapi_want_close_all_fds(struct lxc_container *c)
{
	if (!c || !c->lxc_conf)
		return false;
	if (container_mem_lock(c)) {
		ERROR("Error getting mem lock");
		return false;
	}
	c->lxc_conf->close_all_fds = 1;
	container_mem_unlock(c);
	return true;
}

static bool lxcapi_wait(struct lxc_container *c, const char *state, int timeout)
{
	int ret;

	if (!c)
		return false;

	ret = lxc_wait(c->name, state, timeout, c->config_path);
	return ret == 0;
}


static bool wait_on_daemonized_start(struct lxc_container *c)
{
	/* we'll probably want to make this timeout configurable? */
	int timeout = 5, ret, status;

	/*
	 * our child is going to fork again, then exit.  reap the
	 * child
	 */
	ret = wait(&status);
	if (ret == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
		DEBUG("failed waiting for first dual-fork child");
	return lxcapi_wait(c, "RUNNING", timeout);
}

/*
 * I can't decide if it'd be more convenient for callers if we accept '...',
 * or a null-terminated array (i.e. execl vs execv)
 */
static bool lxcapi_start(struct lxc_container *c, int useinit, char * const argv[])
{
	int ret;
	struct lxc_conf *conf;
	int daemonize = 0;
	char *default_args[] = {
		"/sbin/init",
		'\0',
	};

	/* container exists */
	if (!c)
		return false;
	/* container has been setup */
	if (!c->lxc_conf)
		return false;

	if ((ret = ongoing_create(c)) < 0) {
		ERROR("Error checking for incomplete creation");
		return false;
	}
	if (ret == 2) {
		ERROR("Error: %s creation was not completed", c->name);
		c->destroy(c);
		return false;
	} else if (ret == 1) {
		ERROR("Error: creation of %s is ongoing", c->name);
		return false;
	}

	/* is this app meant to be run through lxcinit, as in lxc-execute? */
	if (useinit && !argv)
		return false;

	if (container_mem_lock(c))
		return false;
	conf = c->lxc_conf;
	daemonize = c->daemonize;
	container_mem_unlock(c);

	if (useinit) {
		ret = lxc_execute(c->name, argv, 1, conf, c->config_path);
		return ret == 0 ? true : false;
	}

	if (!argv)
		argv = default_args;

	/*
	* say, I'm not sure - what locks do we want here?  Any?
	* Is liblxc's locking enough here to protect the on disk
	* container?  We don't want to exclude things like lxc_info
	* while container is running...
	*/
	if (daemonize) {
		if (!lxc_container_get(c))
			return false;
		lxc_monitord_spawn(c->config_path);

		pid_t pid = fork();
		if (pid < 0) {
			lxc_container_put(c);
			return false;
		}
		if (pid != 0)
			return wait_on_daemonized_start(c);

		process_unlock(); // we're no longer sharing
		/* second fork to be reparented by init */
		pid = fork();
		if (pid < 0) {
			SYSERROR("Error doing dual-fork");
			return false;
		}
		if (pid != 0)
			exit(0);
		/* like daemon(), chdir to / and redirect 0,1,2 to /dev/null */
		if (chdir("/")) {
			SYSERROR("Error chdir()ing to /.");
			return false;
		}
		close(0);
		close(1);
		close(2);
		open("/dev/zero", O_RDONLY);
		open("/dev/null", O_RDWR);
		open("/dev/null", O_RDWR);
		setsid();
	}

reboot:
	conf->reboot = 0;
	ret = lxc_start(c->name, argv, conf, c->config_path);

	if (conf->reboot) {
		INFO("container requested reboot");
		conf->reboot = 0;
		goto reboot;
	}

	if (daemonize) {
		lxc_container_put(c);
		exit (ret == 0 ? true : false);
	} else {
		return (ret == 0 ? true : false);
	}
}

/*
 * note there MUST be an ending NULL
 */
static bool lxcapi_startl(struct lxc_container *c, int useinit, ...)
{
	va_list ap;
	char **inargs = NULL;
	bool bret = false;

	/* container exists */
	if (!c)
		return false;

	va_start(ap, useinit);
	inargs = lxc_va_arg_list_to_argv(ap, 0, 1);
	va_end(ap);

	if (!inargs) {
		ERROR("Memory allocation error.");
		goto out;
	}

	/* pass NULL if no arguments were supplied */
	bret = lxcapi_start(c, useinit, *inargs ? inargs : NULL);

out:
	if (inargs) {
		char *arg;
		for (arg = *inargs; arg; arg++)
			free(arg);
		free(inargs);
	}

	return bret;
}

static bool lxcapi_stop(struct lxc_container *c)
{
	int ret;

	if (!c)
		return false;

	ret = lxc_cmd_stop(c->name, c->config_path);

	return ret == 0;
}

/*
 * create the standard expected container dir
 */
static bool create_container_dir(struct lxc_container *c)
{
	char *s;
	int len, ret;

	len = strlen(c->config_path) + strlen(c->name) + 2;
	s = malloc(len);
	if (!s)
		return false;
	ret = snprintf(s, len, "%s/%s", c->config_path, c->name);
	if (ret < 0 || ret >= len) {
		free(s);
		return false;
	}
	ret = mkdir(s, 0755);
	if (ret) {
		if (errno == EEXIST)
			ret = 0;
		else
			SYSERROR("failed to create container path for %s\n", c->name);
	}
	free(s);
	return ret == 0;
}

static const char *lxcapi_get_config_path(struct lxc_container *c);
static bool lxcapi_set_config_item(struct lxc_container *c, const char *key, const char *v);

/*
 * do_bdev_create: thin wrapper around bdev_create().  Like bdev_create(),
 * it returns a mounted bdev on success, NULL on error.
 */
static struct bdev *do_bdev_create(struct lxc_container *c, const char *type,
			 struct bdev_specs *specs)
{
	char *dest;
	const char *lxcpath = lxcapi_get_config_path(c);
	size_t len;
	struct bdev *bdev;
	int ret;

	/* lxcpath/lxcname/rootfs */
	len = strlen(c->name) + strlen(lxcpath) + 9;
	dest = alloca(len);
	ret = snprintf(dest, len, "%s/%s/rootfs", lxcpath, c->name);
	if (ret < 0 || ret >= len)
		return NULL;

	bdev = bdev_create(dest, type, c->name, specs);
	if (!bdev) {
		ERROR("Failed to create backing store type %s\n", type);
		return NULL;
	}

	lxcapi_set_config_item(c, "lxc.rootfs", bdev->src);

	/* if we are not root, chown the rootfs dir to root in the
	 * target uidmap */

	if (geteuid() != 0) {
		if (chown_mapped_root(bdev->dest, c->lxc_conf) < 0) {
			ERROR("Error chowning %s to container root\n", bdev->dest);
			bdev_put(bdev);
			return NULL;
		}
	}

	return bdev;
}

/*
 * Given the '-t' template option to lxc-create, figure out what to
 * do.  If the template is a full executable path, use that.  If it
 * is something like 'sshd', then return $templatepath/lxc-sshd.
 * On success return the template, on error return NULL.
 */
static char *get_template_path(const char *t)
{
	int ret, len;
	char *tpath;

	if (t[0] == '/' && access(t, X_OK) == 0) {
		tpath = strdup(t);
		return tpath;
	}

	len = strlen(LXCTEMPLATEDIR) + strlen(t) + strlen("/lxc-") + 1;
	tpath = malloc(len);
	if (!tpath)
		return NULL;
	ret = snprintf(tpath, len, "%s/lxc-%s", LXCTEMPLATEDIR, t);
	if (ret < 0 || ret >= len) {
		free(tpath);
		return NULL;
	}
	if (access(tpath, X_OK) < 0) {
		SYSERROR("bad template: %s\n", t);
		free(tpath);
		return NULL;
	}

	return tpath;
}

static char *lxcbasename(char *path)
{
	char *p = path + strlen(path) - 1;
	while (*p != '/' && p > path)
		p--;
	return p;
}

static bool create_run_template(struct lxc_container *c, char *tpath, bool quiet,
				char *const argv[])
{
	pid_t pid;

	if (!tpath)
		return true;

	pid = fork();
	if (pid < 0) {
		SYSERROR("failed to fork task for container creation template\n");
		return false;
	}

	if (pid == 0) { // child
		char *patharg, *namearg, *rootfsarg, *src;
		struct bdev *bdev = NULL;
		int i;
		int ret, len, nargs = 0;
		char **newargv;
		struct lxc_conf *conf = c->lxc_conf;

		process_unlock(); // we're no longer sharing
		if (quiet) {
			close(0);
			close(1);
			close(2);
			open("/dev/zero", O_RDONLY);
			open("/dev/null", O_RDWR);
			open("/dev/null", O_RDWR);
		}

		src = c->lxc_conf->rootfs.path;
		/*
		 * for an overlayfs create, what the user wants is the template to fill
		 * in what will become the readonly lower layer.  So don't mount for
		 * the template
		 */
		if (strncmp(src, "overlayfs:", 10) == 0) {
			src = overlayfs_getlower(src+10);
		}
		bdev = bdev_init(src, c->lxc_conf->rootfs.mount, NULL);
		if (!bdev) {
			ERROR("Error opening rootfs");
			exit(1);
		}

		if (strcmp(bdev->type, "dir") != 0) {
			if (unshare(CLONE_NEWNS) < 0) {
				ERROR("error unsharing mounts");
				exit(1);
			}
			if (bdev->ops->mount(bdev) < 0) {
				ERROR("Error mounting rootfs");
				exit(1);
			}
		} else { // TODO come up with a better way here!
			if (bdev->dest)
				free(bdev->dest);
			bdev->dest = strdup(bdev->src);
		}

		/*
		 * create our new array, pre-pend the template name and
		 * base args
		 */
		if (argv)
			for (nargs = 0; argv[nargs]; nargs++) ;
		nargs += 4;  // template, path, rootfs and name args

		newargv = malloc(nargs * sizeof(*newargv));
		if (!newargv)
			exit(1);
		newargv[0] = lxcbasename(tpath);

		len = strlen(c->config_path) + strlen(c->name) + strlen("--path=") + 2;
		patharg = malloc(len);
		if (!patharg)
			exit(1);
		ret = snprintf(patharg, len, "--path=%s/%s", c->config_path, c->name);
		if (ret < 0 || ret >= len)
			exit(1);
		newargv[1] = patharg;
		len = strlen("--name=") + strlen(c->name) + 1;
		namearg = malloc(len);
		if (!namearg)
			exit(1);
		ret = snprintf(namearg, len, "--name=%s", c->name);
		if (ret < 0 || ret >= len)
			exit(1);
		newargv[2] = namearg;

		len = strlen("--rootfs=") + 1 + strlen(bdev->dest);
		rootfsarg = malloc(len);
		if (!rootfsarg)
			exit(1);
		ret = snprintf(rootfsarg, len, "--rootfs=%s", bdev->dest);
		if (ret < 0 || ret >= len)
			exit(1);
		newargv[3] = rootfsarg;

		/* add passed-in args */
		if (argv)
			for (i = 4; i < nargs; i++)
				newargv[i] = argv[i-4];

		/* add trailing NULL */
		nargs++;
		newargv = realloc(newargv, nargs * sizeof(*newargv));
		if (!newargv)
			exit(1);
		newargv[nargs - 1] = NULL;

		/*
		 * If we're running the template in a mapped userns, then
		 * we prepend the template command with:
		 * lxc-usernsexec <-m map1> ... <-m mapn> --
		 */
		if (geteuid() != 0 && !lxc_list_empty(&conf->id_map)) {
			int n2args = 1;
			char **n2 = malloc(n2args * sizeof(*n2));
			struct lxc_list *it;
			struct id_map *map;

			newargv[0] = tpath;
			tpath = "lxc-usernsexec";
			n2[0] = "lxc-usernsexec";
			lxc_list_for_each(it, &conf->id_map) {
				map = it->elem;
				n2args += 2;
				n2 = realloc(n2, n2args * sizeof(*n2));
				if (!n2)
					exit(1);
				n2[n2args-2] = "-m";
				n2[n2args-1] = malloc(200);
				if (!n2[n2args-1])
					exit(1);
				ret = snprintf(n2[n2args-1], 200, "%c:%lu:%lu:%lu",
					map->idtype == ID_TYPE_UID ? 'u' : 'g',
					map->nsid, map->hostid, map->range);
				if (ret < 0 || ret >= 200)
					exit(1);
			}
			bool hostid_mapped = hostid_is_mapped(geteuid(), conf);
			int extraargs = hostid_mapped ?  1 : 3;
			n2 = realloc(n2, (nargs + n2args + extraargs) * sizeof(*n2));
			if (!n2)
				exit(1);
			if (!hostid_mapped) {
				int free_id = find_unmapped_nsuid(conf);
				n2[n2args++] = "-m";
				if (free_id < 0) {
					ERROR("Could not find free uid to map");
					exit(1);
				}
				n2[n2args++] = malloc(200);
				if (!n2[n2args-1]) {
					SYSERROR("out of memory");
					exit(1);
				}
				ret = snprintf(n2[n2args-1], 200, "u:%d:%d:1",
					free_id, geteuid());
				if (ret < 0 || ret >= 200) {
					ERROR("string too long");
					exit(1);
				}
			}
			n2[n2args++] = "--";
			for (i = 0; i < nargs; i++)
				n2[i + n2args] = newargv[i];
			free(newargv);
			newargv = n2;
		}
		/* execute */
		execvp(tpath, newargv);
		SYSERROR("failed to execute template %s", tpath);
		exit(1);
	}

	if (wait_for_pid(pid) != 0) {
		ERROR("container creation template for %s failed\n", c->name);
		return false;
	}

	return true;
}

bool prepend_lxc_header(char *path, const char *t, char *const argv[])
{
	long flen;
	char *contents;
	FILE *f;
	int ret = -1;
#if HAVE_LIBGNUTLS
	int i;
	unsigned char md_value[SHA_DIGEST_LENGTH];
	char *tpath;
#endif

	process_lock();
	f = fopen(path, "r");
	process_unlock();
	if (f == NULL)
		return false;

	if (fseek(f, 0, SEEK_END) < 0)
		goto out_error;
	if ((flen = ftell(f)) < 0)
		goto out_error;
	if (fseek(f, 0, SEEK_SET) < 0)
		goto out_error;
	if ((contents = malloc(flen + 1)) == NULL)
		goto out_error;
	if (fread(contents, 1, flen, f) != flen)
		goto out_free_contents;

	contents[flen] = '\0';
	process_lock();
	ret = fclose(f);
	process_unlock();
	f = NULL;
	if (ret < 0)
		goto out_free_contents;

#if HAVE_LIBGNUTLS
	tpath = get_template_path(t);
	if (!tpath) {
		ERROR("bad template: %s\n", t);
		goto out_free_contents;
	}

	ret = sha1sum_file(tpath, md_value);
	if (ret < 0) {
		ERROR("Error getting sha1sum of %s", tpath);
		free(tpath);
		goto out_free_contents;
	}
	free(tpath);
#endif

	process_lock();
	f = fopen(path, "w");
	process_unlock();
	if (f == NULL) {
		SYSERROR("reopening config for writing");
		free(contents);
		return false;
	}
	fprintf(f, "# Template used to create this container: %s\n", t);
	if (argv) {
		fprintf(f, "# Parameters passed to the template:");
		while (*argv) {
			fprintf(f, " %s", *argv);
			argv++;
		}
		fprintf(f, "\n");
	}
#if HAVE_LIBGNUTLS
	fprintf(f, "# Template script checksum (SHA-1): ");
	for (i=0; i<SHA_DIGEST_LENGTH; i++)
		fprintf(f, "%02x", md_value[i]);
	fprintf(f, "\n");
#endif
	if (fwrite(contents, 1, flen, f) != flen) {
		SYSERROR("Writing original contents");
		free(contents);
		process_lock();
		fclose(f);
		process_unlock();
		return false;
	}
	ret = 0;
out_free_contents:
	free(contents);
out_error:
	if (f) {
		int newret;
		process_lock();
		newret = fclose(f);
		process_unlock();
		if (ret == 0)
			ret = newret;
	}
	if (ret < 0) {
		SYSERROR("Error prepending header");
		return false;
	}
	return true;
}

static bool lxcapi_destroy(struct lxc_container *c);
/*
 * lxcapi_create:
 * create a container with the given parameters.
 * @c: container to be created.  It has the lxcpath, name, and a starting
 *     configuration already set
 * @t: the template to execute to instantiate the root filesystem and
 *     adjust the configuration.
 * @bdevtype: backing store type to use.  If NULL, dir will be used.
 * @specs: additional parameters for the backing store, i.e. LVM vg to
 *         use.
 *
 * @argv: the arguments to pass to the template, terminated by NULL.  If no
 * arguments, you can just pass NULL.
 */
static bool lxcapi_create(struct lxc_container *c, const char *t,
		const char *bdevtype, struct bdev_specs *specs, int flags,
		char *const argv[])
{
	bool bret = false;
	pid_t pid;
	char *tpath = NULL;
	int partial_fd;

	if (!c)
		return false;

	if (t) {
		tpath = get_template_path(t);
		if (!tpath) {
			ERROR("bad template: %s\n", t);
			goto out;
		}
	}

	if (!c->save_config(c, NULL)) {
		ERROR("failed to save starting configuration for %s\n", c->name);
		goto out;
	}

	/* container is already created if we have a config and rootfs.path is accessible */
	if (lxcapi_is_defined(c) && c->lxc_conf && c->lxc_conf->rootfs.path && access(c->lxc_conf->rootfs.path, F_OK) == 0)
		goto out;

	/* Mark that this container is being created */
	if ((partial_fd = create_partial(c)) < 0)
		goto out;

	/* no need to get disk lock bc we have the partial locked */

	/*
	 * Create the backing store
	 * Note we can't do this in the same task as we use to execute the
	 * template because of the way zfs works.
	 * After you 'zfs create', zfs mounts the fs only in the initial
	 * namespace.
	 */
	pid = fork();
	if (pid < 0) {
		SYSERROR("failed to fork task for container creation template\n");
		goto out_unlock;
	}

	if (pid == 0) { // child
		struct bdev *bdev = NULL;

		process_unlock(); // we're no longer sharing
		if (!(bdev = do_bdev_create(c, bdevtype, specs))) {
			ERROR("Error creating backing store type %s for %s",
				bdevtype ? bdevtype : "(none)", c->name);
			exit(1);
		}

		/* save config file again to store the new rootfs location */
		if (!c->save_config(c, NULL)) {
			ERROR("failed to save starting configuration for %s\n", c->name);
			// parent task won't see bdev in config so we delete it
			bdev->ops->umount(bdev);
			bdev->ops->destroy(bdev);
			exit(1);
		}
		exit(0);
	}
	if (wait_for_pid(pid) != 0)
		goto out_unlock;

	/* reload config to get the rootfs */
	if (c->lxc_conf)
		lxc_conf_free(c->lxc_conf);
	c->lxc_conf = NULL;
	if (!load_config_locked(c, c->configfile))
		goto out_unlock;

	if (!create_run_template(c, tpath, !!(flags & LXC_CREATE_QUIET), argv))
		goto out_unlock;

	// now clear out the lxc_conf we have, reload from the created
	// container
	if (c->lxc_conf)
		lxc_conf_free(c->lxc_conf);
	c->lxc_conf = NULL;

	if (t) {
		if (!prepend_lxc_header(c->configfile, tpath, argv)) {
			ERROR("Error prepending header to configuration file");
			goto out_unlock;
		}
	}
	bret = load_config_locked(c, c->configfile);

out_unlock:
	if (partial_fd >= 0)
		remove_partial(c, partial_fd);
out:
	if (tpath)
		free(tpath);
	if (!bret && c)
		lxcapi_destroy(c);
	return bret;
}

static bool lxcapi_reboot(struct lxc_container *c)
{
	pid_t pid;

	if (!c)
		return false;
	if (!c->is_running(c))
		return false;
	pid = c->init_pid(c);
	if (pid <= 0)
		return false;
	if (kill(pid, SIGINT) < 0)
		return false;
	return true;

}

static bool lxcapi_shutdown(struct lxc_container *c, int timeout)
{
	bool retv;
	pid_t pid;

	if (!c)
		return false;

	if (!timeout)
		timeout = -1;
	if (!c->is_running(c))
		return true;
	pid = c->init_pid(c);
	if (pid <= 0)
		return true;
	kill(pid, SIGPWR);
	retv = c->wait(c, "STOPPED", timeout);
	if (!retv && timeout > 0) {
		c->stop(c);
		retv = c->wait(c, "STOPPED", 0); // 0 means don't wait
	}
	return retv;
}

static bool lxcapi_createl(struct lxc_container *c, const char *t,
		const char *bdevtype, struct bdev_specs *specs, int flags, ...)
{
	bool bret = false;
	char **args = NULL;
	va_list ap;

	if (!c)
		return false;

	/*
	 * since we're going to wait for create to finish, I don't think we
	 * need to get a copy of the arguments.
	 */
	va_start(ap, flags);
	args = lxc_va_arg_list_to_argv(ap, 0, 0);
	va_end(ap);
	if (!args) {
		ERROR("Memory allocation error.");
		goto out;
	}

	bret = c->create(c, t, bdevtype, specs, flags, args);

out:
	free(args);
	return bret;
}

static bool lxcapi_clear_config_item(struct lxc_container *c, const char *key)
{
	int ret;

	if (!c || !c->lxc_conf)
		return false;
	if (container_mem_lock(c))
		return false;
	ret = lxc_clear_config_item(c->lxc_conf, key);
	container_mem_unlock(c);
	return ret == 0;
}

static inline void exit_from_ns(struct lxc_container *c, int *old_netns, int *new_netns) {
	/* Switch back to original netns */
	if (*old_netns >= 0 && setns(*old_netns, CLONE_NEWNET))
		SYSERROR("failed to setns");
	process_lock();
	if (*new_netns >= 0)
		close(*new_netns);
	if (*old_netns >= 0)
		close(*old_netns);
	process_unlock();
}

static inline bool enter_to_ns(struct lxc_container *c, int *old_netns, int *new_netns) {
	int ret = 0;
	char new_netns_path[MAXPATHLEN];

	if (!c->is_running(c))
		goto out;

	/* Save reference to old netns */
	process_lock();
	*old_netns = open("/proc/self/ns/net", O_RDONLY);
	process_unlock();
	if (*old_netns < 0) {
		SYSERROR("failed to open /proc/self/ns/net");
		goto out;
	}

	/* Switch to new netns */
	ret = snprintf(new_netns_path, MAXPATHLEN, "/proc/%d/ns/net", c->init_pid(c));
	if (ret < 0 || ret >= MAXPATHLEN)
		goto out;

	process_lock();
	*new_netns = open(new_netns_path, O_RDONLY);
	process_unlock();
	if (*new_netns < 0) {
		SYSERROR("failed to open %s", new_netns_path);
		goto out;
	}

	if (setns(*new_netns, CLONE_NEWNET)) {
		SYSERROR("failed to setns");
		goto out;
	}
	return true;
out:
	exit_from_ns(c, old_netns, new_netns);
	return false;
}

// used by qsort and bsearch functions for comparing names
static inline int string_cmp(char **first, char **second)
{
	return strcmp(*first, *second);
}

// used by qsort and bsearch functions for comparing container names
static inline int container_cmp(struct lxc_container **first, struct lxc_container **second)
{
	return strcmp((*first)->name, (*second)->name);
}

static bool add_to_array(char ***names, char *cname, int pos)
{
	char **newnames = realloc(*names, (pos+1) * sizeof(char *));
	if (!newnames) {
		ERROR("Out of memory");
		return false;
	}

	*names = newnames;
	newnames[pos] = strdup(cname);
	if (!newnames[pos])
		return false;

	// sort the arrray as we will use binary search on it
	qsort(newnames, pos + 1, sizeof(char *), (int (*)(const void *,const void *))string_cmp);

	return true;
}

static bool add_to_clist(struct lxc_container ***list, struct lxc_container *c, int pos)
{
	struct lxc_container **newlist = realloc(*list, (pos+1) * sizeof(struct lxc_container *));
	if (!newlist) {
		ERROR("Out of memory");
		return false;
	}

	*list = newlist;
	newlist[pos] = c;

	// sort the arrray as we will use binary search on it
	qsort(newlist, pos + 1, sizeof(struct lxc_container *), (int (*)(const void *,const void *))container_cmp);

	return true;
}

static char** get_from_array(char ***names, char *cname, int size)
{
	return (char **)bsearch(&cname, *names, size, sizeof(char *), (int (*)(const void *, const void *))string_cmp);
}


static bool array_contains(char ***names, char *cname, int size) {
	if(get_from_array(names, cname, size) != NULL)
		return true;
	return false;
}

static bool remove_from_array(char ***names, char *cname, int size)
{
	char **result = get_from_array(names, cname, size);
	if (result != NULL) {
		free(result);
		return true;
	}
	return false;
}

static char** lxcapi_get_interfaces(struct lxc_container *c)
{
	int i, count = 0;
	struct ifaddrs *interfaceArray = NULL, *tempIfAddr = NULL;
	char **interfaces = NULL;
	int old_netns = -1, new_netns = -1;

	if (!enter_to_ns(c, &old_netns, &new_netns))
		goto out;

	/* Grab the list of interfaces */
	if (getifaddrs(&interfaceArray)) {
		SYSERROR("failed to get interfaces list");
		goto out;
	}

	/* Iterate through the interfaces */
	for (tempIfAddr = interfaceArray; tempIfAddr != NULL; tempIfAddr = tempIfAddr->ifa_next) {
		if (array_contains(&interfaces, tempIfAddr->ifa_name, count))
			continue;

		if(!add_to_array(&interfaces, tempIfAddr->ifa_name, count))
			goto err;
		count++;
	}

out:
	if (interfaceArray)
		freeifaddrs(interfaceArray);

	exit_from_ns(c, &old_netns, &new_netns);

	/* Append NULL to the array */
	if(interfaces)
		interfaces = (char **)lxc_append_null_to_array((void **)interfaces, count);

	return interfaces;

err:
	for(i=0;i<count;i++)
		free(interfaces[i]);
	free(interfaces);
	interfaces = NULL;
	goto out;
}

static char** lxcapi_get_ips(struct lxc_container *c, char* interface, char* family, int scope)
{
	int i, count = 0;
	struct ifaddrs *interfaceArray = NULL, *tempIfAddr = NULL;
	char addressOutputBuffer[INET6_ADDRSTRLEN];
	void *tempAddrPtr = NULL;
	char **addresses = NULL;
	char *address = NULL;
	int old_netns = -1, new_netns = -1;

	if (!enter_to_ns(c, &old_netns, &new_netns))
		goto out;

	/* Grab the list of interfaces */
	if (getifaddrs(&interfaceArray)) {
		SYSERROR("failed to get interfaces list");
		goto out;
	}

	/* Iterate through the interfaces */
	for (tempIfAddr = interfaceArray; tempIfAddr != NULL; tempIfAddr = tempIfAddr->ifa_next) {
		if (tempIfAddr->ifa_addr == NULL)
			continue;

		if(tempIfAddr->ifa_addr->sa_family == AF_INET) {
			if (family && strcmp(family, "inet"))
				continue;
			tempAddrPtr = &((struct sockaddr_in *)tempIfAddr->ifa_addr)->sin_addr;
		}
		else {
			if (family && strcmp(family, "inet6"))
				continue;

			if (((struct sockaddr_in6 *)tempIfAddr->ifa_addr)->sin6_scope_id != scope)
				continue;

			tempAddrPtr = &((struct sockaddr_in6 *)tempIfAddr->ifa_addr)->sin6_addr;
		}

		if (interface && strcmp(interface, tempIfAddr->ifa_name))
			continue;
		else if (!interface && strcmp("lo", tempIfAddr->ifa_name) == 0)
			continue;

		address = (char *)inet_ntop(tempIfAddr->ifa_addr->sa_family,
					   tempAddrPtr,
					   addressOutputBuffer,
					   sizeof(addressOutputBuffer));
		if (!address)
			continue;

		if(!add_to_array(&addresses, address, count))
			goto err;
		count++;
	}

out:
	if(interfaceArray)
		freeifaddrs(interfaceArray);

	exit_from_ns(c, &old_netns, &new_netns);

	/* Append NULL to the array */
	if(addresses)
		addresses = (char **)lxc_append_null_to_array((void **)addresses, count);

	return addresses;

err:
	for(i=0;i<count;i++)
		free(addresses[i]);
	free(addresses);
	addresses = NULL;

	goto out;
}

static int lxcapi_get_config_item(struct lxc_container *c, const char *key, char *retv, int inlen)
{
	int ret;

	if (!c || !c->lxc_conf)
		return -1;
	if (container_mem_lock(c))
		return -1;
	ret = lxc_get_config_item(c->lxc_conf, key, retv, inlen);
	container_mem_unlock(c);
	return ret;
}

static int lxcapi_get_keys(struct lxc_container *c, const char *key, char *retv, int inlen)
{
	if (!key)
		return lxc_listconfigs(retv, inlen);
	/*
	 * Support 'lxc.network.<idx>', i.e. 'lxc.network.0'
	 * This is an intelligent result to show which keys are valid given
	 * the type of nic it is
	 */
	if (!c || !c->lxc_conf)
		return -1;
	if (container_mem_lock(c))
		return -1;
	int ret = -1;
	if (strncmp(key, "lxc.network.", 12) == 0)
		ret =  lxc_list_nicconfigs(c->lxc_conf, key, retv, inlen);
	container_mem_unlock(c);
	return ret;
}

static bool lxcapi_save_config(struct lxc_container *c, const char *alt_file)
{
	FILE *fout;
	bool ret = false, need_disklock = false;
	int lret;

	if (!alt_file)
		alt_file = c->configfile;
	if (!alt_file)
		return false;  // should we write to stdout if no file is specified?

	// If we haven't yet loaded a config, load the stock config
	if (!c->lxc_conf) {
		if (!c->load_config(c, LXC_DEFAULT_CONFIG)) {
			ERROR("Error loading default configuration file %s while saving %s\n", LXC_DEFAULT_CONFIG, c->name);
			return false;
		}
	}

	if (!create_container_dir(c))
		return false;

	/*
	 * If we're writing to the container's config file, take the
	 * disk lock.  Otherwise just take the memlock to protect the
	 * struct lxc_container while we're traversing it.
	 */
	if (strcmp(c->configfile, alt_file) == 0)
		need_disklock = true;

	if (need_disklock)
		lret = container_disk_lock(c);
	else
		lret = container_mem_lock(c);

	if (lret)
		return false;

	fout = fopen(alt_file, "w");
	if (!fout)
		goto out;
	write_config(fout, c->lxc_conf);
	fclose(fout);
	ret = true;

out:
	if (need_disklock)
		container_disk_unlock(c);
	else
		container_mem_unlock(c);
	return ret;
}

static bool mod_rdep(struct lxc_container *c, bool inc)
{
	char path[MAXPATHLEN];
	int ret, v = 0;
	FILE *f;
	bool bret = false;

	if (container_disk_lock(c))
		return false;
	ret = snprintf(path, MAXPATHLEN, "%s/%s/lxc_snapshots", c->config_path,
			c->name);
	if (ret < 0 || ret > MAXPATHLEN)
		goto out;
	process_lock();
	f = fopen(path, "r");
	process_unlock();
	if (f) {
		ret = fscanf(f, "%d", &v);
		process_lock();
		fclose(f);
		process_unlock();
		if (ret != 1) {
			ERROR("Corrupted file %s", path);
			goto out;
		}
	}
	v += inc ? 1 : -1;
	process_lock();
	f = fopen(path, "w");
	process_unlock();
	if (!f)
		goto out;
	if (fprintf(f, "%d\n", v) < 0) {
		ERROR("Error writing new snapshots value");
		process_lock();
		fclose(f);
		process_unlock();
		goto out;
	}
	process_lock();
	ret = fclose(f);
	process_unlock();
	if (ret != 0) {
		SYSERROR("Error writing to or closing snapshots file");
		goto out;
	}

	bret = true;

out:
	container_disk_unlock(c);
	return bret;
}

static void strip_newline(char *p)
{
	size_t len = strlen(p);
	if (len < 1)
		return;
	if (p[len-1] == '\n')
		p[len-1] = '\0';
}

static void mod_all_rdeps(struct lxc_container *c, bool inc)
{
	struct lxc_container *p;
	char *lxcpath = NULL, *lxcname = NULL, path[MAXPATHLEN];
	size_t pathlen = 0, namelen = 0;
	FILE *f;
	int ret;

	ret = snprintf(path, MAXPATHLEN, "%s/%s/lxc_rdepends",
		c->config_path, c->name);
	if (ret < 0 || ret >= MAXPATHLEN) {
		ERROR("Path name too long");
		return;
	}
	process_lock();
	f = fopen(path, "r");
	process_unlock();
	if (f == NULL)
		return;
	while (getline(&lxcpath, &pathlen, f) != -1) {
		if (getline(&lxcname, &namelen, f) == -1) {
			ERROR("badly formatted file %s\n", path);
			goto out;
		}
		strip_newline(lxcpath);
		strip_newline(lxcname);
		if ((p = lxc_container_new(lxcname, lxcpath)) == NULL) {
			ERROR("Unable to find dependent container %s:%s",
				lxcpath, lxcname);
			continue;
		}
		if (!mod_rdep(p, inc))
			ERROR("Failed to increase numsnapshots for %s:%s",
				lxcpath, lxcname);
		lxc_container_put(p);
	}
out:
	if (lxcpath) free(lxcpath);
	if (lxcname) free(lxcname);
	process_lock();
	fclose(f);
	process_unlock();
}

static bool has_snapshots(struct lxc_container *c)
{
	char path[MAXPATHLEN];
	int ret, v;
	FILE *f;
	bool bret = false;

	ret = snprintf(path, MAXPATHLEN, "%s/%s/lxc_snapshots", c->config_path,
			c->name);
	if (ret < 0 || ret > MAXPATHLEN)
		goto out;
	process_lock();
	f = fopen(path, "r");
	process_unlock();
	if (!f)
		goto out;
	ret = fscanf(f, "%d", &v);
	process_lock();
	fclose(f);
	process_unlock();
	if (ret != 1)
		goto out;
	bret = v != 0;

out:
	return bret;
}

// do we want the api to support --force, or leave that to the caller?
static bool lxcapi_destroy(struct lxc_container *c)
{
	struct bdev *r = NULL;
	bool ret = false;

	if (!c || !lxcapi_is_defined(c))
		return false;

	if (container_disk_lock(c))
		return false;

	if (!is_stopped(c)) {
		// we should queue some sort of error - in c->error_string?
		ERROR("container %s is not stopped", c->name);
		goto out;
	}

	if (c->lxc_conf && has_snapshots(c)) {
		ERROR("container %s has dependent snapshots", c->name);
		goto out;
	}

	if (c->lxc_conf && c->lxc_conf->rootfs.path && c->lxc_conf->rootfs.mount)
		r = bdev_init(c->lxc_conf->rootfs.path, c->lxc_conf->rootfs.mount, NULL);
	if (r) {
		if (r->ops->destroy(r) < 0) {
			bdev_put(r);
			ERROR("Error destroying rootfs for %s", c->name);
			goto out;
		}
		bdev_put(r);
	}

	mod_all_rdeps(c, false);

	const char *p1 = lxcapi_get_config_path(c);
	char *path = alloca(strlen(p1) + strlen(c->name) + 2);
	sprintf(path, "%s/%s", p1, c->name);
	if (lxc_rmdir_onedev(path) < 0) {
		ERROR("Error destroying container directory for %s", c->name);
		goto out;
	}
	ret = true;

out:
	container_disk_unlock(c);
	return ret;
}

static bool set_config_item_locked(struct lxc_container *c, const char *key, const char *v)
{
	struct lxc_config_t *config;

	if (!c->lxc_conf)
		c->lxc_conf = lxc_conf_init();
	if (!c->lxc_conf)
		return false;
	config = lxc_getconfig(key);
	if (!config)
		return false;
	return (0 == config->cb(key, v, c->lxc_conf));
}

static bool lxcapi_set_config_item(struct lxc_container *c, const char *key, const char *v)
{
	bool b = false;

	if (!c)
		return false;

	if (container_mem_lock(c))
		return false;

	b = set_config_item_locked(c, key, v);

	container_mem_unlock(c);
	return b;
}

static char *lxcapi_config_file_name(struct lxc_container *c)
{
	if (!c || !c->configfile)
		return NULL;
	return strdup(c->configfile);
}

static const char *lxcapi_get_config_path(struct lxc_container *c)
{
	if (!c || !c->config_path)
		return NULL;
	return (const char *)(c->config_path);
}

/*
 * not for export
 * Just recalculate the c->configfile based on the
 * c->config_path, which must be set.
 * The lxc_container must be locked or not yet public.
 */
static bool set_config_filename(struct lxc_container *c)
{
	char *newpath;
	int len, ret;

	if (!c->config_path)
		return false;

	/* $lxc_path + "/" + c->name + "/" + "config" + '\0' */
	len = strlen(c->config_path) + strlen(c->name) + strlen("config") + 3;
	newpath = malloc(len);
	if (!newpath)
		return false;

	ret = snprintf(newpath, len, "%s/%s/config", c->config_path, c->name);
	if (ret < 0 || ret >= len) {
		fprintf(stderr, "Error printing out config file name\n");
		free(newpath);
		return false;
	}

	if (c->configfile)
		free(c->configfile);
	c->configfile = newpath;

	return true;
}

static bool lxcapi_set_config_path(struct lxc_container *c, const char *path)
{
	char *p;
	bool b = false;
	char *oldpath = NULL;

	if (!c)
		return b;

	if (container_mem_lock(c))
		return b;

	p = strdup(path);
	if (!p) {
		ERROR("Out of memory setting new lxc path");
		goto err;
	}

	b = true;
	if (c->config_path)
		oldpath = c->config_path;
	c->config_path = p;

	/* Since we've changed the config path, we have to change the
	 * config file name too */
	if (!set_config_filename(c)) {
		ERROR("Out of memory setting new config filename");
		b = false;
		free(c->config_path);
		c->config_path = oldpath;
		oldpath = NULL;
	}
err:
	if (oldpath)
		free(oldpath);
	container_mem_unlock(c);
	return b;
}


static bool lxcapi_set_cgroup_item(struct lxc_container *c, const char *subsys, const char *value)
{
	int ret;

	if (!c)
		return false;

	if (is_stopped(c))
		return false;

	if (container_disk_lock(c))
		return false;

	ret = lxc_cgroup_set(subsys, value, c->name, c->config_path);

	container_disk_unlock(c);
	return ret == 0;
}

static int lxcapi_get_cgroup_item(struct lxc_container *c, const char *subsys, char *retv, int inlen)
{
	int ret;

	if (!c || !c->lxc_conf)
		return -1;

	if (is_stopped(c))
		return -1;

	if (container_disk_lock(c))
		return -1;

	ret = lxc_cgroup_get(subsys, retv, inlen, c->name, c->config_path);

	container_disk_unlock(c);
	return ret;
}

const char *lxc_get_default_config_path(void)
{
	return default_lxc_path();
}

const char *lxc_get_default_lvm_vg(void)
{
	return default_lvm_vg();
}

const char *lxc_get_default_lvm_thin_pool(void)
{
	return default_lvm_thin_pool();
}

const char *lxc_get_default_zfs_root(void)
{
	return default_zfs_root();
}

const char *lxc_get_version(void)
{
	return lxc_version();
}

static int copy_file(char *old, char *new)
{
	int in, out;
	ssize_t len, ret;
	char buf[8096];
	struct stat sbuf;

	if (file_exists(new)) {
		ERROR("copy destination %s exists", new);
		return -1;
	}
	ret = stat(old, &sbuf);
	if (ret < 0) {
		INFO("Error stat'ing %s", old);
		return -1;
	}

	process_lock();
	in = open(old, O_RDONLY);
	process_unlock();
	if (in < 0) {
		SYSERROR("Error opening original file %s", old);
		return -1;
	}
	process_lock();
	out = open(new, O_CREAT | O_EXCL | O_WRONLY, 0644);
	process_unlock();
	if (out < 0) {
		SYSERROR("Error opening new file %s", new);
		process_lock();
		close(in);
		process_unlock();
		return -1;
	}

	while (1) {
		len = read(in, buf, 8096);
		if (len < 0) {
			SYSERROR("Error reading old file %s", old);
			goto err;
		}
		if (len == 0)
			break;
		ret = write(out, buf, len);
		if (ret < len) {  // should we retry?
			SYSERROR("Error: write to new file %s was interrupted", new);
			goto err;
		}
	}
	process_lock();
	close(in);
	close(out);
	process_unlock();

	// we set mode, but not owner/group
	ret = chmod(new, sbuf.st_mode);
	if (ret) {
		SYSERROR("Error setting mode on %s", new);
		return -1;
	}

	return 0;

err:
	process_lock();
	close(in);
	close(out);
	process_unlock();
	return -1;
}

static int copyhooks(struct lxc_container *oldc, struct lxc_container *c)
{
	int i;
	int ret;
	struct lxc_list *it;

	for (i=0; i<NUM_LXC_HOOKS; i++) {
		lxc_list_for_each(it, &c->lxc_conf->hooks[i]) {
			char *hookname = it->elem;
			char *fname = strrchr(hookname, '/');
			char tmppath[MAXPATHLEN];
			if (!fname) // relative path - we don't support, but maybe we should
				return 0;
			// copy the script, and change the entry in confile
			ret = snprintf(tmppath, MAXPATHLEN, "%s/%s/%s",
					c->config_path, c->name, fname+1);
			if (ret < 0 || ret >= MAXPATHLEN)
				return -1;
			ret = copy_file(it->elem, tmppath);
			if (ret < 0)
				return -1;
			free(it->elem);
			it->elem = strdup(tmppath);
			if (!it->elem) {
				ERROR("out of memory copying hook path");
				return -1;
			}
		}
	}

	c->save_config(c, NULL);
	return 0;
}

static void new_hwaddr(char *hwaddr)
{
	FILE *f;
	process_lock();
	f = fopen("/dev/urandom", "r");
	process_unlock();
	if (f) {
		unsigned int seed;
		int ret = fread(&seed, sizeof(seed), 1, f);
		if (ret != 1)
			seed = time(NULL);
		process_lock();
		fclose(f);
		process_unlock();
		srand(seed);
	} else
		srand(time(NULL));
	snprintf(hwaddr, 18, "00:16:3e:%02x:%02x:%02x",
			rand() % 255, rand() % 255, rand() % 255);
}

static void network_new_hwaddrs(struct lxc_container *c)
{
	struct lxc_list *it;

	lxc_list_for_each(it, &c->lxc_conf->network) {
		struct lxc_netdev *n = it->elem;
		if (n->hwaddr)
			new_hwaddr(n->hwaddr);
	}
}

static int copy_fstab(struct lxc_container *oldc, struct lxc_container *c)
{
	char newpath[MAXPATHLEN];
	char *oldpath = oldc->lxc_conf->fstab;
	int ret;

	if (!oldpath)
		return 0;

	char *p = strrchr(oldpath, '/');
	if (!p)
		return -1;
	ret = snprintf(newpath, MAXPATHLEN, "%s/%s%s",
			c->config_path, c->name, p);
	if (ret < 0 || ret >= MAXPATHLEN) {
		ERROR("error printing new path for %s", oldpath);
		return -1;
	}
	if (file_exists(newpath)) {
		ERROR("error: fstab file %s exists", newpath);
		return -1;
	}

	if (copy_file(oldpath, newpath) < 0) {
		ERROR("error: copying %s to %s", oldpath, newpath);
		return -1;
	}
	free(c->lxc_conf->fstab);
	c->lxc_conf->fstab = strdup(newpath);
	if (!c->lxc_conf->fstab) {
		ERROR("error: allocating pathname");
		return -1;
	}

	return 0;
}

static void copy_rdepends(struct lxc_container *c, struct lxc_container *c0)
{
	char path0[MAXPATHLEN], path1[MAXPATHLEN];
	int ret;

	ret = snprintf(path0, MAXPATHLEN, "%s/%s/lxc_rdepends", c0->config_path,
		c0->name);
	if (ret < 0 || ret >= MAXPATHLEN) {
		WARN("Error copying reverse dependencies");
		return;
	}
	ret = snprintf(path1, MAXPATHLEN, "%s/%s/lxc_rdepends", c->config_path,
		c->name);
	if (ret < 0 || ret >= MAXPATHLEN) {
		WARN("Error copying reverse dependencies");
		return;
	}
	if (copy_file(path0, path1) < 0) {
		INFO("Error copying reverse dependencies");
		return;
	}
}

static bool add_rdepends(struct lxc_container *c, struct lxc_container *c0)
{
	int ret;
	char path[MAXPATHLEN];
	FILE *f;
	bool bret;

	ret = snprintf(path, MAXPATHLEN, "%s/%s/lxc_rdepends", c->config_path,
		c->name);
	if (ret < 0 || ret >= MAXPATHLEN)
		return false;
	process_lock();
	f = fopen(path, "a");
	process_unlock();
	if (!f)
		return false;
	bret = true;
	// if anything goes wrong, just return an error
	if (fprintf(f, "%s\n%s\n", c0->config_path, c0->name) < 0)
		bret = false;
	process_lock();
	if (fclose(f) != 0)
		bret = false;
	process_unlock();
	return bret;
}

static int copy_storage(struct lxc_container *c0, struct lxc_container *c,
		const char *newtype, int flags, const char *bdevdata, unsigned long newsize)
{
	struct bdev *bdev;
	int need_rdep;

	bdev = bdev_copy(c0->lxc_conf->rootfs.path, c0->name, c->name,
			c0->config_path, c->config_path, newtype, !!(flags & LXC_CLONE_SNAPSHOT),
			bdevdata, newsize, &need_rdep);
	if (!bdev) {
		ERROR("Error copying storage");
		return -1;
	}
	free(c->lxc_conf->rootfs.path);
	c->lxc_conf->rootfs.path = strdup(bdev->src);
	bdev_put(bdev);
	if (!c->lxc_conf->rootfs.path) {
		ERROR("Out of memory while setting storage path");
		return -1;
	}
	if (flags & LXC_CLONE_SNAPSHOT)
		copy_rdepends(c, c0);
	if (need_rdep) {
		if (!add_rdepends(c, c0))
			WARN("Error adding reverse dependency from %s to %s",
				c->name, c0->name);
	}

	mod_all_rdeps(c, true);

	return 0;
}

static int clone_update_rootfs(struct lxc_container *c0,
			       struct lxc_container *c, int flags,
			       char **hookargs)
{
	int ret = -1;
	char path[MAXPATHLEN];
	struct bdev *bdev;
	FILE *fout;
	pid_t pid;
	struct lxc_conf *conf = c->lxc_conf;

	/* update hostname in rootfs */
	/* we're going to mount, so run in a clean namespace to simplify cleanup */

	pid = fork();
	if (pid < 0)
		return -1;
	if (pid > 0)
		return wait_for_pid(pid);

	process_unlock(); // we're no longer sharing
	bdev = bdev_init(c->lxc_conf->rootfs.path, c->lxc_conf->rootfs.mount, NULL);
	if (!bdev)
		exit(1);
	if (strcmp(bdev->type, "dir") != 0) {
		if (unshare(CLONE_NEWNS) < 0) {
			ERROR("error unsharing mounts");
			exit(1);
		}
		if (bdev->ops->mount(bdev) < 0)
			exit(1);
	} else { // TODO come up with a better way
		if (bdev->dest)
			free(bdev->dest);
		bdev->dest = strdup(bdev->src);
	}

	if (!lxc_list_empty(&conf->hooks[LXCHOOK_CLONE])) {
		/* Start of environment variable setup for hooks */
		if (setenv("LXC_SRC_NAME", c0->name, 1)) {
			SYSERROR("failed to set environment variable for source container name");
		}
		if (setenv("LXC_NAME", c->name, 1)) {
			SYSERROR("failed to set environment variable for container name");
		}
		if (setenv("LXC_CONFIG_FILE", conf->rcfile, 1)) {
			SYSERROR("failed to set environment variable for config path");
		}
		if (setenv("LXC_ROOTFS_MOUNT", conf->rootfs.mount, 1)) {
			SYSERROR("failed to set environment variable for rootfs mount");
		}
		if (setenv("LXC_ROOTFS_PATH", conf->rootfs.path, 1)) {
			SYSERROR("failed to set environment variable for rootfs mount");
		}

		if (run_lxc_hooks(c->name, "clone", conf, c->get_config_path(c), hookargs)) {
			ERROR("Error executing clone hook for %s", c->name);
			exit(1);
		}
	}

	if (!(flags & LXC_CLONE_KEEPNAME)) {
		ret = snprintf(path, MAXPATHLEN, "%s/etc/hostname", bdev->dest);
		if (ret < 0 || ret >= MAXPATHLEN)
			exit(1);
		if (!file_exists(path))
			exit(0);
		if (!(fout = fopen(path, "w"))) {
			SYSERROR("unable to open %s: ignoring\n", path);
			exit(0);
		}
		if (fprintf(fout, "%s", c->name) < 0)
			exit(1);
		if (fclose(fout) < 0)
			exit(1);
	}
	exit(0);
}

/*
 * We want to support:
sudo lxc-clone -o o1 -n n1 -s -L|-fssize fssize -v|--vgname vgname \
        -p|--lvprefix lvprefix -t|--fstype fstype  -B backingstore

-s [ implies overlayfs]
-s -B overlayfs
-s -B aufs

only rootfs gets converted (copied/snapshotted) on clone.
*/

static int create_file_dirname(char *path)
{
	char *p = strrchr(path, '/');
	int ret;

	if (!p)
		return -1;
	*p = '\0';
	ret = mkdir(path, 0755);
	if (ret && errno != EEXIST)
		SYSERROR("creating container path %s\n", path);
	*p = '/';
	return ret;
}

struct lxc_container *lxcapi_clone(struct lxc_container *c, const char *newname,
		const char *lxcpath, int flags,
		const char *bdevtype, const char *bdevdata, unsigned long newsize,
		char **hookargs)
{
	struct lxc_container *c2 = NULL;
	char newpath[MAXPATHLEN];
	int ret, storage_copied = 0;
	const char *n, *l;
	FILE *fout;

	if (!c || !c->is_defined(c))
		return NULL;

	if (container_mem_lock(c))
		return NULL;

	if (!is_stopped(c)) {
		ERROR("error: Original container (%s) is running", c->name);
		goto out;
	}

	// Make sure the container doesn't yet exist.
	n = newname ? newname : c->name;
	l = lxcpath ? lxcpath : c->get_config_path(c);
	ret = snprintf(newpath, MAXPATHLEN, "%s/%s/config", l, n);
	if (ret < 0  || ret >= MAXPATHLEN) {
		SYSERROR("clone: failed making config pathname");
		goto out;
	}
	if (file_exists(newpath)) {
		ERROR("error: clone: %s exists", newpath);
		goto out;
	}

	ret = create_file_dirname(newpath);
	if (ret < 0 && errno != EEXIST) {
		ERROR("Error creating container dir for %s", newpath);
		goto out;
	}

	// copy the configuration, tweak it as needed,
	process_lock();
	fout = fopen(newpath, "w");
	process_unlock();
	if (!fout) {
		SYSERROR("open %s", newpath);
		goto out;
	}
	write_config(fout, c->lxc_conf);
	process_lock();
	fclose(fout);
	process_unlock();

	sprintf(newpath, "%s/%s/rootfs", l, n);
	if (mkdir(newpath, 0755) < 0) {
		SYSERROR("error creating %s", newpath);
		goto out;
	}

	c2 = lxc_container_new(n, l);
	if (!c2) {
		ERROR("clone: failed to create new container (%s %s)", n, l);
		goto out;
	}

	// update utsname
	if (!set_config_item_locked(c2, "lxc.utsname", newname)) {
		ERROR("Error setting new hostname");
		goto out;
	}


	// copy hooks if requested
	if (flags & LXC_CLONE_COPYHOOKS) {
		ret = copyhooks(c, c2);
		if (ret < 0) {
			ERROR("error copying hooks");
			goto out;
		}
	}

	if (copy_fstab(c, c2) < 0) {
		ERROR("error copying fstab");
		goto out;
	}

	// update macaddrs
	if (!(flags & LXC_CLONE_KEEPMACADDR))
		network_new_hwaddrs(c2);

	// copy/snapshot rootfs's
	ret = copy_storage(c, c2, bdevtype, flags, bdevdata, newsize);
	if (ret < 0)
		goto out;

	// We've now successfully created c2's storage, so clear it out if we
	// fail after this
	storage_copied = 1;

	if (!c2->save_config(c2, NULL))
		goto out;

	if (clone_update_rootfs(c, c2, flags, hookargs) < 0)
		goto out;

	// TODO: update c's lxc.snapshot = count
	container_mem_unlock(c);
	return c2;

out:
	container_mem_unlock(c);
	if (c2) {
		if (!storage_copied)
			c2->lxc_conf->rootfs.path = NULL;
		c2->destroy(c2);
		lxc_container_put(c2);
	}

	return NULL;
}

static int lxcapi_attach(struct lxc_container *c, lxc_attach_exec_t exec_function, void *exec_payload, lxc_attach_options_t *options, pid_t *attached_process)
{
	if (!c)
		return -1;

	return lxc_attach(c->name, c->config_path, exec_function, exec_payload, options, attached_process);
}

static int lxcapi_attach_run_wait(struct lxc_container *c, lxc_attach_options_t *options, const char *program, const char * const argv[])
{
	lxc_attach_command_t command;
	pid_t pid;
	int r;

	if (!c)
		return -1;

	command.program = (char*)program;
	command.argv = (char**)argv;
	r = lxc_attach(c->name, c->config_path, lxc_attach_run_command, &command, options, &pid);
	if (r < 0) {
		ERROR("ups");
		return r;
	}
	return lxc_wait_for_pid_status(pid);
}

int get_next_index(const char *lxcpath, char *cname)
{
	char *fname;
	struct stat sb;
	int i = 0, ret;

	fname = alloca(strlen(lxcpath) + 20);
	while (1) {
		sprintf(fname, "%s/snap%d", lxcpath, i);
		ret = stat(fname, &sb);
		if (ret != 0)
			return i;
		i++;
	}
}

static int lxcapi_snapshot(struct lxc_container *c, char *commentfile)
{
	int i, flags, ret;
	struct lxc_container *c2;
	char snappath[MAXPATHLEN], newname[20];

	// /var/lib/lxc -> /var/lib/lxcsnaps \0
	ret = snprintf(snappath, MAXPATHLEN, "%ssnaps/%s", c->config_path, c->name);
	if (ret < 0 || ret >= MAXPATHLEN)
		return -1;
	i = get_next_index(snappath, c->name);

	if (mkdir_p(snappath, 0755) < 0) {
		ERROR("Failed to create snapshot directory %s", snappath);
		return -1;
	}

	ret = snprintf(newname, 20, "snap%d", i);
	if (ret < 0 || ret >= 20)
		return -1;

	flags = LXC_CLONE_SNAPSHOT | LXC_CLONE_KEEPMACADDR | LXC_CLONE_KEEPNAME;
	c2 = c->clone(c, newname, snappath, flags, NULL, NULL, 0, NULL);
	if (!c2) {
		ERROR("clone of %s:%s failed\n", c->config_path, c->name);
		return -1;
	}

	lxc_container_put(c2);

	// Now write down the creation time
	time_t timer;
	char buffer[25];
	struct tm* tm_info;
	FILE *f;

	time(&timer);
	tm_info = localtime(&timer);

	strftime(buffer, 25, "%Y:%m:%d %H:%M:%S", tm_info);

	char *dfnam = alloca(strlen(snappath) + strlen(newname) + 5);
	sprintf(dfnam, "%s/%s/ts", snappath, newname);
	process_lock();
	f = fopen(dfnam, "w");
	process_unlock();
	if (!f) {
		ERROR("Failed to open %s\n", dfnam);
		return -1;
	}
	if (fprintf(f, "%s", buffer) < 0) {
		SYSERROR("Writing timestamp");
		fclose(f);
		return -1;
	}
	process_lock();
	ret = fclose(f);
	process_unlock();
	if (ret != 0) {
		SYSERROR("Writing timestamp");
		return -1;
	}

	if (commentfile) {
		// $p / $name / comment \0
		int len = strlen(snappath) + strlen(newname) + 10;
		char *path = alloca(len);
		sprintf(path, "%s/%s/comment", snappath, newname);
		return copy_file(commentfile, path) < 0 ? -1 : i;
	}

	return i;
}

static void lxcsnap_free(struct lxc_snapshot *s)
{
	if (s->name)
		free(s->name);
	if (s->comment_pathname)
		free(s->comment_pathname);
	if (s->timestamp)
		free(s->timestamp);
	if (s->lxcpath)
		free(s->lxcpath);
}

static char *get_snapcomment_path(char* snappath, char *name)
{
	// $snappath/$name/comment
	int ret, len = strlen(snappath) + strlen(name) + 10;
	char *s = malloc(len);

	if (s) {
		ret = snprintf(s, len, "%s/%s/comment", snappath, name);
		if (ret < 0 || ret >= len) {
			free(s);
			s = NULL;
		}
	}
	return s;
}

static char *get_timestamp(char* snappath, char *name)
{
	char path[MAXPATHLEN], *s = NULL;
	int ret, len;
	FILE *fin;

	ret = snprintf(path, MAXPATHLEN, "%s/%s/ts", snappath, name);
	if (ret < 0 || ret >= MAXPATHLEN)
		return NULL;
	process_lock();
	fin = fopen(path, "r");
	process_unlock();
	if (!fin)
		return NULL;
	(void) fseek(fin, 0, SEEK_END);
	len = ftell(fin);
	(void) fseek(fin, 0, SEEK_SET);
	if (len > 0) {
		s = malloc(len+1);
		if (s) {
			s[len] = '\0';
			if (fread(s, 1, len, fin) != len) {
				SYSERROR("reading timestamp");
				free(s);
				s = NULL;
			}
		}
	}
	process_lock();
	fclose(fin);
	process_unlock();
	return s;
}

static int lxcapi_snapshot_list(struct lxc_container *c, struct lxc_snapshot **ret_snaps)
{
	char snappath[MAXPATHLEN], path2[MAXPATHLEN];
	int dirlen, count = 0, ret;
	struct dirent dirent, *direntp;
	struct lxc_snapshot *snaps =NULL, *nsnaps;
	DIR *dir;

	if (!c || !lxcapi_is_defined(c))
		return -1;
	// snappath is ${lxcpath}snaps/${lxcname}/
	dirlen = snprintf(snappath, MAXPATHLEN, "%ssnaps/%s", c->config_path, c->name);
	if (dirlen < 0 || dirlen >= MAXPATHLEN) {
		ERROR("path name too long");
		return -1;
	}
	process_lock();
	dir = opendir(snappath);
	process_unlock();
	if (!dir) {
		INFO("failed to open %s - assuming no snapshots", snappath);
		return 0;
	}

	while (!readdir_r(dir, &dirent, &direntp)) {
		if (!direntp)
			break;

		if (!strcmp(direntp->d_name, "."))
			continue;

		if (!strcmp(direntp->d_name, ".."))
			continue;

		ret = snprintf(path2, MAXPATHLEN, "%s/%s/config", snappath, direntp->d_name);
		if (ret < 0 || ret >= MAXPATHLEN) {
			ERROR("pathname too long");
			goto out_free;
		}
		if (!file_exists(path2))
			continue;
		nsnaps = realloc(snaps, (count + 1)*sizeof(*snaps));
		if (!nsnaps) {
			SYSERROR("Out of memory");
			goto out_free;
		}
		snaps = nsnaps;
		snaps[count].free = lxcsnap_free;
		snaps[count].name = strdup(direntp->d_name);
		if (!snaps[count].name)
			goto out_free;
		snaps[count].lxcpath = strdup(snappath);
		if (!snaps[count].lxcpath) {
			free(snaps[count].name);
			goto out_free;
		}
		snaps[count].comment_pathname = get_snapcomment_path(snappath, direntp->d_name);
		snaps[count].timestamp = get_timestamp(snappath, direntp->d_name);
		count++;
	}

	process_lock();
	if (closedir(dir))
		WARN("failed to close directory");
	process_unlock();

	*ret_snaps = snaps;
	return count;

out_free:
	if (snaps) {
		int i;
		for (i=0; i<count; i++)
			lxcsnap_free(&snaps[i]);
		free(snaps);
	}
	process_lock();
	if (closedir(dir))
		WARN("failed to close directory");
	process_unlock();
	return -1;
}

static bool lxcapi_snapshot_restore(struct lxc_container *c, char *snapname, char *newname)
{
	char clonelxcpath[MAXPATHLEN];
	int ret;
	struct lxc_container *snap, *rest;
	struct bdev *bdev;
	bool b = false;

	if (!c || !c->name || !c->config_path)
		return false;

	bdev = bdev_init(c->lxc_conf->rootfs.path, c->lxc_conf->rootfs.mount, NULL);
	if (!bdev) {
		ERROR("Failed to find original backing store type");
		return false;
	}

	if (!newname)
		newname = c->name;
	if (strcmp(c->name, newname) == 0) {
		if (!lxcapi_destroy(c)) {
			ERROR("Could not destroy existing container %s", newname);
			bdev_put(bdev);
			return false;
		}
	}
	ret = snprintf(clonelxcpath, MAXPATHLEN, "%ssnaps/%s", c->config_path, c->name);
	if (ret < 0 || ret >= MAXPATHLEN) {
		bdev_put(bdev);
		return false;
	}
	// how should we lock this?

	snap = lxc_container_new(snapname, clonelxcpath);
	if (!snap || !lxcapi_is_defined(snap)) {
		ERROR("Could not open snapshot %s", snapname);
		if (snap) lxc_container_put(snap);
		bdev_put(bdev);
		return false;
	}

	rest = lxcapi_clone(snap, newname, c->config_path, 0, bdev->type, NULL, 0, NULL);
	bdev_put(bdev);
	if (rest && lxcapi_is_defined(rest))
		b = true;
	if (rest)
		lxc_container_put(rest);
	lxc_container_put(snap);
	return b;
}

static bool lxcapi_snapshot_destroy(struct lxc_container *c, char *snapname)
{
	int ret;
	char clonelxcpath[MAXPATHLEN];
	struct lxc_container *snap = NULL;

	if (!c || !c->name || !c->config_path)
		return false;

	ret = snprintf(clonelxcpath, MAXPATHLEN, "%ssnaps/%s", c->config_path, c->name);
	if (ret < 0 || ret >= MAXPATHLEN)
		goto err;

	snap = lxc_container_new(snapname, clonelxcpath);
	if (!snap || !lxcapi_is_defined(snap)) {
		ERROR("Could not find snapshot %s", snapname);
		goto err;
	}

	if (!lxcapi_destroy(snap)) {
		ERROR("Could not destroy snapshot %s", snapname);
		goto err;
	}
	lxc_container_put(snap);

	return true;
err:
	if (snap)
		lxc_container_put(snap);
	return false;
}

static bool lxcapi_may_control(struct lxc_container *c)
{
	return lxc_try_cmd(c->name, c->config_path) == 0;
}

static int lxcapi_attach_run_waitl(struct lxc_container *c, lxc_attach_options_t *options, const char *program, const char *arg, ...)
{
	va_list ap;
	const char **argv;
	int ret;

	if (!c)
		return -1;

	va_start(ap, arg);
	argv = lxc_va_arg_list_to_argv_const(ap, 1);
	va_end(ap);

	if (!argv) {
		ERROR("Memory allocation error.");
		return -1;
	}
	argv[0] = arg;

	ret = lxcapi_attach_run_wait(c, options, program, (const char * const *)argv);
	free((void*)argv);
	return ret;
}

struct lxc_container *lxc_container_new(const char *name, const char *configpath)
{
	struct lxc_container *c;

	c = malloc(sizeof(*c));
	if (!c) {
		fprintf(stderr, "failed to malloc lxc_container\n");
		return NULL;
	}
	memset(c, 0, sizeof(*c));

	if (configpath)
		c->config_path = strdup(configpath);
	else
		c->config_path = strdup(default_lxc_path());

	if (!c->config_path) {
		fprintf(stderr, "Out of memory");
		goto err;
	}

	remove_trailing_slashes(c->config_path);
	c->name = malloc(strlen(name)+1);
	if (!c->name) {
		fprintf(stderr, "Error allocating lxc_container name\n");
		goto err;
	}
	strcpy(c->name, name);

	c->numthreads = 1;
	if (!(c->slock = lxc_newlock(c->config_path, name))) {
		fprintf(stderr, "failed to create lock\n");
		goto err;
	}

	if (!(c->privlock = lxc_newlock(NULL, NULL))) {
		fprintf(stderr, "failed to alloc privlock\n");
		goto err;
	}

	if (!set_config_filename(c)) {
		fprintf(stderr, "Error allocating config file pathname\n");
		goto err;
	}

	if (file_exists(c->configfile))
		lxcapi_load_config(c, NULL);

	if (ongoing_create(c) == 2) {
		ERROR("Error: %s creation was not completed", c->name);
		lxcapi_destroy(c);
		lxc_conf_free(c->lxc_conf);
		c->lxc_conf = NULL;
	}

	// assign the member functions
	c->is_defined = lxcapi_is_defined;
	c->state = lxcapi_state;
	c->is_running = lxcapi_is_running;
	c->freeze = lxcapi_freeze;
	c->unfreeze = lxcapi_unfreeze;
	c->console = lxcapi_console;
	c->console_getfd = lxcapi_console_getfd;
	c->init_pid = lxcapi_init_pid;
	c->load_config = lxcapi_load_config;
	c->want_daemonize = lxcapi_want_daemonize;
	c->want_close_all_fds = lxcapi_want_close_all_fds;
	c->start = lxcapi_start;
	c->startl = lxcapi_startl;
	c->stop = lxcapi_stop;
	c->config_file_name = lxcapi_config_file_name;
	c->wait = lxcapi_wait;
	c->set_config_item = lxcapi_set_config_item;
	c->destroy = lxcapi_destroy;
	c->save_config = lxcapi_save_config;
	c->get_keys = lxcapi_get_keys;
	c->create = lxcapi_create;
	c->createl = lxcapi_createl;
	c->shutdown = lxcapi_shutdown;
	c->reboot = lxcapi_reboot;
	c->clear_config_item = lxcapi_clear_config_item;
	c->get_config_item = lxcapi_get_config_item;
	c->get_cgroup_item = lxcapi_get_cgroup_item;
	c->set_cgroup_item = lxcapi_set_cgroup_item;
	c->get_config_path = lxcapi_get_config_path;
	c->set_config_path = lxcapi_set_config_path;
	c->clone = lxcapi_clone;
	c->get_interfaces = lxcapi_get_interfaces;
	c->get_ips = lxcapi_get_ips;
	c->attach = lxcapi_attach;
	c->attach_run_wait = lxcapi_attach_run_wait;
	c->attach_run_waitl = lxcapi_attach_run_waitl;
	c->snapshot = lxcapi_snapshot;
	c->snapshot_list = lxcapi_snapshot_list;
	c->snapshot_restore = lxcapi_snapshot_restore;
	c->snapshot_destroy = lxcapi_snapshot_destroy;
	c->may_control = lxcapi_may_control;

	/* we'll allow the caller to update these later */
	if (lxc_log_init(NULL, "none", NULL, "lxc_container", 0, c->config_path)) {
		fprintf(stderr, "failed to open log\n");
		goto err;
	}

	return c;

err:
	lxc_container_free(c);
	return NULL;
}

int lxc_get_wait_states(const char **states)
{
	int i;

	if (states)
		for (i=0; i<MAX_STATE; i++)
			states[i] = lxc_state2str(i);
	return MAX_STATE;
}

/*
 * These next two could probably be done smarter with reusing a common function
 * with different iterators and tests...
 */
int list_defined_containers(const char *lxcpath, char ***names, struct lxc_container ***cret)
{
	DIR *dir;
	int i, cfound = 0, nfound = 0;
	struct dirent dirent, *direntp;
	struct lxc_container *c;

	if (!lxcpath)
		lxcpath = default_lxc_path();

	process_lock();
	dir = opendir(lxcpath);
	process_unlock();

	if (!dir) {
		SYSERROR("opendir on lxcpath");
		return -1;
	}

	if (cret)
		*cret = NULL;
	if (names)
		*names = NULL;

	while (!readdir_r(dir, &dirent, &direntp)) {
		if (!direntp)
			break;
		if (!strcmp(direntp->d_name, "."))
			continue;
		if (!strcmp(direntp->d_name, ".."))
			continue;

		if (!config_file_exists(lxcpath, direntp->d_name))
			continue;

		if (names) {
			if (!add_to_array(names, direntp->d_name, cfound))
				goto free_bad;
		}
		cfound++;

		if (!cret) {
			nfound++;
			continue;
		}

		c = lxc_container_new(direntp->d_name, lxcpath);
		if (!c) {
			INFO("Container %s:%s has a config but could not be loaded",
				lxcpath, direntp->d_name);
			if (names)
				if(!remove_from_array(names, direntp->d_name, cfound--))
					goto free_bad;
			continue;
		}
		if (!lxcapi_is_defined(c)) {
			INFO("Container %s:%s has a config but is not defined",
				lxcpath, direntp->d_name);
			if (names)
				if(!remove_from_array(names, direntp->d_name, cfound--))
					goto free_bad;
			lxc_container_put(c);
			continue;
		}

		if (!add_to_clist(cret, c, nfound)) {
			lxc_container_put(c);
			goto free_bad;
		}
		nfound++;
	}

	process_lock();
	closedir(dir);
	process_unlock();
	return nfound;

free_bad:
	if (names && *names) {
		for (i=0; i<cfound; i++)
			free((*names)[i]);
		free(*names);
	}
	if (cret && *cret) {
		for (i=0; i<nfound; i++)
			lxc_container_put((*cret)[i]);
		free(*cret);
	}
	process_lock();
	closedir(dir);
	process_unlock();
	return -1;
}

int list_active_containers(const char *lxcpath, char ***names, struct lxc_container ***cret)
{
	int i, cfound = 0, nfound = 0;
	int lxcpath_len;
	char *line = NULL;
	char **unique_names = NULL;
	size_t len = 0;
	struct lxc_container *c;

	if (!lxcpath)
		lxcpath = default_lxc_path();
	lxcpath_len = strlen(lxcpath);

	if (cret)
		*cret = NULL;
	if (names)
		*names = NULL;

	process_lock();
	FILE *f = fopen("/proc/net/unix", "r");
	process_unlock();
	if (!f)
		return -1;

	while (getline(&line, &len, f) != -1) {
		char *p = strrchr(line, ' '), *p2;
		if (!p)
			continue;
		p++;
		if (*p != 0x40)
			continue;
		p++;
		if (strncmp(p, lxcpath, lxcpath_len) != 0)
			continue;
		p += lxcpath_len;
		while (*p == '/')
			p++;

		// Now p is the start of lxc_name
		p2 = index(p, '/');
		if (!p2 || strncmp(p2, "/command", 8) != 0)
			continue;
		*p2 = '\0';

		if (array_contains(&unique_names, p, nfound))
			continue;

		if (!add_to_array(&unique_names, p, nfound))
			goto free_bad;

		cfound++;

		if (!cret) {
			nfound++;
			continue;
		}

		c = lxc_container_new(p, lxcpath);
		if (!c) {
			INFO("Container %s:%s is running but could not be loaded",
				lxcpath, p);
			if (names) {
				if(!remove_from_array(&unique_names, p, cfound--))
					goto free_bad;
			}
			continue;
		}

		/*
		 * If this is an anonymous container, then is_defined *can*
		 * return false.  So we don't do that check.  Count on the
		 * fact that the command socket exists.
		 */

		if (!add_to_clist(cret, c, nfound)) {
			lxc_container_put(c);
			goto free_bad;
		}
		nfound++;
	}

	if (names)
		*names = unique_names;

	if (line)
		free(line);

	process_lock();
	fclose(f);
	process_unlock();
	return nfound;

free_bad:
	if (names && *names) {
		for (i=0; i<cfound; i++)
			free((*names)[i]);
		free(*names);
	}
	if (cret && *cret) {
		for (i=0; i<nfound; i++)
			lxc_container_put((*cret)[i]);
		free(*cret);
	}
	if (line)
		free(line);

	process_lock();
	fclose(f);
	process_unlock();
	return -1;
}
