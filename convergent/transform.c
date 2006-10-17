#include <linux/highmem.h>
#include <linux/vmalloc.h>
#include <linux/interrupt.h>
#include <linux/crypto.h>
#include <linux/zlib.h>
#include "convergent.h"
#include "lzf.h"

/* XXX this needs to go away. */
static void scatterlist_transfer(struct convergent_dev *dev,
			struct scatterlist *sg, void *buf, int dir)
{
	void *page;
	int i;
	
	/* We use KM_USER0 */
	BUG_ON(in_interrupt());
	for (i=0; i<chunk_pages(dev); i++) {
		BUG_ON(sg[i].offset != 0);
		page=kmap_atomic(sg[i].page, KM_USER0);
		if (dir == READ)
			memcpy(buf + i * PAGE_SIZE, page, sg[i].length);
		else
			memcpy(page, buf + i * PAGE_SIZE, sg[i].length);
		kunmap_atomic(page, KM_USER0);
	}
}

/* Perform PKCS padding on a buffer.  Return the new buffer length. */
static int crypto_pad(struct convergent_dev *dev, void *buf,
			unsigned datalen, unsigned buflen)
{
	unsigned padlen=dev->cipher_block - (datalen % dev->cipher_block);
	char *cbuf=buf;
	unsigned i;
	ndebug("Pad %u", padlen);
	if (datalen + padlen >= buflen) {
		/* If padding would bring us exactly to the length of
		   the buffer, we refuse to do it.  Rationale: we're only
		   doing padding if we're doing compression, and compression
		   failed to reduce the size of the chunk after padding,
		   so we're better off just not compressing. */
		return -EFBIG;
	}
	for (i=0; i<padlen; i++)
		cbuf[datalen + i]=(char)padlen;
	return datalen + padlen;
}

/* Perform PKCS unpadding on a buffer.  Return the new buffer length. */
static int crypto_unpad(struct convergent_dev *dev, void *buf, int len)
{
	char *cbuf=buf;
	unsigned padlen=(unsigned)cbuf[len - 1];
	unsigned i;
	ndebug("Unpad %u", padlen);
	if (padlen == 0 || padlen > dev->cipher_block)
		return -EINVAL;
	for (i=2; i <= padlen; i++)
		if (cbuf[len - i] != padlen)
			return -EINVAL;
	return len - padlen;
}

/* XXX consolidate duplicate code between this and lzf? */
/* XXX this should be converted to use scatterlists rather than a vmalloc
   buffer */
static int compress_chunk_zlib(struct convergent_dev *dev,
			struct scatterlist *sg)
{
	z_stream strm;
	int ret;
	int ret2;
	int size;
	
	BUG_ON(!mutex_is_locked(&dev->lock));
	scatterlist_transfer(dev, sg, dev->buf_uncompressed, READ);
	strm.workspace=dev->zlib_deflate;
	/* XXX keep persistent stream structures? */
	ret=zlib_deflateInit(&strm, Z_DEFAULT_COMPRESSION);
	if (ret != Z_OK) {
		debug("zlib_deflateInit failed");
		return -EIO;
	}
	strm.next_in=dev->buf_uncompressed;
	strm.avail_in=dev->chunksize;
	strm.next_out=dev->buf_compressed;
	strm.avail_out=dev->chunksize;
	ret=zlib_deflate(&strm, Z_FINISH);
	size=strm.total_out;
	ret2=zlib_deflateEnd(&strm);
	if (ret == Z_OK) {
		/* Compressed data larger than uncompressed data */
		return -EFBIG;
	} else if (ret != Z_STREAM_END || ret2 != Z_OK) {
		debug("zlib compression failed");
		return -EIO;
	}
	ret=crypto_pad(dev, dev->buf_compressed, size, dev->chunksize);
	if (ret < 0) {
		/* We tried padding the compresstext, but that made it
		   at least as large as chunksize */
		return ret;
	}
	size=ret;
	/* We write the whole chunk out to disk, so make sure we're not
	   leaking data. */
	memset(dev->buf_compressed + size, 0, dev->chunksize - size);
	scatterlist_transfer(dev, sg, dev->buf_compressed, WRITE);
	return size;
}

/* XXX this should be converted to use scatterlists rather than a vmalloc
   buffer */
static int decompress_chunk_zlib(struct convergent_dev *dev,
			struct scatterlist *sg, unsigned len)
{
	z_stream strm;
	int ret;
	int ret2;
	int size;
	
	BUG_ON(!mutex_is_locked(&dev->lock));
	/* XXX don't need to transfer whole scatterlist */
	scatterlist_transfer(dev, sg, dev->buf_compressed, READ);
	size=crypto_unpad(dev, dev->buf_compressed, len);
	if (size < 0)
		return -EIO;
	strm.workspace=dev->zlib_inflate;
	/* XXX keep persistent stream structures? */
	ret=zlib_inflateInit(&strm);
	if (ret != Z_OK) {
		debug("zlib_inflateInit failed");
		return -EIO;
	}
	strm.next_in=dev->buf_compressed;
	strm.avail_in=size;
	strm.next_out=dev->buf_uncompressed;
	strm.avail_out=dev->chunksize;
	ret=zlib_inflate(&strm, Z_FINISH);
	size=strm.total_out;
	ret2=zlib_inflateEnd(&strm);
	if (ret != Z_STREAM_END || ret2 != Z_OK)
		return -EIO;
	if (size != dev->chunksize)
		return -EIO;
	scatterlist_transfer(dev, sg, dev->buf_uncompressed, WRITE);
	return 0;
}

static int compress_chunk_lzf(struct convergent_dev *dev,
			struct scatterlist *sg)
{
	int size;
	
	BUG_ON(!mutex_is_locked(&dev->lock));
	scatterlist_transfer(dev, sg, dev->buf_uncompressed, READ);
	size=lzf_compress(dev->buf_uncompressed, dev->chunksize,
				dev->buf_compressed, dev->chunksize,
				dev->lzf_compress);
	if (size == 0) {
		/* Compressed data larger than uncompressed data */
		return -EFBIG;
	}
	size=crypto_pad(dev, dev->buf_compressed, size, dev->chunksize);
	if (size < 0) {
		/* We tried padding the compresstext, but that made it
		   at least as large as chunksize */
		return size;
	}
	/* We write the whole chunk out to disk, so make sure we're not
	   leaking data. */
	memset(dev->buf_compressed + size, 0, dev->chunksize - size);
	scatterlist_transfer(dev, sg, dev->buf_compressed, WRITE);
	return size;
}

static int decompress_chunk_lzf(struct convergent_dev *dev,
			struct scatterlist *sg, unsigned len)
{
	int size;
	
	BUG_ON(!mutex_is_locked(&dev->lock));
	/* XXX don't need to transfer whole scatterlist */
	scatterlist_transfer(dev, sg, dev->buf_compressed, READ);
	size=crypto_unpad(dev, dev->buf_compressed, len);
	if (size < 0)
		return -EIO;
	size=lzf_decompress(dev->buf_compressed, size,
				dev->buf_uncompressed, dev->chunksize);
	if (size != dev->chunksize)
		return -EIO;
	scatterlist_transfer(dev, sg, dev->buf_uncompressed, WRITE);
	return 0;
}

/* Guaranteed to return data which is an even multiple of the crypto
   block size. */
int compress_chunk(struct convergent_dev *dev, struct scatterlist *sg,
			compress_t type)
{
	BUG_ON(!mutex_is_locked(&dev->lock));
	switch (type) {
	case ISR_COMPRESS_NONE:
		return dev->chunksize;
	case ISR_COMPRESS_ZLIB:
		return compress_chunk_zlib(dev, sg);
	case ISR_COMPRESS_LZF:
		return compress_chunk_lzf(dev, sg);
	default:
		BUG();
		return -EIO;
	}
}

int decompress_chunk(struct convergent_dev *dev, struct scatterlist *sg,
			compress_t type, unsigned len)
{
	BUG_ON(!mutex_is_locked(&dev->lock));
	switch (type) {
	case ISR_COMPRESS_NONE:
		if (len != dev->chunksize)
			return -EIO;
		return 0;
	case ISR_COMPRESS_ZLIB:
		return decompress_chunk_zlib(dev, sg, len);
	case ISR_COMPRESS_LZF:
		return decompress_chunk_lzf(dev, sg, len);
	default:
		BUG();
		return -EIO;
	}
}

/* For some reason, the cryptoapi digest functions expect nsg rather than
   nbytes.  However, when we're hashing compressed data, we may want the
   hash to include only part of a page.  Thus this nonsense. */
/* XXX verify this against test vectors */
void crypto_hash(struct convergent_dev *dev, struct scatterlist *sg,
			unsigned nbytes, u8 *out)
{
	int i;
	unsigned saved;
	
	BUG_ON(!mutex_is_locked(&dev->lock));
	for (i=0; sg[i].length < nbytes; i++)
		nbytes -= sg[i].length;
	saved=sg[i].length;
	sg[i].length=nbytes;
	crypto_digest_digest(dev->hash, sg, i + 1, out);
	sg[i].length=saved;
}

int crypto_cipher(struct convergent_dev *dev, struct scatterlist *sg,
			char key[], unsigned len, int dir)
{
	char iv[8]={0}; /* XXX */
	int ret;
	
	BUG_ON(!mutex_is_locked(&dev->lock));
	crypto_cipher_set_iv(dev->cipher, iv, sizeof(iv));
	ret=crypto_cipher_setkey(dev->cipher, key, dev->hash_len);
	if (ret)
		return ret;
	if (dir == READ)
		ret=crypto_cipher_decrypt(dev->cipher, sg, sg, len);
	else
		ret=crypto_cipher_encrypt(dev->cipher, sg, sg, len);
	if (ret)
		return ret;
	return 0;
}

int compression_type_ok(struct convergent_dev *dev, compress_t compress)
{
	/* Make sure only one bit is set */
	if (compress & (compress - 1))
		return 0;
	/* Make sure we have been configured to accept the bit */
	if (!(compress & dev->supported_compression))
		return 0;
	return 1;
}

#define SUPPORTED_COMPRESSION  (ISR_COMPRESS_NONE | \
				ISR_COMPRESS_ZLIB | \
				ISR_COMPRESS_LZF)
int transform_alloc(struct convergent_dev *dev, cipher_t cipher, hash_t hash,
			compress_t default_compress,
			compress_t supported_compress)
{
	char *cipher_name;
	unsigned cipher_mode;
	char *hash_name;
	
	switch (cipher) {
	case ISR_CIPHER_BLOWFISH:
		cipher_name="blowfish";
		cipher_mode=CRYPTO_TFM_MODE_CBC;
		break;
	default:
		log(KERN_ERR, "Unsupported cipher requested");
		return -EINVAL;
	}
	
	switch (hash) {
	case ISR_HASH_SHA1:
		hash_name="sha1";
		break;
	default:
		log(KERN_ERR, "Unsupported hash requested");
		return -EINVAL;
	}
	
	if ((supported_compress & SUPPORTED_COMPRESSION)
				!= supported_compress) {
		log(KERN_ERR, "Unsupported compression algorithm requested");
		return -EINVAL;
	}
	dev->supported_compression=supported_compress;
	if (!compression_type_ok(dev, default_compress)) {
		log(KERN_ERR, "Requested invalid default compression "
					"algorithm");
		return -EINVAL;
	}
	dev->default_compression=default_compress;
	
	dev->cipher=crypto_alloc_tfm(cipher_name, cipher_mode);
	dev->hash=crypto_alloc_tfm(hash_name, 0);
	if (dev->cipher == NULL || dev->hash == NULL)
		return -EINVAL;
	dev->cipher_block=crypto_tfm_alg_blocksize(dev->cipher);
	dev->hash_len=crypto_tfm_alg_digestsize(dev->hash);
	
	if (dev->supported_compression != ISR_COMPRESS_NONE) {
		/* XXX this is not ideal, but there's no good way to support
		   scatterlists in LZF without hacking the code. */
		dev->buf_compressed=vmalloc(dev->chunksize);
		dev->buf_uncompressed=vmalloc(dev->chunksize);
		if (dev->buf_compressed == NULL ||
					dev->buf_uncompressed == NULL)
			return -ENOMEM;
	}
	
	if (dev->supported_compression & ISR_COMPRESS_ZLIB) {
		/* The deflate workspace size is too large for kmalloc */
		dev->zlib_deflate=vmalloc(zlib_deflate_workspacesize());
		dev->zlib_inflate=kmalloc(zlib_inflate_workspacesize(),
					GFP_KERNEL);
		if (dev->zlib_deflate == NULL || dev->zlib_inflate == NULL)
			return -ENOMEM;
	}
	
	if (dev->supported_compression & ISR_COMPRESS_LZF) {
		dev->lzf_compress=kmalloc(sizeof(LZF_STATE), GFP_KERNEL);
		if (dev->lzf_compress == NULL)
			return -ENOMEM;
	}
	return 0;
}

void transform_free(struct convergent_dev *dev)
{
	if (dev->lzf_compress)
		kfree(dev->lzf_compress);
	if (dev->zlib_inflate)
		kfree(dev->zlib_inflate);
	if (dev->zlib_deflate)
		vfree(dev->zlib_deflate);
	if (dev->buf_uncompressed)
		vfree(dev->buf_uncompressed);
	if (dev->buf_compressed)
		vfree(dev->buf_compressed);
	if (dev->hash)
		crypto_free_tfm(dev->hash);
	if (dev->cipher)
		crypto_free_tfm(dev->cipher);
}
