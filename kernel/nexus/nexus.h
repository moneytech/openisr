/* nexus.h - interface exported to userspace via character device */

/* 
 * Nexus - convergently encrypting virtual disk driver for the OpenISR (R)
 *         system
 * 
 * Copyright (C) 2006-2008 Carnegie Mellon University
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

#ifndef ISR_NEXUS_H
#define ISR_NEXUS_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define NEXUS_MAX_DEVICE_LEN 64
#define NEXUS_MAX_HASH_LEN 32

/* ioctls */
#define NEXUS_IOC_REGISTER      _IOWR(0x1a, 0, struct nexus_setup)
#define NEXUS_IOC_UNREGISTER      _IO(0x1a, 1)
#define NEXUS_IOC_CONFIG_THREAD   _IO(0x1a, 2)

#define NEXUS_INTERFACE_VERSION 8

typedef __u16 compressmask_t;
typedef __u16 msgtype_t;
typedef __u8  nexus_err_t;

/* The numeric values of these symbols are not guaranteed to remain constant!
   Don't use them in an on-disk format! */
enum nexus_crypto {
	NEXUS_CRYPTO_NONE_SHA1,
	NEXUS_CRYPTO_BLOWFISH_SHA1,
	NEXUS_CRYPTO_AES_SHA1,
	NEXUS_NR_CRYPTO
};

enum nexus_compress {
	NEXUS_COMPRESS_NONE,
	NEXUS_COMPRESS_ZLIB,
	NEXUS_COMPRESS_LZF,
	NEXUS_NR_COMPRESS
};

/* nexus_err_t is composed of an error code (&enum nexus_chunk_err) and
   a flag bit (%NEXUS_ERR_IS_WRITE). */
enum nexus_chunk_err {
	NEXUS_ERR_NOERR=0,
	NEXUS_ERR_IO,
	NEXUS_ERR_TAG,
	NEXUS_ERR_KEY,
	NEXUS_ERR_HASH,
	NEXUS_ERR_CRYPT,
	NEXUS_ERR_COMPRESS,
};
#define NEXUS_ERR_IS_WRITE 0x80

/**
 * struct nexus_setup - information exchanged during NEXUS_IOC_REGISTER
 * @ident            : unique identifier for this device (null-terminated) (k)
 * @chunk_device     : path to the chunk-store block device (k)
 * @offset           : starting sector of first chunk in chunk store (k)
 * @chunksize        : chunk size in bytes (k)
 * @cachesize        : size of in-core chunk cache in entries (k)
 * @crypto           : &enum nexus_crypto choice for this device (k)
 * @compress_default : &enum nexus_compress choice for new chunks (k)
 * @compress_required: bitmask of compression algorithms we must support (k)
 * @pad              : reserved
 * @chunks           : number of chunks the chunk store will hold (u)
 * @major            : the major number of the allocated block device (u)
 * @num_minors       : number of minor numbers given to each Nexus blkdev (u)
 * @index            : the index of this block device (u)
 * @hash_len         : length of key and tag values in bytes (u)
 *
 * Fields labeled (k) are provided to the kernel on NEXUS_IOC_REGISTER.
 * Fields labeled (u) are filled in by the kernel when the ioctl returns.
 *
 * @index can be used to determine the name of the device node (e.g.,
 * printf("/dev/openisr%c", 'a' + index)).
 *
 * @compress_required is a bitmask with bits of the form
 * (1 << NEXUS_COMPRESS_FOO).
 *
 * This structure must have an identical layout on 32-bit and 64-bit systems.
 * We don't use enum types for crypto and compress fields because the compiler
 * is allowed to pick any size for them.
 **/
struct nexus_setup {
	/* To kernel: */
	__u8 ident[NEXUS_MAX_DEVICE_LEN];
	__u8 chunk_device[NEXUS_MAX_DEVICE_LEN];
	__u64 offset;
	__u32 chunksize;
	__u32 cachesize;
	__u8 crypto;
	__u8 compress_default;
	compressmask_t compress_required;
	
	__u32 pad;
	
	/* To user: */
	__u64 chunks;
	__u32 major;
	__u32 num_minors;
	__u32 index;
	__u8 hash_len;
};

/**
 * struct nexus_message - a message sent over the character device
 * @chunk      : the chunk number
 * @length     : the data length (may be < chunksize if compressed)
 * @type       : message type
 * @compression: compression type for this chunk (&enum nexus_compress)
 * @key        : encryption key
 * @tag        : CAS tag
 * @err        : &enum nexus_chunk_err with 0x80 set if this is a write error
 * @expected   : expected tag/key if this is a mismatch error
 * @found      : found tag/key if this is a mismatch error
 *
 * This structure must have an identical layout on 32-bit and 64-bit systems.
 * We don't use enum types for crypto and compress fields because the compiler
 * is allowed to pick any size for them.
 *
 * NEXUS_MSGTYPE_GET_META:
 * Sent from kernel to userspace to request information on a chunk.  @chunk is
 * valid.  Userspace must respond with SET_META or META_HARDERR.  Responses do
 * not need to be in the same order as requests.
 *
 * NEXUS_MSGTYPE_UPDATE_META:
 * Sent from kernel to userspace to report new metadata for a chunk.  @chunk,
 * @length, @compression, @tag, and @key are valid.  No reply is necessary.
 *
 * NEXUS_MSGTYPE_CHUNK_ERR:
 * Sent from kernel to userspace to report an I/O or decoding error for a
 * chunk.  @chunk and @err are always valid.  @expected and @found are valid
 * if @err is %NEXUS_ERR_TAG or %NEXUS_ERR_KEY.
 *
 * NEXUS_MSGTYPE_SET_META:
 * Sent from userspace to kernel to supply requested information for a chunk.
 * May only be sent in response to GET_META; unsolicited SET_META is not
 * allowed.  @chunk, @length, @compression, @key, and @tag must be valid.
 *
 * NEXUS_MSGTYPE_META_HARDERR:
 * Sent from userspace to kernel to report inability to supply the information
 * requested via GET_META.  May only be sent in response to GET_META.  Only
 * @chunk need be valid.  This causes the chunk to enter an state in which all
 * I/O to the chunk will be failed with an I/O error.  The chunk will remain
 * in this state until it ages out of cache or the entire chunk is overwritten
 * in a single I/O.
 **/
struct nexus_message {
	__u64 chunk;
	__u32 length;
	msgtype_t type;
	union {
		__u8 compression;
		nexus_err_t err;
	};
	union {
		__u8 key[NEXUS_MAX_HASH_LEN];
		__u8 expected[NEXUS_MAX_HASH_LEN];
	};
	union {
		__u8 tag[NEXUS_MAX_HASH_LEN];
		__u8 found[NEXUS_MAX_HASH_LEN];
	};
};

/* Kernel to user */
#define NEXUS_MSGTYPE_GET_META     ((msgtype_t) 0x0000)
#define NEXUS_MSGTYPE_UPDATE_META  ((msgtype_t) 0x0001)
#define NEXUS_MSGTYPE_CHUNK_ERR    ((msgtype_t) 0x0002)
/* User to kernel */
#define NEXUS_MSGTYPE_SET_META     ((msgtype_t) 0x1000)
#define NEXUS_MSGTYPE_META_HARDERR ((msgtype_t) 0x1001)

/* The names of the states in the chunkdata state machine.  These can be used
   to interpret the "states" and "state_times" attributes in sysfs. */
#define NEXUS_STATES \
	/* No key or data */						\
	NEXUS_STATE(INVALID)						\
	/* Loading metadata */						\
	NEXUS_STATE(LOAD_META)						\
	/* Have metadata but not data */				\
	NEXUS_STATE(META)						\
	/* Loading data */						\
	NEXUS_STATE(LOAD_DATA)						\
	/* Have metadata and clean, encrypted data */			\
	NEXUS_STATE(ENCRYPTED)						\
	/* Decrypting data */						\
	NEXUS_STATE(DECRYPTING)						\
	/* Have metadata and data */					\
	NEXUS_STATE(CLEAN)						\
	/* Data is dirty */						\
	NEXUS_STATE(DIRTY)						\
	/* Encrypting data */						\
	NEXUS_STATE(ENCRYPTING)						\
	/* Data is dirty and encryption has finished */			\
	NEXUS_STATE(DIRTY_ENCRYPTED)					\
	/* Storing data */						\
	NEXUS_STATE(STORE_DATA)						\
	/* Metadata is dirty */						\
	NEXUS_STATE(DIRTY_META)						\
	/* Storing metadata */						\
	NEXUS_STATE(STORE_META)						\
	/* Error; data not valid; must notify userspace */		\
	NEXUS_STATE(ERROR_USER)						\
	/* Userspace notification queued */				\
	NEXUS_STATE(ERROR_PENDING)					\
	/* I/O error occurred; data not valid */			\
	NEXUS_STATE(ERROR)            

#ifdef __KERNEL__
static inline void __nexus_h_sanity_check(void)
{
	struct nexus_setup setup;
	struct nexus_message message;
	
	/* Make sure the relevant structure fields are big enough to hold
	   every possible value of their corresponding enums */
	BUILD_BUG_ON((1 << (8 * sizeof(setup.crypto))) < NEXUS_NR_CRYPTO);
	BUILD_BUG_ON((1 << (8 * sizeof(setup.compress_default)))
				< NEXUS_NR_COMPRESS);
	BUILD_BUG_ON((1 << (8 * sizeof(message.compression)))
				< NEXUS_NR_COMPRESS);
	BUILD_BUG_ON(8 * sizeof(compressmask_t) < NEXUS_NR_COMPRESS);
	
	/* Alignment of struct nexus_setup depends on this */
	BUILD_BUG_ON(NEXUS_MAX_DEVICE_LEN % 8 != 0);
}
#endif

#endif
