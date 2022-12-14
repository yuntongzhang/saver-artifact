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

#ifndef _confile_h
#define _confile_h

struct lxc_conf;
struct lxc_list;

typedef int (*config_cb)(const char *, char *, struct lxc_conf *);
struct lxc_config_t {
	char *name;
	config_cb cb;
};

extern struct lxc_config_t *lxc_getconfig(const char *key);
extern int lxc_list_nicconfigs(struct lxc_conf *c, char *key, char *retv, int inlen);
extern int lxc_listconfigs(char *retv, int inlen);
extern int lxc_config_read(const char *file, struct lxc_conf *conf);
extern int lxc_config_readline(char *buffer, struct lxc_conf *conf);

extern int lxc_config_define_add(struct lxc_list *defines, char* arg);
extern int lxc_config_define_load(struct lxc_list *defines,
				  struct lxc_conf *conf);

/* needed for lxc-attach */
extern signed long lxc_config_parse_arch(const char *arch);

extern int lxc_get_config_item(struct lxc_conf *c, char *key, char *retv, int inlen);
extern int lxc_clear_config_item(struct lxc_conf *c, char *key);
extern void write_config(FILE *fout, struct lxc_conf *c);
#endif
