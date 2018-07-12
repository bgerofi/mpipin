/*
 * mpipin: an MPI implementation agnostic process pinning tool.
 *
 * Balazs Gerofi <bgerofi@riken.jp>, 2018/07/11
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <ctype.h>
#include <sys/mman.h>
#include <asm/unistd.h>
#include <sched.h>
#include <dirent.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <sys/fsuid.h>
#include <time.h>
#include <sys/time.h>
#include <signal.h>
#include <sys/wait.h>
#include <dirent.h>
#include <sys/syscall.h>
#include <sys/ptrace.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <getopt.h>
#include <numa.h>
#include <numaif.h>

#include <bitmap.h>

int compact = 1;
struct option options[] = {
	{
		.name =		"compact",
		.has_arg =	no_argument,
		.flag =		&compact,
		.val =		1,
	},
	{
		.name =		"scatter",
		.has_arg =	no_argument,
		.flag =		&compact,
		.val =		0,
	},
	{
		.name =		"tpp",
		.has_arg =	required_argument,
		.flag =		NULL,
		.val =		't',
	},
	{
		.name =		"threads-per-process",
		.has_arg =	required_argument,
		.flag =		NULL,
		.val =		't',
	},
	{
		.name =		"ppn",
		.has_arg =	required_argument,
		.flag =		NULL,
		.val =		'p',
	},
	{
		.name =		"processes-per-node",
		.has_arg =	required_argument,
		.flag =		NULL,
		.val =		'p',
	},
	{
		.name =		"ranks-per-node",
		.has_arg =	required_argument,
		.flag =		NULL,
		.val =		'p',
	},
	/* end */
	{ NULL, 0, NULL, 0, },
};


void print_usage(char **argv)
{
	printf("usage: %s <options> prog [args]\n", argv[0]);
}

/*
 * Topology information
 */

#define PAGE_SIZE	(4096)

int read_file(void *buf, size_t size, char *fmt, va_list ap)
{
	int n, ss;
	int error = -1;
	int fd = -1;
	char *filename = NULL;

	filename = malloc(PATH_MAX);
	if (!filename) {
		error = -ENOMEM;
		fprintf(stderr, "%s: error: allocating memory\n", __FUNCTION__);
		goto out;
	}

	n = vsnprintf(filename, PATH_MAX, fmt, ap);
	if (n >= PATH_MAX) {
		error = -ENAMETOOLONG;
		goto out;
	}

	fd = open(filename, 0, O_RDONLY);
	if (fd < 0) {
		error = -EINVAL;
		fprintf(stderr, "%s: error: opening file %s\n", __FUNCTION__, filename);
		goto out;
	}

	ss = read(fd, buf, size);
	if (ss < 0) {
		error = -EINVAL;
		fprintf(stderr, "%s: error: reading file %s\n", __FUNCTION__, filename);
		goto out;
	}

	*(char *)(buf + ss) = '\0';

	error = 0;
out:

	if (fd > 0)
		close(fd);

	if (filename)
		free(filename);

	return error;
}

#if 0
int file_readable(char *fmt, ...)
{
	int ret;
	va_list ap;
	int n;
	char *filename = NULL;
	struct file *fp = NULL;

	filename = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!filename) {
		eprintk("%s: kmalloc failed. %d\n",
				__FUNCTION__, -ENOMEM);
		ret = 0;
		goto out;
	}

	va_start(ap, fmt);
	n = vsnprintf(filename, PATH_MAX, fmt, ap);
	va_end(ap);

	if (n >= PATH_MAX) {
		eprintk("%s: vsnprintf failed. %d\n",
				__FUNCTION__, -ENAMETOOLONG);
		ret = 0;
		goto out;
	}

	fp = filp_open(filename, O_RDONLY, 0);
	if (IS_ERR(fp)) {
		ret = 0;
		goto out;
	}

	filp_close(fp, NULL);
	ret = 1;

out:
	kfree(filename);
	return ret;
}
#endif

int read_long(long *valuep, char *fmt, ...)
{
	int error;
	char *buf = NULL;
	va_list ap;
	int n;

	buf = malloc(PAGE_SIZE);
	if (!buf) {
		fprintf(stderr, "%s: error: allocating memory\n", __FUNCTION__);
		error = -ENOMEM;
		goto out;
	}

	va_start(ap, fmt);
	error = read_file(buf, PAGE_SIZE - 1, fmt, ap);
	va_end(ap);
	if (error) {
		fprintf(stderr, "%s: error: reading data\n", __FUNCTION__);
		goto out;
	}

	n = sscanf(buf, "%ld", valuep);
	if (n != 1) {
		error = -EIO;
		fprintf(stderr, "%s: error: interpreting long\n", __FUNCTION__);
		goto out;
	}

	error = 0;
out:
	free(buf);
	return error;
}

int read_bitmap(long *valuep, char *fmt, ...)
{
	int error;
	char *buf = NULL;
	va_list ap;
	int n;

	buf = malloc(PAGE_SIZE);
	if (!buf) {
		fprintf(stderr, "%s: error: allocating memory\n", __FUNCTION__);
		error = -ENOMEM;
		goto out;
	}

	va_start(ap, fmt);
	error = read_file(buf, PAGE_SIZE - 1, fmt, ap);
	va_end(ap);
	if (error) {
		fprintf(stderr, "%s: error: reading data\n", __FUNCTION__);
		goto out;
	}

	n = sscanf(buf, "%ld", valuep);
	if (n != 1) {
		error = -EIO;
		fprintf(stderr, "%s: error: interpreting long\n", __FUNCTION__);
		goto out;
	}

	error = 0;
out:
	free(buf);
	return error;
}

#if 0
int read_bitmap(void *map, int nbits, char *fmt, ...)
{
	int error;
	char *buf = NULL;
	va_list ap;

	dprintk("read_bitmap(%p,%d,%s)\n", map, nbits, fmt);
	buf = (void *)__get_free_pages(GFP_KERNEL, 0);
	if (!buf) {
		error = -ENOMEM;
		eprintk("ihk:read_bitmap:__get_free_pages failed. %d\n", error);
		goto out;
	}

	va_start(ap, fmt);
	error = read_file(buf, PAGE_SIZE, fmt, ap);
	va_end(ap);
	if (error) {
		eprintk("ihk:read_bitmap:read_file failed. %d\n", error);
		goto out;
	}

	error = bitmap_parse(buf, PAGE_SIZE, map, nbits);
	if (error) {
		eprintk("ihk:read_bitmap:bitmap_parse failed. %d\n", error);
		goto out;
	}

	error = 0;
out:
	free_pages((long)buf, 0);
	dprintk("read_bitmap(%p,%d,%s): %d\n", map, nbits, fmt, error);
	return error;
} /* read_bitmap() */

int read_string(char **valuep, char *fmt, ...)
{
	int error;
	char *buf = NULL;
	va_list ap;
	char *p = NULL;
	int len;

	dprintk("read_string(%p,%s)\n", valuep, fmt);
	buf = (void *)__get_free_pages(GFP_KERNEL, 0);
	if (!buf) {
		error = -ENOMEM;
		eprintk("ihk:read_string:"
				"__get_free_pages failed. %d\n", error);
		goto out;
	}

	va_start(ap, fmt);
	error = read_file(buf, PAGE_SIZE, fmt, ap);
	va_end(ap);
	if (error) {
		eprintk("ihk:read_string:read_file failed. %d\n", error);
		goto out;
	}

	p = kstrdup(buf, GFP_KERNEL);
	if (!p) {
		error = -ENOMEM;
		eprintk("ihk:read_string:kstrdup failed. %d\n", error);
		goto out;
	}

	len = strlen(p);
	if (len && (p[len-1] == '\n')) {
		p[len-1] = '\0';
	}

	error = 0;
	*valuep = p;
	p = NULL;

out:
	kfree(p);
	free_pages((long)buf, 0);
	dprintk("read_string(%p,%s): %d\n", valuep, fmt, error);
	return error;
} /* read_string() */
#endif



int main(int argc, char **argv)
{
	int ppn = 0;
	int tpp = 0;
	pid_t ppid;
	int opt;

	/* Parse options */
	while ((opt = getopt_long(argc, argv, "n:p:t:", options, NULL)) != -1) {
		char *tmp;

		switch (opt) {
			case 'p':
			case 'n':
				ppn = strtol(optarg, &tmp, 0);
				if (*tmp != '\0' || ppn <= 0) {
					fprintf(stderr, "error: -p: invalid number of processes\n");
					exit(EXIT_FAILURE);
				}
				break;

			case 't':
				tpp = strtol(optarg, &tmp, 0);
				if (*tmp != '\0' || tpp <= 0) {
					fprintf(stderr, "error: -t: invalid number of threads\n");
					exit(EXIT_FAILURE);
				}
				break;

			default:
				print_usage(argv);
				exit(EXIT_FAILURE);
		}
	}

	/* Sanity checks.. */
	if (optind >= argc) {
		fprintf(stderr, "error: you must specify a program to execute\n");
		print_usage(argv);
		exit(EXIT_FAILURE);
	}

	if (ppn == 0) {
		fprintf(stderr, "error: you must specify the number of processes per node\n");
		print_usage(argv);
		exit(EXIT_FAILURE);	
	}

	ppid = getppid();

	printf("[ppid: %d] ppn: %d, tpp: %d\n", ppid, ppn, tpp);

	{
		long size;
		if (read_long(&size,
			"/sys/devices/system/cpu/cpu%d/cache/index0/coherency_line_size",
			0) < 0) {
			exit(EXIT_FAILURE);
		}

		printf("coherency_line_size: %lu\n", size);
	}


	exit(0);
}
