/* liblxcapi
 *
 * Copyright © 2017 Christian Brauner <christian.brauner@ubuntu.com>.
 * Copyright © 2017 Canonical Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2, as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
#include <lxc/lxccontainer.h>

#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

#define MYNAME "shortlived"

static int destroy_container(void)
{
	int status, ret;
	pid_t pid = fork();

	if (pid < 0) {
		perror("fork");
		return -1;
	}
	if (pid == 0) {
		ret = execlp("lxc-destroy", "lxc-destroy", "-f", "-n", MYNAME, NULL);
		// Should not return
		perror("execl");
		exit(1);
	}
again:
	ret = waitpid(pid, &status, 0);
	if (ret == -1) {
		if (errno == EINTR)
			goto again;
		perror("waitpid");
		return -1;
	}
	if (ret != pid)
		goto again;
	if (!WIFEXITED(status))  { // did not exit normally
		fprintf(stderr, "%d: lxc-create exited abnormally\n", __LINE__);
		return -1;
	}
	return WEXITSTATUS(status);
}

static int create_container(void)
{
	int status, ret;
	pid_t pid = fork();

	if (pid < 0) {
		perror("fork");
		return -1;
	}
	if (pid == 0) {
		ret = execlp("lxc-create", "lxc-create", "-t", "busybox", "-n", MYNAME, NULL);
		// Should not return
		perror("execl");
		exit(1);
	}
again:
	ret = waitpid(pid, &status, 0);
	if (ret == -1) {
		if (errno == EINTR)
			goto again;
		perror("waitpid");
		return -1;
	}
	if (ret != pid)
		goto again;
	if (!WIFEXITED(status))  { // did not exit normally
		fprintf(stderr, "%d: lxc-create exited abnormally\n", __LINE__);
		return -1;
	}
	return WEXITSTATUS(status);
}

int main(int argc, char *argv[])
{
	struct lxc_container *c;
	const char *s;
	bool b;
	int i;
	int ret = 0;

	ret = 1;
	/* test a real container */
	c = lxc_container_new(MYNAME, NULL);
	if (!c) {
		fprintf(stderr, "%d: error creating lxc_container %s\n", __LINE__, MYNAME);
		ret = 1;
		goto out;
	}

	if (c->is_defined(c)) {
		fprintf(stderr, "%d: %s thought it was defined\n", __LINE__, MYNAME);
		goto out;
	}

	ret = create_container();
	if (ret) {
		fprintf(stderr, "%d: failed to create a container\n", __LINE__);
		goto out;
	}

	b = c->is_defined(c);
	if (!b) {
		fprintf(stderr, "%d: %s thought it was not defined\n", __LINE__, MYNAME);
		goto out;
	}

	s = c->state(c);
	if (!s || strcmp(s, "STOPPED")) {
		fprintf(stderr, "%d: %s is in state %s, not in STOPPED.\n", __LINE__, c->name, s ? s : "undefined");
		goto out;
	}

	b = c->load_config(c, NULL);
	if (!b) {
		fprintf(stderr, "%d: %s failed to read its config\n", __LINE__, c->name);
		goto out;
	}

	if (!c->set_config_item(c, "lxc.init.cmd", "echo hello")) {
		fprintf(stderr, "%d: failed setting lxc.init.cmd\n", __LINE__);
		goto out;
	}

	c->want_daemonize(c, true);

	/* Test whether we can start a really short-lived daemonized container.
	 */
	for (i = 0; i < 10; i++) {
		if (!c->startl(c, 0, NULL)) {
			fprintf(stderr, "%d: %s failed to start\n", __LINE__, c->name);
			goto out;
		}
		sleep(1);
	}

	if (!c->set_config_item(c, "lxc.init.cmd", "you-shall-fail")) {
		fprintf(stderr, "%d: failed setting lxc.init.cmd\n", __LINE__);
		goto out;
	}

	/* Test whether we catch the start failure of a really short-lived
	 * daemonized container.
	 */
	for (i = 0; i < 10; i++) {
		if (c->startl(c, 0, NULL)) {
			fprintf(stderr, "%d: %s failed to start\n", __LINE__, c->name);
			goto out;
		}
		sleep(1);
	}

	c->stop(c);

	fprintf(stderr, "all lxc_container tests passed for %s\n", c->name);
	ret = 0;

out:
	if (c) {
		c->stop(c);
		destroy_container();
	}
	lxc_container_put(c);
	exit(ret);
}
