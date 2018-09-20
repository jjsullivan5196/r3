/***********************************************************************
**
**  REBOL [R3] Language Interpreter and Run-time Environment
**
**  Copyright 2012 REBOL Technologies
**  Copyright 2012-2017 Rebol Open Source Contributors
**  REBOL is a trademark of REBOL Technologies
**
**  Licensed under the Apache License, Version 2.0 (the "License");
**  you may not use this file except in compliance with the License.
**  You may obtain a copy of the License at
**
**  http://www.apache.org/licenses/LICENSE-2.0
**
**  Unless required by applicable law or agreed to in writing, software
**  distributed under the License is distributed on an "AS IS" BASIS,
**  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
**  See the License for the specific language governing permissions and
**  limitations under the License.
**
************************************************************************
**
**  Module:  n-crypt.c
**  Summary: native functions for data sets
**  Section: natives
**  Author:  Oldes, Cyphre
**  Notes:
**
***********************************************************************/

#include "sys-core.h"
#include "sys-rc4.h"
#include "sys-aes.h"
#include "sys-rsa.h"
#include "sys-dh.h"

const REBYTE rc4_name[] = "RC4-context"; //Used as a context handle name
const REBYTE aes_name[] = "AES-context";
const REBYTE rsa_name[] = "RSA-context";
const REBYTE  dh_name[] = "DH-Key";

/***********************************************************************
**
*/	REBNATIVE(rc4)
/*
//	rc4: native [
//		"Encrypt/decrypt data (modifies) using RC4 algorithm."
//
//		/key "Provided only for the first time to get stream HANDLE!"
//			crypt-key [binary!]  "Crypt key."
//		/stream
//			ctx [handle!]        "Stream cipher context."
//			data [binary! none!] "Data to encrypt/decrypt."
//	]
***********************************************************************/
{
    REBOOL  ref_key       = D_REF(1);
    REBVAL *val_crypt_key = D_ARG(2); 
    REBOOL  ref_stream    = D_REF(3);
    REBVAL *val_ctx       = D_ARG(4);
    REBVAL *val_data      = D_ARG(5);

    REBVAL *ret = D_RET;
	REBSER *ctx;

    if(ref_stream) {
    	ctx = (REBSER*)VAL_HANDLE(val_ctx);

    	if (VAL_HANDLE_NAME(val_ctx) != rc4_name) {
    		Trap0(RE_INVALID_HANDLE);
    	}

    	REBYTE *data = VAL_BIN_AT(val_data);
    	RC4_crypt((RC4_CTX*)ctx->data, data, data, VAL_LEN(val_data));
    	DS_RET_VALUE(val_data);

    } else if (ref_key) {
    	//key defined - setup new context
		//making series from POOL so it will be GCed automaticaly
		REBSER* ctx = Make_Series(sizeof(RC4_CTX), (REBCNT)1, FALSE);

		RC4_setup(
			(RC4_CTX*)ctx->data,
            VAL_BIN_AT(val_crypt_key),
            VAL_LEN(val_crypt_key)
        );

        SET_HANDLE(ret, ctx);
		VAL_HANDLE_NAME(ret) = rc4_name;
    }
    return R_RET;
}

/***********************************************************************
**
*/	REBNATIVE(aes)
/*
//  aes: native [
//		"Encrypt/decrypt data using AES algorithm. Returns stream cipher context handle or encrypted/decrypted data."
//		/key                "Provided only for the first time to get stream HANDLE!"
//			crypt-key [binary!] "Crypt key (16 or 32 bytes)."
//			iv  [none! binary!] "Optional initialization vector (16 bytes)."
//		/decrypt            "Use the crypt-key for decryption (default is to encrypt)"
//		/stream
//			ctx [handle!]   "Stream cipher context."
//			data [binary! none!]  "Data to encrypt/decrypt. Or NONE to close the cipher stream."
//  ]
***********************************************************************/
{
	REBOOL  ref_key       = D_REF(1);
    REBVAL *val_crypt_key = D_ARG(2);
    REBVAL *val_iv        = D_ARG(3);
	REBOOL  ref_decrypt   = D_REF(4);
    REBOOL  ref_stream    = D_REF(5);
    REBVAL *val_ctx       = D_ARG(6);
    REBVAL *val_data      = D_ARG(7);
    
    REBVAL *ret = D_RET;
	REBSER *ctx;
	REBINT  len, pad_len;

	if (ref_key) {
    	//key defined - setup new context

    	uint8_t iv[AES_IV_SIZE];

		if (IS_BINARY(val_iv)) {
			if (VAL_LEN(val_iv) < AES_IV_SIZE) {
				return R_NONE;
			}
			memcpy(iv, VAL_BIN_AT(val_iv), AES_IV_SIZE);
		} else {
			//TODO: Use ECB encryption if IV is not specified
			memset(iv, 0, AES_IV_SIZE);
		}
		
		len = VAL_LEN(val_crypt_key) << 3;

		if (len != 128 && len != 256) {
			return R_NONE;
		}

		//making series from POOL so it will be GCed automaticaly
		ctx = Make_Series(sizeof(AES_CTX), (REBCNT)1, FALSE);

		AES_set_key(
			(AES_CTX*)ctx->data,
			VAL_BIN_AT(val_crypt_key),
			(const uint8_t *)iv,
			(len == 128) ? AES_MODE_128 : AES_MODE_256
		);

		if (ref_decrypt) AES_convert_key((AES_CTX*)ctx->data);

		SET_HANDLE(ret, ctx);
		VAL_HANDLE_NAME(ret) = aes_name;
    
    } else if(ref_stream) {

    	if (VAL_HANDLE_NAME(val_ctx) != aes_name) {
    		Trap0(RE_INVALID_HANDLE);
    	}

    	ctx = (REBSER*)VAL_HANDLE(val_ctx);
		if(ctx == NULL) Trap0(RE_INVALID_HANDLE);

		if(IS_NONE(val_data)) {
			//puts("releasing AES ctx");
			Free_Series(ctx);
			SET_HANDLE(val_ctx, NULL);
			return R_TRUE;
		}

    	len = VAL_LEN(val_data);
    	if (len == 0) return R_NONE;

		pad_len = (((len - 1) >> 4) << 4) + AES_BLOCKSIZE;

		REBYTE *data = VAL_BIN_AT(val_data);
		REBYTE *pad_data;

		if (len < pad_len) {
			//  make new data input with zero-padding
			//TODO: instead of making new data, the original could be extended with padding.
			pad_data = (REBYTE*)MAKE_MEM(pad_len);
			memset(pad_data, 0, pad_len);
			memcpy(pad_data, data, len);
			data = pad_data;
		}
		else {
			pad_data = NULL;
		}

		REBSER  *binaryOut = Make_Binary(pad_len);
		AES_CTX *aes_ctx = (AES_CTX *)ctx->data;
		if (aes_ctx->key_mode == AES_MODE_DECRYPT) {
			AES_cbc_decrypt(
				aes_ctx,
				(const uint8_t*)data,
				BIN_HEAD(binaryOut),
				pad_len
			);
		}
		else {
			AES_cbc_encrypt(
				aes_ctx,
				(const uint8_t*)data,
				BIN_HEAD(binaryOut),
				pad_len
			);
		}
		if (pad_data) FREE_MEM(pad_data);

		SET_BINARY(ret, binaryOut);
		VAL_TAIL(ret) = pad_len;

    }
	return R_RET;
}

/***********************************************************************
**
*/	REBNATIVE(rsa_init)
/*
//  rsa-init: native [
//		"Creates a context which is than used to encrypt or decrypt data using RSA"
//		n  [binary!]  "Modulus"
//		e  [binary!]  "Public exponent"
//		/private "Init also private values"
//			d [binary!] "Private exponent"
//			p [binary!] "Prime number 1"
//			q [binary!] "Prime number 2"
//			dP [binary!]
//			dQ [binary!]
//			qInv [binary!]
//  ]
***********************************************************************/
{
	REBSER *n       = VAL_SERIES(D_ARG(1));
	REBSER *e       = VAL_SERIES(D_ARG(2));
	REBOOL  ref_private =        D_REF(3);
	REBSER *d       = VAL_SERIES(D_ARG(4));
	REBSER *p       = VAL_SERIES(D_ARG(5));
	REBSER *q       = VAL_SERIES(D_ARG(6));
	REBSER *dP      = VAL_SERIES(D_ARG(7));
	REBSER *dQ      = VAL_SERIES(D_ARG(8));
	REBSER *qInv    = VAL_SERIES(D_ARG(9));

	RSA_CTX *rsa_ctx = NULL;

	REBVAL *ret = D_RET;

	if(ref_private) {
		RSA_priv_key_new(
			&rsa_ctx,
			BIN_DATA(n), BIN_LEN(n),
			BIN_DATA(e), BIN_LEN(e),
			BIN_DATA(d), BIN_LEN(d),
			BIN_DATA(p), BIN_LEN(p),
			BIN_DATA(q), BIN_LEN(q),
			BIN_DATA(dP), BIN_LEN(dP),
			BIN_DATA(dQ), BIN_LEN(dQ),
			BIN_DATA(qInv), BIN_LEN(qInv)
		);
	} else {
		RSA_pub_key_new(
			&rsa_ctx,
			BIN_DATA(n), BIN_LEN(n),
			BIN_DATA(e), BIN_LEN(e)
		);
	}
	//printf("rsa ctx %u %i\n", (uint32_t)rsa_ctx, rsa_ctx->num_octets);
	SET_HANDLE(ret, rsa_ctx);
	VAL_HANDLE_NAME(ret) = rsa_name;
	return R_RET;
}

/***********************************************************************
**
*/	REBNATIVE(rsa)
/*
//  rsa: native [
//		"Encrypt/decrypt/sign/verify data using RSA cryptosystem. Only one refinement must be used!"
//		rsa-key [handle!] "RSA context created using `rsa-init` function"
//		data    [binary!] "Data to work with"
//		/encrypt  "Use public key to encrypt data"
//		/decrypt  "Use private key to decrypt data"
//		/sign     "Use private key to sign data"
//		/verify   "Use public key to verify signed data"
//  ]
***********************************************************************/
{
	REBVAL *key             = D_ARG(1);
	REBSER *data = VAL_SERIES(D_ARG(2));
	REBOOL  refEncrypt      = D_REF(3);
	REBOOL  refDecrypt      = D_REF(4);
	REBOOL  refSign         = D_REF(5);
	REBOOL  refverify       = D_REF(6);

	// make sure that only one refinement is used!
	if(
		(refEncrypt && (refDecrypt || refSign    || refverify)) ||
		(refDecrypt && (refEncrypt || refSign    || refverify)) ||
		(refSign    && (refDecrypt || refEncrypt || refverify)) ||
		(refverify  && (refDecrypt || refSign    || refEncrypt))
	) {
		Trap0(RE_BAD_REFINES);
	}

	REBVAL *ret = D_RET;

	REBINT outBytes;

	bigint *data_bi;
	RSA_CTX *rsa_ctx;


	if(VAL_HANDLE_NAME(key) != rsa_name || VAL_HANDLE(key) == NULL) {
		Trap0(RE_INVALID_HANDLE);
	}

	rsa_ctx = (RSA_CTX*)VAL_HANDLE(key);

	if(
		(rsa_ctx->m == NULL || rsa_ctx->e == NULL) ||
		((refDecrypt || refSign) && (
			rsa_ctx->d  == NULL ||
			rsa_ctx->p  == NULL ||
			rsa_ctx->q  == NULL ||
			rsa_ctx->dP == NULL ||
			rsa_ctx->dQ == NULL ||
			rsa_ctx->qInv == NULL
		))
	) {
		//puts("[RSA] Missing RSA key data!");
		return R_NONE;
	}

	REBYTE* inBinary = BIN_DATA(data);
	REBINT  inBytes = BIN_LEN(data);

	//printf("inbytes: %i\n", inBytes);

	data_bi = bi_import(rsa_ctx->bi_ctx, inBinary, inBytes);

	//allocate new binary!
	REBSER* output = Make_Binary(rsa_ctx->num_octets);
	REBYTE* outBinary = BIN_DATA(output);

	if(refDecrypt || refverify) {
		outBytes = RSA_decrypt(rsa_ctx, inBinary, outBinary, refDecrypt, FALSE);
	} else {
		outBytes = RSA_encrypt(rsa_ctx, inBinary, inBytes, outBinary, refSign, TRUE);
	}

	bi_free(rsa_ctx->bi_ctx, data_bi);

	//printf("result bts %i\n", outBytes);

	if(outBytes < 0) {
		Free_Series(output);
		return R_NONE;
	}

	SET_BINARY(ret, output);
	VAL_TAIL(ret) = outBytes;

	return R_RET;

}


/***********************************************************************
**
*/	REBNATIVE(dh_init)
/*
//  dh-init: native [
//		"Generates a new Diffie-Hellman private/public key pair"
//		g [binary!] "Generator"
//		p [binary!] "Field prime"
//		/into
//			dh-key [handle!] "Existing DH key handle"
//  ]
***********************************************************************/
{
	REBSER *g = VAL_SERIES(D_ARG(1));
	REBSER *p = VAL_SERIES(D_ARG(2));
	REBOOL  ref_into =     D_REF(3);
	REBVAL *val_dh   =     D_ARG(4);
	
	DH_CTX *dh;
	REBYTE *bin;
	REBVAL *ret;

	REBCNT  len_g = BIN_LEN(g);
	REBCNT  len_p = BIN_LEN(p);
	REBYTE *buffer = NULL;

	// allocating buffers for all keys as a one blob
	REBCNT  buffer_len = BIN_LEN(g) + (5 * BIN_LEN(p));
	
	if(ref_into) {
		if(!IS_HANDLE(val_dh) || VAL_HANDLE_NAME(val_dh) != dh_name) {
			//error!
			return R_NONE;
		}
		ret = val_dh;
		*D_RET = *D_ARG(4);
		dh = (DH_CTX*)VAL_HANDLE(val_dh);
		if(dh == NULL) goto new_dh_handle;
		if((REBCNT)dh->len_data < buffer_len) {
			//needs new allocation for keys
			if(dh->data != NULL) FREE_MEM(dh->data);
		} else {
			//skip the buffer allocation
			buffer = dh->data;
		}
	} else {
		ret = D_RET;
new_dh_handle:
		dh = (DH_CTX*)MAKE_NEW(DH_CTX);
		SET_HANDLE(ret, dh);
		VAL_HANDLE_NAME(ret) = dh_name;
	}
	
	if(buffer == NULL) {
		buffer = MAKE_MEM(buffer_len);
		dh->len_data = buffer_len;
	}

	CLEAR(buffer, dh->len_data);

	bin = BIN_DATA(g); //@@ use VAL_BIN_AT instead?
	dh->len_g = len_g;
	dh->g = buffer;
	COPY_MEM(dh->g, bin, len_g);
	
	buffer += len_g;

	bin = BIN_DATA(p);
	dh->len = len_p;
	dh->p = buffer;
	COPY_MEM(dh->p, bin, len_p);
	
	buffer += len_p;
	
	dh->x  = buffer; //private key
	buffer += len_p;
	dh->gx = buffer; //public key (self)
	buffer += len_p;
	dh->gy = buffer; //public key (peer)
	buffer += len_p;
	dh->k  = buffer; //negotiated key
	
	DH_generate_key(dh);
	
	return R_RET;
}

/***********************************************************************
**
*/	REBNATIVE(dh)
/*
//  dh: native [
//		"Diffie-Hellman key exchange"
//		dh-key [handle!] "DH key created using `dh-init` function"
//		/release "Releases internal DH key resources"
//		/public  "Returns public key as a binary"
//		/secret  "Computes secret result using peer's public key"
//			public-key [binary!] "Peer's public key"
//  ]
***********************************************************************/
{
	REBVAL *val_dh      = D_ARG(1);
	REBOOL  ref_release = D_REF(2);
	REBOOL  ref_public  = D_REF(3);
	REBOOL  ref_secret  = D_REF(4);
	REBVAL *pub_key     = D_ARG(5);

	REBVAL *ret = D_RET;
	REBSER *bin;
	REBCNT len;

	if (ref_public && ref_secret) {
		// only one can be used
		Trap0(RE_BAD_REFINES);
	}

	if (VAL_HANDLE_NAME(val_dh) != dh_name || VAL_HANDLE(val_dh) == NULL) {
		Trap0(RE_INVALID_HANDLE);
	}

	DH_CTX *dh = (DH_CTX*)VAL_HANDLE(val_dh);
	
	if (dh->g == NULL) return R_NONE; //or error?

	if(ref_public) {
		bin = Make_Binary(dh->len);
		COPY_MEM(BIN_DATA(bin), dh->gx, dh->len);
		SET_BINARY(ret, bin);
		BIN_LEN(bin) = dh->len;
	}

	if(ref_secret) {
		bin = VAL_SERIES(pub_key); //@@ use VAL_BIN_AT instead?
		len = BIN_LEN(bin);
		if(len != dh->len) {
			return R_NONE; // throw an error?
		}
		COPY_MEM(dh->gy, BIN_DATA(bin), len);

		DH_compute_key(dh);

		bin = Make_Binary(len);
		COPY_MEM(BIN_DATA(bin), dh->k, len);
		SET_BINARY(ret, bin);
		BIN_LEN(bin) = len;
	}

	if(ref_release) {
		if(dh->g != NULL) FREE_MEM(dh->data);
		CLEARS(dh);
		if(!ref_public && !ref_secret) return R_ARG1;
	}
	
	return R_RET;

}
