// Mbed TLS 3.6.8 (third_party/mbedtls: library/ holds only the sources this configuration uses) for the online
// server's accounts and save sync: runtime/acct.c (the game: PC and PS5) and contrib/lzaserver (the server).
// TLS 1.3 only; key exchange ECDHE (X25519, P-256/384/521); server certificates ECDSA (lzaserver's own, made at its
// first start) or RSA / ECDSA from a certificate authority (an operator's own certificate); AES-GCM, ChaCha20-Poly1305.
// Threads: the server serves accounts on several threads, the PSA key store is shared (MBEDTLS_THREADING_PTHREAD).
#pragma once
#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_PTHREAD
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_HAVE_TIME_DATE
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_FS_IO
#define MBEDTLS_ERROR_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_SECP521R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_SSL_PROTO_TLS1_3
#define MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL_ENABLED
#define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_MAX_FRAGMENT_LENGTH
#define MBEDTLS_AES_C
#define MBEDTLS_AES_ROM_TABLES
#define MBEDTLS_GCM_C
#define MBEDTLS_CHACHA20_C
#define MBEDTLS_POLY1305_C
#define MBEDTLS_CHACHAPOLY_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_HKDF_C
#define MBEDTLS_HMAC_DRBG_C
#define MBEDTLS_MD_C
#define MBEDTLS_OID_C
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_PEM_WRITE_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_PK_WRITE_C
#define MBEDTLS_PSA_CRYPTO_C
#define MBEDTLS_RSA_C
#define MBEDTLS_GENPRIME
#define MBEDTLS_SHA1_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_SRV_C
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_CREATE_C
#define MBEDTLS_X509_CRT_WRITE_C
#define MBEDTLS_SSL_IN_CONTENT_LEN 16384
#define MBEDTLS_SSL_OUT_CONTENT_LEN 16384
#if defined(__PROSPERO__)  // (the PS5: no platform entropy source - runtime/acct.c adds arc4random_buf)
#define MBEDTLS_NO_PLATFORM_ENTROPY
#endif
