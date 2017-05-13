/* logs a "user log" on a mounted shall filesystem */

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdint.h>
#include "shallfs-common.h"

static long help = 0;
static const char * fspath = NULL, * message = NULL;

static const shall_options_t options[] = {
    { 'h', &help,            NULL,
      "Print this helpful message" },
    {  0,  NULL,             NULL, NULL }
};

static const shall_args_t args[] = {
    { &fspath,   "PATH",  1,
      "The mountpoint/fspath/device of the mounted shallfs to send a log to" },
    { &message,  "MESSAGE",  1,
      "The message to send as a user log" },
    { NULL,      NULL,           0, NULL }
};

int main(int argc, char *argv[]) {
    const char * pname = strrchr(argv[0], '/');
    const char * errmsg =
	shall_parse_options(argc - 1, argv + 1, options, args);
    struct stat sbuff;
    if (pname)
	pname++;
    else
	pname = argv[0];
    if (help) {
	shall_print_help(stdout, pname, options, args);
	return 0;
    }
    if (errmsg) {
	fprintf(stderr, "%s: %s\nUse \"%s -h\" for help\n",
		pname, errmsg, pname);
	return 1;
    }
    /* search for mounted device */
    if (stat(fspath, &sbuff) < 0) {
	perror(fspath);
	return 1;
    }
    if (S_ISDIR(sbuff.st_mode)) {
	if (! shall_find_device(fspath, &sbuff.st_rdev)) {
	    fprintf(stderr, "%s: cannot find shallfs on %s\n",
		    pname, fspath);
	    return 1;
	}
    } else if (! S_ISBLK(sbuff.st_mode)) {
	fprintf(stderr, "%s: %s: not a block device or directory\n",
		pname, fspath);
	return 1;
    }
    if (shall_ctrl_userlog(sbuff.st_rdev, message) < 0) {
	perror(fspath);
	return 1;
    }
    return 0;
}

