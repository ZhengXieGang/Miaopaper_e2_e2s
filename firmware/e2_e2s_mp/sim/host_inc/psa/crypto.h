#pragma once
/* PSA Crypto SHA-256 接口（本机模拟器采用纯 C 标准 FIPS 180-4 实现）。 */
#include <stddef.h>
#include <stdint.h>

typedef int psa_status_t;
typedef uint32_t psa_algorithm_t;

#define PSA_SUCCESS 0
#define PSA_ERROR_INVALID_ARGUMENT (-135)
#define PSA_ERROR_BAD_STATE (-137)
#define PSA_ALG_SHA_256 0x02000009u

typedef struct {
    uint32_t state[8];
    uint64_t bitcount;
    uint8_t buffer[64];
    uint32_t buflen;
    uint8_t active;
} psa_hash_operation_t;

#define PSA_HASH_OPERATION_INIT { {0}, 0, {0}, 0, 0 }

psa_status_t psa_hash_setup(psa_hash_operation_t *operation, psa_algorithm_t alg);
psa_status_t psa_hash_update(psa_hash_operation_t *operation,
                             const uint8_t *input, size_t input_length);
psa_status_t psa_hash_finish(psa_hash_operation_t *operation,
                             uint8_t *hash, size_t hash_size, size_t *hash_length);
psa_status_t psa_hash_abort(psa_hash_operation_t *operation);
