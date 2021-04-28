# FUSE Reference FS (ReFS)

In this (2-part) assignment,
you will implement a variant of the reference file system described in the textbook
([chapter 40](http://pages.cs.wisc.edu/~remzi/OSTEP/file-implementation.pdf)).
You may create additional files if you would like,
but your FUSE file system's `main()` method should be in a file named `refs.c`.

 * [Lab Handout](http://cs.williams.edu/~jannen/teaching/s21/cs333/labs/fuse/fuse-fs.html)


## Repository Contents

 * __`bitmap.c`__: a straightforward implementation of a bitmap data structure that supports
   setting/clearing individual bits, as well as querying a given bit's value.
 * __`refs.h`__: struct and function declarations for the implementation of ReFS.
 * __`refs.c`__: definitions of the main functions that define ReFS behavior.
 * __`Makefile`__: includes rules to compile ReFS as well as a testable version of the bitmap code (`make bitmap`)

## Implementation details

To compile our code, simply run the 'make' command in the terminal.

To mount the file system, simply run '.\refs -s -d mnt', or '.\refs -s -f mnt'.
(The code won't run without the -d, or -f flags).

Once the file system is mounted, our file system currently supports twelve
operations: getattr(), mkdir(), readdir(),
access(), truncate(), mknod(), create(), release(), open(), rmdir(), write(), unlink(), fgetattr(), chmod(), and read(). As such, the file system will support any calls of mkdir, ls, cd, stat, touch, cat, unlink, chmod, and rmdir. Additionally, any directories created using 'mkdir' will remain
persistently even after unmounting and mounting.

To unmount the file system, simply run 'fusermount -u mnt'.

To run our testing code (which is still a work in progress):

First, delete the refs_disk folder.

Mount the file system as already described.

Compile using 'gcc -o test-access test-access.c'.

To run the testing code, run './test-access'.

After testing, then just unmount the file system as described.
