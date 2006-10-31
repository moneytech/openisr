/*
                               Fauxide

		      A virtual disk drive tool
 
               Copyright (c) 2002-2004, Intel Corporation
                          All Rights Reserved

This software is distributed under the terms of the Eclipse Public License, 
Version 1.0 which can be found in the file named LICENSE.  ANY USE, 
REPRODUCTION OR DISTRIBUTION OF THIS SOFTWARE CONSTITUTES RECIPIENT'S 
ACCEPTANCE OF THIS AGREEMENT

*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

#include <linux/fs.h>

#include "fauxide.h"
#include "vulpes.h"

typedef int simple_mapping_special_t;

static int long_seek(int fid, unsigned long long lloffset)
{
    const off_t max_offset = 0x7fffffff;

    off_t off, tmp_off;

    if (lloffset > max_offset) {
	printf("ERROR: long_seek() lloffset = %llu\n", lloffset);
	return (-1);
    }

    off = (off_t) lloffset;
    tmp_off = lseek(fid, off, SEEK_SET);
    if (tmp_off != off) {
	printf("ERROR: long_seek() off=%ld tmp_off=%ld\n",
	       (long) off, (long) tmp_off);
	return (-1);
    }

    return 0;
}

vulpes_volsize_t simple_file_volsize_func(void)
{
    int fileno;
    struct stat filestat;
    off_t size_bytes;
    vulpes_volsize_t volsize;	/* sectors */

    fileno = *((int *) config.special);

    /* Get file statistics */
    if (fstat(fileno, &filestat)) {
	printf("ERROR: unable to fstat().\n");
	return (off_t) 0;
    }

    size_bytes = filestat.st_size;

    volsize = size_bytes / FAUXIDE_HARDSECT_SIZE;

    return volsize;
}

vulpes_volsize_t simple_disk_volsize_func(void)
{
    int result;
    int dev;
    unsigned long long devsize;
    vulpes_volsize_t volsize;	/* sectors */

    dev = *((int *) config.special);

    result = ioctl(dev, BLKGETSIZE64, &devsize);
    if (result) {
	printf("ERROR: BLKGETSIZE64 returned %d.\n\n", result);
	return -1;
    }

    volsize = devsize / FAUXIDE_HARDSECT_SIZE;

    return volsize;
}

int simple_shutdown_func(void)
{
    if (config.special != NULL) {
	close(*((int *) config.special));
	free(config.special);
	config.special = NULL;
    }
    return 0;
}

int simple_read_func(vulpes_cmdblk_t * cmdblk)
{
    unsigned long long start;
    ssize_t bytes, tmp_size;
    int result = 0;
    int fid;

    fid = *((int *) config.special);

    start =
	(unsigned long long) cmdblk->head.start_sect *
	FAUXIDE_HARDSECT_SIZE;

    result = long_seek(fid, start);
    if (result) {
	printf("ERROR: seeking %s to sector %lu (byte %llu)\n",
	       config.cache_name, (unsigned long) cmdblk->head.start_sect,
	       (unsigned long long) start);
    }

    bytes = cmdblk->head.num_sect * FAUXIDE_HARDSECT_SIZE;
    tmp_size = read(fid, cmdblk->buffer, bytes);
    if (tmp_size != bytes) {
	printf("ERROR: reading %s. %llu bytes\n", config.cache_name,
	       (unsigned long long) bytes);
	result = -1;
    }

    return result;
}

int simple_write_func(const vulpes_cmdblk_t * cmdblk)
{
    unsigned long long start;
    ssize_t bytes, tmp_size;
    int result = 0;
    int fid;

    fid = *((int *) config.special);

    start =
	(unsigned long long) cmdblk->head.start_sect *
	FAUXIDE_HARDSECT_SIZE;

    result = long_seek(fid, start);
    if (result) {
	printf("ERROR: seeking %s to sector %lu (byte %llu)\n",
	       config.cache_name, (unsigned long) cmdblk->head.start_sect,
	       (unsigned long long) start);
    }

    bytes = cmdblk->head.num_sect * FAUXIDE_HARDSECT_SIZE;
    tmp_size = write(fid, cmdblk->buffer, bytes);
    if (tmp_size != bytes) {
	printf("ERROR: writing %s. %llu bytes\n",
	       config.cache_name, (unsigned long long) bytes);
	result = -1;
    }

    return result;
}


int initialize_simple_mapping(void)
{
    int fid;

    if (config.mapping == SIMPLE_FILE_MAPPING) {
	config.volsize_func = simple_file_volsize_func;
    } else if (config.mapping == SIMPLE_DISK_MAPPING) {
	config.volsize_func = simple_disk_volsize_func;
    } else {
	return -1;
    }

    config.special = malloc(sizeof(simple_mapping_special_t));
    if (config.special == NULL) {
	return -1;
    }

    fid = open(config.cache_name, O_RDWR);
    if (fid < 0) {
        free(config.special);
	printf("ERROR: unable to open %s.\n", config.cache_name);
	return -1;
    }

    *((int *) (config.special)) = fid;

    config.read_func = simple_read_func;
    config.write_func = simple_write_func;
    config.shutdown_func = simple_shutdown_func;

    return 0;
}
