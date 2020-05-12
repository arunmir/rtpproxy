/*
 * Copyright (c) 2004-2006 Maxim Sobolev <sobomax@FreeBSD.org>
 * Copyright (c) 2006-20015 Sippy Software, Inc., http://www.sippysoft.com
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "config.h"

#include "rtpp_ssrc.h"
#include "rtpa_stats.h"
#include "rtpp_defines.h"
#include "rtpp_log.h"
#include "rtpp_types.h"
#include "rtpp_log_obj.h"
#include "rtp.h"
#include "rtp_analyze.h"
#include "rtpp_analyzer.h"
#include "rtp_resizer.h"
#include "rtpp_time.h"
#include "rtpp_command.h"
#include "rtpp_command_args.h"
#include "rtpp_command_private.h"
#include "rtpp_genuid_singlet.h"
#include "rtp_info.h"
#include "rtp_packet.h"
#include "rtpp_mallocs.h"
#include "rtpp_refcnt.h"
#include "rtpp_network.h"
#include "rtpp_pcount.h"
#include "rtpp_pcnt_strm.h"
#include "rtpp_proc.h"
#include "rtpp_record.h"
#include "rtpp_stats.h"
#include "rtpp_stream.h"
#include "rtpp_stream_fin.h"
#include "rtpp_server.h"
#include "rtpp_socket.h"
#include "rtpp_weakref.h"
#include "rtpp_ttl.h"
#include "rtpp_pipe.h"
#include "rtpp_netaddr.h"
#include "rtpp_debug.h"
#include "rtpp_acct_pipe.h"

#define  SEQ_SYNC_IVAL   1.0    /* in seconds */

struct rtpps_latch {
    int latched;
    struct rtpp_ssrc ssrc;
    int seq;
    double last_sync;
};

struct rtps {
    uint64_t uid;
    int inact;
};

struct rtpp_stream_priv
{
    struct rtpp_stream pub;
    struct rtpp_weakref_obj *servers_wrt;
    struct rtpp_stats *rtpp_stats;
    pthread_mutex_t lock;
    /* Weak reference to the "rtpp_server" (player) */
    struct rtps rtps;
    /* Timestamp of the last session update */
    double last_update;
    /* Flag that indicates whether or not address supplied by client can't be trusted */
    int untrusted_addr;
    /* Save previous address when doing update */
    struct rtpp_netaddr *raddr_prev;
    /* Flag which tells if we are allowed to update address with RTP src IP */
    struct rtpps_latch latch_info;
    /* Structure to track hold requests */
    struct rtpp_acct_hold hld_stat;
    /* Descriptor */
    struct rtpp_socket *fd;
    /* Remote source address */
    struct rtpp_netaddr *rem_addr;
    /* Placeholder for per-module structures */
    unsigned int nmodules;
    _Atomic(struct rtpp_refcnt *) pmod_data[];
};

static void rtpp_stream_dtor(struct rtpp_stream_priv *);
static int rtpp_stream_handle_play(struct rtpp_stream *, const char *,
  const char *, int, struct rtpp_command *, int);
static void rtpp_stream_handle_noplay(struct rtpp_stream *);
static int rtpp_stream_isplayer_active(struct rtpp_stream *);
static void rtpp_stream_finish_playback(struct rtpp_stream *, uint64_t);
static const char *rtpp_stream_get_actor(struct rtpp_stream *);
static const char *rtpp_stream_get_proto(struct rtpp_stream *);
static int _rtpp_stream_latch(struct rtpp_stream_priv *, double,
  struct rtp_packet *);
static int _rtpp_stream_check_latch_override(struct rtpp_stream_priv *,
  struct rtp_packet *, double);
static void __rtpp_stream_fill_addr(struct rtpp_stream_priv *,
  struct rtp_packet *);
static int rtpp_stream_guess_addr(struct rtpp_stream *,
  struct rtp_packet *);
static void rtpp_stream_prefill_addr(struct rtpp_stream *,
  struct sockaddr **, double);
static void rtpp_stream_set_skt(struct rtpp_stream *, struct rtpp_socket *);
static struct rtpp_socket *rtpp_stream_get_skt(struct rtpp_stream *);
static struct rtpp_socket *rtpp_stream_update_skt(struct rtpp_stream *,
  struct rtpp_socket *);
static int rtpp_stream_send_pkt(struct rtpp_stream *, struct sthread_args *,
  struct rtp_packet *);
static struct rtp_packet *_rtpp_stream_recv_pkt(struct rtpp_stream_priv *,
  const struct rtpp_timestamp *);
static int rtpp_stream_issendable(struct rtpp_stream *);
static int _rtpp_stream_islatched(struct rtpp_stream_priv *);
static void rtpp_stream_locklatch(struct rtpp_stream *);
static void rtpp_stream_reg_onhold(struct rtpp_stream *);
void rtpp_stream_get_stats(struct rtpp_stream *, struct rtpp_acct_hold *);
static struct rtp_packet *rtpp_stream_rx(struct rtpp_stream *,
  struct rtpp_weakref_obj *, const struct rtpp_timestamp *, struct rtpp_proc_rstats *);
static struct rtpp_netaddr *rtpp_stream_get_rem_addr(struct rtpp_stream *, int);

static const struct rtpp_stream_smethods rtpp_stream_smethods = {
    .handle_play = &rtpp_stream_handle_play,
    .handle_noplay = &rtpp_stream_handle_noplay,
    .isplayer_active = &rtpp_stream_isplayer_active,
    .finish_playback = &rtpp_stream_finish_playback,
    .get_actor = &rtpp_stream_get_actor,
    .get_proto = &rtpp_stream_get_proto,
    .prefill_addr = &rtpp_stream_prefill_addr,
    .set_skt = &rtpp_stream_set_skt,
    .get_skt = &rtpp_stream_get_skt,
    .update_skt = &rtpp_stream_update_skt,
    .send_pkt = &rtpp_stream_send_pkt,
    .guess_addr = &rtpp_stream_guess_addr,
    .issendable = &rtpp_stream_issendable,
    .locklatch = &rtpp_stream_locklatch,
    .reg_onhold = &rtpp_stream_reg_onhold,
    .get_stats = &rtpp_stream_get_stats,
    .rx = &rtpp_stream_rx,
    .get_rem_addr = &rtpp_stream_get_rem_addr
};

struct rtpp_stream *
rtpp_stream_ctor(const struct r_stream_ctor_args *ap)
{
    struct rtpp_stream_priv *pvt;
    size_t alen;

    alen = offsetof(struct rtpp_stream_priv, pmod_data) + 
      (ap->nmodules * sizeof(pvt->pmod_data[0]));
    pvt = rtpp_rzmalloc(alen, PVT_RCOFFS(pvt));
    if (pvt == NULL) {
        goto e0;
    }
    if (pthread_mutex_init(&pvt->lock, NULL) != 0) {
        goto e1;
    }
    if (ap->pipe_type == PIPE_RTP) {
        pvt->pub.analyzer = rtpp_analyzer_ctor(ap->log);
        if (pvt->pub.analyzer == NULL) {
            goto e3;
        }
    }
    pvt->pub.pcnt_strm = rtpp_pcnt_strm_ctor();
    if (pvt->pub.pcnt_strm == NULL) {
        goto e4;
    }
    pvt->raddr_prev = rtpp_netaddr_ctor();
    if (pvt->raddr_prev == NULL) {
        goto e5;
    }
    pvt->rem_addr = rtpp_netaddr_ctor();
    if (pvt->rem_addr == NULL) {
        goto e6;
    }
    pvt->servers_wrt = ap->servers_wrt;
    pvt->rtpp_stats = ap->rtpp_stats;
    pvt->pub.log = ap->log;
    RTPP_OBJ_INCREF(ap->log);
    pvt->pub.side = ap->side;
    pvt->pub.pipe_type = ap->pipe_type;
    pvt->pub.smethods = &rtpp_stream_smethods;

    rtpp_gen_uid(&pvt->pub.stuid);
    pvt->pub.seuid = ap->seuid;
    for (unsigned int i = 0; i < ap->nmodules; i++) {
        atomic_init(&(pvt->pmod_data[i]), NULL);
    }
    pvt->pub.pmod_data = &(pvt->pmod_data[0]);
    pvt->nmodules = ap->nmodules;
    CALL_SMETHOD(pvt->pub.rcnt, attach, (rtpp_refcnt_dtor_t)&rtpp_stream_dtor,
      pvt);
    return (&pvt->pub);

e6:
    RTPP_OBJ_DECREF(pvt->raddr_prev);
e5:
    RTPP_OBJ_DECREF(pvt->pub.pcnt_strm);
e4:
    if (ap->pipe_type == PIPE_RTP) {
         RTPP_OBJ_DECREF(pvt->pub.analyzer);
    }
e3:
    pthread_mutex_destroy(&pvt->lock);
e1:
    RTPP_OBJ_DECREF(&(pvt->pub));
    free(pvt);
e0:
    return (NULL);
}

static inline void
_s_rtps(struct rtpp_stream_priv *pvt, uint64_t rtps, int replace)
{

    RTPP_DBG_ASSERT(pvt->pub.pipe_type == PIPE_RTP);
    if (replace == 0) {
        RTPP_DBG_ASSERT(pvt->rtps.uid == RTPP_UID_NONE);
    }
    pvt->rtps.uid = rtps;
    if (CALL_SMETHOD(pvt->rem_addr, isempty) || pvt->fd == NULL) {
        pvt->rtps.inact = 1;
    }
}

static const char *
_rtpp_stream_get_actor(struct rtpp_stream_priv *pvt)
{

    return ((pvt->pub.side == RTPP_SSIDE_CALLER) ? "caller" : "callee");
}

static const char *
_rtpp_stream_get_proto(struct rtpp_stream_priv *pvt)
{

    return (PP_NAME(pvt->pub.pipe_type));
}

static void
rtpp_stream_dtor(struct rtpp_stream_priv *pvt)
{
    struct rtpp_stream *pub;

    pub = &(pvt->pub);
    rtpp_stream_fin(pub);
    if (pub->analyzer != NULL) {
         struct rtpa_stats rst;
         char ssrc_buf[11];
         const char *actor, *ssrc;

         actor = _rtpp_stream_get_actor(pvt);
         CALL_METHOD(pub->analyzer, get_stats, &rst);
         if (rst.ssrc_changes != 0) {
             snprintf(ssrc_buf, sizeof(ssrc_buf), SSRC_FMT, rst.last_ssrc.val);
             ssrc = ssrc_buf;
         } else {
             ssrc = "NONE";
         }
         RTPP_LOG(pvt->pub.log, RTPP_LOG_INFO, "RTP stream from %s: "
           "SSRC=%s, ssrc_changes=%lu, psent=%lu, precvd=%lu, plost=%lu, pdups=%lu",
           actor, ssrc, rst.ssrc_changes, rst.psent, rst.precvd,
           rst.plost, rst.pdups);
         if (rst.psent > 0) {
             CALL_SMETHOD(pvt->rtpp_stats, updatebyname, "rtpa_nsent", rst.psent);
         }
         if (rst.precvd > 0) {
             CALL_SMETHOD(pvt->rtpp_stats, updatebyname, "rtpa_nrcvd", rst.precvd);
         }
         if (rst.pdups > 0) {
             CALL_SMETHOD(pvt->rtpp_stats, updatebyname, "rtpa_ndups", rst.pdups);
         }
         if (rst.pecount > 0) {
             CALL_SMETHOD(pvt->rtpp_stats, updatebyname, "rtpa_perrs", rst.pecount);
         }
         RTPP_OBJ_DECREF(pvt->pub.analyzer);
    }
    if (pvt->fd != NULL)
        RTPP_OBJ_DECREF(pvt->fd);
    if (pub->codecs != NULL)
        free(pub->codecs);
    if (pvt->rtps.uid != RTPP_UID_NONE)
        CALL_METHOD(pvt->servers_wrt, unreg, pvt->rtps.uid);
    if (pub->resizer != NULL)
        rtp_resizer_free(pvt->rtpp_stats, pub->resizer);
    if (pub->rrc != NULL)
        RTPP_OBJ_DECREF(pub->rrc);
    if (pub->pcount != NULL)
        RTPP_OBJ_DECREF(pub->pcount);
    for (unsigned int i = 0; i < pvt->nmodules; i++) {
        struct rtpp_refcnt *mdata_rcnt;
        mdata_rcnt = atomic_load(&(pvt->pmod_data[i]));
        if (mdata_rcnt != NULL) {
            CALL_SMETHOD(mdata_rcnt, decref);
        }
    }
    if (pub->ttl != NULL)
        RTPP_OBJ_DECREF(pub->ttl);
    RTPP_OBJ_DECREF(pub->pcnt_strm);
    RTPP_OBJ_DECREF(pvt->pub.log);
    RTPP_OBJ_DECREF(pvt->rem_addr);
    RTPP_OBJ_DECREF(pvt->raddr_prev);

    pthread_mutex_destroy(&pvt->lock);
    free(pvt);
}

static void
player_predestroy_cb(struct rtpp_stats *rtpp_stats)
{

    CALL_SMETHOD(rtpp_stats, updatebyname, "nplrs_destroyed", 1);
}

static int
rtpp_stream_handle_play(struct rtpp_stream *self, const char *codecs,
  const char *pname, int playcount, struct rtpp_command *cmd, int ptime)
{
    struct rtpp_stream_priv *pvt;
    int n;
    char *cp;
    struct rtpp_server *rsrv;
    uint16_t seq;
    uint32_t ssrc;
    const char *plerror;
    struct rtpp_server_ctor_args sca = {.name = pname, .loop = playcount,
      .ptime = ptime};

    PUB2PVT(self, pvt);
    pthread_mutex_lock(&pvt->lock);
    plerror = "reason unknown";
    while (*codecs != '\0') {
        n = strtol(codecs, &cp, 10);
        if (cp == codecs) {
            plerror = "invalid codecs";
            break;
        }
        codecs = cp;
        if (*codecs != '\0')
            codecs++;
        sca.codec = n;
        rsrv = rtpp_server_ctor(&sca);
        if (rsrv == NULL) {
            RTPP_LOG(pvt->pub.log, RTPP_LOG_DBUG, "rtpp_server_ctor(\"%s\", %d, %d) failed",
              pname, n, playcount);
            plerror = "rtpp_server_ctor() failed";
            if (sca.result == RTPP_SERV_NOENT)
                continue;
            break;
        }
        rsrv->stuid = self->stuid;
        ssrc = CALL_SMETHOD(rsrv, get_ssrc);
        seq = CALL_SMETHOD(rsrv, get_seq);
        _s_rtps(pvt, rsrv->sruid, 0);
        if (CALL_METHOD(pvt->servers_wrt, reg, rsrv->rcnt, rsrv->sruid) != 0) {
            RTPP_OBJ_DECREF(rsrv);
            plerror = "servers_wrt->reg() method failed";
            break;
        }
        if (pvt->rtps.inact == 0) {
            CALL_SMETHOD(rsrv, start, cmd->dtime->mono);
        }
        pthread_mutex_unlock(&pvt->lock);
        cmd->csp->nplrs_created.cnt++;
        CALL_SMETHOD(rsrv->rcnt, reg_pd, (rtpp_refcnt_dtor_t)player_predestroy_cb,
          pvt->rtpp_stats);
        RTPP_OBJ_DECREF(rsrv);
        RTPP_LOG(pvt->pub.log, RTPP_LOG_INFO,
          "%d times playing prompt %s codec %d: SSRC=" SSRC_FMT ", seq=%u",
          playcount, pname, n, ssrc, seq);
        return 0;
    }
    pthread_mutex_unlock(&pvt->lock);
    RTPP_LOG(pvt->pub.log, RTPP_LOG_ERR, "can't create player: %s", plerror);
    return -1;
}

static void
rtpp_stream_handle_noplay(struct rtpp_stream *self)
{
    struct rtpp_stream_priv *pvt;
    uint64_t ruid;
    int stopped;

    stopped = 0;
    PUB2PVT(self, pvt);
    pthread_mutex_lock(&pvt->lock);
    ruid = pvt->rtps.uid;
    pthread_mutex_unlock(&pvt->lock);
    if (ruid != RTPP_UID_NONE) {
        if (CALL_METHOD(pvt->servers_wrt, unreg, ruid) != NULL) {
            pthread_mutex_lock(&pvt->lock);
            if (pvt->rtps.uid == ruid) {
                pvt->rtps.uid = RTPP_UID_NONE;
                pvt->rtps.inact = 0;
                stopped = 1;
            }
            pthread_mutex_unlock(&pvt->lock);
        }
        if (stopped != 0) {
            RTPP_LOG(pvt->pub.log, RTPP_LOG_INFO,
              "stopping player at port %d", self->port);
        }
    }
}

static int
rtpp_stream_isplayer_active(struct rtpp_stream *self)
{
    struct rtpp_stream_priv *pvt;
    int rval;

    PUB2PVT(self, pvt);
    pthread_mutex_lock(&pvt->lock);
    rval = (pvt->rtps.uid != RTPP_UID_NONE) ? 1 : 0;
    pthread_mutex_unlock(&pvt->lock);
    return (rval);
}

static void
rtpp_stream_finish_playback(struct rtpp_stream *self, uint64_t sruid)
{
    struct rtpp_stream_priv *pvt;

    PUB2PVT(self, pvt);
    pthread_mutex_lock(&pvt->lock);
    if (pvt->rtps.uid != RTPP_UID_NONE && pvt->rtps.uid == sruid) {
        _s_rtps(pvt, RTPP_UID_NONE, 1);
        RTPP_LOG(pvt->pub.log, RTPP_LOG_INFO,
          "player at port %d has finished", self->port);
    }
    pthread_mutex_unlock(&pvt->lock);
}

static const char *
rtpp_stream_get_actor(struct rtpp_stream *self)
{
    struct rtpp_stream_priv *pvt;

    PUB2PVT(self, pvt);
    return (_rtpp_stream_get_actor(pvt));
}

static const char *
rtpp_stream_get_proto(struct rtpp_stream *self)
{

    return (PP_NAME(self->pipe_type));
}

static int
_rtpp_stream_latch(struct rtpp_stream_priv *pvt, double dtime,
  struct rtp_packet *packet)
{
    const char *actor, *ptype, *ssrc, *seq, *relatch;
    char ssrc_buf[11], seq_buf[6];
    char saddr[MAX_AP_STRBUF];
    int newlatch;

    if (pvt->last_update != 0 && \
      dtime - pvt->last_update < UPDATE_WINDOW) {
        return (0);
    }

    actor = _rtpp_stream_get_actor(pvt);
    ptype = _rtpp_stream_get_proto(pvt);

    if (pvt->pub.pipe_type == PIPE_RTP) {
        if (rtp_packet_parse(packet) == RTP_PARSER_OK) {
            pvt->latch_info.ssrc.val = packet->parsed->ssrc;
            pvt->latch_info.ssrc.inited = 1;
            pvt->latch_info.seq = packet->parsed->seq;
            pvt->latch_info.last_sync = dtime;
            snprintf(ssrc_buf, sizeof(ssrc_buf), SSRC_FMT, packet->parsed->ssrc);
            snprintf(seq_buf, sizeof(seq_buf), "%u", packet->parsed->seq);
            ssrc = ssrc_buf;
            seq = seq_buf;
        } else {
            pvt->latch_info.ssrc.val = 0;
            pvt->latch_info.ssrc.inited = 0;
            ssrc = seq = "INVALID";
        }
    } else {
        pvt->latch_info.ssrc.inited = 0;
        ssrc = seq = "UNKNOWN";
    }

    addrport2char_r(sstosa(&packet->raddr), saddr, sizeof(saddr), ':');
    newlatch = SSRC_IS_BAD(&pvt->latch_info.ssrc) ? 0 : 1;
    if (pvt->latch_info.latched == 0) {
        relatch = (newlatch != 0) ? "latched in" : "not latched (bad SSRC)";
    } else {
        relatch = "re-latched";
    }
    RTPP_LOG(pvt->pub.log, RTPP_LOG_INFO,
      "%s's address %s: %s (%s), SSRC=%s, Seq=%s", actor, relatch,
      saddr, ptype, ssrc, seq);
    pvt->latch_info.latched = newlatch;
    return (1);
}

static void
_rtpp_stream_latch_sync(struct rtpp_stream_priv *pvt, double dtime,
  struct rtp_packet *packet)
{
    struct rtpps_latch *lip;

    lip = &pvt->latch_info;
    if (pvt->pub.pipe_type != PIPE_RTP || lip->ssrc.inited == 0)
        return;
    if (dtime - lip->last_sync < SEQ_SYNC_IVAL)
        return;
    if (rtp_packet_parse(packet) != RTP_PARSER_OK)
        return;
    if (lip->ssrc.val != packet->parsed->ssrc)
        return;
    lip->seq = packet->parsed->seq;
    lip->last_sync = dtime;
}

static int
_rtpp_stream_check_latch_override(struct rtpp_stream_priv *pvt,
  struct rtp_packet *packet, double dtime)
{
    const char *actor;
    char saddr[MAX_AP_STRBUF];

    if (pvt->pub.pipe_type == PIPE_RTCP || pvt->latch_info.ssrc.inited == 0)
        return (0);
    if (rtp_packet_parse(packet) != RTP_PARSER_OK)
        return (0);
    if (pvt->last_update == 0 || dtime - pvt->last_update > UPDATE_WINDOW) {
        if (packet->parsed->ssrc != pvt->latch_info.ssrc.val)
            return (0);
        if (SEQ_DIST(pvt->latch_info.seq, packet->parsed->seq) > 536)
            return (0);
    }

    actor = _rtpp_stream_get_actor(pvt);

    addrport2char_r(sstosa(&packet->raddr), saddr, sizeof(saddr), ':');
    RTPP_LOG(pvt->pub.log, RTPP_LOG_INFO,
      "%s's address re-latched: %s (%s), SSRC=" SSRC_FMT ", Seq=%u->%u", actor,
      saddr, "RTP", pvt->latch_info.ssrc.val, pvt->latch_info.seq,
      packet->parsed->seq);

    pvt->latch_info.seq = packet->parsed->seq;
    pvt->latch_info.last_sync = packet->rtime.mono;
    return (1);
}

static void
_rtpp_stream_plr_start(struct rtpp_stream_priv *pvt, double dtime)
{
    struct rtpp_server *rsrv;

    RTPP_DBG_ASSERT(pvt->rtps.inact != 0);
    rsrv = CALL_METHOD(pvt->servers_wrt, get_by_idx, pvt->rtps.uid);
    if (rsrv == NULL) {
        return;
    }
    CALL_SMETHOD(rsrv, start, dtime);
    RTPP_OBJ_DECREF(rsrv);
    pvt->rtps.inact = 0;
}

static void
__rtpp_stream_fill_addr(struct rtpp_stream_priv *pvt, struct rtp_packet *packet)
{
    const char *actor, *ptype;
    char saddr[MAX_AP_STRBUF];

    pvt->untrusted_addr = 1;
    CALL_SMETHOD(pvt->rem_addr, set, sstosa(&packet->raddr), packet->rlen);
    if (CALL_SMETHOD(pvt->raddr_prev, isempty) ||
      CALL_SMETHOD(pvt->raddr_prev, cmp, sstosa(&packet->raddr), packet->rlen) != 0) {
        pvt->latch_info.latched = 1;
    }
    if (pvt->rtps.inact != 0 && pvt->fd != NULL) {
        _rtpp_stream_plr_start(pvt, packet->rtime.mono);
    }

    actor = _rtpp_stream_get_actor(pvt);
    ptype = _rtpp_stream_get_proto(pvt);
    addrport2char_r(sstosa(&packet->raddr), saddr, sizeof(saddr), ':');
    RTPP_LOG(pvt->pub.log, RTPP_LOG_INFO,
      "%s's address filled in: %s (%s)", actor, saddr, ptype);
    return;
}

static int
rtpp_stream_guess_addr(struct rtpp_stream *self,
  struct rtp_packet *packet)
{
    int rport;
    const char *actor, *ptype;
    struct rtpp_stream_priv *pvt;
    struct sockaddr_storage ta;

    RTPP_DBG_ASSERT(self->pipe_type == PIPE_RTCP);
    PUB2PVT(self, pvt);

    if (!CALL_SMETHOD(pvt->rem_addr, isempty) &&
      CALL_SMETHOD(pvt->rem_addr, cmphost, sstosa(&packet->raddr))) {
        return (0);
    }
#if 0
    if (self->addr == NULL) {
        self->addr = malloc(packet->rlen);
        if (self->addr == NULL) {
            return (-1);
        }
    }
#endif
    actor = rtpp_stream_get_actor(self);
    ptype = rtpp_stream_get_proto(self);
    rport = ntohs(satosin(&packet->raddr)->sin_port);
    if (IS_LAST_PORT(rport)) {
        return (-1);
    }

    memcpy(&ta, &packet->raddr, packet->rlen);
    setport(sstosa(&ta), rport + 1);

    CALL_SMETHOD(pvt->rem_addr, set, sstosa(&ta), packet->rlen);
    /* Use guessed value as the only true one for asymmetric clients */
    pvt->latch_info.latched = self->asymmetric;
    RTPP_LOG(pvt->pub.log, RTPP_LOG_INFO, "guessing %s port "
      "for %s to be %d", ptype, actor, rport + 1);

    return (0);
}

static void
rtpp_stream_prefill_addr(struct rtpp_stream *self, struct sockaddr **iapp,
  double dtime)
{
    struct rtpp_stream_priv *pvt;
    char saddr[MAX_AP_STRBUF];
    const char *actor, *ptype;

    PUB2PVT(self, pvt);

    actor = rtpp_stream_get_actor(self);
    ptype = rtpp_stream_get_proto(self);
    pthread_mutex_lock(&pvt->lock);
    if (pvt->hld_stat.status != 0) {
        RTPP_LOG(pvt->pub.log, RTPP_LOG_INFO, "taking %s's %s stream off-hold",
            actor, ptype);
        pvt->hld_stat.status = 0;
    }

    if (!CALL_SMETHOD(pvt->rem_addr, isempty))
        pvt->last_update = dtime;

    /*
     * Unless the address provided by client historically
     * cannot be trusted and address is different from one
     * that we recorded update it.
     */
    if (pvt->untrusted_addr != 0) {
        pthread_mutex_unlock(&pvt->lock);
        return;
    }
    if (!CALL_SMETHOD(pvt->rem_addr, isempty) && CALL_SMETHOD(pvt->rem_addr,
      isaddrseq, *iapp)) {
        pthread_mutex_unlock(&pvt->lock);
        return;
    }

    addrport2char_r(*iapp, saddr, sizeof(saddr), ':');
    RTPP_LOG(pvt->pub.log, RTPP_LOG_INFO, "pre-filling %s's %s address "
      "with %s", actor, ptype, saddr);
    if (!CALL_SMETHOD(pvt->rem_addr, isempty)) {
        if (pvt->latch_info.latched != 0) {
            CALL_SMETHOD(pvt->raddr_prev, copy, pvt->rem_addr);
        }
    }
    CALL_SMETHOD(pvt->rem_addr, set, *iapp, SA_LEN(*iapp));
    if (pvt->rtps.inact != 0 && pvt->fd != NULL) {
        _rtpp_stream_plr_start(pvt, dtime);
    }
    pthread_mutex_unlock(&pvt->lock);
}

static void rtpp_stream_reg_onhold(struct rtpp_stream *self)
{
    struct rtpp_stream_priv *pvt;
    const char *actor, *ptype;

    PUB2PVT(self, pvt);
    pthread_mutex_lock(&pvt->lock);
    if (pvt->hld_stat.status == 0) {
        actor = rtpp_stream_get_actor(self);
        ptype = rtpp_stream_get_proto(self);
        RTPP_LOG(pvt->pub.log, RTPP_LOG_INFO, "putting %s's %s stream on hold",
           actor, ptype);
        pvt->hld_stat.status = 1;
    }
    pvt->hld_stat.cnt++;
    pthread_mutex_unlock(&pvt->lock);
}

static void
rtpp_stream_set_skt(struct rtpp_stream *self, struct rtpp_socket *new_skt)
{
    struct rtpp_stream_priv *pvt;

    PUB2PVT(self, pvt);
    pthread_mutex_lock(&pvt->lock);
    if (new_skt == NULL) {
        RTPP_DBG_ASSERT(pvt->fd != NULL);
        RTPP_OBJ_DECREF(pvt->fd);
        pvt->fd = NULL;
        pthread_mutex_unlock(&pvt->lock);
        return;
    }
    RTPP_DBG_ASSERT(pvt->fd == NULL);
    pvt->fd = new_skt;
    RTPP_OBJ_INCREF(pvt->fd);
    if (pvt->rtps.inact != 0 && !CALL_SMETHOD(pvt->rem_addr, isempty)) {
        _rtpp_stream_plr_start(pvt, getdtime());
    }
    pthread_mutex_unlock(&pvt->lock);
}

static struct rtpp_socket *
rtpp_stream_get_skt(struct rtpp_stream *self)
{
    struct rtpp_stream_priv *pvt;
    struct rtpp_socket *rval;

    PUB2PVT(self, pvt);
    pthread_mutex_lock(&pvt->lock);
    if (pvt->fd == NULL) {
        pthread_mutex_unlock(&pvt->lock);
        return (NULL);
    }
    RTPP_OBJ_INCREF(pvt->fd);
    rval = pvt->fd;
    pthread_mutex_unlock(&pvt->lock);
    return (rval);
}

static struct rtpp_socket *
rtpp_stream_update_skt(struct rtpp_stream *self, struct rtpp_socket *new_skt)
{
    struct rtpp_socket *old_skt;
    struct rtpp_stream_priv *pvt;

    RTPP_DBG_ASSERT(new_skt != NULL);
    PUB2PVT(self, pvt);
    pthread_mutex_lock(&pvt->lock);
    old_skt = pvt->fd;
    pvt->fd = new_skt;
    RTPP_OBJ_INCREF(pvt->fd);
    if (pvt->rtps.inact != 0 && !CALL_SMETHOD(pvt->rem_addr, isempty)) {
        _rtpp_stream_plr_start(pvt, getdtime());
    }
    pthread_mutex_unlock(&pvt->lock);
    return (old_skt);
}

static int
rtpp_stream_send_pkt(struct rtpp_stream *self, struct sthread_args *sap,
  struct rtp_packet *pkt)
{
    struct rtpp_stream_priv *pvt;
    int rval;

    PUB2PVT(self, pvt);
    pthread_mutex_lock(&pvt->lock);
    rval = CALL_METHOD(pvt->fd, send_pkt_na, sap, pvt->rem_addr, pkt,
      self->log);
    pthread_mutex_unlock(&pvt->lock);
    return (rval);
}

static struct rtp_packet *
_rtpp_stream_recv_pkt(struct rtpp_stream_priv *pvt,
  const struct rtpp_timestamp *dtime)
{
    struct rtp_packet *pkt;

    pkt = CALL_METHOD(pvt->fd, rtp_recv, dtime, pvt->pub.laddr, pvt->pub.port);
    return (pkt);
}

static int
rtpp_stream_issendable(struct rtpp_stream *self)
{
    struct rtpp_stream_priv *pvt;

    PUB2PVT(self, pvt);
    pthread_mutex_lock(&pvt->lock);
    if (CALL_SMETHOD(pvt->rem_addr, isempty)) {
        pthread_mutex_unlock(&pvt->lock);
        return (0);
    }
    if (pvt->fd == NULL) {
        pthread_mutex_unlock(&pvt->lock);
        return (0);
    }
    pthread_mutex_unlock(&pvt->lock);
    return (1);
}

static int
_rtpp_stream_islatched(struct rtpp_stream_priv *pvt)
{
    int rval;

    rval = pvt->latch_info.latched;
    return (rval);
}

static void
rtpp_stream_locklatch(struct rtpp_stream *self)
{
    struct rtpp_stream_priv *pvt;

    PUB2PVT(self, pvt);
    pthread_mutex_lock(&pvt->lock);
    pvt->latch_info.latched = 1;
    pthread_mutex_unlock(&pvt->lock);
}

void
rtpp_stream_get_stats(struct rtpp_stream *self, struct rtpp_acct_hold *ahp)
{
    struct rtpp_stream_priv *pvt;

    PUB2PVT(self, pvt);
    pthread_mutex_lock(&pvt->lock);
    *ahp = pvt->hld_stat;
    pthread_mutex_unlock(&pvt->lock);
}

static int
_rtpp_stream_fill_addr(struct rtpp_stream_priv *pvt,
  struct rtpp_weakref_obj *rtcps_wrt, struct rtp_packet *packet)
{
    struct rtpp_stream *stp_rtcp;
    int rval;

    __rtpp_stream_fill_addr(pvt, packet);
    if (pvt->pub.stuid_rtcp == RTPP_UID_NONE) {
        return (0);
    }
    stp_rtcp = CALL_METHOD(rtcps_wrt, get_by_idx,
      pvt->pub.stuid_rtcp);
    if (stp_rtcp == NULL) {
        return (0);
    }
    rval = CALL_SMETHOD(stp_rtcp, guess_addr, packet);
    RTPP_OBJ_DECREF(stp_rtcp);
    return (rval);
}

static struct rtp_packet *
rtpp_stream_rx(struct rtpp_stream *self, struct rtpp_weakref_obj *rtcps_wrt,
  const struct rtpp_timestamp *dtime, struct rtpp_proc_rstats *rsp)
{
    struct rtp_packet *packet = NULL;
    struct rtpp_stream_priv *pvt;

    PUB2PVT(self, pvt);
    pthread_mutex_lock(&pvt->lock);
    packet = _rtpp_stream_recv_pkt(pvt, dtime);
    if (packet == NULL) {
        /* Move on to the next session */
        pthread_mutex_unlock(&pvt->lock);
        return (NULL);
    }
    rsp->npkts_rcvd.cnt++;

    if (!CALL_SMETHOD(pvt->rem_addr, isempty)) {
        /* Check that the packet is authentic, drop if it isn't */
        if (self->asymmetric == 0) {
            if (CALL_SMETHOD(pvt->rem_addr, cmp, sstosa(&packet->raddr),
              packet->rlen) != 0) {
                if (_rtpp_stream_islatched(pvt) && \
                  _rtpp_stream_check_latch_override(pvt, packet, dtime->mono) == 0) {
                    /*
                     * Continue, since there could be good packets in
                     * queue.
                     */
                    CALL_METHOD(self->pcount, reg_ignr);
                    goto discard_and_continue;
                } else if (!_rtpp_stream_islatched(pvt)) {
                    _rtpp_stream_latch(pvt, dtime->mono, packet);
                }
                /* Signal that an address has to be updated */
                _rtpp_stream_fill_addr(pvt, rtcps_wrt, packet);
            } else if (!_rtpp_stream_islatched(pvt)) {
                _rtpp_stream_latch(pvt, dtime->mono, packet);
            }
        } else {
            /*
             * For asymmetric clients don't check
             * source port since it may be different.
             */
            if (!CALL_SMETHOD(pvt->rem_addr, cmphost, sstosa(&packet->raddr))) {
                /*
                 * Continue, since there could be good packets in
                 * queue.
                 */
                CALL_METHOD(self->pcount, reg_ignr);
                goto discard_and_continue;
            }
        }
        CALL_METHOD(self->pcnt_strm, reg_pktin, packet);
    } else {
        CALL_METHOD(self->pcnt_strm, reg_pktin, packet);
        /* Update address recorded in the session */
        _rtpp_stream_fill_addr(pvt, rtcps_wrt, packet);
    }
    if (self->analyzer != NULL) {
        if (CALL_METHOD(self->analyzer, update, packet) == UPDATE_SSRC_CHG) {
            _rtpp_stream_latch(pvt, dtime->mono, packet);
        }
    }
    _rtpp_stream_latch_sync(pvt, dtime->mono, packet);
    if (self->resizer != NULL) {
        rtp_resizer_enqueue(self->resizer, &packet, rsp);
        if (packet == NULL) {
            rsp->npkts_resizer_in.cnt++;
        }
    }
    pthread_mutex_unlock(&pvt->lock);
    return (packet);

discard_and_continue:
    pthread_mutex_unlock(&pvt->lock);
    RTPP_OBJ_DECREF(packet);
    rsp->npkts_discard.cnt++;
    return (RTPP_S_RX_DCONT);
}

static struct rtpp_netaddr *
rtpp_stream_get_rem_addr(struct rtpp_stream *self, int retempty)
{
    struct rtpp_stream_priv *pvt;
    struct rtpp_netaddr *rval;

    PUB2PVT(self, pvt);
    pthread_mutex_lock(&pvt->lock);
    if (retempty == 0 && CALL_SMETHOD(pvt->rem_addr, isempty)) {
        pthread_mutex_unlock(&pvt->lock);
        return (NULL);
    }
    rval = pvt->rem_addr;
    RTPP_OBJ_INCREF(rval);
    pthread_mutex_unlock(&pvt->lock);
    return (rval);
}
