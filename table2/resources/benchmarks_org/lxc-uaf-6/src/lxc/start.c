/*
 * lxc: linux Container library
 *
 * (C) Copyright IBM Corp. 2007, 2008
 *
 * Authors:
 * Daniel Lezcano <dlezcano at fr.ibm.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include "config.h"

#include <stdio.h>
#undef _GNU_SOURCE
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/param.h>
#include <sys/file.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/capability.h>
#include <sys/wait.h>
#include <sys/un.h>
#include <sys/poll.h>

#ifdef HAVE_SYS_SIGNALFD_H
#  include <sys/signalfd.h>
#else
/* assume kernel headers are too old */
#include <stdint.h>
struct signalfd_siginfo
{
	uint32_t ssi_signo;
	int32_t ssi_errno;
	int32_t ssi_code;
	uint32_t ssi_pid;
	uint32_t ssi_uid;
	int32_t ssi_fd;
	uint32_t ssi_tid;
	uint32_t ssi_band;
	uint32_t ssi_overrun;
	uint32_t ssi_trapno;
	int32_t ssi_status;
	int32_t ssi_int;
	uint64_t ssi_ptr;
	uint64_t ssi_utime;
	uint64_t ssi_stime;
	uint64_t ssi_addr;
	uint8_t __pad[48];
};

#  ifndef __NR_signalfd4
/* assume kernel headers are too old */
#    if __i386__
#      define __NR_signalfd4 327
#    elif __x86_64__
#      define __NR_signalfd4 289
#    elif __powerpc__
#      define __NR_signalfd4 313
#    elif __s390x__
#      define __NR_signalfd4 322
#    endif
#endif

#  ifndef __NR_signalfd
/* assume kernel headers are too old */
#    if __i386__
#      define __NR_signalfd 321
#    elif __x86_64__
#      define __NR_signalfd 282
#    elif __powerpc__
#      define __NR_signalfd 305
#    elif __s390x__
#      define __NR_signalfd 316
#    endif
#endif

int signalfd(int fd, const sigset_t *mask, int flags)
{
	int retval;

	retval = syscall (__NR_signalfd4, fd, mask, _NSIG / 8, flags);
	if (errno == ENOSYS && flags == 0)
		retval = syscall (__NR_signalfd, fd, mask, _NSIG / 8);
	return retval;
}
#endif

#if !HAVE_DECL_PR_CAPBSET_DROP
#define PR_CAPBSET_DROP 24
#endif

#include "start.h"
#include "conf.h"
#include "log.h"
#include "cgroup.h"
#include "error.h"
#include "af_unix.h"
#include "mainloop.h"
#include "utils.h"
#include "utmp.h"
#include "monitor.h"
#include "commands.h"
#include "console.h"
#include "sync.h"
#include "namespace.h"
#include "apparmor.h"
#include "lxcseccomp.h"

lxc_log_define(lxc_start, lxc);

static int match_fd(int fd)
{
	return (fd == 0 || fd == 1 || fd == 2);
}

int lxc_check_inherited(struct lxc_conf *conf, int fd_to_ignore)
{
	struct dirent dirent, *direntp;
	int fd, fddir;
	DIR *dir;

restart:
	dir = opendir("/proc/self/fd");
	if (!dir) {
		WARN("failed to open directory: %m");
		return -1;
	}

	fddir = dirfd(dir);

	while (!readdir_r(dir, &dirent, &direntp)) {
		if (!direntp)
			break;

		if (!strcmp(direntp->d_name, "."))
			continue;

		if (!strcmp(direntp->d_name, ".."))
			continue;

		fd = atoi(direntp->d_name);

		if (fd == fddir || fd == lxc_log_fd || fd == fd_to_ignore)
			continue;

		if (match_fd(fd))
			continue;

		if (conf->close_all_fds) {
			close(fd);
			closedir(dir);
			INFO("closed inherited fd %d", fd);
			goto restart;
		}
		WARN("inherited fd %d", fd);
	}

	closedir(dir); /* cannot fail */
	return 0;
}

static int setup_signal_fd(sigset_t *oldmask)
{
	sigset_t mask;
	int fd;

	/* Block everything except serious error signals */
	if (sigfillset(&mask) ||
	    sigdelset(&mask, SIGILL) ||
	    sigdelset(&mask, SIGSEGV) ||
	    sigdelset(&mask, SIGBUS) ||
	    sigprocmask(SIG_BLOCK, &mask, oldmask)) {
		SYSERROR("failed to set signal mask");
		return -1;
	}

	fd = signalfd(-1, &mask, 0);
	if (fd < 0) {
		SYSERROR("failed to create the signal fd");
		return -1;
	}

	if (fcntl(fd, F_SETFD, FD_CLOEXEC)) {
		SYSERROR("failed to set sigfd to close-on-exec");
		close(fd);
		return -1;
	}

	DEBUG("sigchild handler set");

	return fd;
}

static int signal_handler(int fd, void *data,
			   struct lxc_epoll_descr *descr)
{
	struct signalfd_siginfo siginfo;
	int ret;
	pid_t *pid = data;

	ret = read(fd, &siginfo, sizeof(siginfo));
	if (ret < 0) {
		ERROR("failed to read signal info");
		return -1;
	}

	if (ret != sizeof(siginfo)) {
		ERROR("unexpected siginfo size");
		return -1;
	}

	if (siginfo.ssi_signo != SIGCHLD) {
		kill(*pid, siginfo.ssi_signo);
		INFO("forwarded signal %d to pid %d", siginfo.ssi_signo, *pid);
		return 0;
	}

	if (siginfo.ssi_code == CLD_STOPPED ||
	    siginfo.ssi_code == CLD_CONTINUED) {
		INFO("container init process was stopped/continued");
		return 0;
	}

	/* more robustness, protect ourself from a SIGCHLD sent
	 * by a process different from the container init
	 */
	if (siginfo.ssi_pid != *pid) {
		WARN("invalid pid for SIGCHLD");
		return 0;
	}

	DEBUG("container init process exited");
	return 1;
}

int lxc_pid_callback(int fd, struct lxc_request *request,
		     struct lxc_handler *handler)
{
	struct lxc_answer answer;
	int ret;

	answer.pid = handler->pid;
	answer.ret = 0;

	ret = send(fd, &answer, sizeof(answer), 0);
	if (ret < 0) {
		WARN("failed to send answer to the peer");
		return -1;
	}

	if (ret != sizeof(answer)) {
		ERROR("partial answer sent");
		return -1;
	}

	return 0;
}

int lxc_clone_flags_callback(int fd, struct lxc_request *request,
			     struct lxc_handler *handler)
{
	struct lxc_answer answer;
	int ret;

	answer.pid = 0;
	answer.ret = handler->clone_flags;

	ret = send(fd, &answer, sizeof(answer), 0);
	if (ret < 0) {
		WARN("failed to send answer to the peer");
		return -1;
	}

	if (ret != sizeof(answer)) {
		ERROR("partial answer sent");
		return -1;
	}

	return 0;
}

int lxc_set_state(const char *name, struct lxc_handler *handler, lxc_state_t state)
{
	handler->state = state;
	lxc_monitor_send_state(name, state);
	return 0;
}

int lxc_poll(const char *name, struct lxc_handler *handler)
{
	int sigfd = handler->sigfd;
	int pid = handler->pid;
	struct lxc_epoll_descr descr;

	if (lxc_mainloop_open(&descr)) {
		ERROR("failed to create mainloop");
		goto out_sigfd;
	}

	if (lxc_mainloop_add_handler(&descr, sigfd, signal_handler, &pid)) {
		ERROR("failed to add handler for the signal");
		goto out_mainloop_open;
	}

	if (lxc_console_mainloop_add(&descr, handler)) {
		ERROR("failed to add console handler to mainloop");
		goto out_mainloop_open;
	}

	if (lxc_command_mainloop_add(name, &descr, handler)) {
		ERROR("failed to add command handler to mainloop");
		goto out_mainloop_open;
	}

	if (handler->conf->need_utmp_watch) {
		if (lxc_utmp_mainloop_add(&descr, handler)) {
			ERROR("failed to add utmp handler to mainloop");
			goto out_mainloop_open;
		}
	}

	return lxc_mainloop(&descr);

out_mainloop_open:
	lxc_mainloop_close(&descr);
out_sigfd:
	close(sigfd);
	return -1;
}

extern int lxc_caps_check(void);

struct lxc_handler *lxc_init(const char *name, struct lxc_conf *conf)
{
	struct lxc_handler *handler;

	if (!lxc_caps_check()) {
		ERROR("Not running with sufficient privilege");
		return NULL;
	}

	handler = malloc(sizeof(*handler));
	if (!handler)
		return NULL;

	memset(handler, 0, sizeof(*handler));

	handler->conf = conf;

	apparmor_handler_init(handler);
	handler->name = strdup(name);
	if (!handler->name) {
		ERROR("failed to allocate memory");
		goto out_free;
	}

	if (lxc_read_seccomp_config(conf) != 0) {
		ERROR("failed loading seccomp policy");
		goto out_free_name;
	}

	/* Begin the set the state to STARTING*/
	if (lxc_set_state(name, handler, STARTING)) {
		ERROR("failed to set state '%s'", lxc_state2str(STARTING));
		goto out_free_name;
	}

	if (run_lxc_hooks(name, "pre-start", conf)) {
		ERROR("failed to run pre-start hooks for container '%s'.", name);
		goto out_aborting;
	}

	if (lxc_create_tty(name, conf)) {
		ERROR("failed to create the ttys");
		goto out_aborting;
	}

	if (lxc_create_console(conf)) {
		ERROR("failed to create console");
		goto out_delete_tty;
	}

	/* the signal fd has to be created before forking otherwise
	 * if the child process exits before we setup the signal fd,
	 * the event will be lost and the command will be stuck */
	handler->sigfd = setup_signal_fd(&handler->oldmask);
	if (handler->sigfd < 0) {
		ERROR("failed to set sigchild fd handler");
		goto out_delete_console;
	}

	INFO("'%s' is initialized", name);
	return handler;

out_delete_console:
	lxc_delete_console(&conf->console);
out_delete_tty:
	lxc_delete_tty(&conf->tty_info);
out_aborting:
	lxc_set_state(name, handler, ABORTING);
out_free_name:
	free(handler->name);
	handler->name = NULL;
out_free:
	free(handler);
	return NULL;
}

void lxc_fini(const char *name, struct lxc_handler *handler)
{
	/* The STOPPING state is there for future cleanup code
	 * which can take awhile
	 */
	lxc_set_state(name, handler, STOPPING);
	lxc_set_state(name, handler, STOPPED);

	if (run_lxc_hooks(name, "post-stop", handler->conf))
		ERROR("failed to run post-stop hooks for container '%s'.", name);

	/* reset mask set by setup_signal_fd */
	if (sigprocmask(SIG_SETMASK, &handler->oldmask, NULL))
		WARN("failed to restore sigprocmask");

	lxc_delete_console(&handler->conf->console);
	lxc_delete_tty(&handler->conf->tty_info);
	free(handler->name);
	free(handler);
}

void lxc_abort(const char *name, struct lxc_handler *handler)
{
	lxc_set_state(name, handler, ABORTING);
	if (handler->pid > 0)
		kill(handler->pid, SIGKILL);
}

#include <sys/reboot.h>
#include <linux/reboot.h>

/*
 * reboot(LINUX_REBOOT_CMD_CAD_ON) will return -EINVAL
 * in a child pid namespace if container reboot support exists.
 * Otherwise, it will either succeed or return -EPERM.
 */
static int container_reboot_supported(void *arg)
{
        int *cmd = arg;
	int ret;

        ret = reboot(*cmd);
	if (ret == -1 && errno == EINVAL)
		return 1;
	return 0;
}

static int must_drop_cap_sys_boot(void)
{
	FILE *f = fopen("/proc/sys/kernel/ctrl-alt-del", "r");
	int ret, cmd, v;
        long stack_size = 4096;
        void *stack = alloca(stack_size) + stack_size;
        int status;
        pid_t pid;

	if (!f) {
		DEBUG("failed to open /proc/sys/kernel/ctrl-alt-del");
		return 1;
	}

	ret = fscanf(f, "%d", &v);
	fclose(f);
	if (ret != 1) {
		DEBUG("Failed to read /proc/sys/kernel/ctrl-alt-del");
		return 1;
	}
	cmd = v ? LINUX_REBOOT_CMD_CAD_ON : LINUX_REBOOT_CMD_CAD_OFF;

        pid = clone(container_reboot_supported, stack, CLONE_NEWPID | SIGCHLD, &cmd);
        if (pid < 0) {
                SYSERROR("failed to clone\n");
                return -1;
        }
        if (wait(&status) < 0) {
                SYSERROR("unexpected wait error: %m\n");
                return -1;
        }

	if (WEXITSTATUS(status) != 1)
		return 1;

	return 0;
}

static int do_start(void *data)
{
	struct lxc_handler *handler = data;

	if (sigprocmask(SIG_SETMASK, &handler->oldmask, NULL)) {
		SYSERROR("failed to set sigprocmask");
		return -1;
	}

        /* This prctl must be before the synchro, so if the parent
	 * dies before we set the parent death signal, we will detect
	 * its death with the synchro right after, otherwise we have
	 * a window where the parent can exit before we set the pdeath
	 * signal leading to a unsupervized container.
	 */
	if (prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0)) {
		SYSERROR("failed to set pdeath signal");
		return -1;
	}

	lxc_sync_fini_parent(handler);

	/* Tell the parent task it can begin to configure the
	 * container and wait for it to finish
	 */
	if (lxc_sync_barrier_parent(handler, LXC_SYNC_CONFIGURE))
		return -1;

	if (handler->conf->need_utmp_watch) {
		if (prctl(PR_CAPBSET_DROP, CAP_SYS_BOOT, 0, 0, 0)) {
			SYSERROR("failed to remove CAP_SYS_BOOT capability");
			return -1;
		}
		DEBUG("Dropped cap_sys_boot\n");
	}

	/* Setup the container, ip, names, utsname, ... */
	if (lxc_setup(handler->name, handler->conf)) {
		ERROR("failed to setup the container");
		goto out_warn_father;
	}

	if (apparmor_load(handler) < 0)
		goto out_warn_father;

	if (lxc_seccomp_load(handler->conf) != 0)
		goto out_warn_father;

	if (run_lxc_hooks(handler->name, "start", handler->conf)) {
		ERROR("failed to run start hooks for container '%s'.", handler->name);
		goto out_warn_father;
	}

	close(handler->sigfd);

	/* after this call, we are in error because this
	 * ops should not return as it execs */
	if (handler->ops->start(handler, handler->data))
		return -1;

out_warn_father:
	lxc_sync_wake_parent(handler, LXC_SYNC_POST_CONFIGURE);
	return -1;
}

int lxc_spawn(struct lxc_handler *handler)
{
	int failed_before_rename = 0;
	const char *name = handler->name;
	int pinfd;

	if (lxc_sync_init(handler))
		return -1;

	handler->clone_flags = CLONE_NEWUTS|CLONE_NEWPID|CLONE_NEWIPC|CLONE_NEWNS;
	if (!lxc_list_empty(&handler->conf->network)) {

		handler->clone_flags |= CLONE_NEWNET;

		/* Find gateway addresses from the link device, which is
		 * no longer accessible inside the container. Do this
		 * before creating network interfaces, since goto
		 * out_delete_net does not work before lxc_clone. */
		if (lxc_find_gateway_addresses(handler)) {
			ERROR("failed to find gateway addresses");
			lxc_sync_fini(handler);
			return -1;
		}

		/* that should be done before the clone because we will
		 * fill the netdev index and use them in the child
		 */
		if (lxc_create_network(handler)) {
			ERROR("failed to create the network");
			lxc_sync_fini(handler);
			return -1;
		}
	}

	/*
	 * if the rootfs is not a blockdev, prevent the container from
	 * marking it readonly.
	 */

	pinfd = pin_rootfs(handler->conf->rootfs.path);
	if (pinfd == -1) {
		ERROR("failed to pin the container's rootfs");
		goto out_abort;
	}

	/* Create a process in a new set of namespaces */
	handler->pid = lxc_clone(do_start, handler, handler->clone_flags);
	if (handler->pid < 0) {
		SYSERROR("failed to fork into a new namespace");
		goto out_delete_net;
	}

	lxc_sync_fini_child(handler);

	if (lxc_sync_wait_child(handler, LXC_SYNC_CONFIGURE))
		failed_before_rename = 1;

	if (lxc_cgroup_create(name, handler->pid))
		goto out_delete_net;

	if (failed_before_rename)
		goto out_delete_net;

	/* Create the network configuration */
	if (handler->clone_flags & CLONE_NEWNET) {
		if (lxc_assign_network(&handler->conf->network, handler->pid)) {
			ERROR("failed to create the configured network");
			goto out_delete_net;
		}
	}

	/* Tell the child to continue its initialization and wait for
	 * it to exec or return an error
	 */
	if (lxc_sync_barrier_child(handler, LXC_SYNC_POST_CONFIGURE))
		return -1;

	if (handler->ops->post_start(handler, handler->data))
		goto out_abort;

	if (lxc_set_state(name, handler, RUNNING)) {
		ERROR("failed to set state to %s",
			      lxc_state2str(RUNNING));
		goto out_abort;
	}

	lxc_sync_fini(handler);

	if (pinfd >= 0)
		close(pinfd);

	return 0;

out_delete_net:
	if (handler->clone_flags & CLONE_NEWNET)
		lxc_delete_network(handler);
out_abort:
	lxc_abort(name, handler);
	lxc_sync_fini(handler);
	return -1;
}

int __lxc_start(const char *name, struct lxc_conf *conf,
		struct lxc_operations* ops, void *data)
{
	struct lxc_handler *handler;
	int err = -1;
	int status;

	handler = lxc_init(name, conf);
	if (!handler) {
		ERROR("failed to initialize the container");
		return -1;
	}
	handler->ops = ops;
	handler->data = data;

	if (must_drop_cap_sys_boot()) {
		DEBUG("Dropping cap_sys_boot\n");
	} else {
		DEBUG("Not dropping cap_sys_boot or watching utmp\n");
		handler->conf->need_utmp_watch = 0;
	}

	err = lxc_spawn(handler);
	if (err) {
		ERROR("failed to spawn '%s'", name);
		goto out_fini_nonet;
	}

	err = lxc_poll(name, handler);
	if (err) {
		ERROR("mainloop exited with an error");
		goto out_abort;
	}

	while (waitpid(handler->pid, &status, 0) < 0 && errno == EINTR)
		continue;

	/*
	 * If the child process exited but was not signaled,
	 * it didn't call reboot.  This should mean it was an
	 * lxc-execute which simply exited.  In any case, treat
	 * it as a 'halt'
	 */
        if (WIFSIGNALED(status)) {
		switch(WTERMSIG(status)) {
		case SIGINT: /* halt */
			DEBUG("Container halting");
			break;
		case SIGHUP: /* reboot */
			DEBUG("Container rebooting");
			handler->conf->reboot = 1;
			break;
		default:
			DEBUG("unknown exit status for init: %d\n", WTERMSIG(status));
			break;
		}
        }

	err =  lxc_error_set_and_log(handler->pid, status);
out_fini:
	lxc_delete_network(handler);

out_fini_nonet:
	lxc_cgroup_destroy(name);
	lxc_fini(name, handler);
	return err;

out_abort:
	lxc_abort(name, handler);
	goto out_fini;
}

struct start_args {
	char *const *argv;
};

static int start(struct lxc_handler *handler, void* data)
{
	struct start_args *arg = data;

	NOTICE("exec'ing '%s'", arg->argv[0]);

	execvp(arg->argv[0], arg->argv);
	SYSERROR("failed to exec %s", arg->argv[0]);
	return 0;
}

static int post_start(struct lxc_handler *handler, void* data)
{
	struct start_args *arg = data;

	NOTICE("'%s' started with pid '%d'", arg->argv[0], handler->pid);
	return 0;
}

static struct lxc_operations start_ops = {
	.start = start,
	.post_start = post_start
};

int lxc_start(const char *name, char *const argv[], struct lxc_conf *conf)
{
	struct start_args start_arg = {
		.argv = argv,
	};

	if (lxc_check_inherited(conf, -1))
		return -1;

	conf->need_utmp_watch = 1;
	return __lxc_start(name, conf, &start_ops, &start_arg);
}
