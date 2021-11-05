// SPDX-License-Identifier: GPL-2.0
// Interval Tree based Range Lock by Jan Kara, Davidlohr Bueso.
// To publish hybridF2FS, updated by Soon Hwang
// SPDX-FileCopyrightText: Copyright (c) 2021 Sogang University
/*
 * Copyright (C) 2017 Jan Kara, Davidlohr Bueso.
 */
/*
 * Range/interval rw-locking
 * -------------------------
 *
 * Interval-tree based range locking is about controlling tasks' forward
 * progress when adding an arbitrary interval (node) to the tree, depending
 * on any overlapping ranges. A task can only continue (wakeup) if there are
 * no intersecting ranges, thus achieving mutual exclusion. To this end, a
 * reference counter is kept for each intersecting range in the tree
 * (_before_ adding itself to it). To enable shared locking semantics,
 * the reader to-be-locked will not take reference if an intersecting node
 * is also a reader, therefore ignoring the node altogether.
 *
 * Fairness and freedom of starvation are guaranteed by the lack of lock
 * stealing, thus range locks depend directly on interval tree semantics.
 * This is particularly for iterations, where the key for the rbtree is
 * given by the interval's low endpoint, and duplicates are walked as it
 * would an inorder traversal of the tree.
 *
 * The cost of lock and unlock of a range is O((1+R_int)log(R_all)) where
 * R_all is total number of ranges and R_int is the number of ranges
 * intersecting the operated range.
 */
#ifndef _LINUX_RANGE_LOCK_H
#define _LINUX_RANGE_LOCK_H

#include <linux/rbtree.h>
#include <linux/interval_tree.h>
#include <linux/list.h>
#include <linux/spinlock.h>

/*
 * The largest range will span [0,RANGE_LOCK_FULL].
 */
#define RANGE_LOCK_FULL  ~0UL

struct range_lock {
	struct interval_tree_node node;
	struct task_struct *tsk;
	/* Number of ranges which are blocking acquisition of the lock */
	unsigned int blocking_ranges;
	u64 seqnum;
        unsigned int holds; /* copy of holds in struct range_lock_tree. including me ! */
};

struct range_lock_tree {
	struct rb_root_cached root;
	spinlock_t lock;
	struct interval_tree_node *leftmost; /* compute smallest 'start' */
	u64 seqnum; /* track order of incoming ranges, avoid overflows */
        unsigned int holds; /* how many locks are being acuired */
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	struct lockdep_map dep_map;
#endif
};

#ifdef CONFIG_DEBUG_LOCK_ALLOC
# define __RANGE_LOCK_DEP_MAP_INIT(lockname) , .dep_map = { .name = #lockname }
#else
# define __RANGE_LOCK_DEP_MAP_INIT(lockname)
#endif

#define __RANGE_LOCK_TREE_INITIALIZER(name)		\
	{ .leftmost = NULL                              \
	, .root = RB_ROOT_CACHED			\
	, .seqnum = 0					\
	, .lock = __SPIN_LOCK_UNLOCKED(name.lock)       \
	__RANGE_LOCK_DEP_MAP_INIT(name) }		\

#define DEFINE_RANGE_LOCK_TREE(name) \
	struct range_lock_tree name = __RANGE_LOCK_TREE_INITIALIZER(name)

#define __RANGE_LOCK_INITIALIZER(__start, __last) {	\
		.node = {				\
			.start = (__start)		\
			,.last = (__last)		\
		}					\
		, .task = NULL				\
		, .blocking_ranges = 0			\
		, .reader = false			\
		, .seqnum = 0				\
	}

#define DEFINE_RANGE_LOCK(name, start, last)				\
	struct range_lock name = __RANGE_LOCK_INITIALIZER((start), (last))

#define DEFINE_RANGE_LOCK_FULL(name)					\
	struct range_lock name = __RANGE_LOCK_INITIALIZER(0, RANGE_LOCK_FULL)

static inline void
__range_lock_tree_init(struct range_lock_tree *tree,
		       const char *name, struct lock_class_key *key)
{
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	/*
	 * Make sure we are not reinitializing a held lock:
	 */
	debug_check_no_locks_freed((void *)tree, sizeof(*tree));
	lockdep_init_map(&tree->dep_map, name, key, 0);
#endif
	tree->root = RB_ROOT_CACHED;
	spin_lock_init(&tree->lock);
	tree->leftmost = NULL;
	tree->seqnum = 0;
        tree->holds = 0;
}

#define range_lock_tree_init(tree)				\
do {								\
	static struct lock_class_key __key;			\
								\
	__range_lock_tree_init((tree), #tree, &__key);		\
} while (0)

void range_lock_init(struct range_lock *lock,
		       unsigned long start, unsigned long last);
void range_lock_init_full(struct range_lock *lock);

/*
 * lock for reading
 */
void range_read_lock(struct range_lock_tree *tree, struct range_lock *lock);
int range_read_lock_interruptible(struct range_lock_tree *tree,
				  struct range_lock *lock);
int range_read_lock_killable(struct range_lock_tree *tree,
			     struct range_lock *lock);
int range_read_trylock(struct range_lock_tree *tree, struct range_lock *lock);
void range_read_unlock(struct range_lock_tree *tree, struct range_lock *lock);

/*
 * lock for writing
 */
void range_write_lock(struct range_lock_tree *tree, struct range_lock *lock);
int range_write_lock_interruptible(struct range_lock_tree *tree,
				   struct range_lock *lock);
int range_write_lock_killable(struct range_lock_tree *tree,
			      struct range_lock *lock);
int range_write_trylock(struct range_lock_tree *tree, struct range_lock *lock);
void range_write_unlock(struct range_lock_tree *tree, struct range_lock *lock);

void range_downgrade_write(struct range_lock_tree *tree,
			   struct range_lock *lock);


//struct rb_root_cached* create_rb_root_cached(struct range_lock_tree *tree);
#ifdef CONFIG_DEBUG_LOCK_ALLOC
/*
 * nested locking. NOTE: range locks are not allowed to recurse
 * (which occurs if the same task tries to acquire the same
 * lock instance multiple times), but multiple locks of the
 * same lock class might be taken, if the order of the locks
 * is always the same. This ordering rule can be expressed
 * to lockdep via the _nested() APIs, but enumerating the
 * subclasses that are used. (If the nesting relationship is
 * static then another method for expressing nested locking is
 * the explicit definition of lock class keys and the use of
 * lockdep_set_class() at lock initialization time.
 * See Documentation/locking/lockdep-design.txt for more details.)
 */
extern void range_read_lock_nested(struct range_lock_tree *tree,
		struct range_lock *lock, int subclass);
extern void range_write_lock_nested(struct range_lock_tree *tree,
		struct range_lock *lock, int subclass);
extern int range_write_lock_killable_nested(struct range_lock_tree *tree,
		struct range_lock *lock, int subclass);
extern void _range_write_lock_nest_lock(struct range_lock_tree *tree,
		struct range_lock *lock, struct lockdep_map *nest_lock);

# define range_write_lock_nest_lock(tree, lock, nest_lock)		\
do {									\
	typecheck(struct lockdep_map *, &(nest_lock)->dep_map);		\
	_range_write_lock_nest_lock(tree, lock, &(nest_lock)->dep_map);	\
} while (0);

#else
# define range_read_lock_nested(tree, lock, subclass) \
	range_read_lock(tree, lock)
# define range_write_lock_nest_lock(tree, lock, nest_lock) \
	range_write_lock(tree, lock)
# define range_write_lock_nested(tree, lock, subclass) \
	range_write_lock(tree, lock)
# define range_write_lock_killable_nested(tree, lock, subclass) \
	range_write_lock_killable(tree, lock)
#endif

#endif
