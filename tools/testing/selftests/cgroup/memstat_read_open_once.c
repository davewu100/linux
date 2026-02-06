/*
 * Open a file once, then loop: lseek(0) + read().
 * This mirrors the "open-once" benchmark style used in mcg_test.
 *
 * Usage: memstat_read_open_once <path> [loops]
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define DEFAULT_LOOPS 100
#define BUF_SIZE 4096

int main(int argc, char **argv)
{
	const char *path;
	long loops = DEFAULT_LOOPS;
	char buf[BUF_SIZE];
	int fd;
	long i;
	ssize_t n;

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

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		perror(path);
		return 1;
	}

	for (i = 0; i < loops; i++) {
		if (lseek(fd, 0, SEEK_SET) < 0) {
			perror("lseek");
			break;
		}
		n = read(fd, buf, sizeof(buf));
		(void)n;
	}

	close(fd);
	return 0;
}
