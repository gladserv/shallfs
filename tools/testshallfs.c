/* run shallfs test suite
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * this file is part of SHALLFS
 *
 * Copyright (c) 2019 Claudio Calvelli <shallfs@gladserv.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program (see the file COPYING in the distribution).
 * If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <unistd.h>
#include <endian.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include "shallfs-common.h"

enum {
    err_ok               =  0,   /* no errors */
    err_syntax           =  1,   /* replace user */
    err_operation        =  2,   /* operation error */
    err_cancelled        =  4,   /* cancelled by user */
    err_failed           =  8,   /* some tests failed: see output file for details */
};

static long runs = 100, passes = 1, runtime = 0, do_help = 0;
static long runs_ok = 0, runs_failed = 0;
static const char * test_root = NULL, * output = NULL;
static FILE * OF = NULL;

static const shall_options_t options[] = {
    { 'h', &do_help,         NULL,
      "Print this helpful message" },
    { 'r', &runs,            "N-TESTS",
      "Run N-TESTS tests for each filesystem function (default: 100" },
    { 'p', &passes,          "N-PASSES",
      "Run N-PASSES complete testing cycles (default: 1)" },
    { 't', &runtime,         "SECONDS",
      "Stop after SECONDS seconds, even if the testing is not complete"
      " (default: 0, which disables it)" },
    {  0,  NULL,             NULL, NULL }
};

static const shall_args_t args[] = {
    { &test_root,  "TEST_ROOT",  1,
      "Directory to use for testing; must be on a mounted shallfs" },
    { &output,     "OUTPUT",     0,
      "File to record test result; if omitted, just return status code" },
    { NULL,        NULL,         0, NULL }
};

static const char * parse_options(int argc, char *argv[]) {
    const char * err = shall_parse_options(argc, argv, options, args);
    if (err) return err;
    if (do_help) return NULL;
    if (runs < 1) return "-r requires an argument > 0";
    if (passes < 1) return "-p requires an argument > 0";
    if (runtime < 0) return "-t requires an argument >= 0";
    return NULL;
}

static struct {
    const char * name;
    const char * (*code)(void);
} functions[] = {
    // XXX
    { NULL, NULL }
};

int main(int argc, char *argv[]) {
    struct stat sbuff;
    const char * pname = strrchr(argv[0], '/');
    const char * errmsg = parse_options(argc - 1, argv + 1);
    time_t endtime = 0;
    int pass;
    if (pname)
	pname++;
    else
	pname = argv[0];
    if (do_help) {
	shall_print_help(stdout, pname, options, args);
	return err_ok;
    }
    if (errmsg) {
	fprintf(stderr, "%s: %s\nUse \"%s -h\" for help\n",
		pname, errmsg, pname);
	return err_syntax;
    }
    if (stat(test_root, &sbuff) < 0) {
	perror(test_root);
	return err_operation;
    }
    if (! S_ISDIR(sbuff.st_mode)) {
	fprintf(stderr, "%s: %s is not a directory\n", pname, test_root);
	return err_syntax;
    }
    if (! shall_find_device(test_root, &sbuff.st_rdev)) {
	fprintf(stderr, "%s: cannot find shallfs on %s\n",
		pname, test_root);
	return err_syntax;
    }
    if (output) {
	OF = fopen(output, "w");
	if (! OF) {
	    perror(output);
	    return err_operation;
	}
    } else {
	OF = NULL;
    }
    if (runtime > 0)
	endtime = time(NULL) + runtime;
    for (pass = 0; pass < passes; pass++) {
	int func;
	if (OF)
	    fprintf(OF, "Pass: %d\n", pass + 1);
	for (func = 0; functions[func].name; func++) {
	    int run;
	    if (OF)
		fprintf(OF, "Running: %s\n", functions[func].name);
	    for (run = 0; run  < runs; run++) {
		errmsg = functions[func].code();
		if (errmsg) {
		    runs_failed ++;
		    if (OF) fprintf(OF, "%d: ERROR %s\n", run + 1, errmsg);
		} else {
		    runs_ok ++;
		    if (OF) fprintf(OF, "%d: OK\n", run + 1);
		}
		if (runtime > 0 && endtime < time(NULL)) goto out;
	    }
	}
    }
out:
    if (OF) {
	fprintf(OF, "Result: %ld OK, %ld FAILED\n", runs_ok, runs_failed);
	fclose(OF);
    }
    return runs_failed ? err_failed : err_ok;
}

