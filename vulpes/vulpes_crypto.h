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

#ifndef VULPES_CRYPTO
#define VULPES_CRYPTO

#define HASH_LEN 20
#define HASH_LEN_HEX (2 * HASH_LEN + 1)
#define CIPHER_BLOCK_SIZE 8
#define CIPHER_IV_SIZE 8

void digest(const void *mesg, unsigned mesgLen, void *result);
vulpes_err_t vulpes_encrypt(const void *inString, int inStringLength,
			    void **outString, int *outStringLength,
			    const void *key, int keyLen, int doPad);
vulpes_err_t vulpes_decrypt(const void *inString, int inStringLength,
			    void **outString, int *outStringLength,
			    const void *key, int keyLen, int doPad);

#endif
