# Part A feedback

This is a good start, and I'm happy to talk through any of your
functions in more detail to help you finish and debug!

## Functionality

I've tried to break the functionality into the four main parts, and
give feedback/pointers on each.

### Pathname resolution

It looks like your core pathname resolution code is done inside
`get_parent_inum()` and `find_child_inum()`.  Your breakdown of
functionality into two steps seems reasonable.  However, I would
suggest having your `get_parent_inum()` function resolve the entire
path argument.  Then, when you want to resolve just the parent of some
path, you can call `dirname()` and pass the resulting path as your
`get_parent_inum()`'s path argument.

Also, see my note about structuring the code to separate the
return value (the inode number) from the "status code" of success/failure.
There are more notes in the [Lab handout](http://cs.williams.edu/~jannen/teaching/s21/cs333/labs/fuse/fuse-fs-dirs.html#programming-strategies).


### Stat/Attributes

If you also want to extract the current user's uid and gid values, you
can use the `fuse_context` struct's values:

```C
struct fuse_context *ctx = fuse_get_context();
stbuf->st_uid = ctx->uid;
stbuf->st_gid = ctx->gid;
```

This isn't necessarily the user who created the file, but you could
also use this strategy track that information in your inode as well
(which would let you implement `chown()`)

In order to make `chmod()` easier for Part II, I suggest moving the
file type from the flags directly into the permissions.  Then, your
inode's permissions field would more closely match the meaning of the
mode.

Otherwise, your `getattr()` function looks good.


### Creating Directories

Your mkdir is great. It is one of the few implementations that both
expanded the directory when necessary AND incremented the parent's link count.
Nice work!


### Listing directories

You are correctly using the offset argument, but you are always starting from
the beginning of the directory. By rethinking the structure of the for loops,
we could jump directly to the target block. Here is one suggestion:

Since the offset corresponds to the index within our directory's
global list of children where we should start searching for the
directory's next child, we first need to figure out which directory
data block that portion of the array resides in, and then which
offset within that block. We can use the following math for that task:

```C
	int block_num = offset / DIRENTS_PER_BLOCK;
	int block_off = offset % DIRENTS_PER_BLOCK;
```

After these calculations:

 * `block_num` tells us which of the directory inode's `block_ptr`
   entries stores the directory entry referred to by `offset`, and
 * `block_off` tells us which entry within the directory's data block.
 
Then we can use the loop pseudocode from the HMC documentation (note the direct inode table access instead of `refs_getattr()`:

 * For each valid block starting at `block_num`, read that block
   * For each valid entry starting at `block_off`:
    ```C
		struct stat stbuf;
		// only mode and ino are used
		stbuf.st_mode = inode_table[dirent.inum].inode.mode;
		stbuf.st_ino = dirent.inum;
		if (filler(buf, dirent.path, &stbuf, block_number*DIRENTS_PER_BLOCK + block_offset))
			return 0;
		}
    ```


## General Comments

### Strengths

Your code organization and functionality look pretty good. I think your
foundation is very solid, and you should have a good codebase for building
upon in Part II.

 * You do a good job checking error conditions and returning the
   appropriate error numbers.
 * You've effectively structured your methods to free resources when
   an error is encountered.
 * You've extended the inode structure to include additional
   functionality (but consider changing permissions to mode).

### Areas for Improvement

The largest single suggestion is to separate your return values from the status codes in your functions. Accessing the return values from functions and propagating the error "up the stack" is a helpful way of building composable code. 

As a minor note, C utility functions like `strdup()`, which allocates
memory and copies a string, can help to simplify some common tasks.
