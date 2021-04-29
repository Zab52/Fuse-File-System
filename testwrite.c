#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>

void usage() {
	printf("Please provide a pathname, offset, and string.\n");
	printf("Example usage:\n");
	printf("\t./testwrite mnt/hello 2 \"newdata\"\n");
}

/**
 * Function that calls the pwrite library function to write
 * data to an existing file at the specified offset.
 */
int do_write(char *path, size_t offset, char *contents) {

	int fd = open(path, O_WRONLY);
	if (fd < 0) {
		perror("open error");
		return errno;
	}

	return write(fd, contents, strlen(contents), offset);
}

int main(int argc, char **argv) {
	if (argc < 4) {
		usage();
		exit(-1);
	}

	char *path = argv[1];
	unsigned long offset = strtoul(argv[2], NULL, 0);
	char *data = argv[3];

	int ret = do_write(path, offset, data);

	if (ret == strlen(data))
		// 0 is "success", so we return success if we wrote
		// all of the input data as requested
		return 0;

	return ret;
}
