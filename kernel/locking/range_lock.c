/*
 * Copyright (C) 2017 Jan Kara, Davidlohr Bueso.
 */
#include <linux/rbtree.h>
#include <linux/spinlock.h>
#include <linux/range_lock.h>
#include <linux/lockdep.h>
#include <linux/sched/signal.h>
#include <linux/sched/debug.h>
#include <linux/sched/wake_q.h>
#include <linux/sched.h>
#include <linux/export.h>
//rl
#include <linux/range_lock.h>
#include <linux/slab.h>
#define range_interval_tree_foreach(node, root, start, last)	\
	for (node = interval_tree_iter_first(root, start, last); \
	     node; node = interval_tree_iter_next(node, start, last))

#define to_range_lock(ptr) container_of(ptr, struct range_lock, node)
#define to_interval_tree_node(ptr) \
	container_of(ptr, struct interval_tree_node, rb)

/*
 * Fastpath range intersection/overlap between A: [a0, a1] and B: [b0, b1]
 * is given by:
 *
 *        a0 <= b1 && b0 <= a1
 *
 * ... where A holds the lock range and B holds the smallest 'start' and
 * largest 'last' in the tree. For the later, we rely on the root node,
 * which by augmented interval tree property, holds the largest value in
 * its last-in-subtree. This allows mitigating some of the tree walk overhead
 * for non-intersecting ranges, maintained and consulted in O(1).
 */
static inline bool
__range_intersects_intree(struct range_lock_tree *tree, struct range_lock *lock)
{
	struct interval_tree_node *root;

	if (unlikely(RB_EMPTY_ROOT(&(tree->root.rb_root))))
		return false;

	root = to_interval_tree_node(tree->root.rb_root.rb_node);

	return lock->node.start <= root->__subtree_last &&
		tree->leftmost->start <= lock->node.last;
}

static inline void
__range_tree_insert(struct range_lock_tree *tree, struct range_lock *lock)
{
	if (unlikely(RB_EMPTY_ROOT(&(tree->root.rb_root))) ||
	    lock->node.start < tree->leftmost->start)
		tree->leftmost = &lock->node;

	lock->seqnum = tree->seqnum++;
	interval_tree_insert(&lock->node, &tree->root);
}

static inline void
__range_tree_remove(struct range_lock_tree *tree, struct range_lock *lock)
{
	if (tree->leftmost == &lock->node) {
		struct rb_node *next = rb_next(&tree->leftmost->rb);
		tree->leftmost = to_interval_tree_node(next);
	}

	interval_tree_remove(&lock->node, &tree->root);
}

/*
 * lock->tsk reader tracking.
 */
#define RANGE_FLAG_READER	1UL

static inline struct task_struct *range_lock_waiter(struct range_lock *lock)
{
	return (struct task_struct *)
		((unsigned long) lock->tsk & ~RANGE_FLAG_READER);
}

static inline void range_lock_set_reader(struct range_lock *lock)
{
	lock->tsk = (struct task_struct *)
		((unsigned long)lock->tsk | RANGE_FLAG_READER);
}

static inline void range_lock_clear_reader(struct range_lock *lock)
{
	lock->tsk = (struct task_struct *)
		((unsigned long)lock->tsk & ~RANGE_FLAG_READER);
}

static inline bool range_lock_is_reader(struct range_lock *lock)
{
	return (unsigned long) lock->tsk & RANGE_FLAG_READER;
}

static inline void
__range_lock_init(struct range_lock *lock,
		  unsigned long start, unsigned long last)
{
	WARN_ON(start > last);

	lock->node.start = start;
	lock->node.last = last;
	RB_CLEAR_NODE(&lock->node.rb);
	lock->blocking_ranges = 0;
	lock->tsk = NULL;
	lock->seqnum = 0;

        lock->holds = 0; /* copy of holds in struct range_lock_tree including me! */
}

/**
 * range_lock_init - Initialize the range lock
 * @lock: the range lock to be initialized
 * @start: start of the interval (inclusive)
 * @last: last location in the interval (inclusive)
 *
 * Initialize the range's [start, last] such that it can
 * later be locked. User is expected to enter a sorted
 * range, such that @start <= @last.
 *
 * It is not allowed to initialize an already locked range.
 */
void range_lock_init(struct range_lock *lock,
		     unsigned long start, unsigned long last)
{
	__range_lock_init(lock, start, last);
}
EXPORT_SYMBOL_GPL(range_lock_init);

/**
 * range_lock_init_full - Initialize a full range lock
 * @lock: the range lock to be initialized
 *
 * Initialize the full range.
 *
 * It is not allowed to initialize an already locked range.
 */
void range_lock_init_full(struct range_lock *lock)
{
	__range_lock_init(lock, 0, RANGE_LOCK_FULL);
}
EXPORT_SYMBOL_GPL(range_lock_init_full);

static inline void
range_lock_put(struct range_lock *lock, struct wake_q_head *wake_q)
{
	if (!--lock->blocking_ranges)
		wake_q_add(wake_q, range_lock_waiter(lock));
}

static inline int wait_for_ranges(struct range_lock_tree *tree,
				  struct range_lock *lock, long state)
{
	int ret = 0;
	
        while (true) {
		set_current_state(state);

		/* do we need to go to sleep? */
		if (!lock->blocking_ranges)
			break;

		if (unlikely(signal_pending_state(state, current))) {
			struct interval_tree_node *node;
			unsigned long flags;
			DEFINE_WAKE_Q(wake_q);

			ret = -EINTR;
			/*
			 * We're not taking the lock after all, cleanup
			 * after ourselves.
			 */
			spin_lock_irqsave(&tree->lock, flags);

			range_lock_clear_reader(lock);
			__range_tree_remove(tree, lock);

			if (!__range_intersects_intree(tree, lock))
				goto unlock;

			range_interval_tree_foreach(node, &tree->root,
						    lock->node.start,
						    lock->node.last) {
				struct range_lock *blked;
				blked = to_range_lock(node);

				if (range_lock_is_reader(lock) &&
				    range_lock_is_reader(blked))
					continue;

				/* unaccount for threads _we_ are blocking */
				if (lock->seqnum < blked->seqnum)
					range_lock_put(blked, &wake_q);
			}

		unlock:
			spin_unlock_irqrestore(&tree->lock, flags);
			wake_up_q(&wake_q);
			break;
		}

		schedule();
	}

	__set_current_state(TASK_RUNNING);
	return ret;
}

/**
 * range_read_trylock - Trylock for reading
 * @tree: interval tree
 * @lock: the range lock to be trylocked
 *
 * The trylock is against the range itself, not the @tree->lock.
 *
 * Returns 1 if successful, 0 if contention (must block to acquire).
 */
static inline int __range_read_trylock(struct range_lock_tree *tree,
				       struct range_lock *lock)
{
	int ret = true;
	unsigned long flags;
	struct interval_tree_node *node;
	
	spin_lock_irqsave(&tree->lock, flags);

	if (!__range_intersects_intree(tree, lock))
		goto insert;

	/*
	 * We have overlapping ranges in the tree, ensure that we can
	 * in fact share the lock.
	 */
	range_interval_tree_foreach(node, &tree->root,
				    lock->node.start, lock->node.last) {
		struct range_lock *blocked_lock;
		blocked_lock = to_range_lock(node);

		if (!range_lock_is_reader(blocked_lock)) {
			ret = false;
			goto unlock;
		}
	}
insert:
	range_lock_set_reader(lock);
	__range_tree_insert(tree, lock);
unlock:
	spin_unlock_irqrestore(&tree->lock, flags);

	return ret;
}

int range_read_trylock(struct range_lock_tree *tree, struct range_lock *lock)
{
	int ret = __range_read_trylock(tree, lock);

	if (ret)
		range_lock_acquire_read(&tree->dep_map, 0, 1, _RET_IP_);

	return ret;
}

EXPORT_SYMBOL_GPL(range_read_trylock);

static __always_inline int __sched
__range_read_lock_common(struct range_lock_tree *tree,
			 struct range_lock *lock, long state)
{
	struct interval_tree_node *node;
	unsigned long flags;
	
	spin_lock_irqsave(&tree->lock, flags);

	if (!__range_intersects_intree(tree, lock))
		goto insert;

	range_interval_tree_foreach(node, &tree->root,
				    lock->node.start, lock->node.last) {
		struct range_lock *blocked_lock;
		blocked_lock = to_range_lock(node);

		if (!range_lock_is_reader(blocked_lock))
			lock->blocking_ranges++;
	}
insert:
	__range_tree_insert(tree, lock);

	lock->tsk = current;
	range_lock_set_reader(lock);
	spin_unlock_irqrestore(&tree->lock, flags);

	return wait_for_ranges(tree, lock, state);
}

static __always_inline int
__range_read_lock(struct range_lock_tree *tree, struct range_lock *lock)
{
	return __range_read_lock_common(tree, lock, TASK_UNINTERRUPTIBLE);
}

/**
 * range_read_lock - Lock for reading
 * @tree: interval tree
 * @lock: the range lock to be locked
 *
 * Returns when the lock has been acquired or sleep until
 * until there are no overlapping ranges.
 */
void range_read_lock(struct range_lock_tree *tree, struct range_lock *lock)
{
	might_sleep();
	range_lock_acquire_read(&tree->dep_map, 0, 0, _RET_IP_);

	RANGE_LOCK_CONTENDED(tree, lock,
			     __range_read_trylock, __range_read_lock);
}
EXPORT_SYMBOL_GPL(range_read_lock);

/**
 * range_read_lock_interruptible - Lock for reading (interruptible)
 * @tree: interval tree
 * @lock: the range lock to be locked
 *
 * Lock the range like range_read_lock(), and return 0 if the
 * lock has been acquired or sleep until until there are no
 * overlapping ranges. If a signal arrives while waiting for the
 * lock then this function returns -EINTR.
 */
int range_read_lock_interruptible(struct range_lock_tree *tree,
				  struct range_lock *lock)
{
	might_sleep();
	return __range_read_lock_common(tree, lock, TASK_INTERRUPTIBLE);
}
EXPORT_SYMBOL_GPL(range_read_lock_interruptible);

/**
 * range_read_lock_killable - Lock for reading (killable)
 * @tree: interval tree
 * @lock: the range lock to be locked
 *
 * Lock the range like range_read_lock(), and return 0 if the
 * lock has been acquired or sleep until until there are no
 * overlapping ranges. If a signal arrives while waiting for the
 * lock then this function returns -EINTR.
 */
static __always_inline int
__range_read_lock_killable(struct range_lock_tree *tree,
			   struct range_lock *lock)
{
	return __range_read_lock_common(tree, lock, TASK_KILLABLE);
}

int range_read_lock_killable(struct range_lock_tree *tree,
			     struct range_lock *lock)
{
	might_sleep();
	range_lock_acquire_read(&tree->dep_map, 0, 0, _RET_IP_);

	if (RANGE_LOCK_CONTENDED_RETURN(tree, lock, __range_read_trylock,
					__range_read_lock_killable)) {
		range_lock_release(&tree->dep_map, _RET_IP_);
		return -EINTR;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(range_read_lock_killable);

/**
 * range_read_unlock - Unlock for reading
 * @tree: interval tree
 * @lock: the range lock to be unlocked
 *
 * Wakes any blocked readers, when @lock is the only conflicting range.
 *
 * It is not allowed to unlock an unacquired read lock.
 */
void range_read_unlock(struct range_lock_tree *tree, struct range_lock *lock)
{
	struct interval_tree_node *node;
	unsigned long flags;
	DEFINE_WAKE_Q(wake_q);
	
	spin_lock_irqsave(&tree->lock, flags);

	range_lock_clear_reader(lock);
	__range_tree_remove(tree, lock);

	range_lock_release(&tree->dep_map, _RET_IP_);

	if (!__range_intersects_intree(tree, lock)) {
		/* nobody to wakeup, we're done */
		spin_unlock_irqrestore(&tree->lock, flags);
		return;
	}

	range_interval_tree_foreach(node, &tree->root,
				    lock->node.start, lock->node.last) {
		struct range_lock *blocked_lock;
		blocked_lock = to_range_lock(node);

		if (!range_lock_is_reader(blocked_lock))
			range_lock_put(blocked_lock, &wake_q);
	}

	spin_unlock_irqrestore(&tree->lock, flags);
	wake_up_q(&wake_q);
}
EXPORT_SYMBOL_GPL(range_read_unlock);

/**
 * range_write_trylock - Trylock for writing
 * @tree: interval tree
 * @lock: the range lock to be trylocked
 *
 * The trylock is against the range itself, not the @tree->lock.
 *
 * Returns 1 if successful, 0 if contention (must block to acquire).
 */
static inline int __range_write_trylock(struct range_lock_tree *tree,
					struct range_lock *lock)
{
	int intersects;
	unsigned long flags;

	spin_lock_irqsave(&tree->lock, flags);
	intersects = __range_intersects_intree(tree, lock);

	if (!intersects) {
		range_lock_clear_reader(lock);
		__range_tree_insert(tree, lock);
                lock->holds = ++(tree->holds); /* ACQUIRED in FIRST TRY! */
	}

	spin_unlock_irqrestore(&tree->lock, flags);

	return !intersects;
}

int range_write_trylock(struct range_lock_tree *tree, struct range_lock *lock)
{
	int ret = __range_write_trylock(tree, lock);

	if (ret)
		range_lock_acquire(&tree->dep_map, 0, 1, _RET_IP_);

	return ret;
}
EXPORT_SYMBOL_GPL(range_write_trylock);

static __always_inline int __sched
__range_write_lock_common(struct range_lock_tree *tree,
			  struct range_lock *lock, long state)
{
	struct interval_tree_node *node;
	unsigned long flags;
        int ret;
	
        spin_lock_irqsave(&tree->lock, flags);

	if (!__range_intersects_intree(tree, lock)) {
		goto insert;
        }

	range_interval_tree_foreach(node, &tree->root,
				    lock->node.start, lock->node.last) {
		/*
		 * As a writer, we always consider an existing node. We
		 * need to wait; either the intersecting node is another
		 * writer or we have a reader that needs to finish.
		 */
		lock->blocking_ranges++;
	}
insert:
	__range_tree_insert(tree, lock);

	lock->tsk = current;
	spin_unlock_irqrestore(&tree->lock, flags);

	//return wait_for_ranges(tree, lock, state);
        ret = wait_for_ranges(tree, lock, state);

        spin_lock_irqsave(&tree->lock, flags);
        lock->holds = ++(tree->holds); /* ACQUIRED ! */
	spin_unlock_irqrestore(&tree->lock, flags);
        return ret;
}

static __always_inline int
__range_write_lock(struct range_lock_tree *tree, struct range_lock *lock)
{
	return __range_write_lock_common(tree, lock, TASK_UNINTERRUPTIBLE);
}

/**
 * range_write_lock - Lock for writing
 * @tree: interval tree
 * @lock: the range lock to be locked
 *
 * Returns when the lock has been acquired or sleep until
 * until there are no overlapping ranges.
 */
void range_write_lock(struct range_lock_tree *tree, struct range_lock *lock)
{
	might_sleep();
	range_lock_acquire(&tree->dep_map, 0, 0, _RET_IP_);

	RANGE_LOCK_CONTENDED(tree, lock,
			     __range_write_trylock, __range_write_lock);
}
EXPORT_SYMBOL_GPL(range_write_lock);

/**
 * range_write_lock_interruptible - Lock for writing (interruptible)
 * @tree: interval tree
 * @lock: the range lock to be locked
 *
 * Lock the range like range_write_lock(), and return 0 if the
 * lock has been acquired or sleep until until there are no
 * overlapping ranges. If a signal arrives while waiting for the
 * lock then this function returns -EINTR.
 */
int range_write_lock_interruptible(struct range_lock_tree *tree,
				   struct range_lock *lock)
{
	might_sleep();
	return __range_write_lock_common(tree, lock, TASK_INTERRUPTIBLE);
}
EXPORT_SYMBOL_GPL(range_write_lock_interruptible);

/**
 * range_write_lock_killable - Lock for writing (killable)
 * @tree: interval tree
 * @lock: the range lock to be locked
 *
 * Lock the range like range_write_lock(), and return 0 if the
 * lock has been acquired or sleep until until there are no
 * overlapping ranges. If a signal arrives while waiting for the
 * lock then this function returns -EINTR.
 */
static __always_inline int
__range_write_lock_killable(struct range_lock_tree *tree,
			   struct range_lock *lock)
{
	return __range_write_lock_common(tree, lock, TASK_KILLABLE);
}

int range_write_lock_killable(struct range_lock_tree *tree,
			      struct range_lock *lock)
{
	might_sleep();
	range_lock_acquire(&tree->dep_map, 0, 0, _RET_IP_);

	if (RANGE_LOCK_CONTENDED_RETURN(tree, lock, __range_write_trylock,
					__range_write_lock_killable)) {
		range_lock_release(&tree->dep_map, _RET_IP_);
		return -EINTR;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(range_write_lock_killable);

/**
 * range_write_unlock - Unlock for writing
 * @tree: interval tree
 * @lock: the range lock to be unlocked
 *
 * Wakes any blocked readers, when @lock is the only conflicting range.
 *
 * It is not allowed to unlock an unacquired write lock.
 */
void range_write_unlock(struct range_lock_tree *tree, struct range_lock *lock)
{
	struct interval_tree_node *node;
	unsigned long flags;
	DEFINE_WAKE_Q(wake_q);

	spin_lock_irqsave(&tree->lock, flags);

	range_lock_clear_reader(lock);
	__range_tree_remove(tree, lock);
        (tree->holds)--; /* REALEASE ! */
	range_lock_release(&tree->dep_map, _RET_IP_);

	if (!__range_intersects_intree(tree, lock)) {
		/* nobody to wakeup, we're done */
		spin_unlock_irqrestore(&tree->lock, flags);
		return;
	}

	range_interval_tree_foreach(node, &tree->root,
				    lock->node.start, lock->node.last) {
		struct range_lock *blocked_lock;
		blocked_lock = to_range_lock(node);

		range_lock_put(blocked_lock, &wake_q);
	}

	spin_unlock_irqrestore(&tree->lock, flags);
	wake_up_q(&wake_q);
}
EXPORT_SYMBOL_GPL(range_write_unlock);

/**
 * range_downgrade_write - Downgrade write range lock to read lock
 * @tree: interval tree
 * @lock: the range lock to be downgraded
 *
 * Wakes any blocked readers, when @lock is the only conflicting range.
 *
 * It is not allowed to downgrade an unacquired write lock.
 */
void range_downgrade_write(struct range_lock_tree *tree,
			   struct range_lock *lock)
{
	unsigned long flags;
	struct interval_tree_node *node;
	DEFINE_WAKE_Q(wake_q);

	lock_downgrade(&tree->dep_map, _RET_IP_);

	spin_lock_irqsave(&tree->lock, flags);

	WARN_ON(range_lock_is_reader(lock));

	range_interval_tree_foreach(node, &tree->root,
				    lock->node.start, lock->node.last) {
		struct range_lock *blocked_lock;
		blocked_lock = to_range_lock(node);

		/*
		 * Unaccount for any blocked reader lock. Wakeup if possible.
		 */
		if (range_lock_is_reader(blocked_lock))
			range_lock_put(blocked_lock, &wake_q);
	}

	range_lock_set_reader(lock);
	spin_unlock_irqrestore(&tree->lock, flags);
	wake_up_q(&wake_q);
}
EXPORT_SYMBOL_GPL(range_downgrade_write);

#ifdef CONFIG_DEBUG_LOCK_ALLOC

void range_read_lock_nested(struct range_lock_tree *tree,
			    struct range_lock *lock, int subclass)
{
	might_sleep();
	range_lock_acquire_read(&tree->dep_map, subclass, 0, _RET_IP_);

	RANGE_LOCK_CONTENDED(tree, lock, __range_read_trylock, __range_read_lock);
}
EXPORT_SYMBOL_GPL(range_read_lock_nested);

void _range_write_lock_nest_lock(struct range_lock_tree *tree,
				struct range_lock *lock,
				struct lockdep_map *nest)
{
	might_sleep();
	range_lock_acquire_nest(&tree->dep_map, 0, 0, nest, _RET_IP_);

	RANGE_LOCK_CONTENDED(tree, lock,
			     __range_write_trylock, __range_write_lock);
}
EXPORT_SYMBOL_GPL(_range_write_lock_nest_lock);

void range_write_lock_nested(struct range_lock_tree *tree,
			    struct range_lock *lock, int subclass)
{
	might_sleep();
	range_lock_acquire(&tree->dep_map, subclass, 0, _RET_IP_);

	RANGE_LOCK_CONTENDED(tree, lock,
			     __range_write_trylock, __range_write_lock);
}
EXPORT_SYMBOL_GPL(range_write_lock_nested);


int range_write_lock_killable_nested(struct range_lock_tree *tree,
				     struct range_lock *lock, int subclass)
{
	might_sleep();
	range_lock_acquire(&tree->dep_map, subclass, 0, _RET_IP_);

	if (RANGE_LOCK_CONTENDED_RETURN(tree, lock, __range_write_trylock,
					__range_write_lock_killable)) {
		range_lock_release(&tree->dep_map, _RET_IP_);
		return -EINTR;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(range_write_lock_killable_nested);

#endif
