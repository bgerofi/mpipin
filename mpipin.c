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
#include <sys/sysinfo.h>
#include <sys/ptrace.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <getopt.h>
#include <numa.h>
#include <numaif.h>

#include <bitmap.h>
#include <list.h>

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

struct cache_topology {
	struct list_head chain;
	int index;
	int padding;
	long level;
	char *type;
	long size;
	char *size_str;
	long coherency_line_size;
	long number_of_sets;
	long physical_line_partition;
	long ways_of_associativity;
	cpu_set_t shared_cpu_map;
};

struct cpu_topology {
	struct list_head chain;
	int cpu_number;
	int hw_id;
	long physical_package_id;
	long core_id;
	cpu_set_t core_siblings;
	cpu_set_t thread_siblings;
	struct list_head cache_topology_list;
};

struct node_topology {
	struct list_head chain;
	int node_number;
	int padding;
	cpu_set_t cpumap;
};

LIST_HEAD(cpu_topology_list);
LIST_HEAD(node_topology_list);

#define PAGE_SIZE	(4096)

static int read_file(void *buf, size_t size, char *fmt, va_list ap)
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

static int file_readable(char *fmt, ...)
{
	int error;
	int fd;
	va_list ap;
	int n;
	char *filename = NULL;

	filename = malloc(PATH_MAX);
	if (!filename) {
		error = -ENOMEM;
		goto out;
	}

	va_start(ap, fmt);
	n = vsnprintf(filename, PATH_MAX, fmt, ap);
	va_end(ap);
	if (n >= PATH_MAX) {
		error = -ENAMETOOLONG;
		goto out;
	}

	fd = open(filename, 0, O_RDONLY);
	if (fd < 0) {
		error = -EINVAL;
		goto out;
	}

	close(fd);
	error = 0;
out:
	if (filename)
		free(filename);

	return error;
}

static int read_long(long *valuep, char *fmt, ...)
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
		error = -EINVAL;
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


static int read_bitmap(void *map, int nbits, char *fmt, ...)
{
	int error;
	char *buf = NULL;
	va_list ap;

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
		error = -EINVAL;
		fprintf(stderr, "%s: error: reading data\n", __FUNCTION__);
		goto out;
	}

	error = bitmap_parse(buf, PAGE_SIZE, map, nbits);
	if (error) {
		error = -EINVAL;
		fprintf(stderr, "%s: error: parsing bitmap\n", __FUNCTION__);
		goto out;
	}

	error = 0;
out:
	free(buf);
	return error;
}

static int read_bitmap_parselist(void *map, int nbits, char *fmt, ...)
{
	int error;
	char *buf = NULL;
	va_list ap;

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
		error = -EINVAL;
		fprintf(stderr, "%s: error: reading data\n", __FUNCTION__);
		goto out;
	}

	error = bitmap_parselist(buf, map, nbits);
	if (error) {
		error = -EINVAL;
		fprintf(stderr, "%s: error: parsing bitmap\n", __FUNCTION__);
		goto out;
	}

	error = 0;
out:
	free(buf);
	return error;
}

static int read_string(char **valuep, char *fmt, ...)
{
	int error;
	char *buf = NULL;
	va_list ap;
	char *p = NULL;
	int len;

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
		error = -EINVAL;
		fprintf(stderr, "%s: error: reading data\n", __FUNCTION__);
		goto out;
	}

	p = strdup(buf);
	if (!p) {
		error = -ENOMEM;
		fprintf(stderr, "%s: error: allocating memory\n", __FUNCTION__);
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
	free(buf);
	return error;
}

static int collect_cache_topology(struct cpu_topology *cpu_topo, int index)
{
	int error;
	char *prefix = NULL;
	int n;
	struct cache_topology *p = NULL;

	prefix = malloc(PATH_MAX);
	if (!prefix) {
		error = -ENOMEM;
		fprintf(stderr, "%s: error: allocating memory\n", __FUNCTION__);
		goto out;
	}

	n = snprintf(prefix, PATH_MAX,
			"/sys/devices/system/cpu/cpu%d/cache/index%d",
			cpu_topo->cpu_number, index);
	if (n >= PATH_MAX) {
		error = -ENAMETOOLONG;
		fprintf(stderr, "%s: error: name too long\n", __FUNCTION__);
		goto out;
	}

	if (file_readable("%s/level", prefix) < 0) {
		/* File doesn't exist, it's not an error */
		error = 0;
		goto out;
	}

	p = malloc(sizeof(*p));
	if (!p) {
		error = -ENOMEM;
		fprintf(stderr, "%s: error: allocating memory\n", __FUNCTION__);
		goto out;
	}
	memset(p, 0, sizeof(*p));

	p->index = index;

	error = read_long(&p->level, "%s/level", prefix);
	if (error) {
		fprintf(stderr, "%s: error: accessing sysfs\n", __FUNCTION__);
		goto out;
	}

	error = read_string(&p->type, "%s/type", prefix);
	if (error) {
		fprintf(stderr, "%s: error: accessing sysfs\n", __FUNCTION__);
		goto out;
	}

	error = read_long(&p->size, "%s/size", prefix);
	if (error) {
		fprintf(stderr, "%s: error: accessing sysfs\n", __FUNCTION__);
		goto out;
	}
	p->size *= 1024;	/* XXX */

	error = read_string(&p->size_str, "%s/size", prefix);
	if (error) {
		fprintf(stderr, "%s: error: accessing sysfs\n", __FUNCTION__);
		goto out;
	}

	error = read_long(&p->coherency_line_size,
			"%s/coherency_line_size", prefix);
	if (error) {
		fprintf(stderr, "%s: error: accessing sysfs\n", __FUNCTION__);
		goto out;
	}

	error = read_long(&p->number_of_sets, "%s/number_of_sets", prefix);
	if (error) {
		fprintf(stderr, "%s: error: accessing sysfs\n", __FUNCTION__);
		goto out;
	}

	error = read_long(&p->physical_line_partition,
			"%s/physical_line_partition", prefix);
	if (error) {
		fprintf(stderr, "%s: error: accessing sysfs\n", __FUNCTION__);
		goto out;
	}

	error = read_long(&p->ways_of_associativity,
			"%s/ways_of_associativity", prefix);
	if (error) {
		fprintf(stderr, "%s: error: accessing sysfs\n", __FUNCTION__);
		goto out;
	}

	error = read_bitmap(&p->shared_cpu_map, get_nprocs_conf(),
			"%s/shared_cpu_map", prefix);
	if (error) {
		fprintf(stderr, "%s: error: accessing sysfs\n", __FUNCTION__);
		goto out;
	}

	error = 0;
	list_add(&p->chain, &cpu_topo->cache_topology_list);
	p = NULL;

out:
	if (p) {
		free(p->type);
		free(p->size_str);
		free(p);
	}

	free(prefix);
	return error;
}


static int collect_cpu_topology(int cpu)
{
	int error;
	char *prefix = NULL;
	int n;
	struct cpu_topology *p = NULL;
	int index;

	prefix = malloc(PATH_MAX);
	if (!prefix) {
		error = -ENOMEM;
		fprintf(stderr, "%s: error: allocating memory\n", __FUNCTION__);
		goto out;
	}

	n = snprintf(prefix, PATH_MAX, "/sys/devices/system/cpu/cpu%d", cpu);
	if (n >= PATH_MAX) {
		error = -ENAMETOOLONG;
		fprintf(stderr, "%s: error: name too long\n", __FUNCTION__);
		goto out;
	}

	p = malloc(sizeof(*p));
	if (!p) {
		error = -ENOMEM;
		fprintf(stderr, "%s: error: allocating memory\n", __FUNCTION__);
		goto out;
	}

	memset(p, 0, sizeof(*p));

	INIT_LIST_HEAD(&p->cache_topology_list);
	p->cpu_number = cpu;

	error = read_long(&p->core_id, "%s/topology/core_id", prefix);
	if (error) {
		error = -EINVAL;
		fprintf(stderr, "%s: error: accessing sysfs\n", __FUNCTION__);
		goto out;
	}

	error = read_bitmap(&p->core_siblings, get_nprocs_conf(),
			"%s/topology/core_siblings", prefix);
	if (error) {
		error = -EINVAL;
		fprintf(stderr, "%s: error: accessing sysfs\n", __FUNCTION__);
		goto out;
	}

	error = read_long(&p->physical_package_id,
			"%s/topology/physical_package_id", prefix);
	if (error) {
		error = -EINVAL;
		fprintf(stderr, "%s: error: accessing sysfs\n", __FUNCTION__);
		goto out;
	}

	error = read_bitmap(&p->thread_siblings, get_nprocs_conf(),
			"%s/topology/thread_siblings", prefix);
	if (error) {
		error = -EINVAL;
		fprintf(stderr, "%s: error: accessing sysfs\n", __FUNCTION__);
		goto out;
	}

	for (index = 0; index < 10; ++index) {
		error = collect_cache_topology(p, index);
		if (error) {
			fprintf(stderr, "%s: error: "
					"collecting cache topology\n", __FUNCTION__);
			break;
		}
	}

	error = 0;
	list_add(&p->chain, &cpu_topology_list);
	p = NULL;

out:
	free(p);
	free(prefix);
	return error;
}

static int collect_node_topology(int node)
{
	int error;
	struct node_topology *p = NULL;

	p = malloc(sizeof(*p));
	if (!p) {
		error = -ENOMEM;
		fprintf(stderr, "%s: error: allocating memory\n", __FUNCTION__);
		goto out;
	}
	memset(p, 0, sizeof(*p));

	p->node_number = node;

	error = read_bitmap(&p->cpumap, get_nprocs_conf(),
			"/sys/devices/system/node/node%d/cpumap", node);
	if (error) {
		error = -ENOMEM;
		fprintf(stderr, "%s: error: accessing sysfs\n", __FUNCTION__);
		goto out;
	}

	error = 0;
	list_add(&p->chain, &node_topology_list);
	p = NULL;

out:
	free(p);
	return error;
}

static int collect_topology(void)
{
	int cpu, node;
	cpu_set_t cpus;

	if (numa_available() == -1) {
		return -EINVAL;
	}

	if (read_bitmap_parselist(&cpus, get_nprocs_conf(),
				"/sys/devices/system/cpu/online",
				0) < 0) {
		return -EINVAL;
	}

	for (cpu = 0; cpu < get_nprocs_conf(); ++cpu) {
		if (CPU_ISSET(cpu, &cpus)) {
			if (collect_cpu_topology(cpu) < 0) {
				fprintf(stderr, "error: collecting CPU topology\n");
				return -EINVAL;
			}
		}
	}

	for (node = 0; node < numa_num_configured_nodes(); ++node) {
		if (collect_node_topology(node) < 0) {
			fprintf(stderr, "error: collecting NUMA node topology\n");
			return -EINVAL;
		}
	}

	return 0;
}


/*
 * main()
 */
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

	INIT_LIST_HEAD(&cpu_topology_list);
	INIT_LIST_HEAD(&node_topology_list);

	ppid = getppid();

	printf("[ppid: %d] ppn: %d, tpp: %d\n", ppid, ppn, tpp);

	{
		int error;
		long size;
		int n = 0;
		char *shared_cpu_map;
		cpu_set_t cpu_set;

		if ((error = file_readable(
			"/sys/devices/system/cpu/cpu%d/cache/index0/coherency_line_size",
			0)) < 0) {
			fprintf(stderr, "hee? %d\n", error);
			exit(EXIT_FAILURE);
		}

		if (read_long(&size,
			"/sys/devices/system/cpu/cpu%d/cache/index0/coherency_line_size",
			0) < 0) {
			exit(EXIT_FAILURE);
		}

		printf("coherency_line_size: %lu\n", size);
		
		if (read_string(&shared_cpu_map,
			"/sys/devices/system/cpu/cpu%d/cache/index1/shared_cpu_map",
			0) < 0) {
			exit(EXIT_FAILURE);
		}
		
		printf("shared_cpu_map: %s\n", shared_cpu_map);

		if (read_bitmap(&cpu_set, get_nprocs_conf(),
			"/sys/devices/system/cpu/cpu%d/cache/index1/shared_cpu_map",
			0) < 0) {
			exit(EXIT_FAILURE);
		}

		for (n = 0; n < get_nprocs_conf(); ++n) {
			if (CPU_ISSET(n, &cpu_set)) {
				printf("%s: CPU %d is set\n", __FUNCTION__, n);
			}
		}

		if (read_bitmap_parselist(&cpu_set, get_nprocs_conf(),
			"/sys/devices/system/cpu/online",
			0) < 0) {
			exit(EXIT_FAILURE);
		}

		for (n = 0; n < get_nprocs_conf(); ++n) {
			if (CPU_ISSET(n, &cpu_set)) {
				printf("%s: CPU %d is online\n", __FUNCTION__, n);
			}
		}
	}

	if (collect_topology() < 0) {
		exit(EXIT_FAILURE);
	}

	printf("Topology information collected OK\n");
	{
		struct node_topology *node_topo;
		struct cpu_topology *cpu_topo;
		struct cache_topology *cache_topo;



	}

	exit(0);
}
