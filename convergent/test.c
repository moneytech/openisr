#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "convergent-user.h"

struct chunk {
	char key[MAX_HASH_LEN];
};

static struct chunk *chunks;

void printkey(char *key)
{
	int i;
	
	for (i=0; i<HASH_LEN; i++)
		printf("%.2hhx", key[i]);
	printf("\n");
}

int main(int argc, char **argv)
{
	int fd, ret;
	struct isr_setup setup;
	struct isr_message message;
	
	if (argc != 6) {
		printf("Usage: %s ctldev chunkdev chunksize cachesize offset\n",
					argv[0]);
		return 1;
	}
	
	fd=open(argv[1], O_RDWR);
	if (fd < 0) {
		perror("Opening file");
		return 1;
	}
	snprintf(setup.chunk_device, MAX_DEVICE_LEN, "%s", argv[2]);
	setup.chunksize=atoi(argv[3]);
	setup.cachesize=atoi(argv[4]);
	setup.offset=atoi(argv[5]);
	ret=ioctl(fd, ISR_REGISTER, &setup);
	if (ret) {
		perror("Registering device");
		return 1;
	}
	printf("Allocating %llu KB\n",
				(setup.chunks * sizeof(struct chunk)) >> 10);
	chunks=malloc(setup.chunks * sizeof(struct chunk));
	if (chunks == NULL) {
		printf("malloc failed\n");
		return 1;
	}
	memset(chunks, 0, setup.chunks * sizeof(struct chunk));
	while (1) {
		ret=read(fd, &message, sizeof(message));
		if (ret != sizeof(message)) {
			printf("read() returned %d, expected %d", ret,
						sizeof(message));
			continue;
		}
		if (message.flags & ISR_MSG_HAVE_KEY) {
			printf("Receiving chunk %8llu key ", message.chunk);
			printkey(message.key);
			memcpy(chunks[message.chunk].key, message.key, HASH_LEN);
		}
		if (message.flags & ISR_MSG_WANT_KEY) {
			printf("Sending   chunk %8llu key ", message.chunk);
			printkey(chunks[message.chunk].key);
			memcpy(message.key, chunks[message.chunk].key, HASH_LEN);
			if (write(fd, &message, sizeof(message)) !=
						sizeof(message))
				printf("Error on write\n");
		}
	}
	printf("Exiting\n");
	return 0;
}
