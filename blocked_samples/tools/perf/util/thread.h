/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PERF_THREAD_H
#define __PERF_THREAD_H

#include <linux/refcount.h>
#include <linux/rbtree.h>
#include <linux/list.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include "srccode.h"
#include "symbol_conf.h"
#include <strlist.h>
#include <intlist.h>
#include "rwsem.h"

struct addr_location;
struct map;
struct namespaces_event;
struct thread_stack;
struct unwind_libunwind_ops;

struct thread {
	union {
		struct rb_node	 rb_node;
		struct list_head node;
	};
	struct map_groups	*mg;
	pid_t			pid_; /* Not all tools update this */
	pid_t			tid;
	pid_t			ppid;
	int			cpu;
	refcount_t		refcnt;
	bool			comm_set;
	int			comm_len;
	bool			dead; /* if set thread has exited */
	struct list_head	namespaces_list;
	struct rw_semaphore	namespaces_lock;
	struct list_head	comm_list;
	struct rw_semaphore	comm_lock;
	u64			db_id;

	void			*priv;
	struct thread_stack	*ts;
	struct nsinfo		*nsinfo;
	struct srccode_state	srccode_state;
	bool			filter;
	int			filter_entry_depth;
};

struct machine;
struct namespaces;
struct comm;

struct thread *thread__new(pid_t pid, pid_t tid);
int thread__init_map_groups(struct thread *thread, struct machine *machine);
void thread__delete(struct thread *thread);

struct thread *thread__get(struct thread *thread);
void thread__put(struct thread *thread);

static inline void __thread__zput(struct thread **thread)
{
	thread__put(*thread);
	*thread = NULL;
}

#define thread__zput(thread) __thread__zput(&thread)

static inline void thread__exited(struct thread *thread)
{
	thread->dead = true;
}

struct namespaces *thread__namespaces(struct thread *thread);
int thread__set_namespaces(struct thread *thread, u64 timestamp,
			   struct namespaces_event *event);

int __thread__set_comm(struct thread *thread, const char *comm, u64 timestamp,
		       bool exec);
static inline int thread__set_comm(struct thread *thread, const char *comm,
				   u64 timestamp)
{
	return __thread__set_comm(thread, comm, timestamp, false);
}

int thread__set_comm_from_proc(struct thread *thread);

int thread__comm_len(struct thread *thread);
struct comm *thread__comm(const struct thread *thread);
struct comm *thread__exec_comm(const struct thread *thread);
const char *thread__comm_str(struct thread *thread);
int thread__insert_map(struct thread *thread, struct map *map);
int thread__fork(struct thread *thread, struct thread *parent, u64 timestamp, bool do_maps_clone);
size_t thread__fprintf(struct thread *thread, FILE *fp);

struct thread *thread__main_thread(struct machine *machine, struct thread *thread);

struct map *thread__find_map(struct thread *thread, u16 cpumode, u64 addr,
			     struct addr_location *al);
struct map *thread__find_map_fb(struct thread *thread, u16 cpumode, u64 addr,
				struct addr_location *al);

struct symbol *thread__find_symbol(struct thread *thread, u16 cpumode,
				   u64 addr, struct addr_location *al);
struct symbol *thread__find_symbol_fb(struct thread *thread, u16 cpumode,
				      u64 addr, struct addr_location *al);

void thread__find_cpumode_addr_location(struct thread *thread, u64 addr,
					struct addr_location *al);

int thread__memcpy(struct thread *thread, struct machine *machine,
		   void *buf, u64 ip, int len, bool *is64bit);

static inline void *thread__priv(struct thread *thread)
{
	return thread->priv;
}

static inline void thread__set_priv(struct thread *thread, void *p)
{
	thread->priv = p;
}

static inline bool thread__is_filtered(struct thread *thread)
{
	if (symbol_conf.comm_list &&
	    !strlist__has_entry(symbol_conf.comm_list, thread__comm_str(thread))) {
		return true;
	}

	if (symbol_conf.pid_list &&
	    !intlist__has_entry(symbol_conf.pid_list, thread->pid_)) {
		return true;
	}

	if (symbol_conf.tid_list &&
	    !intlist__has_entry(symbol_conf.tid_list, thread->tid)) {
		return true;
	}

	return false;
}

#endif	/* __PERF_THREAD_H */
