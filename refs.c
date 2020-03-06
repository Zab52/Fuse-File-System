#define FUSE_USE_VERSION 26

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef linux
/* For pread()/pwrite() */
#define _XOPEN_SOURCE 500
#endif
#include "refs.h"
#include <assert.h>
#include <strings.h>
#include <fuse.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/time.h>
#ifdef HAVE_SETXATTR
#include <sys/xattr.h>
#endif

// The first thing we do in init is open the "disk file".
// We use this backing file as our "disk" for all I/O in ReFS.
static int disk_fd;

// Statically allocate the main data structures in our file system.
// Global variables are not ideal, but these are the main pillars
// of our file system state.
// These in-memory representations will be kept consistent with the on-disk
// representation by frequently writing portions of them to disk_fd

static union superblock super;

static union inode *inode_table;

static struct bitmap *inode_bitmap;

static struct bitmap *data_bitmap;

/***********************/
/** Bitmap management **/
/***********************/

/**
 * Read a bitmap from disk into an in-memory representation.
 * @pre bmap points to an allocated struct bitmap, and bmap->n_bytes
 *      and bmap->n_valid_bytes are valid values
 * @return 0 on success, negative value on failure
 */
static int read_bitmap_from_disk(struct bitmap *bmap, lba_t bmap_start) {
	// Our bitmap's region of valid bits may not exactly align
	// with a"block boundary", but we must do block-aligned I/Os
	// when talking to our disk.  First "round up" our bitmap size
	// to the appropriate number of bytes, then make the I/O
	// request to read those blocks into our structure.
	size_t bmap_blocks = BYTES_TO_BLKS(bmap->n_bytes);
	int ret = read_blocks(bmap->bits, bmap_blocks, bmap_start);
	assert(ret == bmap_blocks);

	if (ret != bmap_blocks) {
		DEBUG_PRINT("Error reading bitmap (%d)", ret);
		return -EIO;
	}
	return 0;
}

/**
 * Write just the "bits" of our in-memory bitmap to disk
 * @pre bmap points to an allocated struct bitmap, and bmap->n_bytes
 *      and bmap->bits have valid values
 * @return 0 on success, negative value on failure
 */
static int write_bitmap_to_disk(struct bitmap *bmap, lba_t bmap_start) {
	size_t bmap_blocks = BYTES_TO_BLKS(bmap->n_bytes);
	int ret = write_blocks(bmap->bits, bmap_blocks, bmap_start);

	assert(ret == bmap_blocks);

	if (ret != bmap_blocks) {
		DEBUG_PRINT("Error writing bitmap (%d)", ret);
		return -EIO;
	}

	return 0;

}

/**
 * Utility function that writes the data region's bitmap in its entirety.
 */
static int sync_data_bitmap() {
	return write_bitmap_to_disk(data_bitmap, super.super.d_bitmap_start);
}

/**
 * Utility function that writes the inode table's bitmap in its entirety.
 */
static void sync_inode_bitmap() {
	write_bitmap_to_disk(inode_bitmap, super.super.i_bitmap_start);
}

/****************************/
/** Superblock management **/
/****************************/

static int write_super() {
	int ret = write_block(&super, SUPER_LBA);
	assert(ret == 1);
	return 0;
}


/** Inode / Inode Table management **/

/**
 * Write back the disk block that holds a given inode number.
 * Note that we write back more than just that inode, since we must
 * do I/O at the granularity of a 4096-byte disk block.
 */
static int write_inode(uint64_t inum) {
	// First, calcluate the offset of the block that contains this
	// inode. We know the start of the inode table (it's stored in
	// our super block) and we know the number of inodes per block.
	uint64_t itab_block = inum / INODES_PER_BLOCK;

	// Now, write the portion of the inode table contained in that block
	// to the corresponding lba
	return write_block(((void *)inode_table) + BLKS_TO_BYTES(itab_block),
			   super.super.i_table_start + itab_block);
}

/**
 * Write data to 4096-byte-aligned set of consecutive blocks
 * @param buf The memory buffer that has the data to be written
 * @param lba_count The number of blocks to write is (lba_count * 4096)
 * @param starting_lba The byte offset on our "disk" that we start writing
 *        is (starting_lba * 4096)
 * @return the number of blocks successfully written, or negative on error.
 */
ssize_t write_blocks(void *buf, size_t lba_count, off_t starting_lba) {
	int ret = pwrite(disk_fd, buf, BLKS_TO_BYTES(lba_count),
			 BLKS_TO_BYTES(starting_lba));
	if (ret < 0) {
		// pass along the error
		return ret;
	}

	// pwrite may return a partial success. If we don't write
	// a multiple of 4096 bytes, we have create a confusing situation.
	// it's not clear how to best handle this until we've built the rest
	// of our system
	if (BLOCK_OFFSET(ret) != 0) {
		// possibly handle incomplete write with retry?
		DEBUG_PRINT("partial write: write_blocks\n");
	}

	return BYTES_TO_BLKS(ret);
}

/**
 * Read data from a 4096-byte-aligned set of consecutive blocks
 * @param buf The memory buffer where we will store the data read
 * @param lba_count The number of blocks to read is (lba_count * 4096)
 * @param starting_lba The byte offset on our "disk" that we start reading
 *        is (starting_lba * 4096)
 * @return the number of blocks successfully read, or negative on error.

 */
ssize_t read_blocks(void *buf, size_t lba_count, off_t starting_lba) {
	int ret = pread(disk_fd, buf, BLKS_TO_BYTES(lba_count),
			BLKS_TO_BYTES(starting_lba));

	if (ret < 0) {
		// pass along the error
		return ret;
	}

	// pread may return partial success.
	// for read, the user could just ignore the data that
	// isn't "acknowleged", but we may want to revisit this
	// depending on our implementation
	if (BLOCK_OFFSET(ret) != 0) {
		// possibly handle incomplete read with retry?
		DEBUG_PRINT("partial write: write_blocks\n");
	}

	return BYTES_TO_BLKS(ret);
}

/**
 * Allocates an unused data block, and returns the lba of that data block
 * Since this requires knowing the data region start, we access the global
 * reference to the superblock.
 */
static lba_t reserve_data_block() {
	uint64_t index = first_unset_bit(data_bitmap);
	set_bit(data_bitmap, index);
	// convert our bit number to the actual LBA by adding the data
	// region's starting offset
	return super.super.d_region_start + index;
}


/**
 * Allocates an unused inode, and returns a pointer to that inode via an
 * index into the inode table (which is also the inode's number).
 */
static int reserve_inode() {
	uint64_t inum = first_unset_bit(inode_bitmap);
	set_bit(inode_bitmap, inum);
	inode_table[inum].inode.flags = INODE_IN_USE;
	inode_table[inum].inode.n_links = 0;
	inode_table[inum].inode.size = 0;
	inode_table[inum].inode.blocks = 0;
	return inum;
}

/**
 * allocates a new directory inode by setting the bitmap bits and
 * initializing the inode's type field in the inode_table
 */
static int reserve_dir_inode() {
	uint64_t inum = reserve_inode();
	inode_table[inum].inode.flags |= INODE_TYPE_DIR;
	inode_table[inum].inode.n_links = 2; // . and parent dir
	return inum;
}

/**
 * Called only once during FS initialization:
 *   allocates inode 0 for `/`
 *   creates the data block for the contents of `/` (. and ..)
 */
static int alloc_root_dir() {
	uint64_t inum = reserve_dir_inode();

	// if this is called from an "empty" fs, it should reserve
	// inode number 0.
	assert(inum == ROOT_INUM);

	lba_t data_lba = reserve_data_block();


	// The root dir is a special case, where '.' and '..' both refer to
	// itself. We also require / have inum 0 to simplify our FS layout

	union directory_block *root_data = malloc_blocks(1);

	// init "."
	root_data->dirents[0].inum = ROOT_INUM;
	root_data->dirents[0].is_valid = 1;
	strcpy(root_data->dirents[0].path, ".");
	root_data->dirents[0].path_len = strlen(root_data->dirents[0].path);

	// init ".."
	root_data->dirents[1].inum = ROOT_INUM;
	root_data->dirents[1].is_valid = 1;
	strcpy(root_data->dirents[1].path, "..");
	root_data->dirents[1].path_len = strlen(root_data->dirents[1].path);

	// update the inode's first direct pointer to point to this data
	inode_table[inum].inode.size = 4096;
	inode_table[inum].inode.blocks = 1;
	inode_table[inum].inode.block_ptrs[0] = data_lba;

	// write the directory contents to its data block
	write_block(root_data, data_lba);

	// write the data bitmap
	sync_data_bitmap();

	// write back the inode
	write_inode(inum);

	// write back the inode bitmap
	sync_inode_bitmap();

	return 0;
}

static void release_inode(struct refs_inode *ino) {
	assert(ino->flags & INODE_IN_USE);
	assert(ino->n_links == 0);
	
	ino->flags = INODE_FREE;
	ino->n_links = 0;
	ino->size = 0;
	ino->blocks = 0;
	clear_bit(inode_bitmap, ino->inum);
	// don't bother"clearing" the block pointers because this inode is
	// logically free; future code should never interpret their values
}



static void alloc_inode_table(int num_inodes) {
	size_t itable_bytes = BLKS_TO_BYTES((num_inodes + (INODES_PER_BLOCK - 1)) / INODES_PER_BLOCK);

	inode_table = malloc(itable_bytes);
	assert(inode_table != NULL);

	bzero(inode_table, itable_bytes);

	// initialize each inode
	for (uint64_t i = 0; i < DEFAULT_NUM_INODES; i++) {
		inode_table[i].inode.inum = i;
		inode_table[i].inode.flags = INODE_FREE;
	}
}

static void dump_super(struct refs_superblock *sb) {
	printf("refs_superblock:\n");
	printf("\tblock_size: %"PRIu32"\n", sb->block_size);
	printf("\tnum_inodes: %"PRIu64"\n", sb->num_inodes);
	printf("\timap_start:%"LBA_PRINT"\n", sb->i_bitmap_start);
	printf("\titab_start:%"LBA_PRINT"\n", sb->i_table_start);
	printf("\tnum_d_blks:%"PRIu64"\n", sb->num_data_blocks);
	printf("\tdmap_start:%"LBA_PRINT"\n", sb->d_bitmap_start);
	printf("\tdreg_start:%"LBA_PRINT"\n", sb->d_region_start);
}

static void init_super(struct refs_superblock *sb) {
	// Disk Layout:
	// super | imap | ...inode table... | dmap | ...data blocks... |

	sb->block_size = BLOCK_SIZE;
	sb->num_inodes = DEFAULT_NUM_INODES;
	sb->num_data_blocks = DEFAULT_NUM_DATA_BLOCKS;

	size_t imap_bytes = BLOCK_ROUND_UP(sb->num_inodes);
	lba_t imap_blocks = BYTES_TO_BLKS(imap_bytes);

	lba_t itab_blocks = (DEFAULT_NUM_INODES) / (INODES_PER_BLOCK);

	size_t dmap_bytes = BLOCK_ROUND_UP(sb->num_data_blocks);
	lba_t dmap_blocks = BYTES_TO_BLKS(dmap_bytes);

	sb->i_bitmap_start = 1;
	sb->i_table_start = sb->i_bitmap_start + imap_blocks;
	sb->d_bitmap_start = sb->i_table_start + itab_blocks;
	sb->d_region_start = sb->d_bitmap_start + dmap_blocks;
}

static void* refs_init(struct fuse_conn_info *conn) {
	int ret = 0;

	// check whether we need to initialize an empty file system
	// or if we can populate our existing file system from "disk"
	if (access(DISK_PATH, F_OK) == -1) {
		// In this cond branch, we don't have an existing "file
		// system" to start from, so we initialize one from scratch.
		// Typically, a separate program would do this (e.g., mkfs)
		// but this is not a typical file system...
		
		printf("creating new disk\n");
		
		// First, create a new "disk" file from scratch
		disk_fd = open(DISK_PATH, O_CREAT | O_EXCL | O_SYNC | O_RDWR,
			       S_IRUSR | S_IWUSR);
		assert(disk_fd > 0);
		

		// extend our "disk" file to 10 MiB (per lab spec)
		ret = ftruncate(disk_fd, 10*1024*1024);
		assert(ret >= 0);

		// now initialize the "empty" state of all of our data
		// structures in memory, then write that state to disk
		
		init_super(&super.super);
		dump_super(&super.super);

		inode_bitmap = allocate_bitmap(super.super.num_inodes);
		assert(inode_bitmap != NULL);
		
		data_bitmap = allocate_bitmap(super.super.num_data_blocks);
		assert(inode_bitmap != NULL);

		// allocate our inode table memory and populate initial vals
		alloc_inode_table(DEFAULT_NUM_INODES);
		
		// allocate inode for `/`, create the directory with . and ..
		alloc_root_dir();
		
		// write superblock
		write_super();
		
		// done! now we have all of our metadata initialized and
		// written, and we can reinitialize our file system from
		// this on-disk state in future runs.
	} else {
		// In this cond. branch, we have already created an instance
		// of ReFS. Based on the saved state of our FS, we initialize
		// the in-memory state so that we can pick up where we left
		// off.

		// Step 1: open disk and read the superblock
		
		// Since super is statically allocated, we don't need
		// to do any memory allocation with malloc; we just
		// need to populate its contents by reading them from
		// "disk".  And since we need to access fields in our
		// super in order to know the sizes of our other
		// metadata structures, we read super first.
		
		disk_fd = open(DISK_PATH, O_SYNC | O_RDWR);
		assert(disk_fd > 0);
		
		// read superblock
		ret = read_block(&super, 0);
		dump_super(&super.super);

		
		// Step 2: allocate our other data structures in memory
		// and read from "disk" to populate your data structures

		// bitmaps
		inode_bitmap = allocate_bitmap(super.super.num_inodes);
		assert(inode_bitmap != NULL);
		
		data_bitmap = allocate_bitmap(super.super.num_data_blocks);
		assert(inode_bitmap != NULL);

		read_bitmap_from_disk(inode_bitmap,
				      super.super.i_bitmap_start);
		dump_bitmap(inode_bitmap);
		
		read_bitmap_from_disk(data_bitmap,
				      super.super.d_bitmap_start);
		dump_bitmap(data_bitmap);
		
		//inode table
		alloc_inode_table(super.super.num_inodes);
		ret = read_blocks(inode_table,
				  super.super.num_inodes / INODES_PER_BLOCK,
				  super.super.i_table_start);
		assert(ret == super.super.num_inodes / INODES_PER_BLOCK);
	}

	// before returning you should have your in-memory data structures
	// initialized, and your file system should be able to handle any
	// implemented system call

	printf("done init\n");
	return NULL;
}

// You should implement the functions that you need, but do so in a
// way that lets you incrementally test.
static struct fuse_operations refs_operations = {
	.init		= refs_init,
/*
	.getattr	= NULL,
	.fgetattr	= NULL,
	.access		= NULL,
	.readlink	= NULL,
	.readdir	= NULL,
	.mknod		= NULL,
	.create		= NULL,
	.mkdir		= NULL,
	.symlink	= NULL,
	.unlink		= NULL,
	.rmdir		= NULL,
	.rename		= NULL,
	.link		= NULL,
	.chmod		= NULL,
	.chown		= NULL,
	.truncate	= NULL,
	.utimens	= NULL,
	.open		= NULL,
	.read		= NULL,
	.write		= NULL,
	.statfs		= NULL,
	.release	= NULL,
	.fsync		= NULL,
#ifdef HAVE_SETXATTR
	.setxattr	= NULL,
	.getxattr	= NULL,
	.listxattr	= NULL,
	.removexattr	= NULL,
#endif
*/
};



int main(int argc, char *argv[]) {
	INODE_SIZE_CHECK;
	DIRENT_SIZE_CHECK;
	umask(0);
	return fuse_main(argc, argv, &refs_operations, NULL);
}
