/*
 * from linux-nova/fs/nova/balloc.c
 *
 */

#include <linux/fs.h>
#include <linux/f2fs_fs.h>
#include <linux/bitops.h>
#include "f2fs.h"
#include "node.h"
#include "segment.h"
#include "balloc.h"

int f2fs_alloc_block_free_lists(struct super_block *sb){
	struct f2fs_sb_info *sbi = F2FS_SB(sb);
	struct free_list *free_list;

	sbi->free_list = kcalloc(1, sizeof(struct free_list), GFP_KERNEL);

	if(!sbi->free_list)
		return -ENOMEM;

	free_list = sbi->free_list;
	free_list->block_free_tree = RB_ROOT;
	spin_lock_init(&free_list->s_lock);
	free_list->index = 0;

	return 0;
}

void f2fs_delete_free_lists(struct super_block *sb){
	struct f2fs_sb_info *sbi = F2FS_SB(sb);

	kfree(sbi->free_list);
	sbi->free_list = NULL;
}

static void f2fs_init_free_list(struct super_block *sb, struct free_list *free_list, int index){
	struct f2fs_sb_info *sbi = F2FS_SB(sb);
	unsigned long per_list_blocks;
	struct f2fs_sm_info *sm_info = sbi->sm_info;
	block_t main_blkaddr = sm_info->main_blkaddr;

	per_list_blocks = sbi->pmem_size;

	free_list->block_start = 0;
	free_list->block_end = per_list_blocks -1;

	free_list->block_start += main_blkaddr; // reserved for metadata

	f2fs_msg(sb, KERN_INFO, "f2fs_init_free_list: main_blkaddr = %u", main_blkaddr);
}

struct nova_range_node *f2fs_alloc_blocknode(struct super_block *sb){
	return f2fs_alloc_range_node(sb);
}

void f2fs_free_blocknode(struct nova_range_node *node){
	f2fs_free_range_node(node);
}

void f2fs_init_blockmap(struct super_block *sb, int recovery){
	struct f2fs_sb_info *sbi = F2FS_SB(sb);
	struct rb_root *tree;
	struct nova_range_node *blknode;
	struct free_list *free_list;
	int ret;

	free_list = sbi->free_list;
	tree = &(free_list->block_free_tree);
	f2fs_init_free_list(sb, free_list, 0);

	/* */
	if (recovery ==0){
		free_list->num_free_blocks = free_list->block_end - free_list->block_start +1;
		blknode = f2fs_alloc_blocknode(sb);

		if(blknode == NULL)
			BUG();

		blknode->range_low = free_list->block_start;
		blknode->range_high = free_list->block_end;
		ret = f2fs_insert_blocktree(tree, blknode);

		if(ret){
//			F2FS_MSG
			f2fs_free_blocknode(blknode);
			return;
		}
		free_list->first_node = blknode;
		free_list->last_node = blknode;
		free_list->num_blocknode =1;
	}

}

static inline int nova_rbtree_compare_rangenode(struct nova_range_node *curr,
		        unsigned long key, enum node_type type)
{
	if (type == NODE_DIR) {
		if (key < curr->hash)
			return -1;
		if (key > curr->hash)
			return 1;
		return 0;
	}

	/* Block and inode */
	if (key < curr->range_low)
		return -1;
	if (key > curr->range_high)
		return 1;
	
	return 0;
}

int f2fs_insert_range_node(struct rb_root *tree, struct nova_range_node *new_node, enum node_type type){
	struct nova_range_node *curr;
	struct rb_node **temp, *parent;
	int compVal;

	temp = &(tree->rb_node);
	parent = NULL;

	while(*temp){
		curr = container_of(*temp, struct nova_range_node, node);
		compVal = nova_rbtree_compare_rangenode(curr, new_node->range_low, type);

		parent = *temp;

		if(compVal == -1){
			temp = &((*temp)->rb_left);
		} else if(compVal ==1){
			temp = &((*temp)->rb_right);
		} else {
//			f2fs_msg
			return -EINVAL;
		}
	}
	rb_link_node(&new_node->node, parent, temp);
	rb_insert_color(&new_node->node, tree);

	return 0;
}

int f2fs_insert_blocktree(struct rb_root *tree, struct nova_range_node *new_node){
	int ret;

	ret = f2fs_insert_range_node(tree, new_node, NODE_BLOCK);
	
//	if(ret) f2fs_msg

	return ret;
}
	

static long f2fs_alloc_blocks_in_free_list(struct super_block *sb, struct free_list *free_list, unsigned short btype, enum alloc_type atype, unsigned long num_blocks, unsigned long *new_blocknr, enum nova_alloc_direction from_tail){

	struct rb_root *tree;
	struct nova_range_node *curr, *next=NULL, *prev=NULL;
	struct rb_node *temp, *next_node, *prev_node;
	unsigned long curr_blocks;
	bool found = 0;
	unsigned long step = 0;

	if( !free_list->first_node || free_list->num_free_blocks ==0){
		//f2fs_mgs
		return -ENOSPC;
	}

	//atype skip
	//
	
	tree = &(free_list->block_free_tree);

	if(from_tail == ALLOC_FROM_HEAD)
		temp = &(free_list->first_node->node);
	else
		temp = &(free_list->last_node->node);

	while(temp){
		step++;
		curr = container_of(temp, struct nova_range_node, node);

		curr_blocks = curr->range_high - curr->range_low + 1;

		if(num_blocks >= curr_blocks){
			/* superpage allocation must succeed */
			if(btype > 0 && num_blocks > curr_blocks)
				goto next;

			/* Otherwise, allocate the whole blocknode */
			if( curr == free_list->first_node) {
				next_node = rb_next(temp);
				if(next_node)
					next = container_of(next_node, struct nova_range_node, node);
				free_list->first_node = next;
			}
			
			if( curr == free_list->last_node) {
				prev_node = rb_prev(temp);
				if(prev_node)
					prev = container_of(prev_node, struct nova_range_node, node);
				free_list->last_node = prev;
			}

			rb_erase(&curr->node, tree);
			free_list->num_blocknode--;
			num_blocks = curr_blocks;
			*new_blocknr = curr->range_low;
			f2fs_free_blocknode(curr);
			found = 1;
			break;
		}

		/* Allocate partial blocknode */
		if(from_tail == ALLOC_FROM_HEAD) {
			*new_blocknr = curr->range_low;
			curr->range_low += num_blocks;
		}
		else{
			*new_blocknr = curr->range_high + 1 - num_blocks;
			curr->range_high -= num_blocks;
		}

//		nova_update_range_node_checksum(curr);
		found =1;
		break;
next:
		if(from_tail == ALLOC_FROM_HEAD)
			temp = rb_next(temp);
		else
			temp = rb_prev(temp);
	}

	if(free_list->num_free_blocks < num_blocks){
//		f2fs_mgs
		return -ENOSPC;
	}

	if(found == 1)
		free_list->num_free_blocks -= num_blocks;
	else {
		//f2fs_mgs
		return -ENOSPC;
	}

	return num_blocks;
}

int f2fs_new_blocks(struct super_block *sb, unsigned long *blocknr, unsigned int num, unsigned short btype, int zero, enum alloc_type atype, enum nova_alloc_direction from_tail){
	struct f2fs_sb_info *sbi = F2FS_SB(sb);
	struct free_list *free_list;
	void *bp;
	unsigned long num_blocks = 0;
	unsigned long new_blocknr = 0;
	long ret_blocks = 0;
	int retried = 0;
	struct timespec alloc_time;

	num_blocks = 1; //only needs 1 page for node

	free_list = sbi->free_list;

	spin_lock(&free_list->s_lock);

	ret_blocks = f2fs_alloc_blocks_in_free_list(sb, free_list, btype, atype, num_blocks, &new_blocknr, from_tail);

	f2fs_msg(sb, KERN_INFO, "f2fs_new_block: new_blocknr = %x", new_blocknr);

	if(ret_blocks > 0){
		free_list->alloc_data_count++;
		free_list->alloc_data_pages += ret_blocks;
	}

	spin_unlock(&free_list->s_lock);

	if(ret_blocks <= 0 || new_blocknr==0){
		return -ENOSPC;
	}

	if (zero){
		bp = (void*)new_blocknr;
		memset(bp, 0, PAGE_SIZE * ret_blocks);
	}

	*blocknr = new_blocknr;

	//f2fs_msg;
	//
	return ret_blocks;
}


