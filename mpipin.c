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
#include <sys/file.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <getopt.h>
#include <numa.h>
#include <numaif.h>

#include <bitmap.h>
#include <list.h>

//#define DEBUG

#ifdef DEBUG
#define dprintf(FORMAT, ...) printf("[%d] "FORMAT, getpid(), ##__VA_ARGS__)
#else
#define dprintf(...)
#endif


/**
 * cpuset_first - get the first cpu in a cpuset
 * @srcp: the cpuset pointer
 *
 * Returns >= nr_cpu_ids if no cpus set.
 */
static inline unsigned int cpuset_first(const cpu_set_t *srcp)
{
	return find_first_bit((const long unsigned int*)
			srcp, sizeof(cpu_set_t) * BITS_PER_BYTE);
}

/**
 * cpuset_next - get the next cpu in a cpuset
 * @n: the cpu prior to the place to search (ie. return will be > @n)
 * @srcp: the cpuset pointer
 *
 * Returns >= nr_cpu_ids if no further cpus set.
 */
static inline unsigned int cpuset_next(int n, const cpu_set_t *srcp)
{
	return find_next_bit((const long unsigned int*)srcp,
			sizeof(cpu_set_t) * BITS_PER_BYTE, n+1);
}

/**
 * for_each_cpu - iterate over every cpu in a set
 * @cpu: the (optionally unsigned) integer iterator
 * @set: the cpuset pointer
 *
 * After the loop, cpu is >= nr_cpu_ids.
 */
#define for_each_cpu(cpu, set)				\
	for ((cpu) = -1;				\
		(cpu) = cpuset_next((cpu), (set)),	\
		(cpu) < get_nprocs_conf();)


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
		.name =		"cores-per-process",
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
	{
		.name =		"exclude-cpus",
		.has_arg =	required_argument,
		.flag =		NULL,
		.val =		'e',
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
	struct list_head list;
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
	struct list_head list;
	int cpu_id;
	int node_id;
	int hw_id;
	long physical_package_id;
	long core_id;
	cpu_set_t core_siblings;
	cpu_set_t thread_siblings;
	struct list_head cache_topology_list;
};

struct node_topology {
	struct list_head list;
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
			cpu_topo->cpu_id, index);
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
	list_add_tail(&p->list, &cpu_topo->cache_topology_list);
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
	int node;

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
	p->cpu_id = cpu;

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

	for (node = 0; node < numa_num_configured_nodes(); ++node) {
		char node_dname[PATH_MAX];
		struct stat st;

		sprintf(node_dname, "%s/node%d", prefix, node);
		if (stat(node_dname, &st) < 0) {
			continue;
		}

		p->node_id = node;
		break;
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
	list_add_tail(&p->list, &cpu_topology_list);
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
	list_add_tail(&p->list, &node_topology_list);
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
 * Partitioning information.
 */
struct process_list_item {
	int ready;
	int timeout;
	int pid;
	unsigned long start_ts;

	pthread_condattr_t wait_cv_attr;
	pthread_cond_t wait_cv;
	int next_process_ind;
};

#define MAX_PROCESSES 1024

struct part_exec {
	pthread_mutexattr_t lock_attr;
	pthread_mutex_t lock;
	int nr_processes;
	int nr_processes_left;
	int process_rank;
	cpu_set_t cpus_used;
	cpu_set_t cpus_available;
	int cpus_to_assign;
	int first_process_ind;
	struct process_list_item processes[MAX_PROCESSES];
	cpu_set_t affinities[MAX_PROCESSES];
};

int pin_process(struct part_exec *pe, int ppn)
{
	struct cpu_topology *cpu_top, *cpu_top_i;
	struct cache_topology *cache_top;
	int cpu, cpus_assigned, cpu_prev;
	int ret = 0;
	cpu_set_t *cpus_available = NULL;
	cpu_set_t *cpus_to_use = NULL;
	int my_i, i, prev_i, next_i;

	pthread_mutex_lock(&pe->lock);

	/* First process to enter CPU partitioning */
	if (pe->nr_processes == -1) {
		pe->nr_processes = ppn;
		pe->nr_processes_left = ppn;
		dprintf("%s: nr_processes: %d (partitioned exec starts)\n",
				__FUNCTION__,
				pe->nr_processes);
	}

	if (pe->nr_processes != ppn) {
		fprintf(stderr, "%s: error: requested number of processes"
				" doesn't match current partitioned execution\n",
				__FUNCTION__);
		ret = -EINVAL;
		goto unlock_out;
	}

	--pe->nr_processes_left;
	dprintf("%s: nr_processes: %d, nr_processes_left: %d\n",
			__FUNCTION__,
			pe->nr_processes,
			pe->nr_processes_left);

	/* Find empty process slot */
	my_i = -1;
	for (i = 0; i < MAX_PROCESSES; ++i) {
		if (pe->processes[i].pid == 0) {
			my_i = i;
			break;
		}
	}

	pe->processes[my_i].pid = getpid();
	pe->processes[my_i].ready = 0;
	pe->processes[my_i].timeout = 0;
	pe->processes[my_i].next_process_ind = -1;

	/*
	 * Add ourself to the list in order of PID
	 * TODO: add start time
	 */
	if (pe->first_process_ind == -1) {
		pe->first_process_ind = my_i;
		dprintf("%s: add to empty list as first\n",
				__FUNCTION__);
	}
	else {
		prev_i = -1;
		for (i = pe->first_process_ind; i != -1;
				i = pe->processes[i].next_process_ind) {

			if (pe->processes[i].pid > getpid()) {
				break;
			}

			prev_i = i;
		}

		/* First element */
		if (prev_i == -1) {
			pe->processes[my_i].next_process_ind = pe->first_process_ind;
			pe->first_process_ind = my_i;
			dprintf("%s: add to non-empty list as first\n",
					__FUNCTION__);
		}
		/* After prev_i */
		else {
			pe->processes[my_i].next_process_ind =
				pe->processes[prev_i].next_process_ind;
			pe->processes[prev_i].next_process_ind = my_i;
			dprintf("%s: add to non-empty list after PID %d\n",
					__FUNCTION__, pe->processes[prev_i].pid);
		}
	}

	next_i = -1;

	/* Last process? Wake up first in list */
	if (pe->nr_processes_left == 0) {
		next_i = pe->first_process_ind;
		pe->first_process_ind = pe->processes[next_i].next_process_ind;

		pe->processes[next_i].ready = 1;
		dprintf("%s: waking PID %d\n",
				__FUNCTION__, pe->processes[next_i].pid);
		pthread_cond_signal(&pe->processes[next_i].wait_cv);

		/* Reset process counter */
		pe->nr_processes_left = pe->nr_processes;
		pe->process_rank = 0;
	}

	/* Wait for the rest if we aren't the next */
	if (next_i != my_i) {
		struct process_list_item *pli = &pe->processes[my_i];
		struct timespec ts;

		dprintf("%s: pid: %d, waiting in list\n",
				__FUNCTION__, getpid());
		/* Timeout period: 10 secs + (#procs * 0.1sec) */
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_sec += (10 + pe->nr_processes / 10);

		ret = pthread_cond_timedwait(&pli->wait_cv,
				&pe->lock, &ts);

		/* First timeout task? Wake up everyone else,
		 * but tell them we timed out */
		if (ret == ETIMEDOUT) {
abort_all:
			fprintf(stderr, "%s: error: pid: %d, timed out, waking everyone\n",
					__FUNCTION__, getpid());
			while (pe->first_process_ind != -1) {
				struct process_list_item *pli_next =
					&pe->processes[pe->first_process_ind];

				pe->first_process_ind =
					pe->processes[pe->first_process_ind].next_process_ind;
				if (pli_next->pid == getpid())
					continue;
				dprintf("%s: waking next proc: %d\n",
						__FUNCTION__, pli_next->pid);

				pli_next->ready = 1;
				pli_next->timeout = 1;
				pthread_cond_signal(&pli_next->wait_cv);
			}

			/* Reset process counter to start state */
			pe->nr_processes = -1;
			ret = -ETIMEDOUT;

			fprintf(stderr, "%s: error: pid: %d, woken everyone, out\n",
					__FUNCTION__, getpid());
			goto unlock_out;
		}

		/* Interrupted or woken up by someone else due to timeout? */
		if (pli->timeout) {
			fprintf(stderr, "%s: error: pid: %d, job startup timed out\n",
					__FUNCTION__, getpid());
			ret = -ETIMEDOUT;
			goto unlock_out;
		}

		/* Incorrect wakeup state? */
		if (!pli->ready) {
			fprintf(stderr, "%s: error: pid: %d, not ready but woken?\n",
					__FUNCTION__, getpid());
			ret = -EINVAL;
			goto unlock_out;
		}

		dprintf("%s: pid: %d, woken up\n",
				__FUNCTION__, getpid());
	}

	--pe->nr_processes_left;

	/* First process does the partitioning */
	if (pe->process_rank == 0) {
		/* Collect topology information */
		if (collect_topology() < 0) {
			fprintf(stderr, "%s: error: collecting topology information\n",
					__FUNCTION__);
			/* Ugly.. */
			ret = ETIMEDOUT;
			goto abort_all;
		}

		dprintf("%s: topology information collected\n", __FUNCTION__);

		cpus_available = malloc(sizeof(*cpus_available));
		cpus_to_use = malloc(sizeof(*cpus_to_use));
		if (!cpus_available || !cpus_to_use) {
			fprintf(stderr, "%s: error: allocating cpu masks\n", __FUNCTION__);
			ret = -ENOMEM;
			goto unlock_out;
		}

		for (pe->process_rank = 0;
				pe->process_rank < pe->nr_processes;
				++pe->process_rank) {

			memcpy(cpus_available, &pe->cpus_available, sizeof(cpu_set_t));
			memset(cpus_to_use, 0, sizeof(cpu_set_t));

			/* Find the first unused CPU */
			cpu = cpuset_first(cpus_available);

			CPU_CLR(cpu, cpus_available);
			CPU_SET(cpu, cpus_to_use);

			cpu_prev = cpu;
			dprintf("%s: CPU %d assigned (first)\n", __FUNCTION__, cpu);

			for (cpus_assigned = 1; cpus_assigned < pe->cpus_to_assign;
					++cpus_assigned) {
				int node;

				cpu_top = NULL;
				/* Find the topology object of the last core assigned */
				list_for_each_entry(cpu_top_i, &cpu_topology_list, list) {
					if (cpu_top_i->cpu_id == cpu_prev) {
						cpu_top = cpu_top_i;
						break;
					}
				}

				if (!cpu_top) {
					fprintf(stderr, "%s: error: couldn't find CPU topology info\n",
							__FUNCTION__);
					ret = -EINVAL;
					goto unlock_out;
				}

				node = cpu_top->node_id;

				/* Find a core sharing the same cache iterating caches from
				 * the most inner one outwards */
				list_for_each_entry(cache_top, &cpu_top->cache_topology_list, list) {
					for_each_cpu(cpu, &cache_top->shared_cpu_map) {
						if (CPU_ISSET(cpu, cpus_available)) {
							CPU_CLR(cpu, cpus_available);
							CPU_SET(cpu, cpus_to_use);

							cpu_prev = cpu;
							dprintf("%s: CPU %d assigned (same cache L%lu)\n",
									__FUNCTION__, cpu, cache_top->level);
							goto next_cpu;
						}
					}
				}

				/* Find a CPU from the same NUMA node */
				for_each_cpu(cpu, cpus_available) {
					cpu_top = NULL;
					/* Find the topology object of this CPU */
					list_for_each_entry(cpu_top_i, &cpu_topology_list, list) {
						if (cpu_top_i->cpu_id == cpu) {
							cpu_top = cpu_top_i;
							break;
						}
					}

					if (!cpu_top) {
						fprintf(stderr, "%s: error: couldn't find CPU topology info\n",
								__FUNCTION__);
						ret = -EINVAL;
						goto unlock_out;
					}

					/* Found one */
					if (node == cpu_top->node_id) {
						CPU_CLR(cpu, cpus_available);
						CPU_SET(cpu, cpus_to_use);

						cpu_prev = cpu;
						dprintf("%s: CPU %d assigned (same NUMA)\n",
								__FUNCTION__, cpu);
						goto next_cpu;
					}
				}

				/* No CPU? Simply find the next unused one */
				cpu = cpuset_first(cpus_available);
				CPU_CLR(cpu, cpus_available);
				CPU_SET(cpu, cpus_to_use);

				cpu_prev = cpu;
				dprintf("%s: CPU %d assigned (unused)\n",
						__FUNCTION__, cpu);
next_cpu:
				continue;
			}

			/* Commit unused cores to shared memory */
			memcpy(&pe->cpus_available, cpus_available, sizeof(*cpus_available));
			memcpy(&pe->affinities[pe->process_rank],
				cpus_to_use, sizeof(cpu_set_t));
		}

		pe->process_rank = 0;
		free(cpus_to_use);
		free(cpus_available);
	}

	/* Reset if last process */
	if (pe->nr_processes_left == 0) {
		dprintf("%s: nr_processes: %d (partitioned exec ends)\n",
				__FUNCTION__,
				pe->nr_processes);
		pe->nr_processes = -1;
		memset(&pe->cpus_available, 0, sizeof(pe->cpus_available));
	}
	/* Otherwise wake up next process in list */
	else {
		next_i = pe->first_process_ind;
		pe->first_process_ind = pe->processes[next_i].next_process_ind;

		pe->processes[next_i].ready = 1;
		dprintf("%s: waking PID %d\n",
				__FUNCTION__, pe->processes[next_i].pid);
		pthread_cond_signal(&pe->processes[next_i].wait_cv);
	}

	dprintf("%s: rank: %d, ret: 0\n",
			__FUNCTION__, pe->process_rank);
	if (sched_setaffinity(0, sizeof(cpu_set_t),
				&pe->affinities[pe->process_rank]) < 0) {
		fprintf(stderr, "%s: error: setting CPU affinity\n",
				__FUNCTION__);
		ret = -EINVAL;
		goto unlock_out;

	}

	++pe->process_rank;

	ret = 0;

unlock_out:
	pthread_mutex_unlock(&pe->lock);

	return ret;
}


/*
 * main()
 */

#define MPIPIN_MAGIC	(0xEEEEABCD)

int main(int argc, char **argv)
{
	int error;
	int ppn = 0;
	int tpp = 0;
	int opt;
	int shm_fd;
	int shm_created = 0;
	int cpu;
	pid_t ppid;
	void *shm;
	struct stat st;
	size_t shm_size;
	struct part_exec *pe;
	cpu_set_t cpus_available;
	cpu_set_t cpus_excluded;
	char shm_path[PATH_MAX];

	memset(&cpus_excluded, 0, sizeof(cpu_set_t));

	/* Parse options */
	while ((opt = getopt_long(argc, argv, "+n:p:t:e:", options, NULL)) != -1) {
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

			case 'e':
				error = bitmap_parselist(optarg, (unsigned long int*)&cpus_excluded,
						sizeof(cpu_set_t) * BITS_PER_BYTE);
				if (error) {
					fprintf(stderr, "error: parsing excluded CPU list\n");
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

	dprintf("exec: %s\n", argv[optind]);

	if (ppn == 0) {
		fprintf(stderr, "error: you must specify the number of processes per node\n");
		print_usage(argv);
		exit(EXIT_FAILURE);	
	}

	if (ppn > MAX_PROCESSES) {
		fprintf(stderr, "error: too many processes\n");
		print_usage(argv);
		exit(EXIT_FAILURE);	
	}

	INIT_LIST_HEAD(&cpu_topology_list);
	INIT_LIST_HEAD(&node_topology_list);

	ppid = getppid();

	dprintf("[ppid: %d] ppn: %d, tpp: %d\n", ppid, ppn, tpp);

	/* Get affinity */
	if (sched_getaffinity(0, sizeof(cpu_set_t), &cpus_available) == -1) {
		fprintf(stderr, "error: obtaining CPU affinity\n");
		error = EXIT_FAILURE;
		goto cleanup_shm;
	}

	/* Exclude excluded CPUs.. */
	for_each_cpu(cpu, &cpus_excluded) {
		CPU_CLR(cpu, &cpus_available);
	}

#if 0
	{
		struct node_topology *node_topo;
		struct cpu_topology *cpu_topo_iter;
		struct cpu_topology *cpu_topo = NULL;
		struct cache_topology *cache_topo;

		list_for_each_entry(node_topo, &node_topology_list, list) {
			int cpu;
			printf("NUMA: %d\n", node_topo->node_number);

			for (cpu = 0; cpu < get_nprocs_conf(); ++cpu) {
				if (CPU_ISSET(cpu, &node_topo->cpumap)) {
					printf("  CPU: %d\n", cpu);
				}

				list_for_each_entry(cpu_topo_iter, &cpu_topology_list, list) {
					if (cpu_topo_iter->cpu_id == cpu) {
						cpu_topo = cpu_topo_iter;
						break;
					}
				}

				if (!cpu_topo) {
					fprintf(stderr, "no cpu_topo for %d??\n", cpu);
					continue;
				}

				list_for_each_entry(cache_topo,
						&cpu_topo->cache_topology_list, list) {
					int scpu;

					/*
					if (strcmp(cache_topo->type, "Data")) {
						continue;
					}
					*/

					for (scpu = 0; scpu < get_nprocs_conf(); ++scpu) {
						if (CPU_ISSET(scpu, &cache_topo->shared_cpu_map)) {
							printf("    Cache level: %ld (type: %s), CPU: %d shared\n",
								cache_topo->level,
								cache_topo->type, scpu);
						}
					}
				}
			}
		}
	}
#endif

	/* Shared memory with other ranks */
	sprintf(shm_path, "/mpipin.%d.shm", ppid);

	shm_fd = shm_open(shm_path, O_RDWR | O_CREAT, 0700);
	if (shm_fd < 0) {
		fprintf(stderr, "error: opening shared memory file\n");
		perror("");
		error = EXIT_FAILURE;
		goto cleanup_shm;
	}

	if (flock(shm_fd, LOCK_EX) < 0) {
		fprintf(stderr, "error: locking shared memory\n");
		error = EXIT_FAILURE;
		goto cleanup_shm;
	}

	if (fstat(shm_fd, &st) < 0) {
		fprintf(stderr, "error: stating shm file\n");
		error = EXIT_FAILURE;
		goto unlock_cleanup_shm;
	}

	dprintf("st_size: %lu\n", st.st_size);
	shm_size = sizeof(struct part_exec);
	if (st.st_size == 0) {
		ftruncate(shm_fd, shm_size + PAGE_SIZE);
		shm_created = 1;
	}

	shm = mmap(0, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
	if (shm == MAP_FAILED) {
		fprintf(stderr, "error: mapping shared memory file\n");
		error = EXIT_FAILURE;
		goto unlock_cleanup_shm;
	}

	pe = (struct part_exec *)shm;

	dprintf("shm @ %p %s\n", shm, shm_created ? "(created)" : "(attached)");

	/* First process initializes shared memory variables */
	if (shm_created) {
		int pi;
		memset(shm, 0, shm_size);

		/* Cross-process mutex */
		pthread_mutexattr_init(&pe->lock_attr);
		pthread_mutexattr_setpshared(&pe->lock_attr, PTHREAD_PROCESS_SHARED);
		pthread_mutex_init(&pe->lock, &pe->lock_attr);

		for (pi = 0; pi < MAX_PROCESSES; ++pi) {
			pthread_condattr_init(&pe->processes[pi].wait_cv_attr);
			pthread_condattr_setpshared(&pe->processes[pi].wait_cv_attr,
					PTHREAD_PROCESS_SHARED);
			pthread_cond_init(&pe->processes[pi].wait_cv,
					&pe->processes[pi].wait_cv_attr);
			pe->processes[pi].next_process_ind = -1;
		}

		pe->nr_processes = -1;
		pe->nr_processes_left = -1;
		pe->first_process_ind = -1;

		memcpy(&pe->cpus_available, &cpus_available, sizeof(cpu_set_t));
		pe->cpus_to_assign = CPU_COUNT(&pe->cpus_available) / ppn;
		dprintf("%s: CPUs to assign: %d\n",
				__FUNCTION__, pe->cpus_to_assign);
	}

	if (flock(shm_fd, LOCK_UN) < 0) {
		fprintf(stderr, "error: unlocking shared memory folder\n");
		error = EXIT_FAILURE;
		goto cleanup_shm;
	}

	/* Check if we are pinned already */
	if (memcmp(&cpus_available, &pe->cpus_available, sizeof(cpu_set_t))) {
		fprintf(stderr, "error: CPU affinity already set (differs)\n");
		error = EXIT_FAILURE;
		goto cleanup_shm;
	}

	/* We have the region, now wait for all processes and do the pin */
	if (pin_process(pe, ppn) < 0) {
		fprintf(stderr, "error: pinning\n");
		error = EXIT_FAILURE;
		goto cleanup_shm;
	}

	shm_unlink(shm_path);

	if (execvp(argv[optind], &argv[optind]) < 0) {
		fprintf(stderr, "error: executing %s\n", argv[optind]);
		error = EXIT_FAILURE;
	}

	/* Shouldn't reach here.. */
	error = EXIT_FAILURE;

cleanup_shm:
	shm_unlink(shm_path);

	return error;

unlock_cleanup_shm:
	if (flock(shm_fd, LOCK_UN) < 0) {
		fprintf(stderr, "error: unlocking shared memory folder\n");
		error = EXIT_FAILURE;
	}

	goto cleanup_shm;
}


