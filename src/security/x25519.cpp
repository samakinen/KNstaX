// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/security/x25519.hpp"

#if KNX_SECURE_ENABLED

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>

#include <cstring>
#include <span>

namespace knx {
namespace security {

static int rng_init(mbedtls_ctr_drbg_context& ctr_drbg, mbedtls_entropy_context& entropy)
{
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);

    static const char* pers = "knx-x25519";
    return mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                 reinterpret_cast<const unsigned char*>(pers),
                                 std::strlen(pers));
}

static void rng_free(mbedtls_ctr_drbg_context& ctr_drbg, mbedtls_entropy_context& entropy)
{
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
}

static bool ecp_mul_base(const mbedtls_mpi& d, mbedtls_ecp_point& Q)
{
    mbedtls_ecp_group grp;
    mbedtls_ecp_group_init(&grp);
    if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519) != 0) {
        mbedtls_ecp_group_free(&grp);
        return false;
    }

    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_context entropy;
    const int seed_rc = rng_init(ctr_drbg, entropy);
    if (seed_rc != 0) {
        rng_free(ctr_drbg, entropy);
        mbedtls_ecp_group_free(&grp);
        return false;
    }

    const int rc = mbedtls_ecp_mul(&grp, &Q, &d, &grp.G, mbedtls_ctr_drbg_random, &ctr_drbg);

    rng_free(ctr_drbg, entropy);
    mbedtls_ecp_group_free(&grp);

    return rc == 0;
}

static bool ecp_mul_point(const mbedtls_mpi& d, std::span<const uint8_t, 32> peer, mbedtls_ecp_point& Qout)
{
    mbedtls_ecp_group grp;
    mbedtls_ecp_group_init(&grp);
    if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519) != 0) {
        mbedtls_ecp_group_free(&grp);
        return false;
    }

    mbedtls_ecp_point Qpeer;
    mbedtls_ecp_point_init(&Qpeer);
    if (mbedtls_ecp_point_read_binary(&grp, &Qpeer, peer.data(), peer.size()) != 0) {
        mbedtls_ecp_point_free(&Qpeer);
        mbedtls_ecp_group_free(&grp);
        return false;
    }

    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_context entropy;
    const int seed_rc = rng_init(ctr_drbg, entropy);
    if (seed_rc != 0) {
        rng_free(ctr_drbg, entropy);
        mbedtls_ecp_point_free(&Qpeer);
        mbedtls_ecp_group_free(&grp);
        return false;
    }

    const int rc = mbedtls_ecp_mul(&grp, &Qout, &d, &Qpeer, mbedtls_ctr_drbg_random, &ctr_drbg);

    rng_free(ctr_drbg, entropy);
    mbedtls_ecp_point_free(&Qpeer);
    mbedtls_ecp_group_free(&grp);

    return rc == 0;
}

static bool point_to_u32(const mbedtls_ecp_point& Q, std::span<uint8_t, 32> out)
{
    mbedtls_ecp_group grp;
    mbedtls_ecp_group_init(&grp);
    if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519) != 0) {
        mbedtls_ecp_group_free(&grp);
        return false;
    }

    // mbedTLS output format for Curve25519 has varied between versions/builds.
    // Accept the common variants and always return a 32-byte u-coordinate.
    std::array<uint8_t, 80> tmp{};
    size_t written = 0;
    const int rc = mbedtls_ecp_point_write_binary(&grp, &Q, MBEDTLS_ECP_PF_UNCOMPRESSED,
                                                  &written, tmp.data(), tmp.size());
    mbedtls_ecp_group_free(&grp);
    if (rc != 0) return false;

    if (written == 32) {
        std::memcpy(out.data(), tmp.data(), out.size());
        return true;
    }

    if (written == 33 && tmp[0] == 0x04) {
        std::memcpy(out.data(), tmp.data() + 1, out.size());
        return true;
    }

    if (written == 65 && tmp[0] == 0x04) {
        // Uncompressed point: 0x04 || X(32) || Y(32). For Curve25519, use X.
        std::memcpy(out.data(), tmp.data() + 1, out.size());
        return true;
    }

    return false;
}

util::Result<void> X25519::publicFromPrivate(const Scalar& priv, PublicKey& pub)
{
    mbedtls_mpi d;
    mbedtls_mpi_init(&d);

    // X25519 requires clamping the scalar.
    Scalar clamped = priv;
    clamped[0] &= 248;
    clamped[31] &= 127;
    clamped[31] |= 64;

    // mbedTLS expects little-endian for Curve25519 binary reads.
    if (mbedtls_mpi_read_binary_le(&d, clamped.data(), clamped.size()) != 0) {
        mbedtls_mpi_free(&d);
        return util::ErrorCode::OperationFailed;
    }

    mbedtls_ecp_point Q;
    mbedtls_ecp_point_init(&Q);

    const bool ok = ecp_mul_base(d, Q) && point_to_u32(Q, std::span<uint8_t, 32>(pub));

    mbedtls_ecp_point_free(&Q);
    mbedtls_mpi_free(&d);

    if (!ok) {
        return util::ErrorCode::OperationFailed;
    }
    return util::Result<void>::ok();
}

util::Result<void> X25519::sharedSecret(const Scalar& priv, const PublicKey& peerPub, SharedSecret& secret)
{
    mbedtls_mpi d;
    mbedtls_mpi_init(&d);

    Scalar clamped = priv;
    clamped[0] &= 248;
    clamped[31] &= 127;
    clamped[31] |= 64;

    if (mbedtls_mpi_read_binary_le(&d, clamped.data(), clamped.size()) != 0) {
        mbedtls_mpi_free(&d);
        return util::ErrorCode::OperationFailed;
    }

    mbedtls_ecp_point Q;
    mbedtls_ecp_point_init(&Q);

    const bool ok = ecp_mul_point(d, std::span<const uint8_t, 32>(peerPub), Q)
        && point_to_u32(Q, std::span<uint8_t, 32>(secret));

    mbedtls_ecp_point_free(&Q);
    mbedtls_mpi_free(&d);

    if (!ok) {
        return util::ErrorCode::OperationFailed;
    }
    return util::Result<void>::ok();
}

} // namespace security
} // namespace knx

#endif // KNX_SECURE_ENABLED
