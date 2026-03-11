/*
 * Open file, read entire content to EOF, close. Repeat N times.
 * Used to measure "total file read" time (whole file per iteration).
 *
 * Usage: memstat_read_full <path> [loops]
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define DEFAULT_LOOPS 100
#define BUF_SIZE 4096

static int read_until_eof(int fd, char *buf, size_t size)
{
	ssize_t n;

	while (1) {
		n = read(fd, buf, size);
		if (n < 0)
			return -1;
		if (n == 0)
			return 0;
	}
}

int main(int argc, char **argv)
{
	const char *path;
	long loops = DEFAULT_LOOPS;
	char *buf;
	int fd;
	long i;

	if (argc < 2) {
		fprintf(stderr, "Usage: %s <path> [loops]\n", argv[0]);
		return 1;
	}

	path = argv[1];
	if (argc >= 3) {
		char *endp = NULL;
		errno = 0;
		loops = strtol(argv[2], &endp, 10);
		if (errno || !endp || *endp != '\0' || loops <= 0) {
			fprintf(stderr, "Invalid loops: %s\n", argv[2]);
			return 1;
		}
	}

	buf = malloc(BUF_SIZE);
	if (!buf) {
		perror("malloc");
		return 1;
	}

	for (i = 0; i < loops; i++) {
		fd = open(path, O_RDONLY);
		if (fd < 0) {
			perror(path);
			free(buf);
			return 1;
		}
		if (read_until_eof(fd, buf, BUF_SIZE) < 0) {
			perror("read");
			close(fd);
			free(buf);
			return 1;
		}
		close(fd);
	}

	free(buf);
	return 0;
}
