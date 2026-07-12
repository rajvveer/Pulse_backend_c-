/*
 * bcrypt.c — vendored copy of trusch/libbcrypt (rg3/libbcrypt).
 *
 * Thin, public-domain C wrapper that exposes the simple three-function bcrypt
 * API (bcrypt_gensalt / bcrypt_hashpw / bcrypt_checkpw) on top of Solar
 * Designer's public-domain crypt_blowfish implementation (crypt_rn /
 * crypt_gensalt_rn in wrapper.c).
 *
 * Original wrapper © Andrew Knox / rg3 / trusch — released to the public
 * domain. crypt_blowfish © Solar Designer — public domain.
 *
 * Randomness note: the salt is seeded from a cryptographically secure RNG.
 *   * On Windows we deliberately use rand_s() (declared in <stdlib.h> after
 *     #define _CRT_RAND_S). rand_s is backed by RtlGenRandom (a CSPRNG) and,
 *     crucially, does NOT require including the Windows SDK's <bcrypt.h>
 *     (BCryptGenRandom). Including the Windows <bcrypt.h> here would clash with
 *     OUR bcrypt.h of the same name, so we avoid it entirely.
 *   * Elsewhere we use getentropy() when available, falling back to reading
 *     /dev/urandom.
 */

#if defined(_WIN32)
/* Must precede <stdlib.h> so rand_s() is declared. */
#ifndef _CRT_RAND_S
#define _CRT_RAND_S
#endif
#endif

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#if !defined(_WIN32)
#include <fcntl.h>
#if defined(__has_include)
#if __has_include(<unistd.h>)
#include <unistd.h>
#endif
#if __has_include(<sys/random.h>)
#include <sys/random.h>
#define BCRYPT_HAVE_GETRANDOM 1
#endif
#else
#include <unistd.h>
#endif
#endif

#include "pulse_bcrypt.h"
#include "ow-crypt.h"

#define RANDBYTES (16)

/*
 * fill_random — fill buf with `n` cryptographically secure random bytes.
 * Returns 0 on success, -1 on failure.
 */
static int fill_random(unsigned char *buf, size_t n)
{
#if defined(_WIN32)
	size_t i;
	for (i = 0; i < n; i += sizeof(unsigned int)) {
		unsigned int r = 0;
		size_t chunk = n - i;
		if (chunk > sizeof(unsigned int))
			chunk = sizeof(unsigned int);
		/* rand_s is a CSPRNG (RtlGenRandom) and needs no Windows
		 * <bcrypt.h>, so it does not clash with our header. */
		if (rand_s(&r) != 0)
			return -1;
		memcpy(buf + i, &r, chunk);
	}
	return 0;
#else
#if defined(BCRYPT_HAVE_GETRANDOM)
	{
		size_t off = 0;
		while (off < n) {
			ssize_t got = getrandom(buf + off, n - off, 0);
			if (got < 0) {
				if (errno == EINTR)
					continue;
				break; /* fall through to /dev/urandom */
			}
			off += (size_t)got;
		}
		if (off == n)
			return 0;
	}
#endif
	{
		int fd = open("/dev/urandom", O_RDONLY);
		size_t off = 0;
		if (fd < 0)
			return -1;
		while (off < n) {
			ssize_t got = read(fd, buf + off, n - off);
			if (got < 0) {
				if (errno == EINTR)
					continue;
				close(fd);
				return -1;
			}
			if (got == 0)
				break;
			off += (size_t)got;
		}
		close(fd);
		return (off == n) ? 0 : -1;
	}
#endif
}

int bcrypt_gensalt(int workfactor, char salt[BCRYPT_HASHSIZE])
{
	int fd;
	char input[RANDBYTES];
	char *aux;

	(void)fd;

	if (workfactor < 4)
		workfactor = 4;
	if (workfactor > 31)
		workfactor = 31;

	if (fill_random((unsigned char *)input, RANDBYTES) != 0)
		return 1;

	/* Generate a $2b$ modular-crypt salt at the requested cost. */
	aux = crypt_gensalt_rn("$2b$", (unsigned long)workfactor,
	                       input, RANDBYTES, salt, BCRYPT_HASHSIZE);

	return (aux == NULL) ? 5 : 0;
}

int bcrypt_hashpw(const char *passwd, const char salt[BCRYPT_HASHSIZE],
                  char hash[BCRYPT_HASHSIZE])
{
	char *aux;
	aux = crypt_rn(passwd, salt, hash, BCRYPT_HASHSIZE);
	return (aux == NULL) ? 1 : 0;
}

int bcrypt_checkpw(const char *passwd, const char hash[BCRYPT_HASHSIZE])
{
	int ret;
	char outhash[BCRYPT_HASHSIZE];

	ret = bcrypt_hashpw(passwd, hash, outhash);
	if (ret != 0)
		return -1;

	/* Compare the full re-hash against the stored hash. The two strings
	 * have equal, known length here (both are MCF bcrypt hashes), so a
	 * length-bounded constant-time compare is sufficient and avoids leaking
	 * timing information about how many leading characters matched. */
	{
		size_t i;
		size_t hlen = strlen(hash);
		size_t olen = strlen(outhash);
		unsigned char diff = (unsigned char)(hlen ^ olen);
		size_t n = hlen < olen ? hlen : olen;
		for (i = 0; i < n; i++)
			diff |= (unsigned char)(hash[i] ^ outhash[i]);
		return diff == 0 ? 0 : -1;
	}
}
