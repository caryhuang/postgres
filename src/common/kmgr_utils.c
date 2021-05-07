/*-------------------------------------------------------------------------
 *
 * kmgr_utils.c
 *	  Shared frontend/backend for cryptographic key management
 *
 * Copyright (c) 2020, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/common/kmgr_utils.c
 *
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include <unistd.h>
#include <sys/stat.h>

#include "common/aead.h"
#ifdef FRONTEND
#include "common/logging.h"
#endif
#include "common/file_perm.h"
#include "common/kmgr_utils.h"
#include "common/sha2.h"
#include "crypto/kmgr.h"
#include "utils/elog.h"
#include "storage/fd.h"

#ifndef FRONTEND
#include "pgstat.h"
#include "storage/fd.h"
#endif

#define KMGR_PROMPT_MSG "Enter database encryption pass phrase:"

#ifdef FRONTEND
static FILE *open_pipe_stream(const char *command);
static int	close_pipe_stream(FILE *file);
#endif

static void read_one_keyfile(const char *dataDir, uint32 id,
							 CryptoKey *key_p);

/*
 * Verify the correctness of the given passphrase by unwrapping the given keys.
 * If the given passphrase is correct we set unwrapped keys to keys_out and return
 * true.  Otherwise return false.  Please note that this function changes the
 * contents of keys_out even on failure.  Both keys_in and keys_out must be the
 * same length, nkey.
 */
bool
kmgr_verify_passphrase(char *passphrase, int passlen,
					   CryptoKey *keys_in, CryptoKey *keys_out, int nkeys)
{
	PgAeadCtx *tmpctx;
	uint8		user_enckey[PG_AEAD_ENC_KEY_LEN];
	uint8		user_hmackey[PG_AEAD_MAC_KEY_LEN];

	/*
	 * Create temporary wrap context with encryption key and HMAC key extracted
	 * from the passphrase.
	 */
	kmgr_derive_keys(passphrase, passlen, user_enckey, user_hmackey);
	tmpctx = pg_create_aead_ctx(user_enckey, user_hmackey);

	for (int i = 0; i < nkeys; i++)
	{

		if (!kmgr_unwrap_key(tmpctx, &(keys_in[i]), &(keys_out[i])))
		{
			/* The passphrase is not correct */
			pg_free_aead_ctx(tmpctx);
			return false;
		}
	}

	/* The passphrase is correct, free the cipher context */
	pg_free_aead_ctx(tmpctx);

	return true;
}

/* Generate encryption key and mac key from given passphrase */
void
kmgr_derive_keys(char *passphrase, Size passlen,
				 uint8 enckey[PG_AEAD_ENC_KEY_LEN],
				 uint8 mackey[PG_AEAD_MAC_KEY_LEN])
{
	/*
	 * Student TODO: Implement a function to derive keys from
	 * the given passphrase. We want to derive 2 keys, first one
	 * is the KEK that is used to wrap other encryption keys. Second
	 * one is a HMAC key that is used to compute a hash value.
	 *
	 * inputs:
	 * 		passphrase	=> pointer to the start of the passphrase
	 * 		passlen		=> the length of the passphrase
	 *
	 * outputs:
	 * 		enckey[32]	=> buffer to store the KEK. (32 bytes long)
	 * 		mackey[64]	=> buffer to store the HMAC key for hashing
	 * 						purposes. (64 bytes long)
	 */


	elog(WARNING, "[KMS] Entering %s...", __FUNCTION__);

	/******************* Your Code Starts Here ************************/



	/******************************************************************/

	elog(WARNING, "[KMS] Leaving %s...", __FUNCTION__);
}

/* Wrap the given key */
bool
kmgr_wrap_key(PgAeadCtx *ctx, CryptoKey *in, CryptoKey *out)
{
	if (!pg_aead_encrypt(ctx, in->key, in->klen, out->key, &(out->klen)))
		return false;

	return true;
}

/* Unwrap the given key */
bool
kmgr_unwrap_key(PgAeadCtx *ctx, CryptoKey *in, CryptoKey *out)
{
	if (!pg_aead_decrypt(ctx, in->key, in->klen, out->key, &(out->klen)))
		return false;

	return true;
}

/*
 * Run cluster passphrase command.
 *
 * prompt will be substituted for %p.
 *
 * The result will be put in buffer buf, which is of size size.
 * The return value is the length of the actual result.
 */
int
kmgr_run_cluster_passphrase_command(char *passphrase_command, char *buf,
									int size)
{
	char		command[MAXPGPATH];
	char	   *p;
	char	   *dp;
	char	   *endp;
	FILE	   *fh;
	int			pclose_rc;
	size_t		len = 0;

	Assert(size > 0);
	buf[0] = '\0';

	dp = command;
	endp = command + MAXPGPATH - 1;
	*endp = '\0';

	for (p = passphrase_command; *p; p++)
	{
		if (p[0] == '%')
		{
			switch (p[1])
			{
				case 'p':
					StrNCpy(dp, KMGR_PROMPT_MSG, strlen(KMGR_PROMPT_MSG));
					dp += strlen(KMGR_PROMPT_MSG);
					p++;
					break;
				case '%':
					p++;
					if (dp < endp)
						*dp++ = *p;
					break;
				default:
					if (dp < endp)
						*dp++ = *p;
					break;
			}
		}
		else
		{
			if (dp < endp)
				*dp++ = *p;
		}
	}
	*dp = '\0';

#ifdef FRONTEND
	fh = open_pipe_stream(command);
	if (fh == NULL)
	{
		pg_log_fatal("could not execute command \"%s\": %m",
					 command);
		exit(EXIT_FAILURE);
	}
#else
	fh = OpenPipeStream(command, "r");
	if (fh == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not execute command \"%s\": %m",
						command)));
#endif

	if ((len = fread(buf, sizeof(char), size, fh)) < size)
	{
		if (ferror(fh))
		{
#ifdef FRONTEND
			pg_log_fatal("could not read from command \"%s\": %m",
						 command);
			exit(EXIT_FAILURE);
#else
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read from command \"%s\": %m",
							command)));
#endif
		}
	}

#ifdef FRONTEND
	pclose_rc = close_pipe_stream(fh);
#else
	pclose_rc = ClosePipeStream(fh);
#endif

	if (pclose_rc == -1)
	{
#ifdef FRONTEND
		pg_log_fatal("could not close pipe to external command: %m");
		exit(EXIT_FAILURE);
#else
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close pipe to external command: %m")));
#endif
	}
	else if (pclose_rc != 0)
	{
#ifdef FRONTEND
		pg_log_fatal("command \"%s\" failed", command);
		exit(EXIT_FAILURE);
#else
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("command \"%s\" failed",
						command),
				 errdetail_internal("%s", wait_result_to_str(pclose_rc))));
#endif
	}

	return len;
}

#ifdef FRONTEND
static FILE *
open_pipe_stream(const char *command)
{
	FILE	   *res;

#ifdef WIN32
	size_t		cmdlen = strlen(command);
	char	   *buf;
	int			save_errno;

	buf = malloc(cmdlen + 2 + 1);
	if (buf == NULL)
	{
		errno = ENOMEM;
		return NULL;
	}
	buf[0] = '"';
	mempcy(&buf[1], command, cmdlen);
	buf[cmdlen + 1] = '"';
	buf[cmdlen + 2] = '\0';

	res = _popen(buf, "r");

	save_errno = errno;
	free(buf);
	errno = save_errno;
#else
	res = popen(command, "r");
#endif							/* WIN32 */
	return res;
}

static int
close_pipe_stream(FILE *file)
{
#ifdef WIN32
	return _pclose(file);
#else
	return pclose(file);
#endif							/* WIN32 */
}
#endif							/* FRONTEND */

CryptoKey *
kmgr_get_cryptokeys(const char *path, int *nkeys)
{
	struct dirent *de;
	DIR			*dir;
	CryptoKey	*keys;

#ifndef FRONTEND
	if ((dir = AllocateDir(path)) == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open directory \"%s\": %m",
						path)));
#else
	if ((dir = opendir(path)) == NULL)
		pg_log_fatal("could not open directory \"%s\": %m", path);
#endif

	keys = (CryptoKey *) palloc0(sizeof(CryptoKey) * KMGR_MAX_INTERNAL_KEYS);
	*nkeys = 0;

#ifndef FRONTEND
	while ((de = ReadDir(dir, KMGR_DIR)) != NULL)
#else
	while ((de = readdir(dir)) != NULL)
#endif
	{
		if (strlen(de->d_name) == 4 &&
			strspn(de->d_name, "0123456789ABCDEF") == 4)
		{
			uint32		id;

			id = strtoul(de->d_name, NULL, 16);

			if (id < 0 || id >= KMGR_MAX_INTERNAL_KEYS)
			{
#ifndef FRONTEND
				elog(ERROR, "invalid cryptographic key identifier %u", id);
#else
				pg_log_fatal("invalid cryptographic key identifier %u", id);
#endif
			}

			if (*nkeys >= KMGR_MAX_INTERNAL_KEYS)
			{
#ifndef FRONTEND
				elog(ERROR, "too many cryptographic kes");
#else
				pg_log_fatal("too many cryptographic keys");
#endif
			}

			read_one_keyfile(path, id, &(keys[id]));
			(*nkeys)++;
		}
	}

#ifndef FRONTEND
	FreeDir(dir);
#else
	closedir(dir);
#endif

	return keys;
}

static void
read_one_keyfile(const char *cryptoKeyDir, uint32 id, CryptoKey *key_p)
{
	char		path[MAXPGPATH];
	int			fd;
	int			r;

	CryptoKeyFilePath(path, cryptoKeyDir, id);

#ifndef FRONTEND
	if ((fd = OpenTransientFile(path, O_RDONLY | PG_BINARY)) == -1)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\" for reading: %m",
						path)));
#else
	if ((fd = open(path, O_RDONLY | PG_BINARY, 0)) == -1)
		pg_log_fatal("could not open file \"%s\" for reading: %m",
					 path);
#endif

#ifndef FRONTEND
	pgstat_report_wait_start(WAIT_EVENT_KEY_FILE_READ);
#endif

	/* Get key bytes */
	r = read(fd, key_p, sizeof(CryptoKey));
	if (r != sizeof(CryptoKey))
	{
		if (r < 0)
		{
#ifndef FRONTEND
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read file \"%s\": %m", path)));
#else
			pg_log_fatal("could not read file \"%s\": %m", path);
#endif
		}
		else
		{
#ifndef FRONTEND
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("could not read file \"%s\": read %d of %zu",
							path, r, sizeof(CryptoKey))));
#else
			pg_log_fatal("could not read file \"%s\": read %d of %zu",
						 path, r, sizeof(CryptoKey));
#endif
		}
	}

#ifndef FRONTEND
	pgstat_report_wait_end();
#endif

#ifndef FRONTEND
	if (CloseTransientFile(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m",
						path)));
#else
	if (close(fd) != 0)
		pg_log_fatal("could not close file \"%s\": %m", path);
#endif
}
