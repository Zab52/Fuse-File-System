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
	inode_table[inum].inode.mode = 0775;

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
int find_child_inum(char * path, struct refs_inode* ino){
	union directory_block * blk = malloc(sizeof(union directory_block));

	// Iterating through each of the blocks connecting to the inode
	for (int i = 0; i < ino->blocks; i++) {
		read_block(blk, ino->block_ptrs[i]);

		// Iterating through each of the directory entries in each block.
		for (int j = 0; j < DIRENTS_PER_BLOCK; j++) {
			struct dir_entry *dir = &blk->dirents[j];

			// Checking to make sure the found directory is valid.
			if (dir != NULL && dir->is_valid){

				// Case where the found directory has the desired path name.
				if (!strcmp(dir->path, path)) {
					free(blk);
					return dir->inum;
				}
			}
		}
	}
	free(blk);
	return -1;
}


int get_path_inum(char * path, int * inum){

	struct refs_inode * current;

	// Tokenizing the path name.
  char delem[2] = "/";
	char * temp = strtok(path + 1, delem);


  * inum = 0;

	// Searching through each inode starting from the root for the next token.
	while(temp != NULL){
		current = (struct refs_inode *) &(inode_table[*inum]).inode;

		// Case where an inode on the path isn't a directory inode.
		if(!(current->flags & INODE_TYPE_DIR)){
			return -ENOTDIR;
		}

		// Finding the inode number of the next inode in the path.
		*inum = find_child_inum(temp, current);

		// Case where the inode isn't found.
		if(*inum == -1){
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

	char * dup = malloc(sizeof(char) * strlen(path));

	struct refs_inode* res;

	int inum = 0;
	int ret = 0;

	// Checking to make sure the path isn't just the root path.
	if(strcmp(path, "/")){

		strcpy(dup, path);

		// Getting the parent inode number of the path.
		ret = get_path_inum((char *) path, &inum);

		free(dup);

		// Case where inodes doesn't exist for the parent path.
		if (ret != 0) {
			return ret;
		}

		res = (struct refs_inode*) &(inode_table[inum]).inode;
	} else{
		// Case where path is just the root path. Thus, we just use the root inode.
		res = (struct refs_inode *) &(inode_table[0]).inode;
	}


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

	struct stat perm;

	refs_getattr(path, &perm);

	if(mask == F_OK && (mask & perm.st_mode)){
		return 0;
	} else if ((mask & (perm.st_mode >> 6)) == mask){
		return 0;
	}

	return -1;
}

/*
// This function creates a new directory at the specified path location, setting
// the mode to mode. If succesful, this function returns 0. Otherwise,
// returns -1.
*/
static int refs_mkdir(const char* path, mode_t mode){

	char * dup = malloc(sizeof(char) * strlen(path));
	strcpy(dup, path);

	char * temp = dirname(dup);
	int parentInum = 0;
	int ret = 0;

	// Case where parent directory doesn't have write mode.
	if(refs_access(temp, W_OK)){
		return -EACCES;
	}

	// Finding the parent inode number of the path,
	ret = get_parent_inum((char *) path, &parentInum);

	// Case where path is invalid.
	if(ret != 0){
		return ret;
	}

	struct refs_inode* parent = (struct refs_inode *) &(inode_table[parentInum]).inode;

	// Finding the path to be stored in the new directory.
	strcpy(dup, path);

	temp = basename(dup);

	// Case where the filename of the new directory is too long.
	if(strlen(temp) > MAX_PATH_LEN){
		return -ENAMETOOLONG;
	}

	union directory_block *blk = malloc(sizeof(union directory_block));
	bzero(blk, BLOCK_SIZE);

	// Checking to see if the directory file already exists.
	if(find_child_inum(temp, parent) != -1){
		return -EEXIST;
	}

	// Reserving space for the new directory.
	int inum = reserve_dir_inode();
	lba_t data_lba = reserve_data_block();
	union directory_block *dir_data = malloc_blocks(1);
	bzero(dir_data, BLOCK_SIZE);

	// init "." for the new directory.
	dir_data->dirents[0].inum = inum;
	dir_data->dirents[0].is_valid = 1;
	strcpy(dir_data->dirents[0].path, ".");
	dir_data->dirents[0].path_len = strlen(dir_data->dirents[0].path);

	// init ".." for the new directory.
	dir_data->dirents[1].inum = parentInum;
	dir_data->dirents[1].is_valid = 1;
	strcpy(dir_data->dirents[1].path, "..");
	dir_data->dirents[1].path_len = strlen(dir_data->dirents[1].path);

	// update the inode's direct pointer to point to itself.
	inode_table[inum].inode.size = 4096;
	inode_table[inum].inode.blocks = 1;
	inode_table[inum].inode.block_ptrs[0] = data_lba;
	inode_table[inum].inode.mode = mode;

	// Saving changes to the filesystem.
	write_block(dir_data, data_lba);
	sync_data_bitmap();
	write_inode(inum);
	sync_inode_bitmap();

	// Updating the number of links for the parent inode.
	parent->n_links += 1;

	// Looking for an open directory in the currently allocated blocks.
	for(int j = 0; j < parent->blocks; j++){
		read_block(blk, parent->block_ptrs[j]);
		for(int n = 0; n < DIRENTS_PER_BLOCK; n++){
			struct dir_entry *dir = blk->dirents + n;
			if(!dir->is_valid){
				dir->is_valid = 1;
				dir->inum = inum;
				strcpy(dir->path, temp);
				dir->path_len = strlen(dir->path);
				write_block(blk, parent->block_ptrs[j]);
				free(dup);
				free(blk);
				write_inode(parentInum);
				return 0;
			}
		}
	}

	// Creating a new directory block if required.
	if(parent->blocks < NUM_DIRECT){
		parent->blocks += 1;
		lba_t data2_lba = reserve_data_block();
		union directory_block *dir2_data = malloc_blocks(1);
		bzero(dir2_data, BLOCK_SIZE);

		dir2_data->dirents[0].inum = inum;
		dir2_data->dirents[0].is_valid = 1;
		strcpy(dir2_data->dirents[0].path, temp);
		dir2_data->dirents[0].path_len = strlen(dir_data->dirents[0].path);

		write_block(dir2_data, data2_lba);
		sync_data_bitmap();
		write_inode(parentInum);
		free(dup);
		free(blk);
		return 0;
	}

	// Case where we cannot allocate any more directory blocks
	// for the parent inode.
	write_inode(parentInum);
	free(dup);
	free(blk);
	return -1;
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

	struct refs_inode* res;
	int inum = 0;
	int ret = 0;

	// Case where the path isn't the root path.
	if(strcmp(path, "/")){

		char * dup = malloc(sizeof(char) * strlen(path));

		strcpy(dup, path);

		// Finding the parent inode number of the path.
		ret = get_path_inum((char *) path, &inum);

		free(dup);

		// Case where the path isn't valid.
		if(ret != 0){
			return ret;
		}

		res = (struct refs_inode *) &inode_table[inum].inode;
	} else {
		// Case where the path is the root path. Thus, we just use the root inode.
		res = (struct refs_inode *) &inode_table[0].inode;
	}

	// Stores the number of found valid directories.
	off_t num = 0;

	int pathlen = strlen(path);

	union directory_block *blk = malloc(sizeof(union directory_block));
	bzero(blk, sizeof(union directory_block));

	// Proceeding through each block of the path inode.
	for (int i = 0; i < res->blocks; i++) {
		read_block(blk, res->block_ptrs[i]);

		// Considering each possible entry in each block.
		for (int j = 0; j < DIRENTS_PER_BLOCK; j++) {
			struct dir_entry *dir = blk->dirents + j;

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
							free(blk);
							return 0;
						}

						free(appendedFilename);
					}
					// Incrementing the number of found valid directories.
					num++;
				}
		}
	}
	free(blk);
	return 0;
}

static void fill(char *str, int begin, int end, char thing) {
	for (int i = begin; i <= end; i++) {
		str[i] = thing;
	}
}

static int min(int a, int b) {
	if (a < b) {
		return a;
	}
	return b;
}

static void ext_read_blocks(char *buf, int numBlocks, off_t start, struct refs_inode *ino) {
	if (start < NUM_DIRECT) {
		int endDirect = min(start + numBlocks - 1, NUM_DIRECT - 1);
		read_blocks(buf, endDirect - start + 1, start);
		int numReadDirect = NUM_DIRECT - start + 1;

		// offsetting start, numBlocks, and buf to make code easier
		numBlocks -= numReadDirect;
		start = NUM_DIRECT;
		buf += numReadDirect * BLOCK_SIZE;
	}
	if (numBlocks > 0) {
		int startIndirect = start - NUM_DIRECT;
		for (int i = startIndirect; i < startIndirect + numBlocks; i++) {
			int indirectBlock = i / BLOCKS_PER_BLOCK;
			int offsetIndirect = i % BLOCKS_PER_BLOCK;
			// TODO: read indirect block #indirectBlock at offsetIndirect into buf
			buf += BLOCK_SIZE;
		}
	}
}

static void ext_write_blocks(char *buf, int numBlocks, off_t start, struct refs_inode *ino) {
	// TODO: do the same thing as read lol
}

static int my_hello_truncate(const char *path, off_t size) {

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
	if (ret == -1) {
		return ret;
	}

	ino = (struct refs_inode *) &(inode_table[inum]).inode;

	if (ino->size < size) {
		int firstBlock = ino->size / BLOCK_SIZE;
		int lastBlock = (size - 1) / BLOCK_SIZE;

		int byteOffsetBegin = ino->size % BLOCK_SIZE;
		int byteOffsetEnd = (ino->size - 1) % BLOCK_SIZE + 1;

		if (firstBlock == lastBlock) {
			char *block = malloc(sizeof(char) * BLOCK_SIZE);

			ext_read_blocks(block, 1, firstBlock, ino);
			fill(block, byteOffsetBegin, byteOffsetEnd - 1, 0);
			ext_write_blocks(block, 1, firstBlock, ino);

			free(block);
		} else {
			char *blockBegin = malloc(sizeof(char) * BLOCK_SIZE);
			char *blockEnd = malloc(sizeof(char) * BLOCK_SIZE);

			ext_read_blocks(blockBegin, 1, firstBlock, ino);
			fill(blockBegin, byteOffsetBegin, BLOCK_SIZE - 1, 0);
			ext_write_blocks(blockBegin, 1, firstBlock, ino);

			ext_read_blocks(blockEnd, 1, lastBlock, ino);
			fill(blockEnd, 0, byteOffsetEnd - 1, 0);
			ext_write_blocks(blockEnd, 1, lastBlock, ino);

			int numMiddleBlocks = lastBlock - firstBlock + 1;
			if (numMiddleBlocks > 0) {
				char *blocksInTheMiddle = malloc(sizeof(char) * (BLOCK_SIZE * numMiddleBlocks));

				ext_read_blocks(blocksInTheMiddle, numMiddleBlocks, firstBlock + 1, ino);
				fill(blocksInTheMiddle, 0, BLOCK_SIZE * numMiddleBlocks - 1, 0);
				ext_write_blocks(blocksInTheMiddle, numMiddleBlocks, firstBlock + 1, ino);

				free(blocksInTheMiddle);
			}
			free(blockBegin);
			free(blockEnd);
		}
	} else {
		// TODO: do the same thing but reversed
	}

	ino->size = size;
	return 0;
}


static int refs_rmdir(const char * path){




	return 0;
}

static int refs_mknod(const char * path, mode_t mode, dev_t dev){
	char * dup = malloc(sizeof(char) * strlen(path));
	strcpy(dup, path);

	char * temp = dirname(dup);

	// Case where parent directory doesn't have write permissions.
	if(refs_access(temp, W_OK)){
		return -EACCES;
	}

	// Finding the parent inode number of the path,
	int parentInum = 0;
	int ret = get_parent_inum((char *) path, &parentInum);

	// Case where path is invalid.
	if(ret  != 0){
		return ret;
	}

	struct refs_inode* parent = (struct refs_inode *) &(inode_table[parentInum]).inode;

	// Finding the path to be stored in the new directory.
	strcpy(dup, path);

	temp = basename(dup);

	// Case where the filename of the new directory is too long.
	if(strlen(temp) > MAX_PATH_LEN){
		return -ENAMETOOLONG;
	}

	union directory_block *blk = malloc(sizeof(union directory_block));
	bzero(blk, BLOCK_SIZE);

	// Checking to see if the directory file already exists.
	if(find_child_inum(temp, parent) != -1){
		return -EEXIST;
	}

	// Reserving space for the new directory.
	int inum = reserve_regfile_inode();

	// update the inode's direct pointer to point to itself.
	inode_table[inum].inode.size = 0;
	inode_table[inum].inode.blocks = 0;
	inode_table[inum].inode.mode = mode;

	// Saving changes to the filesystem.
	write_inode(inum);
	sync_inode_bitmap();

	// Updating the number of links for the parent inode.
	parent->n_links += 1;

	// Looking for an open directory in the currently allocated blocks.
	for(int j = 0; j < parent->blocks; j++){
		read_block(blk, parent->block_ptrs[j]);
		for(int n = 0; n < DIRENTS_PER_BLOCK; n++){
			struct dir_entry *dir = blk->dirents + n;
			if(!dir->is_valid){
				dir->is_valid = 1;
				dir->inum = inum;
				strcpy(dir->path, temp);
				dir->path_len = strlen(dir->path);
				write_block(blk, parent->block_ptrs[j]);
				free(dup);
				free(blk);
				write_inode(parentInum);
				return 0;
			}
		}
	}

	// Creating a new directory block if required.
	if(parent->blocks < NUM_DIRECT){
		parent->blocks += 1;
		lba_t data2_lba = reserve_data_block();
		union directory_block *dir2_data = malloc_blocks(1);
		bzero(dir2_data, BLOCK_SIZE);

		dir2_data->dirents[0].inum = inum;
		dir2_data->dirents[0].is_valid = 1;
		strcpy(dir2_data->dirents[0].path, temp);
		dir2_data->dirents[0].path_len = strlen(dir2_data->dirents[0].path);

		write_block(dir2_data, data2_lba);
		sync_data_bitmap();
		write_inode(parentInum);
		free(dup);
		free(blk);
		return 0;
	}

	// Case where we cannot allocate any more directory blocks
	// for the parent inode.
	write_inode(parentInum);
	free(dup);
	free(blk);
	return -1;
}

int static refs_create(const char * path, mode_t mode, struct fuse_file_info * file){
	return refs_mknod(path, mode | S_IFREG, 0);
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
	.mknod = refs_mknod,
	.create = refs_create
/*
	.fgetattr	= NULL,
	.access		= NULL,
	.readlink	= NULL,
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
