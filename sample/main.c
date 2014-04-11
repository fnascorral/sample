#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <err.h>
#include <kvm.h>
#include <fcntl.h>
#include <sys/syscall.h>

#include <sys/param.h>
#include <sys/linker.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <machine/reg.h>

#include "Hash.h"
#include "Proc.h"
#include "Thread.h"
#include "Stack.h"
#include "Tree.h"
#include "Symbol.h"

#include "sample.h"

int debug = 0;
int verbose = 0;

static int
iterate_procs(kvm_t *kvm, void (^handler)(struct kinfo_proc *))
{
	struct kinfo_proc *procs = NULL;
	int num_procs;

	procs = kvm_getprocs(kvm, KERN_PROC_PROC, 0, &num_procs);
	if (procs) {
		size_t i;
		for (i = 0; i < num_procs; i++) {
			handler(procs + i);
		}
	} else {
		return -1;
	}
	return 0;
}

static const char *
GetProcName(pid_t pid)
{
	int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, pid };
	struct kinfo_proc ki;
	char procname[MAXPATHLEN + 1] = { 0 };
	int rv;
	size_t size = sizeof(ki);

	rv = sysctl(mib, 4, &ki, &size, NULL, 0);
	if (rv != -1) {
		if (ki.ki_comm[0]) {
			return strdup(ki.ki_comm);
		} else {
		}
	} else {
		warn("sysctl KERN_PROC_PID pid %u", pid);
	}
	return NULL;
}

static const char *
GetProcessPathname(pid_t pid)
{
	int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, pid };
	char pathname[MAXPATHLEN + 1] = { 0 };
	int rv;
	size_t size = sizeof(pathname);
	rv = sysctl(mib, 4, pathname, &size, NULL, 0);
	if (rv != -1) {
		if (pathname[0]) {
			return strdup(pathname);
		}
	}
	return NULL;
}

static uint8_t profile_buffer[128 * 1024];	// 128k should be enough
static void
AddSampleInformation(SampleProc_t *proc, kern_sample_t *sample)
{
	int rv;
	Stack_t *stack;
	SampleThread_t *thread;

#if 0
	if (proc->pathname == NULL) {
		fprintf(stderr, "Not tracking kernel path %s\n", proc->name);
		return;
	}
#endif

	if (proc->mmaps == NULL) {
		ProcessGetVMMaps(proc);
	}

	thread = GetThread(proc, sample->tid);
	stack = CreateStack(sample);
	if (stack) {
		ThreadAddStack(thread, stack);
	}
}

static void
usage(void)
{
	errx(1, "usage:  [-n sample_count] [-s sample_duration] [-p pid]\n"
	     "\tsample duration in ms (default 10)");
}

int
main(int ac, char **av)
{
	static const char *kSamplePath = "/dev/sample";
	ssize_t nread;
	int sample_fd;
	int num_syms;
	hash_t ProcHash;
	int i;
	struct ksample_opts opts = { 0 };
	struct timespec dur = { 0 };
	uint8_t *sample_buffer = NULL;
	uint32_t sample_duration = 10;	// in ms
	uint32_t sample_count = 100;
	pid_t target = 0;	// 0 means all processes
	static const int kSampleBufferSize = 1024 * 1024;
	int symbolicate = 0;

	while ((i = getopt(ac, av, "n:s:p:dvS")) != -1) {
		switch (i) {
		case 'S':
			symbolicate = 1;
			break;
		case 'n':
			sample_count = atoi(optarg);
			break;
		case 's':
			sample_duration = atoi(optarg);
			if (sample_duration > 1000) {
				errx(1, "sample duration must be less than 1 second (1000 milliseconds)");
			}
			if (sample_duration < 1) {
				errx(1, "sample duration must be at least 1 millisecond");
			}
			break;
		case 'p':
			target = atoi(optarg);
			break;
		case 'd':
			debug++;
			break;
		case 'v':
			verbose++;
			break;
		default:
			usage();
		}
	}
	
	sample_fd = open(kSamplePath, O_RDONLY);
	if (sample_fd == -1) {
		err(1, "Could not open sample device");
	}

	ProcHash = CreateProcessHash();
	
	sample_buffer = malloc(kSampleBufferSize);
	if (sample_buffer == NULL) {
		err(1, "Could not allocate sample buffer");
	}

	opts.milliseconds = sample_duration;
	opts.count = sample_count;

	if (ioctl(sample_fd, KSIOC_START, &opts) == -1) {
		err(1, "Could not start sampling");
	}

	while ((nread = read(sample_fd, sample_buffer, kSampleBufferSize)) > 0) {
		uint8_t *ptr = sample_buffer,
			*end = ptr + nread;

		while (ptr < end) {
			kern_sample_t *samp = (void*)ptr;
			SampleProc_t *p;
			if (target == 0 || samp->pid == target) {
				p = FindProcess(ProcHash, samp->pid);
				if (p == NULL) {
					const char *pathname = GetProcessPathname(samp->pid);
					p = AddProcess(ProcHash, samp->pid);
					p->pathname = pathname;
					p->name = GetProcName(samp->pid);
				}
				p->num_samples++;
				AddSampleInformation(p, samp);
			}
			ptr += SAMPLE_SIZE(samp);
		}
	}

	if (ioctl(sample_fd, KSIOC_STOP, 0) == -1) {
		warn("Could not stop sampling");
	}
	close(sample_fd);

	free(sample_buffer);

	SymbolPool_t kernelPool = CreateSymbolPool();
        if (kernelPool) {
                int mod_id = 0;
                while ((mod_id = kldnext(mod_id)) > 0) {
                        struct kld_file_stat mod_stat = { .version = sizeof(mod_stat) };
                        if (kldstat(mod_id, &mod_stat) != -1) {
                                SymbolFile_t *f = CreateSymbolFile(mod_stat.pathname,
                                                                   0, // Is this right?
                                                                   mod_stat.address,
                                                                   mod_stat.size);
                                if (f) {
                                        (void)AddSymbolFile(kernelPool, f);
                                        ReleaseSymbolFile(f);
                                }
                        } else {
                                warn("Could not stat kernel mod_id %d", mod_id);
                        }
                }
        }

	IterateHash(ProcHash, ^(void *object) {
			SampleProc_t *proc = object;
			size_t vmIndex = 0;
			printf("Process %d (%s, pathname %s):\n%zu samples\n", proc->pid, proc->name, proc->pathname ? proc->pathname : "unknown", proc->num_samples);
			IterateHash(proc->threads, ^(void *inner) {
					SampleThread_t *thread = inner;
					SymbolPool_t pool;
					pool = CreateSymbolPool();

					if (proc->num_vmaps > 0) {
                                                struct kinfo_vmentry *vme = proc->mmaps;
                                                size_t vmIndex;
                        
                                                for (vmIndex = 0;
                                                     vmIndex < proc->num_vmaps;
                                                     vmIndex++) {
                                                        if (vme[vmIndex].kve_protection & KVME_PROT_EXEC &&
                                                            vme[vmIndex].kve_type == KVME_TYPE_VNODE &&
                                                            vme[vmIndex].kve_path[0]) {
								SymbolFile_t *f = CreateSymbolFile(vme[vmIndex].kve_path,
												   vme[vmIndex].kve_offset,
												   (void*)vme[vmIndex].kve_start,
												   (size_t)(vme[vmIndex].kve_end - vme[vmIndex].kve_start));
								if (f) {
									(void)AddSymbolFile(pool, f);
									ReleaseSymbolFile(f);
								}
							}
						}
					}
					if (pool) {
						if (kernelPool) {
							AddSymbolPool(pool, kernelPool);
						}
					}

					if (thread->numStacks > 0) {
						Node_t *root;
						printf("\nThread ID %u\n", thread->tid);
						root = CreateTree(^(void *val) {
								return (void*)val;
							}, ^(void *left, void *right) {
								vm_offset_t l = (vm_offset_t)left,
									r = (vm_offset_t)right;
								if (l == r)
									return 0;
								if (l < r)
									return -1;
								if (l > r)
									return 1;
								return 2;
							}, ^(void *val) {
								return;
							}, ^(void *val) {
								char *retval;
								off_t off;
								if (symbolicate == 0) {
									SymbolFile_t *sf = NULL;
									if (pool) {
										sf = FindSymbolFileByAddress(pool, val, &off);
									}
									if (sf) {
										asprintf(&retval, "%p (%s + %llu)", val, sf->pathname, (long long)off);
									} else {
										asprintf(&retval, "%p", val);
									}
								} else {
									char *tmp;
									tmp = FindSymbolForAddress(pool, val, &off);
									if (tmp) {
										asprintf(&retval, "%p (%s + %llu)", val, tmp, (long long)off);
										free(tmp);
									} else {
										asprintf(&retval, "%p", val);
									}
								}
								return retval;
							});
						if (root) {
							int stackNum;
							Stack_t **stacks = (Stack_t**)thread->stacks;
							for (stackNum = 0;
							     stackNum < thread->numStacks;
							     stackNum++) {
								Stack_t *curStack = stacks[stackNum];
								int stackLevel;
								Node_t *level = root;

								for (stackLevel = 0;
								     stackLevel < curStack->count;
								     stackLevel++) {
									char *trace = curStack->stacks[stackLevel];
									level = NodeAddValue(level, trace);
								}
							}
							PrintTree(root, 1);
						}
					}
					return 1;
				});
			if (proc->num_vmaps > 0) {
				struct kinfo_vmentry *vme = proc->mmaps;
				printf("\nMapped Files:\n");
				printf("\tStart\tEnd\tFile\n");
				for (vmIndex = 0;
				     vmIndex < proc->num_vmaps;
				     vmIndex++) {
					if (vme[vmIndex].kve_vn_fileid) {
						printf("\t%#llx\t%#llx\t%s\n",
						       (long long)vme[vmIndex].kve_start,
						       (long long)vme[vmIndex].kve_end,
						       vme[vmIndex].kve_path);
					}
				}
			}
			printf("\n");
			return 1;
		});
	DestroyHash(ProcHash);

	return 0;
}