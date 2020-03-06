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

_please fill this section with any relevant details that will help with running/using/reading your code._

