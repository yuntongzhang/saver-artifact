/* liblxcapi
 *
 * Copyright ? 2013 Serge Hallyn <serge.hallyn@ubuntu.com>.
 * Copyright ? 2013 Canonical Ltd.
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

#include <errno.h>
#include <stdlib.h>
#include "../lxc/lxc.h"

#define MYNAME "snapxxx1"
#define RESTNAME "snapxxx2"

void try_to_remove()
{
	struct lxc_container *c;
	char snappath[1024];
	c = lxc_container_new(RESTNAME, NULL);
	if (c) {
		if (c->is_defined(c))
			c->destroy(c);
		lxc_container_put(c);
	}
	snprintf(snappath, 1024, "%ssnaps/%s", lxc_get_default_config_path(), MYNAME);
	c = lxc_container_new("snap0", snappath);
	if (c) {
		if (c->is_defined(c))
			c->destroy(c);
		lxc_container_put(c);
	}
	c = lxc_container_new(MYNAME, NULL);
	if (c) {
		if (c->is_defined(c))
			c->destroy(c);
		lxc_container_put(c);
	}
}

int main(int argc, char *argv[])
{
	struct lxc_container *c;
	char *template = "busybox";

	if (argc > 1)
		template = argv[1];

	try_to_remove();
	c = lxc_container_new(MYNAME, NULL);
	if (!c) {
		fprintf(stderr, "%s: %d: failed to load first container\n", __FILE__, __LINE__);
		exit(1);
	}

	if (c->is_defined(c)) {
		fprintf(stderr, "%d: %s thought it was defined\n", __LINE__, MYNAME);
		(void) c->destroy(c);
	}
	if (!c->set_config_item(c, "lxc.network.type", "empty")) {
		fprintf(stderr, "%s: %d: failed to set network type\n", __FILE__, __LINE__);
		goto err;
	}
	c->save_config(c, NULL);
	if (!c->createl(c, template, NULL, NULL, 0, NULL)) {
		fprintf(stderr, "%s: %d: failed to create %s container\n", __FILE__, __LINE__, template);
		goto err;
	}
	c->load_config(c, NULL);

	if (c->snapshot(c, NULL) != 0) {
		fprintf(stderr, "%s: %d: failed to create snapsot\n", __FILE__, __LINE__);
		goto err;
	}

	// rootfs should be ${lxcpath}snaps/${lxcname}/snap0/rootfs
	struct stat sb;
	int ret;
	char path[1024];
	snprintf(path, 1024, "%ssnaps/%s/snap0/rootfs", lxc_get_default_config_path(), MYNAME);
	ret = stat(path, &sb);
	if (ret != 0) {
		fprintf(stderr, "%s: %d: snapshot was not actually created\n", __FILE__, __LINE__);
		goto err;
	}

	struct lxc_snapshot *s;
	int i, n;

	n = c->snapshot_list(c, &s);
	if (n < 1) {
		fprintf(stderr, "%s: %d: failed listing containers\n", __FILE__, __LINE__);
		goto err;
	}
	if (strcmp(s->name, "snap0") != 0) {
		fprintf(stderr, "%s: %d: snapshot had bad name\n", __FILE__, __LINE__);
		goto err;
	}
	for (i=0; i<n; i++) {
		s[i].free(&s[i]);
	}
	free(s);

	if (!c->snapshot_restore(c, "snap0", RESTNAME)) {
		fprintf(stderr, "%s: %d: failed to restore snapshot\n", __FILE__, __LINE__);
		goto err;
	}

	if (!c->snapshot_destroy(c, "snap0")) {
		fprintf(stderr, "%s: %d: failed to destroy snapshot\n", __FILE__, __LINE__);
		goto err;
	}

	if (!c->destroy(c)) {
		fprintf(stderr, "%s: %d: failed to destroy container\n", __FILE__, __LINE__);
		goto err;
	}

	lxc_container_put(c);
	try_to_remove();

	printf("All tests passed\n");
	exit(0);
err:
	lxc_container_put(c);
	try_to_remove();

	fprintf(stderr, "Exiting on error\n");
	exit(1);
}
