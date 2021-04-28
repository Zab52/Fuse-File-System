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
#include <libgen.h>
#ifdef HAVE_SETXATTR
#include <sys/xattr.h>
#endif

#define BLOCKS_PER_BLOCK (BLOCK_SIZE) / sizeof(lba_t)

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

	struct fuse_context * ctx = fuse_get_context();

	inode_table[inum].inode.uid = ctx->uid;
	inode_table[inum].inode.gid = ctx->gid;
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

static int reserve_regfile_inode(){
	uint64_t inum = reserve_inode();
	inode_table[inum].inode.flags |= INODE_TYPE_REG;
	inode_table[inum].inode.n_links = 1;
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
	bzero(root_data, BLOCK_SIZE);

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
	inode_table[inum].inode.mode = 0777;

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

/*
// This function deallocates each of the global variables stored within the FS,
// and closes the file that is acting as the harddrive.
*/
static void refs_destroy(void *conn){

	free(inode_table);
	free_bitmap(inode_bitmap);
	free_bitmap(data_bitmap);

	close(disk_fd);
}

/*
// A helper function that finds a child directory of a given inode by using
// the path name of the child directory. This function then returns the
// inode number to the child directory, or -1 if the child isn't found.
*/
int find_child_inum(char * path, struct refs_inode* ino, int *childInum){
	union directory_block blk;

	// Iterating through each of the blocks connecting to the inode
	for (int i = 0; i < ino->blocks; i++) {
		read_block(&blk, ino->block_ptrs[i]);

		// Iterating through each of the directory entries in each block.
		for (int j = 0; j < DIRENTS_PER_BLOCK; j++) {
			struct dir_entry *dir = &blk.dirents[j];

			// Checking to make sure the found directory is valid.
			if (dir != NULL && dir->is_valid){

				// Case where the found directory has the desired path name.
				if (!strcmp(dir->path, path)) {
					*childInum = dir->inum;
					return 0;
				}
			}
		}
	}
	return -ENOENT;
}

/*
// Find the child inumber for a path given the entire path, not just the
// child path, and stores it at childInum. Returns zero on success. Otherwise
// returns the oppropriate error code.
*/
int find_child_inum_abspath(char * path, struct refs_inode * ino, int *childInum){
	char * dup = strdup(path);

	if(dup ==  NULL){
		return -ENOMEM;
	}

	char * base = basename(dup);

	int ret = find_child_inum(base, ino, childInum);

	free(dup);

	return ret;
}

/*
// Gets the inode number for a path, and stores it in inum. Returns
// 0 on success. Otherwise returns the appropriate error code.
*/
int get_path_inum(char * path, int * inum){

	struct refs_inode * current;

	char * dup = strdup(path);

	// Tokenizing the path name.
  char delem[2] = "/";
	char * temp = strtok(dup + 1, delem);

  * inum = 0;

	// Searching through each inode starting from the root for the next token.
	while(temp != NULL){
		current = (struct refs_inode *) &(inode_table[*inum]).inode;

		// Case where an inode on the path isn't a directory inode.
		if(!(current->flags & INODE_TYPE_DIR)){
			return -ENOTDIR;
		}

		// Finding the inode number of the next inode in the path.
		int ret = find_child_inum(temp, current, inum);

		// Case where the inode isn't found.
		if(ret){
			return -ENOENT;
		}

		temp = strtok(NULL, delem);
	}

	return 0;
}

/*
// A helper function that finds the parent inode given a directory specified
// by a path. This function then returns the inode number of this parent inode.
// Returns -1 if the parent inode isn't found.
*/
int get_parent_inum(char * path, int * inum){
	char * dup = malloc(sizeof(char) * strlen(path));
	strcpy(dup, path);

	// Retrieving the parent directories path.
	char * parentPath = dirname(dup);

	int ret =  get_path_inum(parentPath, inum);
	free(dup);
	return ret;
}


/*
// Given a path, this function finds the paths corresponding inode, and then
// stores the inode's properties in a struct stat. This function returns 0
// on success, and otherwise returns -1.
*/
static int refs_getattr(const char * path, struct stat* stbuf){

	struct refs_inode* res;

	int inum = 0;
	int ret = 0;

	// Getting the inode number of the path.
	ret = get_path_inum((char *) path, &inum);

	if (ret != 0) {
		return ret;
	}

	// Getting the desired inode.
	res = (struct refs_inode*) &(inode_table[inum]).inode;

	// Storing the inode information in the struct stat.
	stbuf->st_nlink = res->n_links;
	stbuf->st_blocks = res->blocks;

	if(res->flags & INODE_TYPE_DIR){
		stbuf->st_mode = res->mode | S_IFDIR;
	} else if (res->flags & INODE_TYPE_REG){
		stbuf->st_mode = res->mode | S_IFREG;
	} else{
		printf("Unknown File type");
	}

	stbuf->st_size = res->size;
	stbuf->st_dev = 0x1234;
	stbuf->st_ino = res->inum;

	return 0;
}

/*
// This function takes in a path, and a mask, and then checks to separate
// if the directory specified by path has the desired mask permission.
// This function returns zero if the directory has the desired mask permission.
// Otherwise, returns -1.
*/
static int refs_access(const char *path, int mask){

	int ret;

	int inum;

	ret = get_path_inum((char *) path, &inum);

	// Returning possible errors.
	if(ret){
		return ret;
	}

	// Returning true since we know file must already exist.
	if (mask == F_OK){
		return 0;
	}

	struct fuse_context *ctx = fuse_get_context();

	int allowed_mode = 0;

	// user matches
	if (ctx->uid == inode_table[inum].inode.uid) {
		allowed_mode |= inode_table[inum].inode.mode & S_IRUSR ?
			R_OK : 0;
		allowed_mode |= inode_table[inum].inode.mode & S_IWUSR ?
			W_OK : 0;
		allowed_mode |= inode_table[inum].inode.mode & S_IXUSR ?
			X_OK : 0;
	}

	// group matches
	if (ctx->gid == inode_table[inum].inode.gid) {
		allowed_mode |= inode_table[inum].inode.mode & S_IRGRP ?
			R_OK : 0;
		allowed_mode |= inode_table[inum].inode.mode & S_IWGRP ?
			W_OK : 0;
		allowed_mode |= inode_table[inum].inode.mode & S_IXGRP ?
			X_OK : 0;
	}

	// always check "other"
	allowed_mode |= inode_table[inum].inode.mode & S_IROTH ? R_OK : 0;
	allowed_mode |= inode_table[inum].inode.mode & S_IWOTH ? W_OK : 0;
	allowed_mode |= inode_table[inum].inode.mode & S_IXOTH ? X_OK : 0;


	// confirm that if they ask for each permission, they are allowed to perform
	// said permission.
	if ((mask & R_OK) && !(allowed_mode & R_OK))
		    return -EACCES;
	if ((mask & W_OK) && !(allowed_mode & W_OK))
		    return -EACCES;
	if ((mask & X_OK) && !(allowed_mode & X_OK))
		    return -EACCES;

	return 0;
}

/*
// Updates a parent directory inode to store a block pointer to a
// child inode.
*/
static int update_parent_inode(int childInum, int parentInum, char * childPath){

	struct refs_inode* parent = (struct refs_inode *) &(inode_table[parentInum]).inode;

	union directory_block blk;
	bzero(&blk, BLOCK_SIZE);

	// Updating the number of links for the parent inode.
	parent->n_links += 1;

	// Looking for an open directory in the currently allocated blocks.
	for(int j = 0; j < parent->blocks; j++){
		read_block(&blk, parent->block_ptrs[j]);
		for(int n = 0; n < DIRENTS_PER_BLOCK; n++){
			struct dir_entry *dir = &blk.dirents[n];
			if(!dir->is_valid){
				dir->is_valid = 1;
				dir->inum = childInum;
				strcpy(dir->path, childPath);
				dir->path_len = strlen(dir->path);
				write_block(&blk, parent->block_ptrs[j]);
				write_inode(parentInum);
				return 0;
			}
		}
	}

	// Creating a new directory block if required.
	if(parent->blocks < NUM_DIRECT){
		parent->blocks += 1;
		parent->size += 4096;

		lba_t data_lba = reserve_data_block();
		bzero(&blk, BLOCK_SIZE);

		blk.dirents[0].inum = childInum;
		blk.dirents[0].is_valid = 1;
		strcpy(blk.dirents[0].path, childPath);
		blk.dirents[0].path_len = strlen(blk.dirents[0].path);

		write_block(&blk, data_lba);
		sync_data_bitmap();
		write_inode(parentInum);
		return 0;
	}

	// Case where we cannot allocate any more directory blocks
	// for the parent inode.
	parent->n_links -= 1;
	write_inode(parentInum);
	return -ENOSPC;
}

/*
// This function creates a new directory at the specified path location, setting
// the mode to mode. If succesful, this function returns 0. Otherwise,
// returns -1.
*/
static int refs_mkdir(const char* path, mode_t mode){

	char * dup = strdup(path);
	char * temp = dirname(dup);

	// Case where parent directory doesn't have write mode.
	if(refs_access(temp, W_OK)){
		free(dup);
		return -EACCES;
	}

	int parentInum = 0;
	int ret = 0;

	// Finding the parent inode number of the path,
	ret = get_parent_inum((char *) path, &parentInum);

	// Case where path is invalid.
	if(ret != 0){
		free(dup);
		return ret;
	}

	struct refs_inode* parent = (struct refs_inode *) &(inode_table[parentInum]).inode;

	// Finding the path to be stored in the new directory.
	strcpy(dup, path);

	char * childPath = basename(dup);

	// Case where the filename of the new directory is too long.
	if(strlen(childPath) > MAX_PATH_LEN){
		free(dup);
		return -ENAMETOOLONG;
	}

	int childInum = 0;
	ret = find_child_inum(childPath, parent, &childInum);

	// Checking to see if the directory file already exists.
	if(ret == 0){
		free(dup);
		return -EEXIST;
	}

	// Reserving space for the new directory.
	childInum = reserve_dir_inode();
	lba_t data_lba = reserve_data_block();
	union directory_block dir_data;
	bzero(&dir_data, BLOCK_SIZE);

	// init "." for the new directory.
	dir_data.dirents[0].inum = childInum;
	dir_data.dirents[0].is_valid = 1;
	strcpy(dir_data.dirents[0].path, ".");
	dir_data.dirents[0].path_len = strlen(dir_data.dirents[0].path);

	// init ".." for the new directory.
	dir_data.dirents[1].inum = parentInum;
	dir_data.dirents[1].is_valid = 1;
	strcpy(dir_data.dirents[1].path, "..");
	dir_data.dirents[1].path_len = strlen(dir_data.dirents[1].path);

	// update the inode's direct pointer to point to itself.
	inode_table[childInum].inode.size = 4096;
	inode_table[childInum].inode.blocks = 1;
	inode_table[childInum].inode.block_ptrs[0] = data_lba;
	inode_table[childInum].inode.mode = mode;

	// Saving changes to the filesystem.
	write_block(&dir_data, data_lba);
	sync_data_bitmap();
	write_inode(childInum);
	sync_inode_bitmap();

	ret = update_parent_inode(childInum, parentInum, childPath);

	free(dup);

	return ret;
}

/*
// The function reads through a directory specified by path, and then stores
// the name of the children entries in the buffer buf. This function
// returns 0 if succesful, otherwise returns -1. In this case, the offset
// determines the number of valid directories that aren't read. In this case,
// this will be the first offset directories found by just iterating through
// the directory blocks of the path inode.
*/
static int refs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			 off_t offset, struct fuse_file_info *fi) {
	int inum = 0;
	int ret = 0;

	// Finding the parent inode number of the path.
	ret = get_path_inum((char *) path, &inum);

	// Case where the path isn't valid.
	if(ret != 0){
		return ret;
	}

	// Getting the inode of the path.
	struct refs_inode* res = (struct refs_inode *) &inode_table[inum].inode;

	// Stores the number of found valid directories.
	off_t num = 0;

	int pathlen = strlen(path);

	union directory_block blk;
	bzero(&blk, sizeof(union directory_block));

	// Proceeding through each block of the path inode.
	for (int i = 0; i < res->blocks; i++) {
		read_block(&blk, res->block_ptrs[i]);

		// Considering each possible entry in each block.
		for (int j = 0; j < DIRENTS_PER_BLOCK; j++) {
			struct dir_entry *dir = &blk.dirents[j];

			// Checking that the dirtory entry found is valid.
			if (dir->is_valid){
					if(num > offset){
						int subpathlen = strlen(dir->path);

						char *appendedFilename = malloc(sizeof(char) * (pathlen + subpathlen + 1));
						strncpy(appendedFilename, path, pathlen);
						strncpy(appendedFilename + pathlen, dir->path, subpathlen + 1);

						struct stat info;
						refs_getattr(appendedFilename, &info);

						// Using filler function to add the path names to the buffer.
						if (filler(buf, dir->path, &info, num + 1)) {
							free(appendedFilename);
							return 0;
						}

						free(appendedFilename);
					}
					// Incrementing the number of found valid directories.
					num++;
				}
		}
	}

	return 0;
}

/*
// This function fills in a string starting at offset being, and ending at
// end-1 with the character stored in thing.
*/
static void fill(char *str, int begin, int end, char thing) {
	for (int i = begin; i < end; i++) {
		str[i] = thing;
	}
}

/*
// This function removes the last block within a file by unsetting
// the bit for that data block.
*/
void file_remove_data_block(struct refs_inode * fileInode){

	// Next block to be removed.
	int next = fileInode->blocks - 1;

	// Case where we are removing a direct block.
	if(next<NUM_DIRECT){
		clear_bit(data_bitmap, fileInode->block_ptrs[next] - super.super.d_region_start);
	} else{
		// Case where we are removing a block in an indirect block.
		int index = ((next - NUM_DIRECT) / NUM_INDIRECT_POINTERS) + NUM_DIRECT;
		int offset = (next - NUM_DIRECT) % NUM_INDIRECT_POINTERS;

		char * blk[BLOCK_SIZE];

		// Reading in the indirect block.
		read_block(blk, fileInode->block_ptrs[index]);

		// Removing the block within the indirect block.
		lba_t data_lba = ((lba_t *)blk)[offset];
		clear_bit(data_bitmap, data_lba - super.super.d_region_start);

		// Case where an indirect block now contains zero block pointers. As such,
		// we also remove this indirect block.
		if(offset == 0){
			clear_bit(data_bitmap, fileInode->block_ptrs[index] - super.super.d_region_start);
		}
	}

	// Updating the number of blocks in the inode.
	fileInode->blocks -=1;
	write_inode(fileInode->inum);
	return;
}

/*
// Reserves an empty new block for a file.
*/
void file_reserve_data_block(struct refs_inode * fileInode){
	lba_t data_lba = reserve_data_block();

	// Next block to be allocated.
	int next = fileInode->blocks;

	char blk[BLOCK_SIZE];
	bzero(blk, BLOCK_SIZE);

	write_block(blk, data_lba);

	// Case where we are reserving a direct block.
	if(next < NUM_DIRECT){
		fileInode->block_ptrs[next] = data_lba;
	} else{
		// Case where we are reserving a block in an indirect block.
		int index = ((next - NUM_DIRECT) / NUM_INDIRECT_POINTERS) + NUM_DIRECT;
		int offset = (next - NUM_DIRECT) % NUM_INDIRECT_POINTERS;

		// Case where we need to add a new indirect block to store more blocks.
		if(offset == 0){
			lba_t data_lba_indirect = reserve_data_block();
			fileInode->block_ptrs[index] = data_lba_indirect;
		}

		// Storing the new block within the indirect block.
		read_block(blk, fileInode->block_ptrs[index]);
		((lba_t *) blk)[offset] = data_lba;
		write_block(blk, fileInode->block_ptrs[index]);
	}

	// Updating the number of blocks in the inode.
	fileInode->blocks += 1;
	write_inode(fileInode->inum);
	return;
}

/*
// Function to read a specific block from a file.
*/
void file_read_block(char * blk, struct refs_inode* fileInode, int blockNum){
	// Case where we are reading a direct block.
	if(blockNum < NUM_DIRECT){
		read_block(blk, fileInode->block_ptrs[blockNum]);
	} else {
		// Case where we are reading from an indirect block.
		int index = ((blockNum - NUM_DIRECT) / NUM_INDIRECT_POINTERS) + NUM_DIRECT;
		int offset = (blockNum - NUM_DIRECT) % NUM_INDIRECT_POINTERS;

		char blk2[BLOCK_SIZE];

		read_block(blk2, fileInode->block_ptrs[index]);

		read_block(blk, ((lba_t *) blk2)[offset]);
	}
	return;
}

/*
// Function to write to a specific block within a file.
*/
void file_write_block(char * blk, struct refs_inode* fileInode, int blockNum){
	// Case where we are writing to a direct block.
	if(blockNum < NUM_DIRECT){
		write_block(blk, fileInode->block_ptrs[blockNum]);
	} else {
		// Case where we are writing to a block in an indirect block.
		int index = ((blockNum - NUM_DIRECT) / NUM_INDIRECT_POINTERS) + NUM_DIRECT;
		int offset = (blockNum - NUM_DIRECT) % NUM_INDIRECT_POINTERS;

		char blk2[BLOCK_SIZE];

		read_block(blk2, fileInode->block_ptrs[index]);

		write_block(blk, ((lba_t *) blk2)[offset]);
	}
	return;
}

/*
// This function truncates a file specified by path to
// be the inputted size. Returns 0 on success. Otherwise returns the
// appropriate error code.
*/
static int refs_truncate(const char *path, off_t size) {

	struct refs_inode *ino;
	int ret = 0;
	int inum = 0;

	// Case where file doesn't have write mode.
	if(refs_access(path, W_OK)){
		return -EACCES;
	}

	// Getting the parent inode number of the path.
	ret = get_path_inum((char *) path, &inum);

	// Case where inodes doesn't exist for the parent path.
	if (ret) {
		return ret;
	}

	ino = (struct refs_inode *) &(inode_table[inum]).inode;

	// Case where path doesn't refer to a file.
	if(!(ino->flags & INODE_TYPE_REG)){
		return -EINVAL;
	}

	// Case where no truncation is required.
	if (ino->size == size) {
		return 0;
	}

	char blk[BLOCK_SIZE];

	int firstBlock;
	int lastBlock;
	int byteOffsetBegin;

	// Case where we are growing the file.
	if (ino->size < size) {
		firstBlock = ino->size / BLOCK_SIZE;
		lastBlock = size / BLOCK_SIZE;

		// Checking to see if we are trying to make the file too large.
		if(lastBlock >= MAX_FILE_BLOCKS){
			return -EFBIG;
		}

		// Checking to see if our file is empty or not. If it is, we reserve it
		// a data block.
		if(ino->size ==0){
			file_reserve_data_block(ino);
		}

		byteOffsetBegin = ino->size % BLOCK_SIZE;

		// Extending the size of the file.
		file_read_block(blk, ino, firstBlock);
		fill(blk, byteOffsetBegin + 1, BLOCK_SIZE, '0');
		file_write_block(blk, ino, firstBlock);

		for(int i = firstBlock+1; i <= lastBlock; i++){
			file_reserve_data_block(ino);
		}
	} else{
		// Case where we are shrinking the file.

		firstBlock = size / BLOCK_SIZE;
		lastBlock = ino->size / BLOCK_SIZE;

		byteOffsetBegin = size % BLOCK_SIZE;

		// Zeroing out entries removed within the block that will be
		// the new last block of the file after truncation.
		file_read_block(blk, ino, firstBlock);
		fill(blk, byteOffsetBegin + 1, BLOCK_SIZE, '0');
		file_write_block(blk, ino, firstBlock);

		// Removing each of the blocks that are being completely removed.
		for(int i = firstBlock+1; i <= lastBlock; i++){
			file_remove_data_block(ino);
		}
	}

	// Updating the size stored by the inode.
	ino->size = size;
	write_inode(inum);

	return 0;
}


/*
// This function writes to a file specified by path. This function writes
// a specified string of an inputted size at an offset within the file.
// Returns 0 on succes. Otherwise returns the appropriate error code.
*/
static int refs_write(const char *path, const char *buf, size_t size,
		     off_t offset, struct fuse_file_info *fi)
{

	struct refs_inode *ino;

	int ret = 0;
	int inum = 0;

	// Case where file doesn't have write mode.
	if(refs_access(path, W_OK)){
		return -EACCES;
	}

	// Getting the parent inode number of the path.
	ret = get_path_inum((char *) path, &inum);

	// Case where inodes doesn't exist for the parent path.
	if (ret) {
		return ret;
	}

	ino = (struct refs_inode *) &(inode_table[inum]).inode;

	// Case where path doesn't refer to a file.
	if(!(ino->flags & INODE_TYPE_REG)){
		return -EINVAL;
	}

	// Case where we are increasing the size of the file.
	if (size + offset > ino->size) {
		ret = refs_truncate(path, size + offset);
	}

	// Case where truncate failed.
	if(ret){
		return ret;
	}

	int firstBlock = offset / BLOCK_SIZE;
	int lastBlock = (size + offset) / BLOCK_SIZE;

	int byteOffsetBegin = offset % BLOCK_SIZE;

	char blk[BLOCK_SIZE];

	// Case where the write starts and ends in the same file.
	if(firstBlock == lastBlock){
		file_read_block(blk, ino, firstBlock);
		memcpy(blk+byteOffsetBegin, buf, size);
		file_write_block(blk, ino, firstBlock);
	} else {
		// Case where the write starts and ends in different files.

		// Writing to the first block being changed.
		file_read_block(blk, ino, firstBlock);
		memcpy(blk + byteOffsetBegin, buf, BLOCK_SIZE-byteOffsetBegin);
		file_write_block(blk, ino, firstBlock);

		int written = BLOCK_SIZE-byteOffsetBegin;

		// Writing to each of the middle blocks being changed.
		for (int i = firstBlock+1; i < lastBlock; i++) {
			memcpy(blk, buf + written, BLOCK_SIZE);
			file_write_block(blk, ino, i);
			written += BLOCK_SIZE;
		}

		// Writing to the last block being changed.
		file_read_block(blk, ino, lastBlock);
		memcpy(blk, buf + written, size-written);
		file_write_block(blk, ino, lastBlock);
	}

	return size;
}

/*
// Helper function to determine whether or not a directory is empty. Returns
// 1 if empty. Otherwise returns 0.
*/
static int dir_is_empty(struct refs_inode *dir){
	union directory_block data;

	// Iterating through each block in the directory.
	for (int i = 0; i < dir->blocks; i++){
		read_block(&data, dir->block_ptrs[i]);

		int n = 0;

		// Skipping first two entries containing '.' and '..'.
		if(i == 0){
			n = 2;
		}

		// Searching for any valid entries.
		while(n < DIRENTS_PER_BLOCK){
				if(data.dirents[n].is_valid){
						return 0;
				}
				n++;
		}
	}

	// No valid entries found, implying that the directory is empty.
	return 1;
}

/*
// Given a parent inode, a child inode, and a path, removes the correpsponding
// entry in the parent inode for that child, and then updates both inodes.
*/
static int remove_child(struct refs_inode *parent, struct refs_inode * child, char * path){

	char * dup = strdup(path);

	if(dup == NULL){
		return -ENOMEM;
	}

	// Getting the chlid path.
	char * base = basename(dup);

	union directory_block current;
	bzero(&current, BLOCK_SIZE);

	// Searching through each of the blocks in the parent directory.
	for(int i = 0; i < parent->blocks; i++){
		read_block(&current, parent->block_ptrs[i]);

		// Searching through each entry in a block for the specified path.
		for(int n = 0; n < DIRENTS_PER_BLOCK; n++){

			// Case where we found the entry to be removed.
			if(current.dirents[n].is_valid &&
					(strcmp(base, current.dirents[n].path) == 0)){

				// Invalidating the entry in the parent.
				current.dirents[n].is_valid = 0;
				write_block(&current, parent->block_ptrs[i]);

				free(dup);

				// Updating both inodes link count.
				parent->n_links--;
				write_inode(parent->inum);
				child->n_links--;
				write_inode(child->inum);

				return 0;
			}
		}
	}

	// Case where child path isn't found.
	free(dup);
	return -ENOENT;
}

/*
// Given a path to a directory, and the inode numbers for the both the
// directory and its parent, this function removes the directory from the filename
// system if the directory is empty. Returns 0 on success. Otherwise returns
// the appropriate error code.
*/
static int do_rmdir(char * path, int parentInum, int childInum){

	struct refs_inode * parentInode = &inode_table[parentInum].inode;
	struct refs_inode * childInode = &inode_table[childInum].inode;

	// Confirming that the child directory is empty.
	if(!dir_is_empty(childInode)){
		return -ENOTEMPTY;
	}

	// Removing the child from the parent directory
	int ret = remove_child(parentInode, childInode, path);

	if(ret){
		return ret;
	}

	// Updating the inode bitmap to remove the directory.
	clear_bit(inode_bitmap, childInum);

	// Updating the data bitmap to free the data used by the directory.
	for(int i = 0; i < childInode->blocks; i++){
			clear_bit(data_bitmap, childInode->block_ptrs[i] - super.super.d_region_start);
	}

	childInode->n_links = 0;

	// Storing each change to our 'filesystem'.
	release_inode(childInode);
	sync_data_bitmap();
	sync_inode_bitmap();
	write_inode(childInum);

	return 0;
}

/*
// Given a path, this function removes the directory at the end of the path.
// Returns 0 on success. Otherwise returns the appropriate error code.
*/
static int refs_rmdir(const char * path){
	int ret = 0;
	int parentInum = 0;
	int childInum = 0;

	// Checking to make sure filesystem can reach the parent directory.
	ret = get_parent_inum((char *) path, &parentInum);
	if(ret != 0){
		return ret;
	}

	// Checking to make sure target directory exists.
	struct refs_inode * parentInode = &inode_table[parentInum].inode;
	ret = find_child_inum_abspath((char *) path, parentInode, &childInum);
	if(ret){
		return -ENOENT;
	}

	// Checking to make sure path refers to a directory.
	struct refs_inode *dir_inode = &inode_table[childInum].inode;
	if (!(dir_inode->flags & INODE_TYPE_DIR)) {
		return -ENOTDIR;
	}

	return do_rmdir((char *) path, parentInum, childInum);
}


/*
// This function creates a file at the specified path location.
// Returns zero on success. Otherwise returns the appropriate error code.
*/
static int refs_mknod(const char * path, mode_t mode, dev_t dev){
	char * dup = strdup(path);

	if(dup == NULL){
		return -ENOMEM;
	}

	char * temp = dirname(dup);

	// Case where parent directory doesn't have write permissions.
	if(refs_access(temp, W_OK)){
		free(dup);
		return -EACCES;
	}

	// Finding the parent inode number of the path,
	int parentInum = 0;
	int ret = get_parent_inum((char *) path, &parentInum);

	// Case where path is invalid.
	if(ret  != 0){
		free(dup);
		return ret;
	}

	struct refs_inode* parent = (struct refs_inode *) &(inode_table[parentInum]).inode;

	// Finding the path to be stored in the new directory.
	strcpy(dup, path);

	char * childPath = basename(dup);

	// Case where the filename of the new directory is too long.
	if(strlen(childPath) > MAX_PATH_LEN){
		free(dup);
		return -ENAMETOOLONG;
	}

	int childInum = 0;
	// Checking to see if the directory file already exists.

	ret = find_child_inum(childPath, parent, &childInum);
	if(ret == 0){
		free(dup);
		return -EEXIST;
	}

	// Reserving space for the new directory.
	childInum = reserve_regfile_inode();

	// update the inode's values.
	inode_table[childInum].inode.size = 0;
	inode_table[childInum].inode.blocks = 0;
	inode_table[childInum].inode.mode = mode;

	// Saving changes to the filesystem.
	write_inode(childInum);
	sync_inode_bitmap();

	// Adding the new file to the parent directories inode.
	ret = update_parent_inode(childInum, parentInum, childPath);

	free(dup);

	return ret;
}


/*
// This function used mknod to create a file at the location specified by path.
// Returns 0 on success. Otherwise returns the appropriate error code.
*/
int static refs_create(const char * path, mode_t mode, struct fuse_file_info * file){
	return refs_mknod(path, mode, 0);
}


/*
// When a file is opened, we check for existence and permissions
// and return either success or an error code.
*/
static int refs_open(const char* path, struct fuse_file_info* fi) {
	int req_mode = 0;

	// Finding the access requirements.
	if ((fi->flags & O_RDONLY) == O_RDONLY)
		req_mode |= R_OK;
	if ((fi->flags & O_WRONLY) == O_WRONLY)
		req_mode |= W_OK;
	if ((fi->flags & O_RDWR) == O_RDWR)
		req_mode |= R_OK | W_OK;

	return refs_access(path, req_mode);
}

/*
// Freeing all the temporarily stored information for a file.
// Since the implemention of the filesystem doesn't use any temporaryily
// stored information, this function doesn't actually do anything.
*/
static int refs_release(const char* path, struct fuse_file_info *fi) {
	return 0;
}


/*
// Helper function to unlink a file from a directory. Returns 0 on success.
// Otherwise return the appropriate error code.
*/
static int do_unlink(char * path, int parentInum, int childInum){
	struct refs_inode * parentInode = &inode_table[parentInum].inode;
	struct refs_inode * childInode = &inode_table[childInum].inode;

	// Removing the child from the parent directory
	int ret = remove_child(parentInode, childInode, path);

	// Error case
	if(ret){
		return ret;
	}

	// Checking to make sure the number of links for the file is now zero.
	if(childInode->n_links == 0){

		for(int i = 0; i < childInode->blocks; i++){
					file_remove_data_block(childInode);
		}

		clear_bit(inode_bitmap, childInum);

		// Saving changes to the filesystem.
		release_inode(childInode);
		sync_data_bitmap();
		sync_inode_bitmap();
		write_inode(childInum);
	}
	return 0;
}


/*
// This function unlinks a file from a parent directory. If the file
// then has no links, it is then also deleted. Returns 0 on success.
// Otherwise returns the appropriate error code.
*/
static int refs_unlink(const char * path){
	int ret = 0;
	int parentInum = 0;
	int childInum = 0;

	// Checking to make sure filesystem can reach the parent directory.
	ret = get_parent_inum((char *) path, &parentInum);
	if(ret != 0){
		return ret;
	}

	// Checking to make sure target file exists.
	struct refs_inode * parentInode = &inode_table[parentInum].inode;
	ret = find_child_inum_abspath((char *) path, parentInode, &childInum);
	if(ret){
		return -ENOENT;
	}

	// Checking to make sure path refers to a file.
	struct refs_inode *file_inode = &inode_table[childInum].inode;
	if (!(file_inode->flags & INODE_TYPE_REG)) {
		return -EISDIR;
	}

	return do_unlink((char *) path, parentInum, childInum);
}

/*
// Just using the getattr function to perform fgetattr.
*/
static int refs_fgetattr(const char * path, struct stat* stbuf, struct fuse_file_info *fi){
	return refs_getattr(path, stbuf);
}

/*
// Helper function to read from a file. Specifically, reads from the file
// specified by an inputted inode number starting at an inputted offset, and for
// an inputted size.
// Returns zero on succes. Otherwise returns the appropriate error code.
*/
static int do_read(int childInum, char * buf, size_t size, off_t offset){
		struct refs_inode *file_inode = &inode_table[childInum].inode;

		// Case where we are reading starting from the outside of the file.
		if(file_inode->size < offset){
			return 0;
		}

		// Shortening the size to be read if the file is too short.
		if(offset + size > file_inode->size){
			size = file_inode->size - offset;
		}

		int firstBlock = offset / BLOCK_SIZE;
		int lastBlock = (size + offset) / BLOCK_SIZE;

		int byteOffsetBegin = offset % BLOCK_SIZE;

		char blk[BLOCK_SIZE];

		struct refs_inode * ino = &inode_table[childInum].inode;

		// Case where we are only reading from one block.
		if(firstBlock == lastBlock){
			file_read_block(blk, ino, firstBlock);
			memcpy(buf, blk + byteOffsetBegin, size);
		} else {
			// Case where we are reading from multiple blocks.

			// Reading from the first block to be read.
			file_read_block(blk, ino, firstBlock);
			memcpy(buf, blk + byteOffsetBegin, BLOCK_SIZE-byteOffsetBegin);

			int read = BLOCK_SIZE-byteOffsetBegin;

			// Reading from each of the middle blocks.
			for (int i = firstBlock+1; i < lastBlock; i++) {
				file_read_block(blk, ino, i);
				memcpy(buf + read, blk, BLOCK_SIZE);

				read += BLOCK_SIZE;
			}

			// Reading from the last block to be read.
			file_read_block(blk, ino, lastBlock);
			memcpy(buf + read, blk, size-read);
		}

		return size;
}

/*
// Reads the contents of a file starting at some offset, and for some size.
// The read part of the file will be stored in the character buffer buf.
// Returns 0 on succes. Otherwise returns the appropriate error code.
*/
static int refs_read(const char * path, char * buf, size_t size, off_t offset,
 	struct fuse_file_info* fi){

		int ret = 0;
		int parentInum = 0;
		int childInum = 0;

		// Checking to make sure filesystem can reach the parent directory.
		ret = get_parent_inum((char *) path, &parentInum);
		if(ret != 0){
			return ret;
		}

		// Checking to make sure target file exists.
		struct refs_inode * parentInode = &inode_table[parentInum].inode;
		ret = find_child_inum_abspath((char *) path, parentInode, &childInum);
		if(ret){
			return -ENOENT;
		}

		// Checking to make sure path refers to a file.
		struct refs_inode *file_inode = &inode_table[childInum].inode;
		if (!(file_inode->flags & INODE_TYPE_REG)) {
			return -EISDIR;
		}

		// Checking to make sure we can read the file.
		if(refs_access(path, R_OK)){
			return -EACCES;
		}

		return do_read(childInum, buf, size, offset);
}

/*
// Function to change the permissions of a file or directory specified by
// path to the permissions stored by mode. Returns 0 on success. Otherwise,
// returns the appropriate error code.
*/
int static refs_chmod(const char * path, mode_t mode){
	int ret =0;
	int inum;

	// Getting the inumber specified by the path.
	ret = get_path_inum((char*)path, &inum);

	// Checking for errors.
	if(ret){
		return ret;
	}

	struct refs_inode *inode = &inode_table[inum].inode;

	// Updating the permissions of the inode.
	inode->mode =mode;
	write_inode(inum);

	return 0;
}

// You should implement the functions that you need, but do so in a
// way that lets you incrementally test.
static struct fuse_operations refs_operations = {
	.init		= refs_init,
	.destroy = refs_destroy,
	.getattr	= refs_getattr,
	.access = refs_access,
	.mkdir = refs_mkdir,
	.readdir	= refs_readdir,
	.truncate	= refs_truncate,
	.mknod = refs_mknod,
	.create = refs_create,
	.release = refs_release,
	.open = refs_open,
	.rmdir = refs_rmdir,
	.write		= refs_write,
	.unlink = refs_unlink,
	.fgetattr	= refs_fgetattr,
	.read = refs_read,
	.chmod = refs_chmod
	/*
	.readlink	= NULL,
	.symlink	= NULL,
	.rename		= NULL,
	.link		= NULL,
	.chmod		= NULL,
	.chown		= NULL,
	.utimens	= NULL,
	.read		= NULL,
	.statfs		= NULL,
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
