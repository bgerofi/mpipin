/*
 * mpipin: an MPI implementation agnostic process pinning tool.
 *
 * Balazs Gerofi <bgerofi@riken.jp>, 2018/07/11
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
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
		.name =		"threads_per_process",
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
		.name =		"processes_per_node",
		.has_arg =	required_argument,
		.flag =		NULL,
		.val =		'p',
	},
	/* end */
	{ NULL, 0, NULL, 0, },
};

void print_usage(char **argv)
{
	printf("usage: %s \n", argv[0]);
}

int main(int argc, char **argv)
{
	int ret = 0;
	int ppn = 0;
	int tpp = 0;
	int opt;

	while ((opt = getopt_long(argc, argv, "p:t:", options, NULL)) != -1) {
		char *tmp;

		switch (opt) {
			case 'p':
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

	if (optind >= argc) {
		print_usage(argv);
		exit(EXIT_FAILURE);
	}

	printf("ppn: %d, tpp: %d\n", ppn, tpp);


	return 0;
}
