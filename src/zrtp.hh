#pragma once

#ifdef _WIN32
#include <winsock2.h>
#include <mswsock.h>
#include <inaddr.h>
#else
#include <netinet/ip.h>
#include <arpa/inet.h>
#endif

#include <vector>

#include "util.hh"
#include "mzrtp/receiver.hh"

namespace kvz_rtp {

    /* TODO: move to defines.hh */
    typedef struct capabilities {

        /* Supported ZRTP version */
        uint32_t version;

        /* ZID of this ZRTP instance */
        uint8_t *zid;

        /* Header of the supported algos etc. */
        uint32_t header;

        /* Supported hash algorithms (empty for us) */
        std::vector<uint32_t> hash_algos;

        /* Supported cipher algorithms (empty for us) */
        std::vector<uint32_t> cipher_algos;

        /* Supported authentication tag types (empty for us) */
        std::vector<uint32_t> auth_tags;

        /* Supported Key Agreement types (empty for us) */
        std::vector<uint32_t> key_agreements;

        /* Supported SAS types (empty for us) */
        std::vector<uint32_t> sas_types;
    } zrtp_capab_t;

    /* DH exchange related information */
    typedef struct zrtp_dh {
        uint32_t retained1[2]; /* hash of retained shared secret 1 */
        uint32_t retained2[2]; /* hash of retained shared secret 2 */

        uint32_t aux_secret[2]; /* hash of auxiliary secret */
        uint32_t pbx_secret[2]; /* hash of MiTM PBX secret */
    } zrtp_dh_t;

    /* Collection of algorithms that are used by ZRTP
     * (based on information gathered from Hello message) */
    typedef struct zrtp_session {
        uint32_t hash_algo;
        uint32_t cipher_algo;
        uint32_t auth_tag_type;
        uint32_t key_agreement_type;
        uint32_t sas_type;
        uint32_t hvi[8];

        /* Section 9 of RFC 6189 */
        uint8_t hashes[4][32];

        zrtp_dh_t us;
        zrtp_dh_t them;
    } zrtp_session_t;

    typedef struct zrtp_crypto_ctx {
        kvz_rtp::crypto::sha256 sha256;
    } zrtp_crypto_ctx_t;

    class zrtp {
        public:
            zrtp();
            ~zrtp();

            /* Initialize ZRTP session between us and remote
             *
             * Return RTP_OK on success
             * Return RTP_TIMEOUT if remote did not send messages in timely manner */
            rtp_error_t init(uint32_t ssrc, socket_t& socket, sockaddr_in& addr);

        private:
            /* Set timeout for a socket, needed by backoff timers of ZRTP
             *
             * "timeout" tells the timeout in milliseconds
             *
             * Return RTP_OK on success
             * Return RTP_GENERIC_ERROR if timeout could not be set */
            rtp_error_t set_timeout(size_t timeout);

            /* Get our own capabilities, see struct capabilities above for more details */
            zrtp_capab_t get_capabilities();

            /* Generate zid for this ZRTP instance. ZID is a unique, 96-bit long ID */
            void generate_zid();

            /* Initialize the four session hashes defined in Section 9 of RFC 6189 */
            void init_session_hashes();

            /* Being the ZRTP session by sending a Hello message to remote,
             * and responding to remote's Hello message using HelloAck message
             *
             * If session begins successfully, remote zrtp_capab_t are put into
             * "remote_capab" for later use
             *
             * Return RTP_OK on success
             * Return RTP_NOT_SUPPORTED if remote did not answer to our Hello messages */
            rtp_error_t begin_session();

            /* Select algorithms used by the session, exchange this information with remote
             * and based on Commit messages, select roles for the participants (initiator/responder)
             *
             * Return RTP_OK on success
             * Return RTP_TIMEOUT if no message is received from remote before T2 expires */
            rtp_error_t init_session(bool& initiator);

            /* Perform Diffie-Hellman key exchange Part1 (responder)
             * This message also acts as an ACK to Commit message */
            rtp_error_t dh_part1();

            /* Perform Diffie-Hellman key exchange Part2 (initiator)
             * This message also acts as an ACK to DHPart1 message
             *
             * Return RTP_OK if DHPart2 was successful
             * Return RTP_TIMEOUT if no message is received from remote before T2 expires */
            rtp_error_t dh_part2();

            /* TODO:  */
            rtp_error_t responder_finalize_session();

            /* TODO:  */
            rtp_error_t initiator_finalize_session();

            uint32_t ssrc_;
            socket_t socket_;
            sockaddr_in addr_;

            /* Our own and remote capability structs */
            zrtp_capab_t capab_;
            zrtp_capab_t rcapab_;

            kvz_rtp::zrtp_msg::receiver receiver_;

            /* Initialized after Hello messages have been exchanged */
            zrtp_session_t session_;

            uint8_t *zid_;

            zrtp_crypto_ctx_t cctx_;
    };
};
