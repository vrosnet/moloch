/* packet.c  -- Functions for acquiring data
 *
 * Copyright 2012-2016 AOL Inc. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this Software except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "moloch.h"
#include <inttypes.h>
#include <arpa/inet.h>

/******************************************************************************/
extern MolochConfig_t        config;

MolochPcapFileHdr_t          pcapFileHeader;

uint64_t                     totalPackets = 0;
uint64_t                     totalBytes = 0;
uint64_t                     totalSessions = 0;

static uint32_t              initialDropped = 0;
struct timeval               initialPacket;

extern void                 *esServer;
extern uint32_t              pluginsCbs;

static int                   mac1Field;
static int                   mac2Field;
static int                   vlanField;
static int                   greIpField;

time_t                       lastPacketSecs[MOLOCH_MAX_PACKET_THREADS];

/******************************************************************************/
extern MolochSessionHead_t   tcpWriteQ[MOLOCH_MAX_PACKET_THREADS];

static MolochPacketHead_t    packetQ[MOLOCH_MAX_PACKET_THREADS];


int moloch_packet_ip4(MolochPacket_t * const packet, const uint8_t *data, int len);

/******************************************************************************/
void moloch_packet_free(MolochPacket_t *packet)
{
    g_free(packet->pkt);
    MOLOCH_TYPE_FREE(MolochPacket_t, packet);
}
/******************************************************************************/
void moloch_packet_tcp_free(MolochSession_t *session)
{
    MolochTcpData_t *td;
    while (DLL_POP_HEAD(td_, &session->tcpData, td)) {
        moloch_packet_free(td->packet);
        MOLOCH_TYPE_FREE(MolochTcpData_t, td);
    }
}
/******************************************************************************/
// Idea from gopacket tcpassembly/assemply.go
LOCAL int32_t moloch_packet_sequence_diff (uint32_t a, uint32_t b)
{
    if (a > 0xc0000000 && b < 0x40000000)
        return (a + 0xffffffffLL - b);

    if (b > 0xc0000000 && a < 0x40000000)
        return (a - b - 0xffffffffLL);

    return b - a;
}
/******************************************************************************/
void moloch_packet_tcp_finish(MolochSession_t *session)
{
    MolochTcpData_t            *ftd;
    MolochTcpData_t            *next;

    MolochTcpDataHead_t * const tcpData = &session->tcpData;

    DLL_FOREACH_REMOVABLE(td_, tcpData, ftd, next) {
        const int which = ftd->packet->direction;
        const uint32_t tcpSeq = session->tcpSeq[which];

        if (tcpSeq >= ftd->seq && tcpSeq < (ftd->seq + ftd->len)) {
            const int offset = tcpSeq - ftd->seq;
            const uint8_t *data = ftd->packet->pkt + ftd->dataOffset + offset;
            const int len = ftd->len - offset;

            if (session->firstBytesLen[which] < 8) {
                int copy = MIN(8 - session->firstBytesLen[which], len);
                memcpy(session->firstBytes[which] + session->firstBytesLen[which], data, copy);
                session->firstBytesLen[which] += copy;
            }

            if (session->totalDatabytes[which] == session->consumed[which])  {
                moloch_parsers_classify_tcp(session, data, len, which);
            }

            int i;
            int totConsumed = 0;
            int consumed = 0;

            for (i = 0; i < session->parserNum; i++) {
                if (session->parserInfo[i].parserFunc) {
                    consumed = session->parserInfo[i].parserFunc(session, session->parserInfo[i].uw, data, len, which);
                    if (consumed) {
                        totConsumed += consumed;
                        session->consumed[which] += consumed;
                    }

                    if (consumed >= len)
                        break;
                }
            }
            session->tcpSeq[which] += len;
            session->databytes[which] += len;
            session->totalDatabytes[which] += len;

            if (config.yara) {
                moloch_yara_execute(session, data, len, 0);
            }

            DLL_REMOVE(td_, tcpData, ftd);
            moloch_packet_free(ftd->packet);
            MOLOCH_TYPE_FREE(MolochTcpData_t, ftd);
        } else {
            return;
        }
    }
}

/******************************************************************************/
void moloch_packet_process_icmp(MolochSession_t * const UNUSED(session), MolochPacket_t * const UNUSED(packet))
{
}
/******************************************************************************/
void moloch_packet_process_udp(MolochSession_t * const session, MolochPacket_t * const packet)
{
    const uint8_t *data = packet->pkt + packet->payloadOffset + 8;
    int            len = packet->payloadLen - 8;

    if (len <= 0)
        return;

    if (session->firstBytesLen[packet->direction] == 0) {
        session->firstBytesLen[packet->direction] = MIN(8, len);
        memcpy(session->firstBytes[packet->direction], data, session->firstBytesLen[packet->direction]);

        if (!session->stopSPI)
            moloch_parsers_classify_udp(session, data, len, packet->direction);
    }
}
/******************************************************************************/
int moloch_packet_process_tcp(MolochSession_t * const session, MolochPacket_t * const packet)
{
    if (session->stopSPI || session->stopTCP)
        return 1;


    struct tcphdr       *tcphdr = (struct tcphdr *)(packet->pkt + packet->payloadOffset);


    int            len = packet->payloadLen - 4*tcphdr->th_off;

    const uint32_t seq = ntohl(tcphdr->th_seq);

    if (len < 0)
        return 1;

    if (tcphdr->th_flags & TH_SYN) {
        session->haveTcpSession = 1;
        session->tcpSeq[packet->direction] = seq + 1;
        if (!session->tcp_next) {
            DLL_PUSH_TAIL(tcp_, &tcpWriteQ[session->thread], session);
        }
        return 1;
    }

    if (tcphdr->th_flags & TH_RST) {
        if (moloch_packet_sequence_diff(seq, session->tcpSeq[packet->direction]) <= 0) {
            return 1;
        }

        session->tcpState[packet->direction] = MOLOCH_TCP_STATE_FIN_ACK;
    }

    if (tcphdr->th_flags & TH_FIN) {
        session->tcpState[packet->direction] = MOLOCH_TCP_STATE_FIN;
    }

    MolochTcpDataHead_t * const tcpData = &session->tcpData;

    if (DLL_COUNT(td_, tcpData) > 256) {
        moloch_packet_tcp_free(session);
        moloch_session_add_tag(session, "incomplete-tcp");
        session->stopTCP = 1;
        return 1;
    }

    if (tcphdr->th_flags & (TH_ACK | TH_RST)) {
        int owhich = (packet->direction + 1) & 1;
        if (session->tcpState[owhich] == MOLOCH_TCP_STATE_FIN) {
            session->tcpState[owhich] = MOLOCH_TCP_STATE_FIN_ACK;
            if (session->tcpState[packet->direction] == MOLOCH_TCP_STATE_FIN_ACK) {

                if (!session->closingQ) {
                    moloch_session_mark_for_close(session, SESSION_TCP);
                }
                return 1;
            }
        }
    }

    // Empty packet, drop from tcp processing
    if (len <= 0 || tcphdr->th_flags & TH_RST)
        return 1;

    // This packet is before what we are processing
    int32_t diff = moloch_packet_sequence_diff(session->tcpSeq[packet->direction], seq + len);
    if (diff <= 0)
        return 1;

    MolochTcpData_t *ftd, *td = MOLOCH_TYPE_ALLOC(MolochTcpData_t);
    const uint32_t ack = ntohl(tcphdr->th_ack);

    td->packet = packet;
    td->ack = ack;
    td->seq = seq;
    td->len = len;
    td->dataOffset = packet->payloadOffset + 4*tcphdr->th_off;

    if (DLL_COUNT(td_, tcpData) == 0) {
        DLL_PUSH_TAIL(td_, tcpData, td);
    } else {
        uint32_t sortA, sortB;
        DLL_FOREACH_REVERSE(td_, tcpData, ftd) {
            if (packet->direction == ftd->packet->direction) {
                sortA = seq;
                sortB = ftd->seq;
            } else {
                sortA = seq;
                sortB = ftd->ack;
            }

            diff = moloch_packet_sequence_diff(sortB, sortA);
            if (diff == 0) {
                if (packet->direction == ftd->packet->direction) {
                    if (td->len > ftd->len) {
                        DLL_ADD_AFTER(td_, tcpData, ftd, td);

                        DLL_REMOVE(td_, tcpData, ftd);
                        moloch_packet_free(ftd->packet);
                        MOLOCH_TYPE_FREE(MolochTcpData_t, ftd);
                        ftd = td;
                    } else {
                        MOLOCH_TYPE_FREE(MolochTcpData_t, td);
                        return 1;
                    }
                    break;
                } else if (moloch_packet_sequence_diff(ack, ftd->seq) < 0) {
                    DLL_ADD_AFTER(td_, tcpData, ftd, td);
                    break;
                }
            } else if (diff > 0) {
                DLL_ADD_AFTER(td_, tcpData, ftd, td);
                break;
            }
        }
        if ((void*)ftd == (void*)tcpData) {
            DLL_PUSH_HEAD(td_, tcpData, td);
        }
    }

    return 0;
}

/******************************************************************************/
void moloch_packet_thread_wake(int thread)
{
    MOLOCH_COND_BROADCAST(packetQ[thread].lock);
}
/******************************************************************************/
/* Only called on main thread, we busy block until all packet threads are empty.
 * Should only be used by tests and at end
 */
void moloch_packet_flush()
{
    int flushed = 0;
    int t;
    while (!flushed) {
        flushed = 1;
        for (t = 0; t < config.packetThreads; t++) {
            MOLOCH_LOCK(packetQ[t].lock);
            if (DLL_COUNT(packet_, &packetQ[t]) > 0) {
                flushed = 0;
            }
            MOLOCH_UNLOCK(packetQ[t].lock);
            usleep(10000);
        }
    }
}
/******************************************************************************/
LOCAL void *moloch_packet_thread(void *threadp)
{
    MolochPacket_t  *packet;
    int thread = (long)threadp;

    while (1) {
        MOLOCH_LOCK(packetQ[thread].lock);
        if (DLL_COUNT(packet_, &packetQ[thread]) == 0) {
            MOLOCH_COND_WAIT(packetQ[thread].lock);
        }
        DLL_POP_HEAD(packet_, &packetQ[thread], packet);
        MOLOCH_UNLOCK(packetQ[thread].lock);

        moloch_session_process_commands(thread);

        if (!packet)
            continue;

        lastPacketSecs[thread] = packet->ts.tv_sec;

        MolochSession_t     *session;
        struct ip           *ip4 = (struct ip*)(packet->pkt + packet->ipOffset);
        struct ip6_hdr      *ip6 = (struct ip6_hdr*)(packet->pkt + packet->ipOffset);
        struct tcphdr       *tcphdr = 0;
        struct udphdr       *udphdr = 0;

        int isNew;
        session = moloch_session_find_or_create(packet->ses, packet->sessionId, &isNew); // Returns locked session

        if (isNew) {
            session->saveTime = packet->ts.tv_sec + config.tcpSaveTimeout;
            session->firstPacket = packet->ts;

            if (ip4->ip_v == 4) {
                session->protocol = ip4->ip_p;
                ((uint32_t *)session->addr1.s6_addr)[2] = htonl(0xffff);
                ((uint32_t *)session->addr1.s6_addr)[3] = ip4->ip_src.s_addr;
                ((uint32_t *)session->addr2.s6_addr)[2] = htonl(0xffff);
                ((uint32_t *)session->addr2.s6_addr)[3] = ip4->ip_dst.s_addr;
                session->ip_tos = ip4->ip_tos;
            } else {
                session->protocol = ip6->ip6_nxt;
                session->addr1 = ip6->ip6_src;
                session->addr2 = ip6->ip6_dst;
                session->ip_tos = ip4->ip_tos;
            }
            session->thread = thread;

            moloch_parsers_initial_tag(session);

            switch (session->protocol) {
            case IPPROTO_TCP:
                tcphdr = (struct tcphdr *)(packet->pkt + packet->payloadOffset);
               /* If antiSynDrop option is set to true, capture will assume that
                *if the syn-ack ip4 was captured first then the syn probably got dropped.*/
                if ((tcphdr->th_flags & TH_SYN) && (tcphdr->th_flags & TH_ACK) && (config.antiSynDrop)) {
                    struct in6_addr tmp;
                    tmp = session->addr1;
                    session->addr1 = session->addr2;
                    session->addr2 = tmp;
                    session->port1 = ntohs(tcphdr->th_dport);
                    session->port2 = ntohs(tcphdr->th_sport);
                } else {
                    session->port1 = ntohs(tcphdr->th_sport);
                    session->port2 = ntohs(tcphdr->th_dport);
                }
                if (moloch_http_is_moloch(session->h_hash, packet->sessionId)) {
                    if (config.debug) {
                        char buf[1000];
                        LOG("Ignoring connection %s", moloch_session_id_string(session->sessionId, buf));
                    }
                    session->stopSPI = 1;
                    session->stopSaving = 1;
                }
                break;
            case IPPROTO_UDP:
                udphdr = (struct udphdr *)(packet->pkt + packet->payloadOffset);
                session->port1 = ntohs(udphdr->uh_sport);
                session->port2 = ntohs(udphdr->uh_dport);
                break;
            case IPPROTO_ICMP:
                break;
            }

            if (pluginsCbs & MOLOCH_PLUGIN_NEW)
                moloch_plugins_cb_new(session);
        }

        int dir;
        if (ip4->ip_v == 4) {
            dir = (MOLOCH_V6_TO_V4(session->addr1) == ip4->ip_src.s_addr &&
                   MOLOCH_V6_TO_V4(session->addr2) == ip4->ip_dst.s_addr);
        } else {
            dir = (memcmp(session->addr1.s6_addr, ip6->ip6_src.s6_addr, 16) == 0 &&
                   memcmp(session->addr2.s6_addr, ip6->ip6_dst.s6_addr, 16) == 0);
        }

        packet->direction = 0;
        switch (session->protocol) {
        case IPPROTO_UDP:
            udphdr = (struct udphdr *)(packet->pkt + packet->payloadOffset);
            packet->direction = (dir &&
                                 session->port1 == ntohs(udphdr->uh_sport) &&
                                 session->port2 == ntohs(udphdr->uh_dport))?0:1;
            session->databytes[packet->direction] += (packet->pktlen - 8);
            break;
        case IPPROTO_TCP:
            tcphdr = (struct tcphdr *)(packet->pkt + packet->payloadOffset);
            packet->direction = (dir &&
                                 session->port1 == ntohs(tcphdr->th_sport) &&
                                 session->port2 == ntohs(tcphdr->th_dport))?0:1;
            session->tcp_flags |= tcphdr->th_flags;
            break;
        case IPPROTO_ICMP:
            packet->direction = (dir)?0:1;
            break;
        }

        /* Check if the stop saving bpf filters match */
        if (session->packets[packet->direction] == 0 && session->stopSaving == 0 && config.dontSaveBPFsNum) {
            int i = moloch_reader_should_filter(packet);
            if (i >= 0)
                session->stopSaving = config.dontSaveBPFsStop[i];
        }

        session->packets[packet->direction]++;
        session->bytes[packet->direction] += packet->pktlen;
        session->lastPacket = packet->ts;

        uint32_t packets = session->packets[0] + session->packets[1];

        if (session->stopSaving == 0 || packets < session->stopSaving) {
            moloch_writer_write(packet);

            int16_t len;
            if (session->lastFileNum != packet->writerFileNum) {
                session->lastFileNum = packet->writerFileNum;
                g_array_append_val(session->fileNumArray, packet->writerFileNum);
                int64_t pos = -1LL * packet->writerFileNum;
                g_array_append_val(session->filePosArray, pos);
                len = 0;
                g_array_append_val(session->fileLenArray, len);
            }

            g_array_append_val(session->filePosArray, packet->writerFilePos);
            len = 16 + packet->pktlen;
            g_array_append_val(session->fileLenArray, len);

            if (packets >= config.maxPackets) {
                moloch_session_mid_save(session, packet->ts.tv_sec);
            }
        }

        if (pcapFileHeader.linktype == 1 && session->firstBytesLen[packet->direction] < 8) {
            const uint8_t *pcapData = packet->pkt;
            char str1[20];
            char str2[20];
            snprintf(str1, sizeof(str1), "%02x:%02x:%02x:%02x:%02x:%02x",
                    pcapData[0],
                    pcapData[1],
                    pcapData[2],
                    pcapData[3],
                    pcapData[4],
                    pcapData[5]);


            snprintf(str2, sizeof(str2), "%02x:%02x:%02x:%02x:%02x:%02x",
                    pcapData[6],
                    pcapData[7],
                    pcapData[8],
                    pcapData[9],
                    pcapData[10],
                    pcapData[11]);

            if (packet->direction == 1) {
                moloch_field_string_add(mac1Field, session, str1, 17, TRUE);
                moloch_field_string_add(mac2Field, session, str2, 17, TRUE);
            } else {
                moloch_field_string_add(mac1Field, session, str2, 17, TRUE);
                moloch_field_string_add(mac2Field, session, str1, 17, TRUE);
            }

            int n = 12;
            while (pcapData[n] == 0x81 && pcapData[n+1] == 0x00) {
                uint16_t vlan = ((uint16_t)(pcapData[n+2] << 8 | pcapData[n+3])) & 0xfff;
                moloch_field_int_add(vlanField, session, vlan);
                n += 4;
            }
        }

        int freePacket = 1;
        switch(packet->ses) {
        case SESSION_ICMP:
            moloch_packet_process_icmp(session, packet);
            break;
        case SESSION_UDP:
            moloch_packet_process_udp(session, packet);
            break;
        case SESSION_TCP:
            freePacket = moloch_packet_process_tcp(session, packet);
            moloch_packet_tcp_finish(session);
            break;
        }

        if (freePacket) {
            moloch_packet_free(packet);
        }
    }

    return NULL;
}

#ifdef REDOAGAIN
/******************************************************************************/
void moloch_packet_gre4(MolochPacket_t * const packet, const struct ip *ip4, const uint8_t *data, int len)
{
    BSB bsb;

    if (len < 4)
        return;

    BSB_INIT(bsb, data, len);
    uint16_t flags_version = 0;
    BSB_IMPORT_u16(bsb, flags_version);
    uint16_t type = 0;
    BSB_IMPORT_u16(bsb, type);

    if (type != 0x0800) {
        if (config.logUnknownProtocols)
            LOG("Unknown GRE protocol 0x%04x(%d)", type, type);
        return;
    }

    uint16_t offset = 0;

    if (flags_version & (0x8000 | 0x4000)) {
        BSB_IMPORT_skip(bsb, 2);
        BSB_IMPORT_u16(bsb, offset);
    }

    // key
    if (flags_version & 0x2000) {
        BSB_IMPORT_skip(bsb, 4);
    }

    // sequence number
    if (flags_version & 0x1000) {
        BSB_IMPORT_skip(bsb, 4);
    }

    // routing
    if (flags_version & 0x4000) {
        while (BSB_NOT_ERROR(bsb)) {
            BSB_IMPORT_skip(bsb, 3);
            int len = 0;
            BSB_IMPORT_u08(bsb, len);
            if (len == 0)
                break;
            BSB_IMPORT_skip(bsb, len);
        }
    }

    if (BSB_NOT_ERROR(bsb)) {
        MolochSession_t *session = moloch_packet_ip4(packet, BSB_WORK_PTR(bsb), BSB_REMAINING(bsb));
        if (!session)
            return;

        moloch_field_int_add(greIpField, session, ip4->ip_src.s_addr);
        moloch_field_int_add(greIpField, session, ip4->ip_dst.s_addr);
        moloch_session_add_protocol(session, "gre");
    }
}
#endif

/******************************************************************************/
int moloch_packet_ip(MolochPacket_t * const packet)
{
    totalBytes += packet->pktlen;

    if (totalPackets == 0) {
        MolochReaderStats_t stats;
        if (!moloch_reader_stats(&stats)) {
            initialDropped = stats.dropped;
        }
        initialPacket = packet->ts;
        LOG("Initial Packet = %ld", initialPacket.tv_sec);
        LOG("%" PRIu64 " Initial Dropped = %d", totalPackets, initialDropped);
    }

    if ((++totalPackets) % config.logEveryXPackets == 0) {
        MolochReaderStats_t stats;
        if (moloch_reader_stats(&stats)) {
            stats.dropped = 0;
            stats.total = totalPackets;
        }

        LOG("packets: %" PRIu64 " current sessions: %u/%u oldest: %d - recv: %" PRIu64 " drop: %" PRIu64 " (%0.2f) queue: %d disk: %d packet: %d close: %d",
          totalPackets,
          moloch_session_watch_count(packet->ses),
          moloch_session_monitoring(),
          moloch_session_idle_seconds(packet->ses),
          stats.total,
          stats.dropped - initialDropped,
          (stats.dropped - initialDropped)*(double)100.0/stats.total,
          moloch_http_queue_length(esServer),
          moloch_writer_queue_length(),
          moloch_packet_outstanding(),
          moloch_session_close_outstanding()
          );
    }

    packet->pkt = g_memdup(packet->pkt, packet->pktlen);
    uint32_t thread = moloch_session_hash(packet->sessionId) % config.packetThreads;

    MOLOCH_LOCK(packetQ[thread].lock);
    DLL_PUSH_TAIL(packet_, &packetQ[thread], packet);
    MOLOCH_UNLOCK(packetQ[thread].lock);
    MOLOCH_COND_BROADCAST(packetQ[thread].lock);
    return 0;
}
/******************************************************************************/
int moloch_packet_ip4(MolochPacket_t * const packet, const uint8_t *data, int len)
{
    struct ip           *ip4 = (struct ip*)data;
    struct tcphdr       *tcphdr = 0;
    struct udphdr       *udphdr = 0;

    if (len < (int)sizeof(struct ip))
        return 1;

    int ip_len = ntohs(ip4->ip_len);
    if (len < ip_len)
        return 1;

    int ip_hdr_len = 4 * ip4->ip_hl;
    if (len < ip_hdr_len)
        return 1;

    packet->ipOffset = (uint8_t*)data - packet->pkt;
    packet->payloadOffset = packet->ipOffset + ip_hdr_len;
    packet->payloadLen = ip_len - ip_hdr_len;

    switch (ip4->ip_p) {
    case IPPROTO_TCP:
        if (len < ip_hdr_len + (int)sizeof(struct tcphdr)) {
            return 1;
        }

        tcphdr = (struct tcphdr *)((char*)ip4 + ip_hdr_len);
        moloch_session_id(packet->sessionId, ip4->ip_src.s_addr, tcphdr->th_sport,
                          ip4->ip_dst.s_addr, tcphdr->th_dport);
        packet->ses = SESSION_TCP;
        break;
    case IPPROTO_UDP:
        if (len < ip_hdr_len + (int)sizeof(struct udphdr)) {
            return 1;
        }

        udphdr = (struct udphdr *)((char*)ip4 + ip_hdr_len);

        moloch_session_id(packet->sessionId, ip4->ip_src.s_addr, udphdr->uh_sport,
                          ip4->ip_dst.s_addr, udphdr->uh_dport);
        packet->ses = SESSION_UDP;
        break;
    case IPPROTO_ICMP:
        moloch_session_id(packet->sessionId, ip4->ip_src.s_addr, 0,
                          ip4->ip_dst.s_addr, 0);
        packet->ses = SESSION_ICMP;
        break;
#ifdef LATER
    case IPPROTO_GRE:
        return moloch_packet_gre4(packet, ip4, data + ip_hdr_len, len - ip_hdr_len);
#endif
    default:
        if (config.logUnknownProtocols)
            LOG("Unknown protocol %d", ip4->ip_p);
        return 1;
    }

    return moloch_packet_ip(packet);
}
/******************************************************************************/
int moloch_packet_ip6(MolochPacket_t * const UNUSED(packet), const uint8_t *data, int len)
{
    struct ip6_hdr      *ip6 = (struct ip6_hdr *)data;
    struct tcphdr       *tcphdr = 0;
    struct udphdr       *udphdr = 0;

    if (len < (int)sizeof(struct ip6_hdr))
        return 1;

    int ip_len = ntohs(ip6->ip6_plen);
    if (len < ip_len)
        return 1;

    int ip_hdr_len = sizeof(struct ip6_hdr);

    packet->ipOffset = (uint8_t*)data - packet->pkt;
    packet->payloadOffset = packet->ipOffset + ip_hdr_len;
    packet->payloadLen = ip_len - ip_hdr_len;


    switch (ip6->ip6_nxt) {
    case IPPROTO_TCP:
        if (len < ip_hdr_len + (int)sizeof(struct tcphdr)) {
            return 1;
        }

        tcphdr = (struct tcphdr *)((char*)ip6 + ip_hdr_len);

        moloch_session_id6(packet->sessionId, ip6->ip6_src.s6_addr, tcphdr->th_sport,
                           ip6->ip6_dst.s6_addr, tcphdr->th_dport);
        packet->ses = SESSION_TCP;
        break;
    case IPPROTO_UDP:
        if (len < ip_hdr_len + (int)sizeof(struct udphdr)) {
            return 1;
        }

        udphdr = (struct udphdr *)((char*)ip6 + ip_hdr_len);

        moloch_session_id6(packet->sessionId, ip6->ip6_src.s6_addr, udphdr->uh_sport,
                           ip6->ip6_dst.s6_addr, udphdr->uh_dport);
                          
        packet->ses = SESSION_UDP;
        break;
    case IPPROTO_ICMP:
        moloch_session_id6(packet->sessionId, ip6->ip6_src.s6_addr, 0,
                           ip6->ip6_dst.s6_addr, 0);
        packet->ses = SESSION_ICMP;
        break;
    case IPPROTO_ICMPV6:
        moloch_session_id6(packet->sessionId, ip6->ip6_src.s6_addr, 0,
                           ip6->ip6_dst.s6_addr, 0);
        packet->ses = SESSION_ICMP;
        break;
#ifdef LATER
    case IPPROTO_GRE:
        return moloch_packet_gre4(packet, ip4, data + ip_hdr_len, len - ip_hdr_len);
#endif
    default:
        LOG("Unknown protocol %d", ip6->ip6_nxt);
        return 1;
    }

    return moloch_packet_ip(packet);
}
/******************************************************************************/
int moloch_packet_ether(MolochPacket_t * const packet, const uint8_t *data, int len)
{
    if (len < 14) {
        return 1;
    }
    int n = 12;
    while (n+2 < len) {
        int ethertype = data[n] << 8 | data[n+1];
        n += 2;
        switch (ethertype) {
        case 0x0800:
            return moloch_packet_ip4(packet, data+n, len - n);
        case 0x86dd:
            return moloch_packet_ip6(packet, data+n, len - n);
        case 0x8100:
            n += 2;
            break;
        default:
            return 1;
        } // switch
    }
    return 0;
}
/******************************************************************************/
void moloch_packet(MolochPacket_t * const packet)
{
    int rc;

    switch(pcapFileHeader.linktype) {
    case 0: // NULL
        if (packet->pktlen > 4)
            rc = moloch_packet_ip4(packet, packet->pkt+4, packet->pktlen-4);
        else
            rc = 1;
        break;
    case 1: // Ether
        rc = moloch_packet_ether(packet, packet->pkt, packet->pktlen);
        break;
    case 12: // RAW
        rc = moloch_packet_ip4(packet, packet->pkt, packet->pktlen);
        break;
    case 113: // SLL
        rc = moloch_packet_ip4(packet, packet->pkt, packet->pktlen);
        break;
    default:
        LOG("ERROR - Unsupported pcap link type %d", pcapFileHeader.linktype);
        exit (0);
    }
    if (rc)
        MOLOCH_TYPE_FREE(MolochPacket_t, packet);
}
/******************************************************************************/
int moloch_packet_outstanding()
{
    int count = 0;
    int t;

    for (t = 0; t < config.packetThreads; t++) {
        count += DLL_COUNT(packet_, &packetQ[t]);
    }
    return count;
}
/******************************************************************************/
void moloch_packet_init()
{
    pcapFileHeader.magic = 0xa1b2c3d4;
    pcapFileHeader.version_major = 2;
    pcapFileHeader.version_minor = 4;

    pcapFileHeader.thiszone = 0;
    pcapFileHeader.sigfigs = 0;

    mac1Field = moloch_field_define("general", "lotermfield",
        "mac.src", "Src MAC", "mac1-term",
        "Source ethernet mac addresses set for session",
        MOLOCH_FIELD_TYPE_STR_HASH,  MOLOCH_FIELD_FLAG_COUNT | MOLOCH_FIELD_FLAG_LINKED_SESSIONS,
        NULL);

    mac2Field = moloch_field_define("general", "lotermfield",
        "mac.dst", "Dst MAC", "mac2-term",
        "Destination ethernet mac addresses set for session",
        MOLOCH_FIELD_TYPE_STR_HASH,  MOLOCH_FIELD_FLAG_COUNT | MOLOCH_FIELD_FLAG_LINKED_SESSIONS,
        NULL);

    moloch_field_define("general", "lotermfield",
        "mac", "Src or Dst MAC", "macall",
        "Shorthand for mac.src or mac.dst",
        0,  MOLOCH_FIELD_FLAG_FAKE,
        "regex", "^mac\\\\.(?:(?!\\\\.cnt$).)*$",
        NULL);

    vlanField = moloch_field_define("general", "integer",
        "vlan", "VLan", "vlan",
        "vlan value",
        MOLOCH_FIELD_TYPE_INT_GHASH,  MOLOCH_FIELD_FLAG_COUNT | MOLOCH_FIELD_FLAG_LINKED_SESSIONS,
        NULL);

    greIpField = moloch_field_define("general", "ip",
        "gre.ip", "GRE IP", "greip",
        "GRE ip addresses for session",
        MOLOCH_FIELD_TYPE_IP_GHASH,  MOLOCH_FIELD_FLAG_COUNT,
        NULL);

    moloch_field_define("general", "lotermfield",
        "tipv6.src", "IPv6 Src", "tipv61-term",
        "Temporary IPv6 Source",
        0,  MOLOCH_FIELD_FLAG_FAKE,
        "portField", "p1",
        "transform", "ipv6ToHex",
        NULL);

    moloch_field_define("general", "lotermfield",
        "tipv6.dst", "IPv6 Dst", "tipv62-term",
        "Temporary IPv6 Destination",
        0,  MOLOCH_FIELD_FLAG_FAKE,
        "portField", "p2",
        "transform", "ipv6ToHex",
        NULL);

    int t;
    for (t = 0; t < config.packetThreads; t++) {
        char name[100];
        snprintf(name, sizeof(name), "moloch-pkt%d", t);
        g_thread_new(name, &moloch_packet_thread, (gpointer)(long)t);
        DLL_INIT(packet_, &packetQ[t]);
        MOLOCH_LOCK_INIT(packetQ[t].lock);
        MOLOCH_COND_INIT(packetQ[t].lock);
    }

    moloch_add_can_quit(moloch_packet_outstanding);
}
/******************************************************************************/
uint32_t moloch_packet_dropped_packets()
{
    MolochReaderStats_t stats;
    if (moloch_reader_stats(&stats)) {
        return 0;
    }
    return stats.dropped - initialDropped;
}
/******************************************************************************/
void moloch_packet_exit()
{
}