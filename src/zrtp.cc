#ifdef __RTP_CRYPTO__
#include <cstring>
#include <thread>

#include "debug.hh"
#include "crypto.hh"
#include "random.hh"
#include "zrtp.hh"

#include "mzrtp/commit.hh"
#include "mzrtp/confack.hh"
#include "mzrtp/confirm.hh"
#include "mzrtp/dh_kxchng.hh"
#include "mzrtp/hello.hh"
#include "mzrtp/hello_ack.hh"
#include "mzrtp/receiver.hh"

using namespace kvz_rtp::zrtp_msg;

#define ZRTP_VERSION 110

kvz_rtp::zrtp::zrtp():
    initialized_(false),
    receiver_()
{
    cctx_.sha256 = new kvz_rtp::crypto::sha256;
    cctx_.dh     = new kvz_rtp::crypto::dh;
}

kvz_rtp::zrtp::~zrtp()
{
    /* TODO: free everything */
}

rtp_error_t kvz_rtp::zrtp::set_timeout(size_t timeout)
{
    struct timeval tv = {
        .tv_sec  = 0,
        .tv_usec = (int)timeout * 1000,
    };

    if (setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0)
        return RTP_GENERIC_ERROR;

    return RTP_OK;
}

void kvz_rtp::zrtp::generate_zid()
{
    kvz_rtp::crypto::random::generate_random(session_.o_zid, 12);
}

/* ZRTP Key Derivation Function (KDF) (Section 4.5.2)
 *
 * KDF(KI, Label, Context, L) = HMAC(KI, i || Label || 0x00 || Context || L)
 *
 * Where:
 *    - KI      = s0
 *    - Label   = What is the key used
 *    - Context = ZIDi || ZIDr || total_hash
 *    - L       = 256
 */
void kvz_rtp::zrtp::derive_key(const char *label, uint32_t key_len, uint8_t *out_key)
{
    auto hmac_sha256 = kvz_rtp::crypto::hmac::sha256(session_.secrets.s0, 32);
    uint8_t tmp[32]  = { 0 };
    uint32_t length  = htonl(key_len);
    uint32_t counter = 0x1;
    uint8_t delim    = 0x0;

    hmac_sha256.update((uint8_t *)&counter,  4);
    hmac_sha256.update((uint8_t *)label,     strlen(label));

    if (session_.role == INITIATOR) {
        hmac_sha256.update((uint8_t *)session_.o_zid, 12);
        hmac_sha256.update((uint8_t *)session_.r_zid, 12);
    } else {
        hmac_sha256.update((uint8_t *)session_.r_zid, 12);
        hmac_sha256.update((uint8_t *)session_.o_zid, 12);
    }

    hmac_sha256.update((uint8_t *)session_.hash_ctx.total_hash, 32);
    hmac_sha256.update((uint8_t *)&delim,                        1);
    hmac_sha256.update((uint8_t *)&length,                       4);

    /* Key length might differ from the digest length in which case the digest
     * must be generated to a temporary buffer and truncated to fit the "out_key" buffer */
    if (key_len != 256) {
        LOG_DEBUG("Truncate key to %u bits!", key_len);

        hmac_sha256.final((uint8_t *)tmp);
        memcpy(out_key, tmp, key_len / 8);
    } else {
        hmac_sha256.final((uint8_t *)out_key);
    }
}

void kvz_rtp::zrtp::generate_secrets()
{
    cctx_.dh->generate_keys();
    cctx_.dh->get_pk(session_.dh_ctx.public_key, 384);

    /* kvzRTP does not support Preshared mode (for now at least) so
     * there will be no shared secrets between the endpoints.
     *
     * Generate random data for the retained secret values that are sent
     * in the DHPart1/DHPart2 message and, due to mismatch, ignored by remote */
    kvz_rtp::crypto::random::generate_random(session_.secrets.rs1,  32);
    kvz_rtp::crypto::random::generate_random(session_.secrets.rs2,  32);
    kvz_rtp::crypto::random::generate_random(session_.secrets.raux, 32);
    kvz_rtp::crypto::random::generate_random(session_.secrets.rpbx, 32);
}

void kvz_rtp::zrtp::generate_shared_secrets()
{
    cctx_.dh->set_remote_pk(session_.dh_ctx.remote_public, 384);
    cctx_.dh->get_shared_secret(session_.dh_ctx.dh_result, 384);

    /* Section 4.4.1.4, calculation of total_hash includes:
     *    - Hello   (responder)
     *    - Commit  (initator)
     *    - DHPart1 (responder)
     *    - DHPart2 (initator)
     */
    if (session_.role == INITIATOR) {
        cctx_.sha256->update((uint8_t *)session_.r_msg.hello.second,  session_.r_msg.hello.first);
        cctx_.sha256->update((uint8_t *)session_.l_msg.commit.second, session_.l_msg.commit.first);
        cctx_.sha256->update((uint8_t *)session_.r_msg.dh.second,     session_.r_msg.dh.first);
        cctx_.sha256->update((uint8_t *)session_.l_msg.dh.second,     session_.l_msg.dh.first);
    } else {
        cctx_.sha256->update((uint8_t *)session_.l_msg.hello.second,  session_.l_msg.hello.first);
        cctx_.sha256->update((uint8_t *)session_.r_msg.commit.second, session_.r_msg.commit.first);
        cctx_.sha256->update((uint8_t *)session_.l_msg.dh.second,     session_.l_msg.dh.first);
        cctx_.sha256->update((uint8_t *)session_.r_msg.dh.second,     session_.r_msg.dh.first);
    }
    cctx_.sha256->final((uint8_t *)session_.hash_ctx.total_hash);

    /* Finally calculate s0 which is considered to be the final keying material (Section 4.4.1.4)
     *
     * It consist of the following information:
     *    - counter (always 1)
     *    - DHResult (calculated above [get_shared_secret()])
     *    - "ZRTP-HMAC-KDF"
     *    - ZID of initiator
     *    - ZID of responder
     *    - total hash (calculated above)
     *    - len(s1) (0x0)
     *    - s1 (null)
     *    - len(s2) (0x0)
     *    - s2 (null)
     *    - len(s3) (0x0)
     *    - s3 (null)
     */
    uint32_t value  = htonl(0x1);
    const char *kdf = "ZRTP-HMAC-KDF";

    cctx_.sha256->update((uint8_t *)&value,                    sizeof(value));              /* counter */
    cctx_.sha256->update((uint8_t *)session_.dh_ctx.dh_result, sizeof(session_.dh_ctx.dh_result));
    cctx_.sha256->update((uint8_t *)kdf,                       13);

    if (session_.role == INITIATOR) {
        cctx_.sha256->update((uint8_t *)session_.o_zid, 12);
        cctx_.sha256->update((uint8_t *)session_.r_zid, 12);
    } else {
        cctx_.sha256->update((uint8_t *)session_.r_zid, 12);
        cctx_.sha256->update((uint8_t *)session_.o_zid, 12);
    }

    cctx_.sha256->update((uint8_t *)session_.hash_ctx.total_hash, sizeof(session_.hash_ctx.total_hash));

    value = 0;
    cctx_.sha256->update((uint8_t *)&value, sizeof(value)); /* len(s1) */
    cctx_.sha256->update((uint8_t *)&value, sizeof(value)); /* len(s2) */
    cctx_.sha256->update((uint8_t *)&value, sizeof(value)); /* len(s3) */

    /* Calculate digest for s0 and erase DHResult from memory as required by the spec */
    cctx_.sha256->final((uint8_t *)session_.secrets.s0);
    memset(session_.dh_ctx.dh_result, 0, sizeof(session_.dh_ctx.dh_result));

    /* Derive ZRTP Session Key and SAS hash */
    derive_key("ZRTP Session Key", 256, session_.key_ctx.zrtp_sess_key);
    derive_key("SAS",              256, session_.key_ctx.sas_hash); /* TODO: crc32? */

    /* Finally derive ZRTP keys and HMAC keys
     * which are used to encrypt and authenticate Confirm messages */
    derive_key("Initiator ZRTP key", 128, session_.key_ctx.zrtp_keyi);
    derive_key("Responder ZRTP key", 128, session_.key_ctx.zrtp_keyr);
    derive_key("Initiator HMAC key", 256, session_.key_ctx.hmac_keyi);
    derive_key("Responder HMAC key", 256, session_.key_ctx.hmac_keyr);
}

rtp_error_t kvz_rtp::zrtp::verify_hash(uint8_t *key, uint8_t *buf, size_t len, uint64_t mac)
{
    uint64_t truncated = 0;
    uint8_t full[32]   = { 0 };
    auto hmac_sha256   = kvz_rtp::crypto::hmac::sha256(key, 32);

    hmac_sha256.update((uint8_t *)buf, len);
    hmac_sha256.final(full);

    memcpy(&truncated, full, sizeof(uint64_t));

    return (mac == truncated) ? RTP_OK : RTP_INVALID_VALUE;
}

rtp_error_t kvz_rtp::zrtp::validate_session()
{
    /* Verify all MACs received from various messages in order starting from Hello message
     * Calculate HMAC-SHA256 over the saved message using H(i - 1) as the HMAC key and
     * compare the truncated hash against the hash was saved to the message */
    uint8_t hashes[4][32];
    memcpy(hashes[0], session_.hash_ctx.r_hash[0], 32);

    for (size_t i = 1; i < 4; ++i) {
        cctx_.sha256->update(hashes[i - 1], 32);
        cctx_.sha256->final(hashes[i]);
    }

    /* Hello message */
    if (RTP_INVALID_VALUE == verify_hash(
            (uint8_t *)hashes[2],
            (uint8_t *)session_.r_msg.hello.second,
            81,
            session_.hash_ctx.r_mac[3]
        ))
    {
        LOG_ERROR("Hash mismatch for Hello Message!");
        return RTP_INVALID_VALUE;
    }

    /* Check commit message only if our role is responder
     * because the initator might not have gotten a Commit message at all */
    if (session_.role == RESPONDER) {
        if (RTP_INVALID_VALUE == verify_hash(
                (uint8_t *)hashes[1],
                (uint8_t *)session_.r_msg.commit.second,
                session_.r_msg.commit.first - 8 - 4,
                session_.hash_ctx.r_mac[2]
            ))
        {
            LOG_ERROR("Hash mismatch for Commit Message!");
            return RTP_INVALID_VALUE;
        }
    }

    /* DHPart1/DHPart2 message */
    if (RTP_INVALID_VALUE == verify_hash(
            (uint8_t *)hashes[0],
            (uint8_t *)session_.r_msg.dh.second,
            session_.r_msg.dh.first - 8 - 4,
            session_.hash_ctx.r_mac[1]
        ))
    {
        LOG_ERROR("Hash mismatch for DHPart1/DHPart2 Message!");
        return RTP_INVALID_VALUE;
    }

    LOG_DEBUG("All hashes match!");
    return RTP_OK;
}

void kvz_rtp::zrtp::init_session_hashes()
{
    kvz_rtp::crypto::random::generate_random(session_.hash_ctx.o_hash[0], 32);

    for (size_t i = 1; i < 4; ++i) {
        cctx_.sha256->update(session_.hash_ctx.o_hash[i - 1], 32);
        cctx_.sha256->final(session_.hash_ctx.o_hash[i]);
    }
}

bool kvz_rtp::zrtp::are_we_initiator(uint8_t *our_hvi, uint8_t *their_hvi)
{
    for (int i = 31; i >= 0; --i) {

        if (our_hvi[i] > their_hvi[i])
            return true;

        else if (our_hvi[i] < their_hvi[i])
            return false;
    }

    /* never here? */
    return true;
}

rtp_error_t kvz_rtp::zrtp::begin_session()
{
    rtp_error_t ret = RTP_OK;
    auto hello      = kvz_rtp::zrtp_msg::hello(session_);
    auto hello_ack  = kvz_rtp::zrtp_msg::hello_ack();
    bool hello_recv = false;
    size_t rto      = 50;
    int type        = 0;
    int i           = 0;

    for (i = 0; i < 20; ++i) {
        set_timeout(rto);

        if ((ret = hello.send_msg(socket_, addr_)) != RTP_OK) {
            LOG_ERROR("Failed to send Hello message");
        }

        if ((type = receiver_.recv_msg(socket_, 0)) > 0) {
            /* We received something interesting, either Hello message from remote in which case
             * we need to send HelloACK message back and keep sending our Hello until HelloACK is received,
             * or HelloACK message which means we can stop sending our  */

            /* We received Hello message from remote, parse it and send  */
            if (type == ZRTP_FT_HELLO) {
                hello_ack.send_msg(socket_, addr_);

                if (!hello_recv) {
                    hello_recv = true;

                    /* Copy interesting information from receiver's
                     * message buffer to remote capabilities struct for later use */
                    hello.parse_msg(receiver_, session_);

                    if (session_.capabilities.version != ZRTP_VERSION) {

                        /* Section 4.1.1:
                         *
                         * "If an endpoint receives a Hello message with an unsupported
                         *  version number that is lower than the endpoint's current Hello
                         *  message, the endpoint MUST send an Error message (Section 5.9)
                         *  indicating failure to support this ZRTP version."
                         */
                        if (session_.capabilities.version < ZRTP_VERSION) {
                            LOG_ERROR("Remote supports version %d, kvzRTP supports %d. Session cannot continue!",
                                session_.capabilities.version, ZRTP_VERSION);

                            return RTP_NOT_SUPPORTED;
                        }

                        LOG_WARN("ZRTP Protocol version %u not supported, keep sending Hello Messages",
                                session_.capabilities.version);
                        hello_recv = false;
                    }
                }

            /* We received ACK for our Hello message.
             * Make sure we've received Hello message also before exiting */
            } else if (type == ZRTP_FT_HELLO_ACK) {
                if (hello_recv)
                    return RTP_OK;
            } else {
                /* at this point we do not care about other messages */
            }
        }

        if (rto < 200)
            rto *= 2;
    }

    /* Hello timed out, perhaps remote did not answer at all or it has an incompatible ZRTP version in use. */
    return RTP_TIMEOUT;
}

rtp_error_t kvz_rtp::zrtp::init_session()
{
    /* Create ZRTP session from capabilities struct we've constructed */
    session_.hash_algo          = S256;
    session_.cipher_algo        = AES1;
    session_.auth_tag_type      = HS32;
    session_.key_agreement_type = DH3k;
    session_.sas_type           = B32;

    int type        = 0;
    size_t rto      = 0;
    rtp_error_t ret = RTP_OK;
    auto commit     = kvz_rtp::zrtp_msg::commit(session_);

    /* First check if remote has already sent the message.
     * If so, they are the initiator and we're the responder */
    while ((type = receiver_.recv_msg(socket_, MSG_DONTWAIT)) != -EAGAIN) {
        if (type == ZRTP_FT_COMMIT) {
            commit.parse_msg(receiver_, session_);
            session_.role = RESPONDER;
            return RTP_OK;
        }
    }

    /* If we proceed to sending Commit message, we can assume we're the initiator.
     * This assumption may prove to be false if remote also sends Commit message
     * and Commit contention is resolved in their favor. */
    session_.role = INITIATOR;
    rto           = 150;

    for (int i = 0; i < 10; ++i) {
        set_timeout(rto);

        if ((ret = commit.send_msg(socket_, addr_)) != RTP_OK) {
            LOG_ERROR("Failed to send Commit message!");
        }

        if ((type = receiver_.recv_msg(socket_, 0)) > 0) {

            /* As per RFC 6189, if both parties have sent Commit message and the mode is DH,
             * hvi shall determine who is the initiator (the party with larger hvi is initiator) */
            if (type == ZRTP_FT_COMMIT) {
                commit.parse_msg(receiver_, session_);

                /* Our hvi is smaller than remote's meaning we are the responder.
                 *
                 * Commit message must be ACKed with DHPart1 messages so we need exit,
                 * construct that message and sent it to remote */
                if (!are_we_initiator(session_.hash_ctx.o_hvi, session_.hash_ctx.r_hvi)) {
                    session_.role = RESPONDER;
                    return RTP_OK;
                }
            } else if (type == ZRTP_FT_DH_PART1 || type == ZRTP_FT_CONFIRM1) {
                return RTP_OK;
            }
        }

        if (rto < 1200)
            rto *= 2;
    }

    /* Remote didn't send us any messages, it can be considered dead
     * and ZRTP cannot thus continue any further */
    return RTP_TIMEOUT;
}

rtp_error_t kvz_rtp::zrtp::dh_part1()
{
    rtp_error_t ret = RTP_OK;
    auto dhpart     = kvz_rtp::zrtp_msg::dh_key_exchange(session_, 1);
    size_t rto      = 150;
    int type        = 0;

    for (int i = 0; i < 10; ++i) {
        set_timeout(rto);

        if ((ret = dhpart.send_msg(socket_, addr_)) != RTP_OK) {
            LOG_ERROR("Failed to send DHPart1 Message!");
        }

        if ((type = receiver_.recv_msg(socket_, 0)) > 0) {
            if (type == ZRTP_FT_DH_PART2) {
                if ((ret = dhpart.parse_msg(receiver_, session_)) != RTP_OK) {
                    LOG_ERROR("Failed to parse DHPart2 Message!");
                    continue;
                }
                LOG_DEBUG("DHPart2 received and parse successfully!");

                /* parse_msg() above extracted the public key of remote and saved it to session_.
                 * Now we must generate shared secrets (DHResult, total_hash, and s0) */
                generate_shared_secrets();

                return RTP_OK;
            }
        }

        if (rto < 1200)
            rto *= 2;
    }

    return RTP_TIMEOUT;
}

rtp_error_t kvz_rtp::zrtp::dh_part2()
{
    int type        = 0;
    size_t rto      = 0;
    rtp_error_t ret = RTP_OK;
    auto dhpart     = kvz_rtp::zrtp_msg::dh_key_exchange(session_, 2);

    if ((ret = dhpart.parse_msg(receiver_, session_)) != RTP_OK) {
        LOG_ERROR("Failed to parse DHPart1 Message!");
        return RTP_INVALID_VALUE;
    }

    /* parse_msg() above extracted the public key of remote and saved it to session_.
     * Now we must generate shared secrets (DHResult, total_hash, and s0) */
    generate_shared_secrets();

    for (int i = 0; i < 10; ++i) {
        set_timeout(rto);

        if ((ret = dhpart.send_msg(socket_, addr_)) != RTP_OK) {
            LOG_ERROR("Failed to send DHPart2 Message!");
        }

        if ((type = receiver_.recv_msg(socket_, 0)) > 0) {
            if (type == ZRTP_FT_CONFIRM1) {
                LOG_DEBUG("Confirm1 Message received");
                return RTP_OK;
            }
        }

        if (rto < 1200)
            rto *= 2;
    }

    return RTP_TIMEOUT;
}

rtp_error_t kvz_rtp::zrtp::responder_finalize_session()
{
    rtp_error_t ret = RTP_OK;
    auto confirm    = kvz_rtp::zrtp_msg::confirm(session_, 1);
    auto confack    = kvz_rtp::zrtp_msg::confack(session_);
    size_t rto      = 150;
    int type        = 0;

    for (int i = 0; i < 10; ++i) {
        set_timeout(rto);

        if ((ret = confirm.send_msg(socket_, addr_)) != RTP_OK) {
            LOG_ERROR("Failed to send Confirm1 Message!");
        }

        if ((type = receiver_.recv_msg(socket_, 0)) > 0) {
            if (type == ZRTP_FT_CONFIRM2) {
                if ((ret = confirm.parse_msg(receiver_, session_)) != RTP_OK) {
                    LOG_ERROR("Failed to parse Confirm2 Message!");
                    continue;
                }

                if ((ret = validate_session()) != RTP_OK) {
                    LOG_ERROR("Mismatch on one of the received MACs/Hashes, session cannot continue");
                    return RTP_INVALID_VALUE;
                }

                /* TODO: send in a loop? */
                confack.send_msg(socket_, addr_);
                return RTP_OK;
            }
        }

        if (rto < 1200)
            rto *= 2;
    }

    return RTP_TIMEOUT;
}

rtp_error_t kvz_rtp::zrtp::initiator_finalize_session()
{
    rtp_error_t ret = RTP_OK;
    auto confirm    = kvz_rtp::zrtp_msg::confirm(session_, 2);
    size_t rto      = 150;
    int type        = 0;

    if ((ret = confirm.parse_msg(receiver_, session_)) != RTP_OK) {
        LOG_ERROR("Failed to parse Confirm1 Message!");
        return RTP_INVALID_VALUE;
    }

    if ((ret = validate_session()) != RTP_OK) {
        LOG_ERROR("Mismatch on one of the received MACs/Hashes, session cannot continue");
        return RTP_INVALID_VALUE;
    }

    for (int i = 0; i < 10; ++i) {
        set_timeout(rto);

        if ((ret = confirm.send_msg(socket_, addr_)) != RTP_OK) {
            LOG_ERROR("Failed to send Confirm2 Message!");
        }

        if ((type = receiver_.recv_msg(socket_, 0)) > 0) {
            if (type == ZRTP_FT_CONF2_ACK) {
                LOG_DEBUG("Conf2ACK received successfully!");
                return RTP_OK;
            }
        }

        if (rto < 1200)
            rto *= 2;
    }

    return RTP_TIMEOUT;
}

rtp_error_t kvz_rtp::zrtp::init(uint32_t ssrc, socket_t& socket, sockaddr_in& addr)
{
    if (!initialized_)
        return init_dhm(ssrc, socket, addr);
    return init_msm(ssrc, socket, addr);
}

rtp_error_t kvz_rtp::zrtp::init_dhm(uint32_t ssrc, socket_t& socket, sockaddr_in& addr)
{
    rtp_error_t ret = RTP_OK;

    /* TODO: set all fields initially to zero */
    memset(session_.hash_ctx.o_hvi, 0, sizeof(session_.hash_ctx.o_hvi));

    /* Generate ZID and random data for the retained secrets */
    generate_zid();
    generate_secrets();

    /* Initialize the session hashes H0 - H3 defined in Section 9 of RFC 6189 */
    init_session_hashes();

    socket_ = socket;
    addr_   = addr;

    session_.seq  = 0;
    session_.ssrc = ssrc;

    /* Begin session by exchanging Hello and HelloACK messages.
     *
     * After begin_session() we know what remote is capable of
     * and whether we are compatible implementations */
    if ((ret = begin_session()) != RTP_OK) {
        LOG_ERROR("Session initialization failed, ZRTP cannot be used!");
        return ret;
    }

    /* After begin_session() we have remote's Hello message and we can craft
     * DHPart2 in the hopes that we're the Initiator.
     *
     * If this assumption proves to be false, we just discard the message
     * and create DHPart1.
     *
     * Commit message contains hash value of initiator (hvi) which is the
     * the hashed value of Initiators DHPart2 message and Responder's Hello
     * message. This should be calculated now because the next step is choosing
     * the the roles for participants. */
    auto dh_msg = kvz_rtp::zrtp_msg::dh_key_exchange(session_, 2);

    cctx_.sha256->update((uint8_t *)session_.l_msg.dh.second,    session_.l_msg.dh.first);
    cctx_.sha256->update((uint8_t *)session_.r_msg.hello.second, session_.r_msg.hello.first);
    cctx_.sha256->final((uint8_t *)session_.hash_ctx.o_hvi);

    /* We're here which means that remote respond to us and sent a Hello message
     * with same version number as ours. This means that the implementations are
     * compatible with each other and we can start the actual negotiation
     *
     * Both participants create Commit messages which include the used algorithms
     * etc. used during the session + some extra information such as ZID
     *
     * init_session() will exchange the Commit messages and select roles for the
     * participants (initiator/responder) based on rules determined in RFC 6189 */
    if ((ret = init_session()) != RTP_OK) {
        LOG_ERROR("Could not agree on ZRTP session parameters or roles of participants!");
        return ret;
    }

    /* From this point on, the execution deviates because both parties have their own roles
     * and different message that they need to send in order to finalize the ZRTP connection */
    if (session_.role == INITIATOR) {
        if ((ret = dh_part2()) != RTP_OK) {
            LOG_ERROR("Failed to perform Diffie-Hellman key exchange Part2");
            return ret;
        }

        if ((ret = initiator_finalize_session()) != RTP_OK) {
            LOG_ERROR("Failed to finalize session using Confirm2");
            return ret;
        }

    } else {
        if ((ret = dh_part1()) != RTP_OK) {
            LOG_ERROR("Failed to perform Diffie-Hellman key exchange Part1");
            return ret;
        }

        if ((ret = responder_finalize_session()) != RTP_OK) {
            LOG_ERROR("Failed to finalize session using Confirm1/Conf2ACK");
            return ret;
        }
    }

    /* ZRTP has been initialized using DHMode */
    initialized_ = true;

    /* reset the timeout (no longer needed) */
    set_timeout(0);

    /* Session has been initialized successfully and SRTP can start */
    return RTP_OK;
}

rtp_error_t kvz_rtp::zrtp::init_msm(uint32_t ssrc, socket_t& socket, sockaddr_in& addr)
{
    (void)ssrc, (void)socket, (void)addr;

    LOG_WARN("not implemented!");

    return RTP_TIMEOUT;
}
#endif