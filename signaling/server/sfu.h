#ifndef SIGNALING_SFU_H
#define SIGNALING_SFU_H

#include "meeting.h"
#include "signaling/signaling_msg.h"

typedef struct signaling_sfu signaling_sfu_t;

signaling_sfu_t *signaling_sfu_create(meeting_manager_t *manager, const char *listen_ip,
                                      const char *announced_ip, uint16_t port);
/* Offers update existing sessions: append tracks or change direction/restart
 * ICE without replacing DTLS/SCTP. Existing MID order, codec/PT and SCTP port
 * stay fixed; remove tracks using inactive m-sections. Publisher TWCC uses
 * one stable video extension ID (audio is included only with that same ID).
 * Valid TCP ICE alternatives are ineligible for the UDP-only listeners;
 * malformed candidates and unknown transports still fail explicitly.
 * Downstream bitrate allocation is deliberately not implemented. */
int signaling_sfu_handle(signaling_sfu_t *sfu, meeting_peer_t *peer, const sig_msg_t *message);
void signaling_sfu_leave(signaling_sfu_t *sfu, meeting_peer_t *peer);
void signaling_sfu_destroy(signaling_sfu_t *sfu);

#endif
