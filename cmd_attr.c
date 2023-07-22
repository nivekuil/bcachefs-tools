#include <dirent.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>

#include "libbcachefs/bcachefs_ioctl.h"

#include "cmds.h"
#include "libbcachefs.h"

static void do_setattr(char *path, struct bch_opt_strs opts)
{
	unsigned i;

	for (i = 0; i < bch2_opts_nr; i++) {
		if (!opts.by_id[i])
			continue;

		char *n = mprintf("bcachefs.%s", bch2_opt_table[i].attr.name);

		if (setxattr(path, n, opts.by_id[i], strlen(opts.by_id[i]), 0))
			die("setxattr error: %m");

		free(n);
	}

	struct stat st = xstat(path);
	if (!S_ISDIR(st.st_mode))
		return;

	int dirfd = open(path, O_RDONLY);
	if (dirfd < 0)
		die("error opening %s: %m", path);

	propagate_recurse(dirfd);
	close(dirfd);
}

static void setattr_usage(void)
{
	puts("bcachefs setattr - set attributes on files in a bcachefs filesystem\n"
	     "Usage: bcachefs setattr [OPTIONS]... <files>\n"
	     "\n"
	     "Options:");

	bch2_opts_usage(OPT_INODE);
	puts("  -h            Display this help and exit\n"
	     "Report bugs to <linux-bcachefs@vger.kernel.org>");
}

int cmd_setattr(int argc, char *argv[])
{
	struct bch_opt_strs opts =
		bch2_cmdline_opts_get(&argc, argv, OPT_INODE);
	unsigned i;

	for (i = 1; i < argc; i++)
		if (argv[i][0] == '-') {
			printf("invalid option %s\n", argv[i]);
			setattr_usage();
			exit(EXIT_FAILURE);
		}

	if (argc <= 1)
		die("Please supply one or more files");

	for (i = 1; i < argc; i++)
		do_setattr(argv[i], opts);
	bch2_opt_strs_free(&opts);

	return 0;
}
