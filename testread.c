#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>

void usage() {
	printf("Please provide a pathname, offset, and string.\n");
	printf("Example usage:\n");
	printf("\t./testread mnt/hello 2 \"size\"\n");
}

/**
 * Function that calls the pread library function to read
 * data starting at a certain offset
 */
int do_read(char *path, size_t offset, int size, char * contents) {

	int fd = open(path, O_WRONLY);
	if (fd < 0) {
		perror("open error");
		return errno;
	}

	return pread(fd, contents, size, offset);
}

int main(int argc, char **argv) {
	if (argc < 4) {
		usage();
		exit(-1);
	}

	char *path = argv[1];
	unsigned long offset = strtoul(argv[2], NULL, 0);
	unsigned long size = strtoul(argv[3], NULL, 0);
	char contents[size];

	int ret = do_read(path, offset, size, contents);
	printf("%s", contents);

	if (ret == size)
		// 0 is "success", so we return success if we wrote
		// all of the input data as requested
		return 0;

	return ret;
}
