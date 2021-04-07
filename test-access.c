#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

/*
// This program tests to see if the access, mkdir function performs as
// expected upon the mounted file, and root directory.
*/
int main(int argc, char **argv) {
// Cases for where we should be able to access the root directory:

	// Case one: when checking if the file exists.
	if (access("mnt/", F_OK) != 0) {
		printf("failed: not able to confirm that file exists\n");
		return 1;
	}

	// Case two: when checking if the file is readable or writeable.
	if (access("mnt/", R_OK | W_OK) != 0) {
		printf("failed: not able to read, or write directory\n");
		return 1;
	}

	// Case three: when checking if the file is readable.
	if (access("mnt/", R_OK) != 0) {
		printf("failed: not able to read, or write directory\n");
		return 1;
	}

	// Case four: when checking if the file is writeable.
	if (access("mnt/", W_OK) != 0) {
		printf("failed: not able to read, or write directory\n");
		return 1;
	}

	// when checking if a directory can be made.
	if (mkdir("mnt/a", S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH) != 0) {
		printf("failed: not able to make directory\n");
		return 1;
	}
	if (mkdir("mnt/a/b", S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH) != 0) {
		printf("failed: not able to make directory\n");
		return 1;
	}
}
