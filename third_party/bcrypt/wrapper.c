/*
 * Written by Solar Designer and placed in the public domain.
 * See crypt_blowfish.c for more information.
 *
 * This is the crypt(3) / crypt_rn() dispatcher from the crypt_blowfish
 * distribution, reduced to the bcrypt ("$2") subset required by the vendored
 * libbcrypt wrapper. Only bcrypt hashing and bcrypt salt generation are
 * dispatched here; the traditional DES / MD5 / extended-DES helpers from the
 * full crypt_blowfish are intentionally omitted, since this project only ever
 * asks for bcrypt.
 */

#define __SKIP_GNU
#include "ow-crypt.h"

#include "crypt_blowfish.h"

#include <errno.h>
#include <string.h>
#include <stdlib.h>

#ifndef __set_errno
#define __set_errno(val) errno = (val)
#endif

/* Declared in crypt_gensalt.c */
extern char *_crypt_gensalt_blowfish_rn(const char *prefix, unsigned long count,
	const char *input, int size, char *output, int output_size);

static int _crypt_data_alloc(void **data, int *size, int need)
{
	void *updated;

	if (*data && *size >= need) return 0;

	updated = realloc(*data, need);

	if (!updated) {
		__set_errno(ENOMEM);
		return -1;
	}

	*data = updated;
	*size = need;

	return 0;
}

static char *_crypt_retval_magic(char *retval, const char *setting,
	char *output, int size)
{
	if (retval)
		return retval;

	if (_crypt_output_magic(setting, output, size))
		return NULL; /* shouldn't happen */

	return output;
}

char *crypt_rn(const char *key, const char *setting, void *data, int size)
{
	char *retval;

	/* Only the bcrypt prefix is supported by this trimmed wrapper. */
	if (setting[0] == '$' && setting[1] == '2') {
		retval = _crypt_blowfish_rn(key, setting,
			(char *)data, size);
	} else {
		__set_errno(EINVAL);
		retval = NULL;
	}

	return _crypt_retval_magic(retval, setting, (char *)data, size);
}

char *crypt_ra(const char *key, const char *setting,
	void **data, int *size)
{
	char *retval;

	if (_crypt_data_alloc(data, size, 7 + 22 + 31 + 1))
		return NULL;

	if (setting[0] == '$' && setting[1] == '2') {
		retval = _crypt_blowfish_rn(key, setting,
			(char *)*data, *size);
	} else {
		__set_errno(EINVAL);
		retval = NULL;
	}

	return _crypt_retval_magic(retval, setting, (char *)*data, *size);
}

char *crypt_gensalt_rn(const char *prefix, unsigned long count,
	const char *input, int size, char *output, int output_size)
{
	char *retval;

	/* This may be overridden below, but only the bcrypt path is used. */
	if (output_size > 0) output[0] = '\0';
	__set_errno(EINVAL);
	retval = NULL;

	if (size >= 16 && input &&
	    prefix[0] == '$' && prefix[1] == '2') {
		retval = _crypt_gensalt_blowfish_rn(prefix, count,
			input, size, output, output_size);
	}

	return retval;
}

char *crypt_gensalt_ra(const char *prefix, unsigned long count,
	const char *input, int size)
{
	char output[7 + 22 + 1];
	char *retval, *result;

	retval = crypt_gensalt_rn(prefix, count,
		input, size, output, sizeof(output));

	result = NULL;
	if (retval) {
		size_t len = strlen(retval) + 1;
		result = malloc(len);
		if (result)
			memcpy(result, retval, len);
	}

	return result;
}

char *crypt_gensalt(const char *prefix, unsigned long count,
	const char *input, int size)
{
	static char output[7 + 22 + 1];

	return crypt_gensalt_rn(prefix, count,
		input, size, output, sizeof(output));
}
