/*
 * Written by Solar Designer and placed in the public domain.
 * See crypt_blowfish.c for more information.
 *
 * This is ow-crypt.h from the crypt_blowfish distribution, trimmed to the
 * subset used by the vendored libbcrypt wrapper. It declares the reentrant
 * crypt_rn / crypt_gensalt_rn entry points implemented in wrapper.c and
 * crypt_gensalt.c.
 */
#ifndef _OW_CRYPT_H
#define _OW_CRYPT_H

#ifndef __SKIP_GNU
#ifdef __cplusplus
extern "C" {
#endif

char *crypt(const char *key, const char *setting);
char *crypt_r(const char *key, const char *setting, void *data);

#ifdef __cplusplus
}
#endif
#endif

#ifndef __SKIP_OW
#ifdef __cplusplus
extern "C" {
#endif

char *crypt_rn(const char *key, const char *setting, void *data, int size);
char *crypt_ra(const char *key, const char *setting, void **data, int *size);

char *crypt_gensalt(const char *prefix, unsigned long count,
                    const char *input, int size);
char *crypt_gensalt_rn(const char *prefix, unsigned long count,
                       const char *input, int size, char *output, int output_size);
char *crypt_gensalt_ra(const char *prefix, unsigned long count,
                       const char *input, int size);

#ifdef __cplusplus
}
#endif
#endif

#endif
