#include "rtc/rtc_rtcp.h"
#include "rtc/rtc_rtp.h"
#include "rtc/rtc_rtp_ext.h"
#include "rtc/rtc_sdp.h"
#include "rtc_stun.h"
#include "rtc_turn.h"

#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (!size || size > SDP_MAX_SIZE)
        return 0;
    uint8_t parser = data[0] % 6;
    data++;
    size--;
    if (parser == 0) {
        rtc_stun_msg_t message;
        (void)rtc_stun_parse(&message, data, size);
    } else if (parser == 1) {
        rtc_rtp_packet_t packet;
        if (rtc_rtp_parse(&packet, data, size) == RTC_OK && packet.ext_data) {
            char mid[32];
            rtc_rtp_ext_t extensions[16];
            size_t count = 16;
            if (rtc_rtp_ext_parse_body(packet.ext_data, packet.ext_len, extensions, &count) ==
                RTC_OK)
                for (size_t i = 0; i < count; i++)
                    (void)rtc_rtp_ext_read_transport_cc(&extensions[i]);
            for (uint8_t id = 1; id < 15; id++) {
                (void)rtc_rtp_ext_get_string(packet.ext_data, packet.ext_len, id, mid, sizeof(mid));
            }
        }
    } else if (parser == 2) {
        rtc_rtcp_packet_t packet;
        rtc_rtcp_nack_t nack;
        rtc_rtcp_pli_t pli;
        rtc_rtcp_fir_t fir;
        rtc_rtcp_twcc_t twcc;
        rtc_rtcp_remb_t remb;
        rtc_rtcp_bye_t bye;
        (void)rtc_rtcp_parse(&packet, data, size);
        (void)rtc_rtcp_parse_nack(&nack, data, size);
        (void)rtc_rtcp_parse_pli(&pli, data, size);
        (void)rtc_rtcp_parse_fir(&fir, data, size);
        (void)rtc_rtcp_parse_twcc(&twcc, data, size);
        (void)rtc_rtcp_parse_remb(&remb, data, size);
        (void)rtc_rtcp_parse_bye(&bye, data, size);
        (void)rtc_rtcp_validate_compound(data, size);
    } else if (parser == 5) {
        size_t length;
        (void)rtc_turn_stream_frame_length(data, size, &length);
    } else {
        char text[SDP_MAX_SIZE];
        if (size >= sizeof(text))
            return 0;
        memcpy(text, data, size);
        text[size] = '\0';
        if (parser == 3) {
            rtc_sdp_t sdp = {0};
            (void)rtc_sdp_parse(&sdp, text, size);
            rtc_sdp_close(&sdp);
        } else {
            rtc_ice_candidate_t candidate;
            (void)rtc_sdp_parse_candidate_line(text, &candidate);
        }
    }
    return 0;
}
