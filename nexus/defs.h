/* defs.h - struct/symbol definitions, utility fns, exported fn prototypes */

/* 
 * Nexus - convergently encrypting virtual disk driver for the OpenISR (TM)
 *         system
 * 
 * Copyright (C) 2006-2007 Carnegie Mellon University
 * 
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as published
 * by the Free Software Foundation.  A copy of the GNU General Public License
 * should have been distributed along with this program in the file
 * LICENSE.GPL.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#ifndef NEXUS_DEFS_H
#define NEXUS_DEFS_H
#ifdef __KERNEL__

#define DEBUG
#define MAX_SEGS_PER_IO 32
#define MAX_CHUNKS_PER_IO 32
#define MIN_CONCURRENT_REQS 2  /* XXX */
#define MAX_CHUNKSIZE 131072  /* XXX hack for preallocated tfm scratch space */
#define DEVICES 16  /* If this is more than 26, ctr will need to be fixed */
#define MINORS_PER_DEVICE 16
#define MAX_DEV_ALLOCATION_MULT 1  /* don't allocate > 10% RAM per device */
#define MAX_DEV_ALLOCATION_DIV 10
#define MAX_ALLOCATION_MULT 3  /* don't allocate > 30% RAM total */
#define MAX_ALLOCATION_DIV 10
#define LOWMEM_WAIT_TIME (HZ/10)
#define MODULE_NAME "openisr"
#define DEVICE_NAME "openisr"
#define KTHREAD_NAME "kopenisrd"
#define IOTHREAD_NAME "kopenisriod"
#define REQTHREAD_NAME "kopenisrblockd"
#define CD_NR_STATES 14  /* must shadow NR_STATES in chunkdata.c */

#include <linux/blkdev.h>
#include <linux/workqueue.h>
#include "nexus.h"

struct tfm_suite_info {
	char *user_name;
	char *cipher_name;       /* For <= 2.6.18 */
	unsigned cipher_mode;    /* For <= 2.6.18 */
	char *cipher_spec;       /* For >= 2.6.19 */
	unsigned cipher_block;
	unsigned key_len;
	char *hash_name;
	unsigned hash_len;
};

/* This needs access to tfm_suite_info */
#include "kcompat.h"

struct tfm_compress_info {
	char *user_name;
};

typedef sector_t chunk_t;

struct nexus_stats {
	unsigned state_count[CD_NR_STATES];
	unsigned state_time_us[CD_NR_STATES];
	unsigned state_time_samples[CD_NR_STATES];
	unsigned cache_hits;
	unsigned cache_misses;
	unsigned cache_alloc_failures;
	unsigned chunk_reads;
	unsigned chunk_writes;
	u64      data_bytes_written;
	unsigned whole_chunk_updates;
	unsigned encrypted_discards;
	unsigned chunk_errors;
	unsigned sectors_read;
	unsigned sectors_written;
};

struct nexus_tfm_state {
	struct crypto_blkcipher *cipher[NEXUS_NR_CRYPTO];
	struct crypto_hash *hash[NEXUS_NR_CRYPTO];
	struct scatterlist *zlib_sg;
	void *zlib_deflate;
	void *zlib_inflate;
	void *lzf_buf_compressed;
	void *lzf_buf_uncompressed;
	void *lzf_compress;
};

struct nexus_dev {
	struct list_head lh_devs;  /* updates synced by state.lock in init.c */
	struct list_head lh_run_requests;  /* ...queues.lock in thread.c */
	
	struct class_device *class_dev;
	struct gendisk *gendisk;
	request_queue_t *queue;
	spinlock_t queue_lock;
	struct block_device *chunk_bdev;
	struct work_struct cb_add_disk;
	
	struct list_head requests;
	spinlock_t requests_lock;
	struct timer_list requests_oom_timer;
	
	MUTEX lock;
	unsigned chunksize;
	unsigned cachesize;
	sector_t offset;
	chunk_t chunks;
	int devnum;
	uid_t owner;
	unsigned long flags;  /* use only atomic bit operations */
	struct nexus_stats stats;
	
	enum nexus_crypto suite;
	enum nexus_compress default_compression;
	compressmask_t supported_compression;
	
	struct chunkdata_table *chunkdata;
	/* Count of activities that need the userspace process to be there */
	unsigned need_user;
	wait_queue_head_t waiting_users;
};

/* nexus_dev flags */
enum dev_bits {
	__DEV_HAVE_CD_REF,    /* chunkdata holds a dev reference */
	__DEV_THR_REGISTERED, /* registered with thread.c */
	__DEV_REQ_PENDING,    /* a nexus_run_requests() job is pending */
};
#define dev_is_shutdown(dev) (list_empty(&dev->lh_devs))

struct nexus_io_chunk {
	struct list_head lh_pending;
	struct nexus_io *parent;
	chunk_t cid;
	unsigned orig_offset;  /* byte offset into orig_sg */
	unsigned offset;       /* byte offset into chunk */
	unsigned len;          /* bytes */
	unsigned flags;
	int error;
};

enum chunk_bits {
	__CHUNK_READ,         /* Needs to be read in before I/O starts */
	__CHUNK_STARTED,      /* I/O has been initiated */
	__CHUNK_COMPLETED,    /* I/O complete */
	__CHUNK_DEAD,         /* endio called */
};

/* nexus_io_chunk flags */
#define CHUNK_READ            (1 << __CHUNK_READ)
#define CHUNK_STARTED         (1 << __CHUNK_STARTED)
#define CHUNK_COMPLETED       (1 << __CHUNK_COMPLETED)
#define CHUNK_DEAD            (1 << __CHUNK_DEAD)

struct nexus_io {
	struct nexus_dev *dev;
	unsigned flags;
	chunk_t first_cid;
	chunk_t last_cid;
	unsigned prio;
	struct request *orig_req;
	struct scatterlist orig_sg[MAX_SEGS_PER_IO];
	struct nexus_io_chunk chunks[MAX_CHUNKS_PER_IO];
};

enum io_bits {
	__IO_WRITE,        /* Is a write request */
};

/* nexus_io flags */
#define IO_WRITE           (1 << __IO_WRITE)

/* enumerated from highest to lowest priority */
enum callback {
	CB_COMPLETE_IO,      /* completion of I/O to chunk store */
	CB_UPDATE_CHUNK,     /* chunkdata state machine */
	CB_CRYPTO,           /* encryption and decryption */
	NR_CALLBACKS
};

static inline void mutex_lock_thread(MUTEX *lock)
{
	/* Kernel threads can't receive signals, so they should never
	   be interrupted.  On the other hand, if they're in uninterruptible
	   sleep they contribute to the load average. */
	if (mutex_lock_interruptible(lock))
		BUG();
}

#ifdef CONFIG_LBD
#define SECTOR_FORMAT "%llu"
#else
#define SECTOR_FORMAT "%lu"
#endif

#define DBG_ANY		0xffffffff	/* Print if any debugging is enabled */
#define DBG_INIT	0x00000001	/* Module init and shutdown */
#define DBG_CTR		0x00000002	/* Constructor and destructor */
#define DBG_REFCOUNT	0x00000004	/* Device refcounting */
#define DBG_THREAD	0x00000008	/* Thread operations */
#define DBG_TFM		0x00000010	/* Crypto/compress operations */
#define DBG_REQUEST	0x00000020	/* Request processing */
#define DBG_CHARDEV	0x00000040	/* Character device */
#define DBG_CD		0x00000080	/* Chunkdata */
#define DBG_IO		0x00000100	/* Backing device I/O */

#define log(prio, msg, args...) printk(prio MODULE_NAME ": " msg "\n", ## args)
#define log_limit(prio, msg, args...) do { \
		if (printk_ratelimit()) \
			log(prio, msg, ## args); \
	} while (0)
#ifdef DEBUG
#define debug(type, msg, args...) do { \
		if ((type) & debug_mask) \
			log(KERN_DEBUG, msg, ## args); \
	} while (0)
#else
#define debug(args...) do {} while (0)
#endif
#define ndebug(args...) do {} while (0)

/* 512-byte sectors per chunk */
static inline sector_t chunk_sectors(struct nexus_dev *dev)
{
	return dev->chunksize/512;
}

/* PAGE_SIZE-sized pages per chunk, rounding up in case of a partial page */
static inline unsigned chunk_pages(struct nexus_dev *dev)
{
	return (dev->chunksize + PAGE_SIZE - 1) / PAGE_SIZE;
}

/* The sector number of the beginning of the chunk containing @sect */
static inline sector_t chunk_start(struct nexus_dev *dev, sector_t sect)
{
	/* We can't use the divide operator on a sector_t, because sector_t
	   might be 64 bits and 32-bit kernels need do_div() for 64-bit
	   divides */
	return sect & ~(chunk_sectors(dev) - 1);
}

/* The byte offset of sector @sect within its chunk */
static inline unsigned chunk_offset(struct nexus_dev *dev, sector_t sect)
{
	return 512 * (sect - chunk_start(dev, sect));
}

/* The number of bytes between @offset and the end of the chunk */
static inline unsigned chunk_remaining(struct nexus_dev *dev, unsigned offset)
{
	return dev->chunksize - offset;
}

/* The chunk number of @sect */
static inline chunk_t chunk_of(struct nexus_dev *dev, sector_t sect)
{
	/* Again, no division allowed */
	unsigned shift=fls(chunk_sectors(dev)) - 1;
	return sect >> shift;
}

/* The sector number corresponding to the first sector of @chunk */
static inline sector_t chunk_to_sector(struct nexus_dev *dev, chunk_t cid)
{
	unsigned shift=fls(chunk_sectors(dev)) - 1;
	return cid << shift;
}

/* The number of chunks in this io */
static inline unsigned io_chunks(struct nexus_io *io)
{
	return io->last_cid - io->first_cid + 1;
}

/* init.c */
extern int blk_major;
extern unsigned debug_mask;
struct nexus_dev *nexus_dev_ctr(char *devnode, unsigned chunksize,
			unsigned cachesize, sector_t offset,
			enum nexus_crypto crypto,
			enum nexus_compress default_compress,
			compressmask_t supported_compress);
void nexus_dev_get(struct nexus_dev *dev);
void nexus_dev_put(struct nexus_dev *dev, int unlink);
void user_get(struct nexus_dev *dev);
void user_put(struct nexus_dev *dev);
int shutdown_dev(struct nexus_dev *dev, int force);

/* request.c */
int request_start(void);
void request_shutdown(void);
void kick_elevator(struct nexus_dev *dev);
void nexus_request(request_queue_t *q);
void nexus_run_requests(struct list_head *entry);
void nexus_process_chunk(struct nexus_io_chunk *chunk,
			struct scatterlist *chunk_sg);
void oom_timer_fn(unsigned long data);

/* chardev.c */
int chardev_start(void);
void chardev_shutdown(void);

/* chunkdata.c */
int chunkdata_start(void);
void chunkdata_shutdown(void);
int chunkdata_alloc_table(struct nexus_dev *dev);
void chunkdata_free_table(struct nexus_dev *dev);
int have_usermsg(struct nexus_dev *dev);
struct chunkdata *next_usermsg(struct nexus_dev *dev, msgtype_t *type);
void fail_usermsg(struct chunkdata *cd);
void end_usermsg(struct chunkdata *cd);
void shutdown_usermsg(struct nexus_dev *dev);
void get_usermsg_get_meta(struct chunkdata *cd, unsigned long long *cid);
void get_usermsg_update_meta(struct chunkdata *cd, unsigned long long *cid,
			unsigned *length, enum nexus_compress *compression,
			char key[], char tag[]);
void set_usermsg_set_meta(struct nexus_dev *dev, chunk_t cid, unsigned length,
			enum nexus_compress compression, char key[],
			char tag[]);
void set_usermsg_meta_err(struct nexus_dev *dev, chunk_t cid);
int reserve_chunks(struct nexus_io *io);
void unreserve_chunk(struct nexus_io_chunk *chunk);
void run_chunk(struct list_head *entry);
void run_all_chunks(struct nexus_dev *dev);
void chunkdata_complete_io(struct list_head *entry);
void chunk_tfm(struct nexus_tfm_state *ts, struct list_head *entry);
struct scatterlist *alloc_scatterlist(unsigned nbytes);
void free_scatterlist(struct scatterlist *sg, unsigned nbytes);

/* transform.c */
int transform_validate(struct nexus_dev *dev);
int crypto_cipher(struct nexus_dev *dev, struct nexus_tfm_state *ts,
			struct scatterlist *sg, char key[], unsigned len,
			int dir, int doPad);
int crypto_hash(struct nexus_dev *dev, struct nexus_tfm_state *ts,
			struct scatterlist *sg, unsigned nbytes, u8 *out);
int compress_chunk(struct nexus_dev *dev, struct nexus_tfm_state *ts,
			struct scatterlist *sg, enum nexus_compress type);
int decompress_chunk(struct nexus_dev *dev, struct nexus_tfm_state *ts,
			struct scatterlist *sg, enum nexus_compress type,
			unsigned len);
int compression_type_ok(struct nexus_dev *dev, enum nexus_compress compress);
int suite_add(struct nexus_tfm_state *ts, enum nexus_crypto suite);
void suite_remove(struct nexus_tfm_state *ts, enum nexus_crypto suite);
int compress_add(struct nexus_tfm_state *ts, enum nexus_compress alg);
void compress_remove(struct nexus_tfm_state *ts, enum nexus_compress alg);
const struct tfm_suite_info *suite_info(enum nexus_crypto suite);
const struct tfm_compress_info *compress_info(enum nexus_compress alg);

/* thread.c */
int thread_start(void);
void thread_shutdown(void);
int thread_register(struct nexus_dev *dev);
void thread_unregister(struct nexus_dev *dev);
void schedule_callback(enum callback type, struct list_head *entry);
void schedule_io(struct bio *bio);
void schedule_request_callback(struct list_head *entry);
void wake_all_threads(void);

/* sysfs.c */
extern struct class_attribute class_attrs[];
extern struct class_device_attribute class_dev_attrs[];

/* revision.c */
extern char *rcs_revision;

#else  /* __KERNEL__ */
#error This header is not exported outside the Nexus implementation
#endif /* __KERNEL__ */
#endif /* NEXUS_DEFS_H */
