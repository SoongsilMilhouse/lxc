/*
 *
 * Copyright © 2013 Serge Hallyn <serge.hallyn@ubuntu.com>.
 * Copyright © 2013 Canonical Ltd.
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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <dirent.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <lxc/lxccontainer.h>

#include "arguments.h"
#include "config.h"
#include "log.h"
#include "utils.h"

#define GROUPPATH "/usr/local/var/lib/lxcgroup"

lxc_log_define(lxc_group, lxc);

/* Store container info. */
struct ls {
	char *groupname;
	char *containers;
};

/* Keep track of field widths for printing. */
struct lengths {
	unsigned int groupname_length;
	unsigned int containers_length;
};

static int  do_create_group_dir(const char *path);
static bool create_group_dir(char *groupname);
static int  force_destroy_group_dir(const char *groupname);
static bool destroy_group_dir(char *groupname);
static bool create_group_symlink(struct lxc_container *c, const char *groupname, const char *containername);
static bool delete_group_symlink(struct lxc_container *c, const char *groupname, const char *containername);

/*
 * Only print names of monitoring groups.
 */
static void ls_print_names(struct ls *l, struct lengths *lht,
		size_t ls_arr, size_t termwidth, bool list);

static int my_parser(struct lxc_arguments *args, int c, char *arg);

static const struct option my_longopts[] = {
   	{"force", no_argument, 0, 'f'},
	{"list", no_argument, 0, 'l'},
    LXC_COMMON_OPTIONS
};

static struct lxc_arguments my_args = {
    .progname = "lxc-group",
    .help = "\
--create|delete [-f] [-g groupname] [add|del groupname]\n\
\n\
lxc-group manipulates groups\n\
\n\
Options :\n\
  --create=GROUPNAME          create group\n\
  --destroy=GROUPNAME          destroy group\n\
  --add=GROUPNAME             add container to group\n\
  --del=GROUPNAME             delete container from group\n\
  -f, --force                 destroy group and containers in group \n\
  -l, --list                  list all groups\n",
    .options = my_longopts,
    .parser = my_parser,
    .checker = NULL,
    .log_priority = "ERROR",
    .log_file = "none",
};

static int my_parser(struct lxc_arguments *args, int c, char *arg)
{
    switch (c)
    {
    case 'f':
        args->force = 1;
        break;
    case 'l':
        args->group_ls = true;
        break;
    }
    return 0;
}

static int _mkdir(const char *dir) {
        char tmp[256];
        char *p = NULL;
        size_t len;
        int ret = -1;

        snprintf(tmp, sizeof(tmp),"%s",dir);
        len = strlen(tmp);
        if(tmp[len - 1] == '/')
                tmp[len - 1] = 0;
        for(p = tmp + 1; *p; p++)
                if(*p == '/') {
                        *p = 0;
                        ret = mkdir(tmp, 0755);
                        *p = '/';
                }

        if (ret) {
           if (errno != EEXIST)
                return -1;
            
            ret = 0;
        }

        ret = mkdir(tmp, 0755);
        if (ret) {
            if (errno == EEXIST)
                ERROR("Group already exists");
                return -1;
        }

        return ret;
}

static int rmdirs(const char *path, int is_error_stop)
{
    DIR *  dir_ptr      = NULL;
    struct dirent *file = NULL;
    struct stat   buf;
    char   filename[1024];

    if((dir_ptr = opendir(path)) == NULL) {
		return unlink(path);
    }
    
    while((file = readdir(dir_ptr)) != NULL) {
        if(strcmp(file->d_name, ".") == 0 || strcmp(file->d_name, "..") == 0) {
             continue;
        }

        sprintf(filename, "%s/%s", path, file->d_name);

        if(lstat(filename, &buf) == -1) {
            continue;
        }

        if(S_ISDIR(buf.st_mode)) {
            if(rmdirs(filename, is_error_stop) == -1 && is_error_stop) {
                return -1;
            }
        } else if(S_ISREG(buf.st_mode) || S_ISLNK(buf.st_mode)) { 
            if(unlink(filename) == -1 && is_error_stop) {
                return -1;
            }
        }
    }

    closedir(dir_ptr);
    
    return rmdir(path);
}


static int list_contains_entry(char *str_ptr, struct lxc_list *p1)
{
    struct lxc_list *it1;

    /*
	 * If the entry is NULL or the empty string and the list
	 * is NULL, we have a match
	 */
    if (!p1 && (!str_ptr || !*str_ptr))
        return 1;

    if (!p1)
        return 0;

    lxc_list_for_each(it1, p1)
    {
        if (strncmp(it1->elem, str_ptr, strlen(it1->elem)) == 0)
            return 1;
    }

    return 0;
}

static int do_create_group_dir(const char *path)
{
    char *p = NULL;
    int lasterr;
    int ret = -1;

    mode_t mask = umask(0002);
    ret = _mkdir(path);
    lasterr = errno;
    umask(mask);
    errno = lasterr;
    if (ret) {
        if (errno != EEXIST)
            return -1;

        ret = 0;
    }
    
    return ret;    
}

static bool create_group_dir(char *groupname)
{
    int ret;
    size_t len;
    char *s;

    len = strlen(GROUPPATH) + strlen(groupname) + 2;
    s = malloc(len);

    if (!s)
        return false;

    ret = snprintf(s, len, "%s/%s", GROUPPATH, groupname);
    if (ret < 0 || (size_t)ret >= len) {
        free(s);
        return false;
    }

    ret = do_create_group_dir(s);
    free(s);

    return ret == 0;
}

static int force_destroy_group_dir(const char* groupname)
{
    int ret = -1;
    size_t len;
    char *path = NULL;

    len = strlen(groupname) + strlen(GROUPPATH) + 2;
    path = malloc(len);

    if (!path)
        return false;

    ret = snprintf(path, len, "%s/%s", GROUPPATH, groupname);
    if (ret < 0 || (size_t)ret >= len) {
        free(path);
        return false;
    }

    ret = rmdirs(path, 0);
    free(path);

    return ret == 0;
}

static int do_destroy_group_dir(const char *path)
{
    int ret = -1;

    if (rmdir(path) != 0) 
        ERROR("Failed to destroy \"%s\"", path);

    ret = 0;

    return ret;
}

static bool destroy_group_dir(char *groupname)
{
    int ret;
    size_t len;
    char *s;

    len = strlen(GROUPPATH) + strlen(groupname) + 2;
    s = malloc(len);

    if (!s)
        return false;

    ret = snprintf(s, len, "%s/%s", GROUPPATH, groupname);
    if (ret < 0 || (size_t)ret >= len) {
        free(s);
        return false;
    }

    ret = do_destroy_group_dir(s);
    free(s);

    return ret == 0;
}

static bool create_group_symlink(struct lxc_container *c, const char *groupname, const char *containername) 
{
    int ret = -1;
    size_t len, len2;
    char *lxcpath;
    char *oldpath = NULL;
    char *sympath = NULL;

    /* 해당 컨테이너 없을 시 false 반환 */
    if (!c->is_defined(c)) {
        ERROR("\"%s\" container does not exist" , containername);
        return false;
    }

    lxcpath = (char *)lxc_global_config_value("lxc.lxcpath");
    len = strlen(lxcpath) + strlen(containername) + 2;
    oldpath = malloc(len);

    if (!oldpath)
        return false;
    
    ret = snprintf(oldpath, len, "%s/%s", lxcpath, containername);
    if (ret < 0 || (size_t)ret >= len) {
        free(oldpath);
        return false;
    }

    len2 = strlen(groupname) + strlen(GROUPPATH) + strlen(containername) + 3;
    sympath = malloc(len2);

    if (!sympath)
        return false;

    ret = snprintf(sympath, len2, "%s/%s/%s", GROUPPATH, groupname, containername);
    if (ret < 0 || (size_t)ret >= len2) {
        free(oldpath);
        free(sympath);
        return false;
    }

    if (access(sympath, F_OK) == 0) {
        ERROR("Symbolic link already exists \"%s\"", sympath);
        return false;
    }

    ret = symlink(oldpath, sympath);
    free(oldpath);
    free(sympath);

    return ret == 0;
}
static bool delete_group_symlink(struct lxc_container *c, const char *groupname, const char *containername)
{
    int ret = -1;
    size_t len;
    char *sympath = NULL;

    /* 해당 컨테이너 없을 시 false 반환 */
    if (!c->is_defined(c)) {
        ERROR("\"%s\" container does not exist" , containername);
        return false;
    }

    len = strlen(groupname) + strlen(GROUPPATH) + strlen(containername) + 3;
    sympath = malloc(len);
    if (!sympath)
        return false;

    ret = snprintf(sympath, len, "%s/%s/%s", GROUPPATH, groupname, containername);
    if (ret < 0 || (size_t)ret >= len) {
        free(sympath);
        return false;
    }

    ret = remove(sympath);
    free(sympath);

    return ret == 0;
}

int main(int argc, char *argv[])
{
    struct lxc_container *c;
    struct lxc_log log;
    char *cmd, *groupname, *groupname2;
    bool ret;

    my_args.name = "";
    if (geteuid() != 0)
    {
        ERROR("%s must be run as root", argv[0]);
        exit(EXIT_FAILURE);
    }

    if (lxc_arguments_parse(&my_args, argc, argv))
		exit(EXIT_FAILURE);

    log.name = my_args.name;
    log.file = my_args.log_file;
    log.level = my_args.log_priority;
    log.prefix = my_args.progname;      // lxc-group(=progname) create -g group1
    log.quiet = my_args.quiet;
    log.lxcpath = my_args.lxcpath[0];

    if (lxc_log_init(&log))
        exit(EXIT_FAILURE);

    /* create or destroy 할 때는 컨테이너 생성 안함 */ 
    if (!my_args.group_create_or_destroy) {
        c = lxc_container_new(my_args.name, my_args.lxcpath[0]);

        if (!c) {
            ERROR("System error loading container");
            exit(EXIT_FAILURE);
        }
    }

    cmd = my_args.argv[0];                      // lxc-group create(=cmd) -g group1 

    if (strncmp(cmd, "create", strlen(cmd)) == 0) 
    {
        groupname = my_args.groupname;          // lxc-group create -g group1(=groupname)
        ret = create_group_dir(groupname);
        if (ret != true) {
            ERROR("Failed to create %s", groupname);
            goto err;
        }
    }
    else if (strncmp(cmd, "destroy", strlen(cmd)) == 0) 
    {
        groupname = my_args.groupname;
        if (my_args.force) {
            ret = force_destroy_group_dir(groupname);
            if (ret != true) {
              ERROR("Failed to destroy %s and contents in group", groupname);
              goto err;
            }
        } else {
            ret = destroy_group_dir(groupname);
            if (ret != true) {
                ERROR("Failed to destroy %s", groupname);
                goto err;
            }
        }
        
    }
    else if (strncmp(cmd, "add", strlen(cmd)) == 0)
    {
        groupname = my_args.argv[1];            // lxc-group -n c1 add group1(=groupname2)
        ret = create_group_symlink(c, groupname, my_args.name);
        if (ret != true) {
            ERROR("Failed to add %s to %s", my_args.name, groupname);
            goto err;
        }
    }
    else if (strncmp(cmd, "del", strlen(cmd)) == 0)
    {
        groupname = my_args.argv[1];
        ret = delete_group_symlink(c, groupname, my_args.name);
        if (ret != true) {
            ERROR("Failed to del %s from %s", my_args.name, groupname);
            goto err;
        }
    }
    else
    {
        ERROR("Error: Please use add or del (Please see --help output)");
        goto err;
    }

    exit(EXIT_SUCCESS);

err:
    exit(EXIT_FAILURE);
}
