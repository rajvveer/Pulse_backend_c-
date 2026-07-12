/*
 * bcrypt.h — vendored copy of trusch/libbcrypt (rg3/libbcrypt) public API.
 *
 * This is the small, well-known C wrapper around Solar Designer's
 * public-domain crypt_blowfish implementation. It exposes exactly the
 * three-function API the Pulse backend already calls. Hashes produced are
 * standard bcrypt modular-crypt-format ("$2b$..."), interoperable with
 * Node's bcrypt / bcryptjs.
 *
 * Original wrapper © Andrew Knox / rg3 / trusch — released to the public
 * domain. crypt_blowfish © Solar Designer — public domain.
 */
#ifndef BCRYPT_H_
#define BCRYPT_H_

#ifdef __cplusplus
extern "C" {
#endif

#define BCRYPT_HASHSIZE 64

/*
 * bcrypt_gensalt
 *
 * Generates a random salt encoded in modular crypt format, using the given
 * work factor (cost). On success the NUL-terminated salt string is written to
 * `salt` (which must be at least BCRYPT_HASHSIZE bytes). The salt encodes the
 * "$2b$" prefix and the cost.
 *
 * workfactor: bcrypt cost; values < 4 are clamped to 4, > 31 to 31.
 *
 * Returns 0 on success, non-zero on failure.
 */
int bcrypt_gensalt(int workfactor, char salt[BCRYPT_HASHSIZE]);

/*
 * bcrypt_hashpw
 *
 * Hashes the given password with the given salt (as produced by
 * bcrypt_gensalt, or any valid bcrypt MCF salt/hash prefix). On success the
 * NUL-terminated hash string is written to `hash` (at least BCRYPT_HASHSIZE
 * bytes).
 *
 * Returns 0 on success, non-zero on failure.
 */
int bcrypt_hashpw(const char *passwd, const char salt[BCRYPT_HASHSIZE],
                  char hash[BCRYPT_HASHSIZE]);

/*
 * bcrypt_checkpw
 *
 * Verifies a password against a previously produced bcrypt hash. Re-hashes
 * `passwd` using `hash` as the salt and compares the result against `hash` in
 * constant time.
 *
 * Returns 0 if the password matches, non-zero if it does not match or on error.
 */
int bcrypt_checkpw(const char *passwd, const char hash[BCRYPT_HASHSIZE]);

#ifdef __cplusplus
}
#endif

#endif /* BCRYPT_H_ */
