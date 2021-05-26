# FUSE ReFS

## __Correctness/Functionality:__

### Directories (Part a functionality)
   * initializing a fresh file system
     * create `.` and `..` inside `/`
   * reading directories
     * report accurate contents (empty and populated directories)
     * reasonable contents for `getattr` (as illustrated by `ls -l`)
   * creating directories
     * can create directories in `/`
     * can create subdirectories
     * can create directories with (reasonably) large names
     * _Not tested:_ directory size or depth limits
     * _Not tested:_ accuracy of link counts
   * traversing directories
     * can navigate to directories and subdirectories (e.g., path lookup)
   * persistence
     * data persists after unmounting

### Small File (no indirect block) Tests
   * create small files
     * create `empty.txt` inside `/`
     * create `small.txt` inside `/dir1/`
   * Extend the small files by appending to the end (seek to end + write)
     * append the string  to `/dir1/small.txt`
   * Overwrite bytes in the middle of the file in the middle (not the end).
     * change the string `small append` to `smile friend` using prwrite
   * Truncate the file to a larger size, and read the contents.
     * increase the file size to 24 bytes
   * Truncate the file to a smaller size.
     * decrease the file size to 5 bytes (`smile`)
   * Unmount/mount to verify that your data persists.

### Large file (indirect block) tests
   * Create a large file (the file has 15 blocks)
     * Write patterned data to multiple blocks and compare resulting file contents
   * Truncate the file to a smaller size (smaller size but the same number of blocks)
     * Compare resulting file contents
     * Compare reported size/block count
   * Truncate the file to a smaller size (smaller size and a smaller number of blocks)
     * Compare resulting file contents
     * Compare reported size/block count
   * Unmount/mount to verify that your data persists.

### Deletion
   * Delete a file (unlink)
   * Try to delete a non-empty directory (should not actually delete the directory)
   * Delete an empty directory
   * Unmount/mount to verify that your data persists.


## General Comments

Overall, this was very impressive work. Your code is easy to follow,
and you've used good coding pracices (if we can convince ourselves to
consider gotos good practice...) to properly handle errors and successfully
manage memory.

There is one small bug where the `.` entry is omitted from each directory's listing. It is a very small fix (see my inline comments), and then everything works perfectly. Very impressive work!

Building a working file system is no small feat. I hope that the
work that you did helped you build a deeper understanding of the core
principles underlying the FFS design, even though we implemented a
subset of them in ReFS.

This lab was outstanding!

## Test: long listing of empty directory (should see '.' and '..')
```
ls -la /home/bill/mnt
	total 4
	drwxr-xr-x 26 bill bill 4096 May 24 19:50 ..
```
### comments:
  * Because of a `<` instead of `<=` in `readdir()`, there is no `.`
  entry in any of your directory listings reported in the
  tests. Everything else is correct (and the small fix solves the issue).


## Test: making one directory
```
mkdir /home/bill/mnt/dir1
ls -la /home/bill/mnt
	total 5
	drwxr-xr-x 26 bill bill 4096 May 24 19:50 ..
	drwxrwxr-x 2 root root 4096 Dec 31 1969 dir1
ls -la /home/bill/mnt/dir1
	total 1
	drwxrwxrwx 3 root root 4096 Dec 31 1969 ..
```
### comments:
  * 👍. Great! Link counts are correct as well.


## Test: making one directory, then subdirectory
```
mkdir /home/bill/mnt/dir1
ls -la /home/bill/mnt
	total 5
	drwxr-xr-x 26 bill bill 4096 May 24 19:50 ..
	drwxrwxr-x 2 root root 4096 Dec 31 1969 dir1
mkdir /home/bill/mnt/dir1/subdir1
ls -la /home/bill/mnt/dir1
	total 1
	drwxrwxrwx 3 root root 4096 Dec 31 1969 ..
	drwxrwxr-x 2 root root 4096 Dec 31 1969 subdir1
```
### comments:
  * 👍 Nested directories work as expected.


## Test: making directory with moderately large name
```
mkdir /home/bill/mnt/this_is_a_pretty_long_name_for_one_dir
ls -la /home/bill/mnt
	total 5
	drwxr-xr-x 26 bill bill 4096 May 24 19:50 ..
	drwxrwxr-x 3 root root 4096 Dec 31 1969 dir1
	drwxrwxr-x 2 root root 4096 Dec 31 1969 this_is_a_pretty_long_name_for_one_dir
```
### comments:
  * 👍


## Test: removing empty directory
```
rmdir /home/bill/mnt/this_is_a_pretty_long_name_for_one_dir
ls -la /home/bill/mnt
	total 5
	drwxr-xr-x 26 bill bill 4096 May 24 19:50 ..
	drwxrwxr-x 3 root root 4096 Dec 31 1969 dir1
```
### comments:
  * 👍


## Test: can you create small (empty) files?
Creating `/home/bill/mnt/empty.txt` and `/home/bill/mnt/dir1/small.txt`...
```
ls -la /home/bill/mnt
	total 5
	drwxr-xr-x 26 bill bill 4096 May 24 19:50 ..
	drwxrwxr-x 3 root root 4096 Dec 31 1969 dir1
	-rw------- 1 root root 0 Dec 31 1969 empty.txt
ls -la /home/bill/mnt/dir1
	total 1
	drwxrwxrwx 4 root root 4096 Dec 31 1969 ..
	-rw------- 1 root root 0 Dec 31 1969 small.txt
	drwxrwxr-x 2 root root 4096 Dec 31 1969 subdir1
```
### comments:
  * 👍


## Test: can you write to small (empty) files?
Appending 13 bytes to `/home/bill/mnt/dir1/small.txt`...
```
write succeeded with value 13.
ls -la /home/bill/mnt/dir1
	total 2
	drwxrwxrwx 4 root root 4096 Dec 31 1969 ..
	-rw------- 1 root root 13 Dec 31 1969 small.txt
	drwxrwxr-x 2 root root 4096 Dec 31 1969 subdir1
```
File contents match.
### comments:
  * 👍


## Test: can you overwrite bytes in the middle of small files?
Overwriting 7 bytes in the middle of `/home/bill/mnt/dir1/small.txt`...
```
ls -la /home/bill/mnt/dir1
	total 2
	drwxrwxrwx 4 root root 4096 Dec 31 1969 ..
	-rw------- 1 root root 13 Dec 31 1969 small.txt
	drwxrwxr-x 2 root root 4096 Dec 31 1969 subdir1
```
File contents match.
### comments:
  * 👍


## Test: truncate small file, reducing its size
Truncate `/home/bill/mnt/dir1/small.txt` to length 5...
truncate to 5 succeeded.
```
ls -la /home/bill/mnt/dir1/small.txt
	-rw------- 1 root root 5 Dec 31 1969 /home/bill/mnt/dir1/small.txt
```
File contents match.
### comments:
  * 👍


## Test: can you create large (data requires an indirect block) files?
Creating 61440-byte file (61440=15*4096) called `/home/bill/mnt/large.txt`...
```
ls -la /home/bill/mnt
	total 13
	drwxr-xr-x 26 bill bill 4096 May 24 19:50 ..
	drwxrwxr-x 4 root root 4096 Dec 31 1969 dir1
	-rw------- 1 root root 0 Dec 31 1969 empty.txt
	-rw------- 1 root root 61440 Dec 31 1969 large.txt
```
File contents match.
### comments:
  * 👍


## Test: truncate large file with minor size reduction
Truncating 61440-byte file to 61400 bytes...
truncate to 61400 succeeded.
```
ls -la /home/bill/mnt/large.txt
	-rw------- 1 root root 61400 Dec 31 1969 /home/bill/mnt/large.txt
```
File contents match.
### comments:
  * 👍


## Test: truncate large file with major size reduction
Truncating large file to 5 bytes...
truncate to 5 succeeded.
```
ls -la /home/bill/mnt/large.txt
	-rw------- 1 root root 5 Dec 31 1969 /home/bill/mnt/large.txt
```
File contents match.
### comments:
  * 👍


## Test: unlink existing file (should succeed)
unlinking /home/bill/mnt/large.txt
```
ls -la /home/bill/mnt/large.txt
```
### comments:
  * 👍


## Test: unlink existing directory (should fail)
unlinking /home/bill/mnt/dir1
```
ls -la /home/bill/mnt/dir1
	total 2
	drwxrwxrwx 4 root root 4096 Dec 31 1969 ..
	-rw------- 1 root root 5 Dec 31 1969 small.txt
	drwxrwxr-x 2 root root 4096 Dec 31 1969 subdir1
```
### comments:
  * 👍


## Test: unlink empty directory (should succeed)
calling rmdir on /home/bill/mnt/dir1/subdir1
```
ls -la /home/bill/mnt/dir1
	total 1
	drwxrwxrwx 4 root root 4096 Dec 31 1969 ..
	-rw------- 1 root root 5 Dec 31 1969 small.txt
```
### comments:
  * 👍


## Test: unlink populated directory (should fail)
calling rmdir on /home/bill/mnt/dir1
```
ls -la /home/bill/mnt/dir1
	total 1
	drwxrwxrwx 4 root root 4096 Dec 31 1969 ..
	-rw------- 1 root root 5 Dec 31 1969 small.txt
```
### comments:
  * 👍

