/* LibTomCrypt, modular cryptographic library -- Tom St Denis
 *
 * LibTomCrypt is a library that provides various cryptographic
 * algorithms in a highly modular and flexible manner.
 *
 * The library is free for all purposes without any express
 * guarantee it works.
 *
 * Tom St Denis, tomstdenis@gmail.com, http://libtomcrypt.com
 */

/* always stores the same # of bytes, pads with leading zero bytes
   as required
 */
/**
   PKCS #1 Integer to binary
   @param n             The integer to store
   @param modulus_len   The length of the RSA modulus
   @param out           [out] The destination for the integer
   @return CRYPT_OK if successful
*/
int pkcs_1_i2osp(void *n, unsigned long modulus_len, unsigned char *out)
{
   unsigned long size;

   size = mp_unsigned_bin_size(n);

   if (size > modulus_len) {
      return CRYPT_BUFFER_OVERFLOW;
   }

   /* store it */
   zeromem(out, modulus_len);
   return mp_to_unsigned_bin(n, out+(modulus_len-size));
}

/**
  Read a binary string into an mp_int
  @param n          [out] The mp_int destination
  @param in         The binary string to read
  @param inlen      The length of the binary string
  @return CRYPT_OK if successful
*/
int pkcs_1_os2ip(void *n, unsigned char *in, unsigned long inlen)
{
   return mp_read_unsigned_bin(n, in, inlen);
}

/**
   Perform PKCS #1 MGF1 (internal)
   @param seed        The seed for MGF1
   @param seedlen     The length of the seed
   @param hash_idx    The index of the hash desired
   @param mask        [out] The destination
   @param masklen     The length of the mask desired
   @return CRYPT_OK if successful
*/
static int pkcs_1_mgf1(int                  hash_idx,
                const unsigned char *seed, unsigned long seedlen,
                      unsigned char *mask, unsigned long masklen)
{
   unsigned long hLen, x;
   uint32_t      counter;
   int           err;
   hash_state    *md;
   unsigned char *buf;
 
   LTC_ARGCHK(seed != NULL);
   LTC_ARGCHK(mask != NULL);

   /* ensure valid hash */
   if ((err = hash_is_valid(hash_idx)) != CRYPT_OK) { 
      return err;
   }

   /* get hash output size */
   hLen = hash_descriptor[hash_idx].hashsize;

   /* allocate memory */
   md  = XMALLOC(sizeof(hash_state));
   buf = XMALLOC(hLen);
   if (md == NULL || buf == NULL) {
      if (md != NULL) {
         XFREE(md);
      }
      if (buf != NULL) {
         XFREE(buf);
      }
      return CRYPT_MEM;
   }

   /* start counter */
   counter = 0;

   while (masklen > 0) {
       /* handle counter */
       STORE32H(counter, buf);
       ++counter;

       /* get hash of seed || counter */
       if ((err = hash_descriptor[hash_idx].init(md)) != CRYPT_OK) {
          goto LBL_ERR;
       }
       if ((err = hash_descriptor[hash_idx].process(md, seed, seedlen)) != CRYPT_OK) {
          goto LBL_ERR;
       }
       if ((err = hash_descriptor[hash_idx].process(md, buf, 4)) != CRYPT_OK) {
          goto LBL_ERR;
       }
       if ((err = hash_descriptor[hash_idx].done(md, buf)) != CRYPT_OK) {
          goto LBL_ERR;
       }

       /* store it */
       for (x = 0; x < hLen && masklen > 0; x++, masklen--) {
          *mask++ = buf[x];
       }
   }

   err = CRYPT_OK;
LBL_ERR:
#ifdef LTC_CLEAN_STACK
   zeromem(buf, hLen);
   zeromem(md,  sizeof(hash_state));
#endif

   XFREE(buf);
   XFREE(md);

   return err;
}

/**
   PKCS #1 v2.00 Signature Encoding
   @param msghash          The hash to encode
   @param msghashlen       The length of the hash (octets)
   @param saltlen          The length of the salt desired (octets)
   @param prng             An active PRNG context
   @param prng_idx         The index of the PRNG desired
   @param hash_idx         The index of the hash desired
   @param modulus_bitlen   The bit length of the RSA modulus
   @param out              [out] The destination of the encoding
   @param outlen           [in/out] The max size and resulting size of the encoded data
   @return CRYPT_OK if successful
*/
int pkcs_1_pss_encode(const unsigned char *msghash, unsigned long msghashlen,
                            unsigned long saltlen,  prng_state   *prng,     
                            int           prng_idx, int           hash_idx,
                            unsigned long modulus_bitlen,
                            unsigned char *out,     unsigned long *outlen)
{
   unsigned char *DB, *mask, *salt, *hash;
   unsigned long x, y, hLen, modulus_len;
   int           err;
   hash_state    md;

   LTC_ARGCHK(msghash != NULL);
   LTC_ARGCHK(out     != NULL);
   LTC_ARGCHK(outlen  != NULL);

   /* ensure hash and PRNG are valid */
   if ((err = hash_is_valid(hash_idx)) != CRYPT_OK) {
      return err;
   }
   if ((err = prng_is_valid(prng_idx)) != CRYPT_OK) {
      return err;
   }

   hLen        = hash_descriptor[hash_idx].hashsize;
   modulus_len = (modulus_bitlen>>3) + (modulus_bitlen & 7 ? 1 : 0);

   /* check sizes */
   if ((saltlen > modulus_len) || (modulus_len < hLen + saltlen + 2)) {
      return CRYPT_PK_INVALID_SIZE;
   }

   /* allocate ram for DB/mask/salt/hash of size modulus_len */
   DB   = XMALLOC(modulus_len);
   mask = XMALLOC(modulus_len);
   salt = XMALLOC(modulus_len);
   hash = XMALLOC(modulus_len);
   if (DB == NULL || mask == NULL || salt == NULL || hash == NULL) {
      if (DB != NULL) {
         XFREE(DB);
      }
      if (mask != NULL) {
         XFREE(mask);
      }
      if (salt != NULL) {
         XFREE(salt);
      }
      if (hash != NULL) {
         XFREE(hash);
      }
      return CRYPT_MEM;
   }


   /* generate random salt */
   if (saltlen > 0) {
      if (prng_descriptor[prng_idx].read(salt, saltlen, prng) != saltlen) {
         err = CRYPT_ERROR_READPRNG;
         goto LBL_ERR;
      }
   }

   /* M = (eight) 0x00 || msghash || salt, hash = H(M) */
   if ((err = hash_descriptor[hash_idx].init(&md)) != CRYPT_OK) {
      goto LBL_ERR;
   }
   zeromem(DB, 8);
   if ((err = hash_descriptor[hash_idx].process(&md, DB, 8)) != CRYPT_OK) {
      goto LBL_ERR;
   }
   if ((err = hash_descriptor[hash_idx].process(&md, msghash, msghashlen)) != CRYPT_OK) {
      goto LBL_ERR;
   }
   if ((err = hash_descriptor[hash_idx].process(&md, salt, saltlen)) != CRYPT_OK) {
      goto LBL_ERR;
   }
   if ((err = hash_descriptor[hash_idx].done(&md, hash)) != CRYPT_OK) {
      goto LBL_ERR;
   }

   /* generate DB = PS || 0x01 || salt, PS == modulus_len - saltlen - hLen - 2 zero bytes */
   x = 0;
   XMEMSET(DB + x, 0, modulus_len - saltlen - hLen - 2);
   x += modulus_len - saltlen - hLen - 2;
   DB[x++] = 0x01;
   XMEMCPY(DB + x, salt, saltlen);
   x += saltlen;

   /* generate mask of length modulus_len - hLen - 1 from hash */
   if ((err = pkcs_1_mgf1(hash_idx, hash, hLen, mask, modulus_len - hLen - 1)) != CRYPT_OK) {
      goto LBL_ERR;
   }

   /* xor against DB */
   for (y = 0; y < (modulus_len - hLen - 1); y++) {
      DB[y] ^= mask[y];
   }

   /* output is DB || hash || 0xBC */
   if (*outlen < modulus_len) {
      *outlen = modulus_len;
      err = CRYPT_BUFFER_OVERFLOW;
      goto LBL_ERR;
   }

   /* DB len = modulus_len - hLen - 1 */
   y = 0;
   XMEMCPY(out + y, DB, modulus_len - hLen - 1);
   y += modulus_len - hLen - 1;

   /* hash */
   XMEMCPY(out + y, hash, hLen);
   y += hLen;

   /* 0xBC */
   out[y] = 0xBC;

   /* now clear the 8*modulus_len - modulus_bitlen most significant bits */
   out[0] &= 0xFF >> ((modulus_len<<3) - (modulus_bitlen-1));

   /* store output size */
   *outlen = modulus_len;
   err = CRYPT_OK;
LBL_ERR:
#ifdef LTC_CLEAN_STACK
   zeromem(DB,   modulus_len);   
   zeromem(mask, modulus_len);   
   zeromem(salt, modulus_len);   
   zeromem(hash, modulus_len);   
#endif

   XFREE(hash);
   XFREE(salt);
   XFREE(mask);
   XFREE(DB);

   return err;
}

/**
   PKCS #1 v2.00 PSS decode
   @param  msghash         The hash to verify
   @param  msghashlen      The length of the hash (octets)
   @param  sig             The signature data (encoded data)
   @param  siglen          The length of the signature data (octets)
   @param  saltlen         The length of the salt used (octets)
   @param  hash_idx        The index of the hash desired
   @param  modulus_bitlen  The bit length of the RSA modulus
   @param  res             [out] The result of the comparison, 1==valid, 0==invalid
   @return CRYPT_OK if successful (even if the comparison failed)
*/
int pkcs_1_pss_decode(const unsigned char *msghash, unsigned long msghashlen,
                      const unsigned char *sig,     unsigned long siglen,
                            unsigned long saltlen,  int           hash_idx,
                            unsigned long modulus_bitlen, int    *res)
{
   unsigned char *DB, *mask, *salt, *hash;
   unsigned long x, y, hLen, modulus_len;
   int           err;
   hash_state    md;

   LTC_ARGCHK(msghash != NULL);
   LTC_ARGCHK(res     != NULL);

   /* default to invalid */
   *res = 0;

   /* ensure hash is valid */
   if ((err = hash_is_valid(hash_idx)) != CRYPT_OK) {
      return err;
   }

   hLen        = hash_descriptor[hash_idx].hashsize;
   modulus_len = (modulus_bitlen>>3) + (modulus_bitlen & 7 ? 1 : 0);

   /* check sizes */
   if ((saltlen > modulus_len) || 
       (modulus_len < hLen + saltlen + 2) || (siglen != modulus_len)) {
      return CRYPT_PK_INVALID_SIZE;
   }

   /* allocate ram for DB/mask/salt/hash of size modulus_len */
   DB   = XMALLOC(modulus_len);
   mask = XMALLOC(modulus_len);
   salt = XMALLOC(modulus_len);
   hash = XMALLOC(modulus_len);
   if (DB == NULL || mask == NULL || salt == NULL || hash == NULL) {
      if (DB != NULL) {
         XFREE(DB);
      }
      if (mask != NULL) {
         XFREE(mask);
      }
      if (salt != NULL) {
         XFREE(salt);
      }
      if (hash != NULL) {
         XFREE(hash);
      }
      return CRYPT_MEM;
   }

   /* ensure the 0xBC byte */
   if (sig[siglen-1] != 0xBC) {
      err = CRYPT_INVALID_PACKET;
      goto LBL_ERR;
   }

   /* copy out the DB */
   x = 0;
   XMEMCPY(DB, sig + x, modulus_len - hLen - 1);
   x += modulus_len - hLen - 1;

   /* copy out the hash */
   XMEMCPY(hash, sig + x, hLen);
   x += hLen;

   /* check the MSB */
   if ((sig[0] & ~(0xFF >> ((modulus_len<<3) - (modulus_bitlen-1)))) != 0) {
      err = CRYPT_INVALID_PACKET;
      goto LBL_ERR;
   }

   /* generate mask of length modulus_len - hLen - 1 from hash */
   if ((err = pkcs_1_mgf1(hash_idx, hash, hLen, mask, modulus_len - hLen - 1)) != CRYPT_OK) {
      goto LBL_ERR;
   }

   /* xor against DB */
   for (y = 0; y < (modulus_len - hLen - 1); y++) {
      DB[y] ^= mask[y];
   }
   
   /* now clear the first byte [make sure smaller than modulus] */
   DB[0] &= 0xFF >> ((modulus_len<<3) - (modulus_bitlen-1));

   /* DB = PS || 0x01 || salt, PS == modulus_len - saltlen - hLen - 2 zero bytes */

   /* check for zeroes and 0x01 */
   for (x = 0; x < modulus_len - saltlen - hLen - 2; x++) {
       if (DB[x] != 0x00) {
          err = CRYPT_INVALID_PACKET;
          goto LBL_ERR;
       }
   }

   /* check for the 0x01 */
   if (DB[x++] != 0x01) {
      err = CRYPT_INVALID_PACKET;
      goto LBL_ERR;
   }

   /* M = (eight) 0x00 || msghash || salt, mask = H(M) */
   if ((err = hash_descriptor[hash_idx].init(&md)) != CRYPT_OK) {
      goto LBL_ERR;
   }
   zeromem(mask, 8);
   if ((err = hash_descriptor[hash_idx].process(&md, mask, 8)) != CRYPT_OK) {
      goto LBL_ERR;
   }
   if ((err = hash_descriptor[hash_idx].process(&md, msghash, msghashlen)) != CRYPT_OK) {
      goto LBL_ERR;
   }
   if ((err = hash_descriptor[hash_idx].process(&md, DB+x, saltlen)) != CRYPT_OK) {
      goto LBL_ERR;
   }
   if ((err = hash_descriptor[hash_idx].done(&md, mask)) != CRYPT_OK) {
      goto LBL_ERR;
   }

   /* mask == hash means valid signature */
   if (XMEMCMP(mask, hash, hLen) == 0) {
      *res = 1;
   }

   err = CRYPT_OK;
LBL_ERR:
#ifdef LTC_CLEAN_STACK
   zeromem(DB,   modulus_len);   
   zeromem(mask, modulus_len);   
   zeromem(salt, modulus_len);   
   zeromem(hash, modulus_len);   
#endif

   XFREE(hash);
   XFREE(salt);
   XFREE(mask);
   XFREE(DB);

   return err;
}

/** 
   Create an RSA key
   @param prng     An active PRNG state
   @param wprng    The index of the PRNG desired
   @param size     The size of the modulus (key size) desired (octets)
   @param e        The "e" value (public key).  e==65537 is a good choice
   @param key      [out] Destination of a newly created private key pair
   @return CRYPT_OK if successful, upon error all allocated ram is freed
*/
int rsa_make_key(prng_state *prng, int wprng, int size, long e, rsa_key *key)
{
   void *p, *q, *tmp1, *tmp2, *tmp3;
   int    err;

   LTC_ARGCHK(ltc_mp.name != NULL);
   LTC_ARGCHK(key         != NULL);

   if ((size < (MIN_RSA_SIZE/8)) || (size > (MAX_RSA_SIZE/8))) {
      return CRYPT_INVALID_KEYSIZE;
   }

   if ((e < 3) || ((e & 1) == 0)) {
      return CRYPT_INVALID_ARG;
   }

   if ((err = prng_is_valid(wprng)) != CRYPT_OK) {
      return err;
   }

   if ((err = mp_init_multi(&p, &q, &tmp1, &tmp2, &tmp3, NULL)) != CRYPT_OK) {
      return err;
   }

   /* make primes p and q (optimization provided by Wayne Scott) */
   if ((err = mp_set_int(tmp3, e)) != CRYPT_OK)                      { goto errkey; }  /* tmp3 = e */

   /* make prime "p" */
   do {
       if ((err = rand_prime( p, size/2, prng, wprng)) != CRYPT_OK)  { goto errkey; }
       if ((err = mp_sub_d( p, 1,  tmp1)) != CRYPT_OK)               { goto errkey; }  /* tmp1 = p-1 */
       if ((err = mp_gcd( tmp1,  tmp3,  tmp2)) != CRYPT_OK)          { goto errkey; }  /* tmp2 = gcd(p-1, e) */
   } while (mp_cmp_d( tmp2, 1) != 0);                                                  /* while e divides p-1 */

   /* make prime "q" */
   do {
       if ((err = rand_prime( q, size/2, prng, wprng)) != CRYPT_OK)  { goto errkey; }
       if ((err = mp_sub_d( q, 1,  tmp1)) != CRYPT_OK)               { goto errkey; } /* tmp1 = q-1 */
       if ((err = mp_gcd( tmp1,  tmp3,  tmp2)) != CRYPT_OK)          { goto errkey; } /* tmp2 = gcd(q-1, e) */
   } while (mp_cmp_d( tmp2, 1) != 0);                                                 /* while e divides q-1 */

   /* tmp1 = lcm(p-1, q-1) */
   if ((err = mp_sub_d( p, 1,  tmp2)) != CRYPT_OK)                   { goto errkey; } /* tmp2 = p-1 */
                                                                                      /* tmp1 = q-1 (previous do/while loop) */
   if ((err = mp_lcm( tmp1,  tmp2,  tmp1)) != CRYPT_OK)              { goto errkey; } /* tmp1 = lcm(p-1, q-1) */

   /* make key */
   if ((err = mp_init_multi(&key->e, &key->d, &key->N, &key->dQ, &key->dP, &key->qP, &key->p, &key->q, NULL)) != CRYPT_OK) {
      goto errkey;
   }

   if ((err = mp_set_int( key->e, e)) != CRYPT_OK)                     { goto errkey; } /* key->e =  e */
   if ((err = mp_invmod( key->e,  tmp1,  key->d)) != CRYPT_OK)         { goto errkey; } /* key->d = 1/e mod lcm(p-1,q-1) */
   if ((err = mp_mul( p,  q,  key->N)) != CRYPT_OK)                    { goto errkey; } /* key->N = pq */

   /* optimize for CRT now */
   /* find d mod q-1 and d mod p-1 */
   if ((err = mp_sub_d( p, 1,  tmp1)) != CRYPT_OK)                     { goto errkey; } /* tmp1 = q-1 */
   if ((err = mp_sub_d( q, 1,  tmp2)) != CRYPT_OK)                     { goto errkey; } /* tmp2 = p-1 */
   if ((err = mp_mod( key->d,  tmp1,  key->dP)) != CRYPT_OK)           { goto errkey; } /* dP = d mod p-1 */
   if ((err = mp_mod( key->d,  tmp2,  key->dQ)) != CRYPT_OK)           { goto errkey; } /* dQ = d mod q-1 */
   if ((err = mp_invmod( q,  p,  key->qP)) != CRYPT_OK)                { goto errkey; } /* qP = 1/q mod p */

   if ((err = mp_copy( p,  key->p)) != CRYPT_OK)                       { goto errkey; }
   if ((err = mp_copy( q,  key->q)) != CRYPT_OK)                       { goto errkey; }

   /* set key type (in this case it's CRT optimized) */
   key->type = PK_PRIVATE;

   /* return ok and free temps */
   err       = CRYPT_OK;
   goto cleanup;
errkey:
   mp_clear_multi(key->d, key->e, key->N, key->dQ, key->dP, key->qP, key->p, key->q, NULL);
cleanup:
   mp_clear_multi(tmp3, tmp2, tmp1, p, q, NULL);
   return err;
}

/**
  Import an RSAPublicKey or RSAPrivateKey [two-prime only, only support >= 1024-bit keys, defined in PKCS #1 v2.1]
  @param in      The packet to import from
  @param inlen   It's length (octets)
  @param key     [out] Destination for newly imported key
  @return CRYPT_OK if successful, upon error allocated memory is freed
*/
int rsa_import(const unsigned char *in, unsigned long inlen, rsa_key *key)
{
   int           err;
   void         *zero;
   unsigned char *tmpbuf;
   unsigned long  t, x, y, z, tmpoid[16];
   ltc_asn1_list ssl_pubkey_hashoid[2];
   ltc_asn1_list ssl_pubkey[2];

   LTC_ARGCHK(in          != NULL);
   LTC_ARGCHK(key         != NULL);
   LTC_ARGCHK(ltc_mp.name != NULL);

   /* init key */
   if ((err = mp_init_multi(&key->e, &key->d, &key->N, &key->dQ, 
                            &key->dP, &key->qP, &key->p, &key->q, NULL)) != CRYPT_OK) {
      return err;
   }

   /* see if the OpenSSL DER format RSA public key will work */
   tmpbuf = XCALLOC(1, MAX_RSA_SIZE*8);
   if (tmpbuf == NULL) {
       err = CRYPT_MEM;
       goto LBL_ERR;
   }

   /* this includes the internal hash ID and optional params (NULL in this case) */
   LTC_SET_ASN1(ssl_pubkey_hashoid, 0, LTC_ASN1_OBJECT_IDENTIFIER, tmpoid,                sizeof(tmpoid)/sizeof(tmpoid[0]));   
   LTC_SET_ASN1(ssl_pubkey_hashoid, 1, LTC_ASN1_NULL,              NULL,                  0);

   /* the actual format of the SSL DER key is odd, it stores a RSAPublicKey in a **BIT** string ... so we have to extract it
      then proceed to convert bit to octet 
    */
   LTC_SET_ASN1(ssl_pubkey, 0,         LTC_ASN1_SEQUENCE,          &ssl_pubkey_hashoid,   2);
   LTC_SET_ASN1(ssl_pubkey, 1,         LTC_ASN1_BIT_STRING,        tmpbuf,                MAX_RSA_SIZE*8);

   if (der_decode_sequence(in, inlen,
                           ssl_pubkey, 2UL) == CRYPT_OK) {

      /* ok now we have to reassemble the BIT STRING to an OCTET STRING.  Thanks OpenSSL... */
      for (t = y = z = x = 0; x < ssl_pubkey[1].size; x++) {
          y = (y << 1) | tmpbuf[x];
          if (++z == 8) {
             tmpbuf[t++] = (unsigned char)y;
             y           = 0;
             z           = 0;
          }
      }

      /* now it should be SEQUENCE { INTEGER, INTEGER } */
      if ((err = der_decode_sequence_multi(tmpbuf, t,
                                           LTC_ASN1_INTEGER, 1UL, key->N, 
                                           LTC_ASN1_INTEGER, 1UL, key->e, 
                                           LTC_ASN1_EOL,     0UL, NULL)) != CRYPT_OK) {
         XFREE(tmpbuf);
         goto LBL_ERR;
      }
      XFREE(tmpbuf);
      key->type = PK_PUBLIC;
      return CRYPT_OK;
   }
   XFREE(tmpbuf);

   /* not SSL public key, try to match against PKCS #1 standards */
   if ((err = der_decode_sequence_multi(in, inlen, 
                                  LTC_ASN1_INTEGER, 1UL, key->N, 
                                  LTC_ASN1_EOL,     0UL, NULL)) != CRYPT_OK) {
      goto LBL_ERR;
   }

   if (mp_cmp_d(key->N, 0) == LTC_MP_EQ) {
      if ((err = mp_init(&zero)) != CRYPT_OK) { 
         goto LBL_ERR;
      }
      /* it's a private key */
      if ((err = der_decode_sequence_multi(in, inlen, 
                          LTC_ASN1_INTEGER, 1UL, zero, 
                          LTC_ASN1_INTEGER, 1UL, key->N, 
                          LTC_ASN1_INTEGER, 1UL, key->e,
                          LTC_ASN1_INTEGER, 1UL, key->d, 
                          LTC_ASN1_INTEGER, 1UL, key->p, 
                          LTC_ASN1_INTEGER, 1UL, key->q, 
                          LTC_ASN1_INTEGER, 1UL, key->dP,
                          LTC_ASN1_INTEGER, 1UL, key->dQ, 
                          LTC_ASN1_INTEGER, 1UL, key->qP, 
                          LTC_ASN1_EOL,     0UL, NULL)) != CRYPT_OK) {
         mp_clear(zero);
         goto LBL_ERR;
      }
      mp_clear(zero);
      key->type = PK_PRIVATE;
   } else if (mp_cmp_d(key->N, 1) == LTC_MP_EQ) {
      /* we don't support multi-prime RSA */
      err = CRYPT_PK_INVALID_TYPE;
      goto LBL_ERR;
   } else {
      /* it's a public key and we lack e */
      if ((err = der_decode_sequence_multi(in, inlen, 
                                     LTC_ASN1_INTEGER, 1UL, key->N, 
                                     LTC_ASN1_INTEGER, 1UL, key->e, 
                                     LTC_ASN1_EOL,     0UL, NULL)) != CRYPT_OK) {
         goto LBL_ERR;
      }
      key->type = PK_PUBLIC;
   }
   return CRYPT_OK;
LBL_ERR:
   mp_clear_multi(key->d,  key->e, key->N, key->dQ, key->dP, key->qP, key->p, key->q, NULL);
   return err;
}

/**
    This will export either an RSAPublicKey or RSAPrivateKey [defined in PKCS #1 v2.1] 
    @param out       [out] Destination of the packet
    @param outlen    [in/out] The max size and resulting size of the packet
    @param type      The type of exported key (PK_PRIVATE or PK_PUBLIC)
    @param key       The RSA key to export
    @return CRYPT_OK if successful
*/    
int rsa_export(unsigned char *out, unsigned long *outlen, int type, rsa_key *key)
{
   unsigned long zero=0;
   LTC_ARGCHK(out    != NULL);
   LTC_ARGCHK(outlen != NULL);
   LTC_ARGCHK(key    != NULL);

   /* type valid? */
   if (!(key->type == PK_PRIVATE) && (type == PK_PRIVATE)) {
      return CRYPT_PK_INVALID_TYPE;
   }

   if (type == PK_PRIVATE) {
      /* private key */
      /* output is 
            Version, n, e, d, p, q, d mod (p-1), d mod (q - 1), 1/q mod p
       */
      return der_encode_sequence_multi(out, outlen, 
                          LTC_ASN1_SHORT_INTEGER, 1UL, &zero, 
                          LTC_ASN1_INTEGER, 1UL,  key->N, 
                          LTC_ASN1_INTEGER, 1UL,  key->e,
                          LTC_ASN1_INTEGER, 1UL,  key->d, 
                          LTC_ASN1_INTEGER, 1UL,  key->p, 
                          LTC_ASN1_INTEGER, 1UL,  key->q, 
                          LTC_ASN1_INTEGER, 1UL,  key->dP,
                          LTC_ASN1_INTEGER, 1UL,  key->dQ, 
                          LTC_ASN1_INTEGER, 1UL,  key->qP, 
                          LTC_ASN1_EOL,     0UL, NULL);
   } else {
      /* public key */
      return der_encode_sequence_multi(out, outlen, 
                                 LTC_ASN1_INTEGER, 1UL,  key->N, 
                                 LTC_ASN1_INTEGER, 1UL,  key->e, 
                                 LTC_ASN1_EOL,     0UL, NULL);
   }
}

/** 
   Compute an RSA modular exponentiation 
   @param in         The input data to send into RSA
   @param inlen      The length of the input (octets)
   @param out        [out] The destination 
   @param outlen     [in/out] The max size and resulting size of the output
   @param which      Which exponent to use, e.g. PK_PRIVATE or PK_PUBLIC
   @param key        The RSA key to use 
   @return CRYPT_OK if successful
*/   
static int rsa_exptmod(const unsigned char *in,   unsigned long inlen,
                      unsigned char *out,  unsigned long *outlen, int which,
                      rsa_key *key)
{
   void         *tmp, *tmpa, *tmpb;
   unsigned long x;
   int           err;

   LTC_ARGCHK(in     != NULL);
   LTC_ARGCHK(out    != NULL);
   LTC_ARGCHK(outlen != NULL);
   LTC_ARGCHK(key    != NULL);
  
   /* is the key of the right type for the operation? */
   if (which == PK_PRIVATE && (key->type != PK_PRIVATE)) {
      return CRYPT_PK_NOT_PRIVATE;
   }

   /* must be a private or public operation */
   if (which != PK_PRIVATE && which != PK_PUBLIC) {
      return CRYPT_PK_INVALID_TYPE;
   }

   /* init and copy into tmp */
   if ((err = mp_init_multi(&tmp, &tmpa, &tmpb, NULL)) != CRYPT_OK)                                    { return err; }
   if ((err = mp_read_unsigned_bin(tmp, (unsigned char *)in, (int)inlen)) != CRYPT_OK)                 { goto error; }

   /* sanity check on the input */
   if (mp_cmp(key->N, tmp) == LTC_MP_LT) {
      err = CRYPT_PK_INVALID_SIZE;
      goto error;
   }

   /* are we using the private exponent and is the key optimized? */
   if (which == PK_PRIVATE) {
      /* tmpa = tmp^dP mod p */
      if ((err = mp_exptmod(tmp, key->dP, key->p, tmpa)) != CRYPT_OK)                               { goto error; }

      /* tmpb = tmp^dQ mod q */
      if ((err = mp_exptmod(tmp, key->dQ, key->q, tmpb)) != CRYPT_OK)                               { goto error; }

      /* tmp = (tmpa - tmpb) * qInv (mod p) */
      if ((err = mp_sub(tmpa, tmpb, tmp)) != CRYPT_OK)                                              { goto error; }
      if ((err = mp_mulmod(tmp, key->qP, key->p, tmp)) != CRYPT_OK)                                { goto error; }

      /* tmp = tmpb + q * tmp */
      if ((err = mp_mul(tmp, key->q, tmp)) != CRYPT_OK)                                             { goto error; }
      if ((err = mp_add(tmp, tmpb, tmp)) != CRYPT_OK)                                               { goto error; }
   } else {
      /* exptmod it */
      if ((err = mp_exptmod(tmp, key->e, key->N, tmp)) != CRYPT_OK)                                { goto error; }
   }

   /* read it back */
   x = (unsigned long)mp_unsigned_bin_size(key->N);
   if (x > *outlen) {
      *outlen = x;
      err = CRYPT_BUFFER_OVERFLOW;
      goto error;
   }

   /* this should never happen ... */
   if (mp_unsigned_bin_size(tmp) > mp_unsigned_bin_size(key->N)) {
      err = CRYPT_ERROR;
      goto error;
   }
   *outlen = x;

   /* convert it */
   zeromem(out, x);
   if ((err = mp_to_unsigned_bin(tmp, out+(x-mp_unsigned_bin_size(tmp)))) != CRYPT_OK)               { goto error; }

   /* clean up and return */
   err = CRYPT_OK;
error:
   mp_clear_multi(tmp, tmpa, tmpb, NULL);
   return err;
}

/**
  PKCS #1 pad then sign
  @param in        The hash to sign
  @param inlen     The length of the hash to sign (octets)
  @param out       [out] The signature
  @param outlen    [in/out] The max size and resulting size of the signature
  @param prng      An active PRNG state
  @param prng_idx  The index of the PRNG desired
  @param hash_idx  The index of the hash desired
  @param saltlen   The length of the salt desired (octets)
  @param key       The private RSA key to use
  @return CRYPT_OK if successful
*/
int rsa_sign_hash_ex(const unsigned char *in,       unsigned long  inlen,
                           unsigned char *out,      unsigned long *outlen,
                           prng_state    *prng,     int            prng_idx,
                           int            hash_idx, unsigned long  saltlen,
                           rsa_key *key)
{
   unsigned long modulus_bitlen, modulus_bytelen, x;
   int           err;

   LTC_ARGCHK(in       != NULL);
   LTC_ARGCHK(out      != NULL);
   LTC_ARGCHK(outlen   != NULL);
   LTC_ARGCHK(key      != NULL);

   /* valid prng and hash ? */
   if ((err = prng_is_valid(prng_idx)) != CRYPT_OK) {
      return err;
   }
   if ((err = hash_is_valid(hash_idx)) != CRYPT_OK) {
      return err;
   }

   /* get modulus len in bits */
   modulus_bitlen = mp_count_bits((key->N));

  /* outlen must be at least the size of the modulus */
  modulus_bytelen = mp_unsigned_bin_size((key->N));
  if (modulus_bytelen > *outlen) {
     *outlen = modulus_bytelen;
     return CRYPT_BUFFER_OVERFLOW;
  }

  /* PSS pad the key */
  x = *outlen;
  if ((err = pkcs_1_pss_encode(in, inlen, saltlen, prng, prng_idx,
                               hash_idx, modulus_bitlen, out, &x)) != CRYPT_OK) {
     return err;
  }

  /* RSA encode it */
  return rsa_exptmod(out, x, out, outlen, PK_PRIVATE, key);
}

/**
  PKCS #1 de-sign then PSS depad
  @param sig              The signature data
  @param siglen           The length of the signature data (octets)
  @param hash             The hash of the message that was signed
  @param hashlen          The length of the hash of the message that was signed (octets)
  @param hash_idx         The index of the desired hash
  @param saltlen          The length of the salt used during signature
  @param stat             [out] The result of the signature comparison, 1==valid, 0==invalid
  @param key              The public RSA key corresponding to the key that performed the signature
  @return CRYPT_OK on success (even if the signature is invalid)
*/
int rsa_verify_hash_ex(const unsigned char *sig,      unsigned long siglen,
                       const unsigned char *hash,     unsigned long hashlen,
                             int            hash_idx, unsigned long saltlen,
                             int           *stat,     rsa_key      *key)
{
  unsigned long modulus_bitlen, modulus_bytelen, x;
  int           err;
  unsigned char *tmpbuf;

  LTC_ARGCHK(hash  != NULL);
  LTC_ARGCHK(sig   != NULL);
  LTC_ARGCHK(stat  != NULL);
  LTC_ARGCHK(key   != NULL);

  /* default to invalid */
  *stat = 0;

  /* valid hash ? */
  if ((err = hash_is_valid(hash_idx)) != CRYPT_OK) {
     return err;
  }

  /* get modulus len in bits */
  modulus_bitlen = mp_count_bits( (key->N));

  /* outlen must be at least the size of the modulus */
  modulus_bytelen = mp_unsigned_bin_size( (key->N));
  if (modulus_bytelen != siglen) {
     return CRYPT_INVALID_PACKET;
  }

  /* allocate temp buffer for decoded sig */
  tmpbuf = XMALLOC(siglen);
  if (tmpbuf == NULL) {
     return CRYPT_MEM;
  }

  /* RSA decode it  */
  x = siglen;
  if ((err = rsa_exptmod(sig, siglen, tmpbuf, &x, PK_PUBLIC, key)) != CRYPT_OK) {
     XFREE(tmpbuf);
     return err;
  }

  /* make sure the output is the right size */
  if (x != siglen) {
     XFREE(tmpbuf);
     return CRYPT_INVALID_PACKET;
  }

  /* PSS decode and verify it */
  err = pkcs_1_pss_decode(hash, hashlen, tmpbuf, x, saltlen, hash_idx, modulus_bitlen, stat);

#ifdef LTC_CLEAN_STACK
  zeromem(tmpbuf, siglen);
#endif
  XFREE(tmpbuf);
  return err;
}

/**
  Free an RSA key from memory
  @param key   The RSA key to free
*/
void rsa_free(rsa_key *key)
{
   LTC_ARGCHKVD(key != NULL);
   mp_clear_multi(key->e, key->d, key->N, key->dQ, key->dP, key->qP, key->p, key->q, NULL);
}