/*-------------------------------------------------------------------------
 *
 * kmgr.c
 *	 Key manager routines
 *
 * Copyright (c) 2020, PostgreSQL Global Development Group
 *
 * Key manager is enabled if user requests during initdb.  We have one key
 * encryption key (KEK) and one internal key: SQL key.  During bootstrap,
 * we generate internal keys (currently only one), wrap them by using
 * AEAD algorithm with KEK which is derived from the user-provided passphrase
 * and store them into each file located at KMGR_DIR.  Once generated, these
 * are not changed.  During startup, we decrypt all internal keys and load
 * them to the shared memory space.  Internal keys on the shared memory are
 * read-only.
 *
 * IDENTIFICATION
 *	  src/backend/crypto/kmgr.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <sys/stat.h>
#include <unistd.h>

#include "funcapi.h"
#include "miscadmin.h"
#include "pgstat.h"

#include "common/sha2.h"
#include "common/kmgr_utils.h"
#include "crypto/kmgr.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/shmem.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"

/* Struct stores internal keys in plaintext format */
typedef struct KmgrShmemData
{
	/*
	 * Internal cryptographic keys. Keys are stored at its ID'th.
	 */
	CryptoKey	intlKeys[KMGR_MAX_INTERNAL_KEYS];
} KmgrShmemData;
static KmgrShmemData *KmgrShmem;

/* Key lengths of internal keys */
static int internalKeyLengths[KMGR_MAX_INTERNAL_KEYS] =
{
	PG_AEAD_KEY_LEN	/* KMGR_SQL_KEY_ID */
};

/* GUC variables */
bool		key_management_enabled = false;;
char	   *cluster_passphrase_command = NULL;

static void KmgrSaveCryptoKeys(const char *dir, CryptoKey *keys);
static CryptoKey *generate_crypto_key(int len);
static void recoverIncompleteRotation(void);

/*
 * This function must be called ONCE on system install.
 */
void
BootStrapKmgr(void)
{
	PgAeadCtx	*ctx;
	CryptoKey	keys_wrap[KMGR_MAX_INTERNAL_KEYS] = {0};
	char		passphrase[KMGR_MAX_PASSPHRASE_LEN];
	uint8		kekenc[PG_AEAD_ENC_KEY_LEN];
	uint8		kekhmac[PG_AEAD_MAC_KEY_LEN];
	int			passlen;

	/*
	 * Requirement check. We need openssl library to enable key management
	 * because all encryption and decryption calls happen via openssl function
	 * calls.
	 */
#ifndef USE_OPENSSL
	ereport(ERROR,
			(errcode(ERRCODE_CONFIG_FILE_ERROR),
			 (errmsg("cluster encryption is not supported because OpenSSL is not supported by this build"),
			  errhint("Compile with --with-openssl to use cluster encryption."))));
#endif

	/* Get key encryption key from the passphrase command */
	passlen = kmgr_run_cluster_passphrase_command(cluster_passphrase_command,
												  passphrase, KMGR_MAX_PASSPHRASE_LEN);
	if (passlen < KMGR_MIN_PASSPHRASE_LEN)
		ereport(ERROR,
				(errmsg("passphrase must be more than %d bytes",
						KMGR_MIN_PASSPHRASE_LEN)));

	/* Get key encryption key and HMAC key from passphrase */
	kmgr_derive_keys(passphrase, passlen, kekenc, kekhmac);

	/* Create temporarily AEAD context */
	ctx = pg_create_aead_ctx(kekenc, kekhmac);
	if (!ctx)
		elog(ERROR, "could not initialize encryption context");

	/* Wrap all internal keys by key encryption key */
	for (int id = 0; id < KMGR_MAX_INTERNAL_KEYS; id++)
	{
		CryptoKey *key;

		/* generate an internal key */
		key = generate_crypto_key(internalKeyLengths[id]);

		if (!kmgr_wrap_key(ctx, key, &(keys_wrap[id])))
		{
			pg_free_aead_ctx(ctx);
			elog(ERROR, "failed to wrap cluster encryption key");
		}
	}

	/* Save internal keys to the disk */
	KmgrSaveCryptoKeys(KMGR_DIR, keys_wrap);

	pg_free_aead_ctx(ctx);
}

/* Report shared-memory space needed by KmgrShmem */
Size
KmgrShmemSize(void)
{
	if (!key_management_enabled)
		return 0;

	return MAXALIGN(sizeof(KmgrShmemData));
}

/* Allocate and initialize key manager memory */
void
KmgrShmemInit(void)
{
	bool	found;

	if (!key_management_enabled)
		return;

	KmgrShmem = (KmgrShmemData *) ShmemInitStruct("Key manager",
												  KmgrShmemSize(), &found);

	if (!found)
		memset(KmgrShmem, 0, KmgrShmemSize());
}

/*
 * Get encryption key passphrase and verify it, then get the internal keys.
 * This function is called by postmaster at startup time.
 */
void
InitializeKmgr(void)
{
	CryptoKey	*keys_wrap;
	char		passphrase[KMGR_MAX_PASSPHRASE_LEN];
	int			passlen;
	int			nkeys;

	if (!key_management_enabled)
		return;

	elog(DEBUG1, "starting up key management system");

	/* Recover the failure of the last passphrase rotation if necessary */
	recoverIncompleteRotation();

	/* Get the crypto keys from the file */
	keys_wrap = kmgr_get_cryptokeys(KMGR_DIR, &nkeys);
	Assert(nkeys == KMGR_MAX_INTERNAL_KEYS);

	/* Get cluster passphrase */
	passlen = kmgr_run_cluster_passphrase_command(cluster_passphrase_command,
												  passphrase, KMGR_MAX_PASSPHRASE_LEN);

	/*
	 * Verify passphrase and prepare an internal key in plaintext on shared memory.
	 *
	 * XXX: do we need to prevent internal keys from being swapped out using
	 * mlock?
	 */
	if (!kmgr_verify_passphrase(passphrase, passlen, keys_wrap, KmgrShmem->intlKeys,
								KMGR_MAX_INTERNAL_KEYS))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cluster passphrase does not match expected passphrase")));
}

const CryptoKey *
KmgrGetKey(int id)
{
	/*
	 * Student TODO: Implement a function to return a reference to one
	 * of the crypto keys managed internally in the shared memory based on
	 * the "id" given. You can treat the "id" as an index number and returns
	 * the proper crypto keys stored in this shared memory structure, "KmgrShmem"
	 *
	 * inputs:
	 * 		id		=> the index that the caller wants you to return the crypto key
	 * 					from the shared memory structure "KmgrShmem"
	 */
	Assert(id < KMGR_MAX_INTERNAL_KEYS);

	CryptoKey * thekey = NULL;

	/******************* Your Code Starts Here ************************/


	/******************************************************************/

	return thekey;
}

/* Generate an empty CryptoKey */
static CryptoKey *
generate_crypto_key(int len)
{
	/*
	 * Student TODO: Implement a function to generate a crypto key
	 * that has length equal to the input "len". Note that you need
	 * to allocate memory to the "*newkey" pointer below before
	 * you can use it. You can use palloc0 to allocate memory. You can
	 * also use "pg_strong_random()" to generate a random number.
	 *
	 * inputs:
	 * 		len			=> the length of key in bytes that we would
	 * 						like to generate
	 * return values:
	 * 		CryptoKey*	=> please return a pointer to a newly allocated
	 * 						and generated key back to the caller
	 */

	CryptoKey *newkey = NULL;

	elog(WARNING, "[KMS] Entering %s...", __FUNCTION__);

	/******************* Your Code Starts Here ************************/



	/******************************************************************/

	elog(WARNING, "[KMS] Leaving %s...", __FUNCTION__);

	return newkey;
}

/*
 * Save the given crypto keys to the disk. We don't need CRC check for crypto
 * keys because these keys have HMAC which is used for integrity check
 * during unwrapping.
 */
static void
KmgrSaveCryptoKeys(const char *dir, CryptoKey *keys)
{
	/*
	 * Student TODO: Implement a function to save all crypto keys
	 * to disk. Please note that system manages 3 crypto keys so the
	 * for loop below will run 3 times, each time it loops you are
	 * supposed to open the file in "path" variable, write the keys,
	 * and close the file.
	 *
	 * inputs:
	 * 		*dir		=> location to store crypto keys
	 * 		*keys		=> list of keys to be stored
	 */
	elog(WARNING, "[KMS] Entering %s...", __FUNCTION__);

	for (int i = 0; i < KMGR_MAX_INTERNAL_KEYS; i++)
	{
		int			fd;
		char		path[MAXPGPATH];

		CryptoKeyFilePath(path, dir, i);

		/******************* Your Code Starts Here ************************/



		/******************************************************************/
	}

	elog(WARNING, "[KMS] Leaving %s...", __FUNCTION__);
}


/*
 * Check the last passphrase rotation was completed. If not, we decide which wrapped
 * keys will be used according to the status of temporary directory and its wrapped
 * keys.
 */
static void
recoverIncompleteRotation(void)
{
	struct stat st;
	struct stat st_tmp;
	CryptoKey *keys;
	int			nkeys_tmp;

	/* The cluster passphrase rotation was completed, nothing to do */
	if (stat(KMGR_TMP_DIR, &st_tmp) != 0)
		return;

	/*
	 * If there is only temporary directory, it means that the previous
	 * rotation failed after wrapping the all internal keys by the new
	 * passphrase.  Therefore we use the new cluster passphrase.
	 */
	if (stat(KMGR_DIR, &st) != 0)
	{
		ereport(DEBUG1,
				(errmsg("there is only temporary directory, use the newly wrapped keys")));

		if (rename(KMGR_TMP_DIR, KMGR_DIR) != 0)
			ereport(ERROR,
					errmsg("could not rename directory \"%s\" to \"%s\": %m",
						   KMGR_TMP_DIR, KMGR_DIR));
		ereport(LOG,
				errmsg("cryptographic keys wrapped by new passphrase command are chosen"),
				errdetail("last cluster passphrase rotation failed in the middle"));
		return;
	}

	/*
	 * In case where both the original directory and temporary directory
	 * exist, there are two possibilities: (a) the all internal keys are
	 * wrapped by the new passphrase but rotation failed before removing the
	 * original directory, or (b) the rotation failed during wrapping internal
	 * keys by the new passphrase.  In case of (a) we need to use the wrapped
	 * keys in the temporary directory as rotation is essentially completed,
	 * but in case of (b) we use the wrapped keys in the original directory.
	 *
	 * To check the possibility of (b) we validate the wrapped keys in the
	 * temporary directory by checking the number of wrapped keys.  Since the
	 * wrapped key length is smaller than one disk sector, which is 512 bytes
	 * on common hardware, saving wrapped key is atomic write. So we can
	 * ensure that the all wrapped keys are valid if the number of wrapped
	 * keys in the temporary directory is KMGR_MAX_INTERNAL_KEYS.
	 */
	keys = kmgr_get_cryptokeys(KMGR_TMP_DIR, &nkeys_tmp);

	if (nkeys_tmp == KMGR_MAX_INTERNAL_KEYS)
	{
		/*
		 * This is case (a), the all wrapped keys in temporary directory are
		 * valid. Remove the original directory and rename.
		 */
		ereport(DEBUG1,
				(errmsg("last passphrase rotation failed before renaming direcotry name, use the newly wrapped keys")));

		if (!rmtree(KMGR_DIR, true))
			ereport(ERROR,
					(errmsg("could not remove directory \"%s\"",
							KMGR_DIR)));
		if (rename(KMGR_TMP_DIR, KMGR_DIR) != 0)
			ereport(ERROR,
					errmsg("could not rename directory \"%s\" to \"%s\": %m",
						   KMGR_TMP_DIR, KMGR_DIR));

		ereport(LOG,
				errmsg("cryptographic keys wrapped by new passphrase command are chosen"),
				errdetail("last cluster passphrase rotation failed in the middle"));
	}
	else
	{
		/*
		 * This is case (b), the last passphrase rotation failed during
		 * wrapping keys. Remove the keys in the temporary directory and use
		 * keys in the original keys.
		 */
		ereport(DEBUG1,
				(errmsg("last passphrase rotation failed during wrapping keys, use the old wrapped keys")));

		if (!rmtree(KMGR_TMP_DIR, true))
			ereport(ERROR,
					(errmsg("could not remove directory \"%s\"",
							KMGR_DIR)));
		ereport(LOG,
				errmsg("cryptographic keys wrapped by old passphrase command are chosen"),
				errdetail("last cluster passphrase rotation failed in the middle"));
	}

	pfree(keys);
}

/*
 * SQL function to rotate the cluster passphrase. This function assumes that
 * the cluster_passphrase_command is already reloaded to the new value.
 * All internal keys are wrapped by the new passphrase and saved to the disk.
 * To update all crypto keys atomically we save the newly wrapped keys to the
 * temporary directory, pg_cryptokeys_tmp, and remove the original directory,
 * pg_cryptokeys, and rename it. These operation is performed without the help
 * of WAL.  In the case of failure during rotationpg_cryptokeys directory and
 * pg_cryptokeys_tmp directory can be left in incomplete status.  We recover
 * the incomplete situation by checkIncompleteRotation.
 */
Datum
pg_rotate_cluster_passphrase(PG_FUNCTION_ARGS)
{
	/*
	 * Student TODO: Implement a SQL function to rotate the current KEK
	 * and use it to re-wrap all the managed DEKs before saving them on
	 * disk. This SQL function implementation can be triggered by typing
	 * "select pg_rotate_cluster_passphrase()" on your psql prompt.
	 *
	 * This SQL function has to do the following:
	 *
	 * 1) Get the new passphrase from the "kmgr_run_cluster_passphrase_command()"
	 * 2) check that the passphrase is at least KMGR_MIN_PASSPHRASE_LEN bytes long
	 * 3) derive the new KEK and HMAC key from the passphrase
	 * 4) use the new KEK and HMAC keys to wrap all the DEKs stored in KmgrShmem*
	 *    structure
	 * 5) save all the newly wrapped keys on disk
	 * 6) return true and finish
	 *
	 * inputs:
	 * 		=> None
	 *
	 * returns:
	 * 		=> true on success, false on failure.
	 */

	PgAeadCtx	*ctx;
	CryptoKey	newkeys[KMGR_MAX_INTERNAL_KEYS] = {0};
	char		passphrase[KMGR_MAX_PASSPHRASE_LEN];
	uint8		new_kekenc[PG_AEAD_ENC_KEY_LEN];
	uint8		new_kekhmac[PG_AEAD_MAC_KEY_LEN];
	int			passlen;

	elog(WARNING, "[KMS] Entering %s...", __FUNCTION__);

	if (!key_management_enabled)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("could not rotate cluster passphrase because key management is not supported")));

	/* Recover the failure of the last passphrase rotation if necessary */
	recoverIncompleteRotation();

	/******************* Your Code Starts Here ************************/



	/******************************************************************/

	elog(WARNING, "[KMS] Leaving %s...", __FUNCTION__);
	PG_RETURN_BOOL(true);
}
