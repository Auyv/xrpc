/**
 * @file        xrpc_relay.c
 * @brief       服务中继实现。
 * @note        路由表和转发调用表由节点锁保护。发送都在节点锁之外。
 *              询问方和调用方的序号原样保存，到提供者一侧使用本节点的帧序号。
 *              帧序号节点内唯一，和本节点自己的调用不会混淆。
 *              一次询问扇出到多条链路时各帧序号相同，应答才能归到同一条路由。
 * @copyright   Copyright (c) 2026 AuyvJpz
 */

#include "xrpc_node_inner.h"
#include "xrpc_relay_inner.h"

#include "xrpc_port.h"

#include <string.h>

#define XRPC_RELAY_KNOCK_TIMEOUT_MS    (500U)
#define XRPC_RELAY_CALL_TIMEOUT_MS     (5000U)
#define XRPC_RELAY_NOTIFY_TIMEOUT_MS   (1000U)

static Bool L_XrpcNodeLock(TXrpcNode *self)
{
    Bool result = True;

    if (self->lock_ops != NULL)
    {
        result = self->lock_ops->lock(self->lock_ops->user_data);
    }

    return result;
}

static void L_XrpcNodeUnlock(TXrpcNode *self)
{
    if (self->lock_ops != NULL)
    {
        (void)self->lock_ops->unlock(self->lock_ops->user_data);
    }
}

/* 这一帧还能不能再转发一跳。收到 1 时减 1 就是 0，到此为止 */
static Bool L_XrpcRelayCanForward(UInt8 ttl)
{
    return (ttl > 1U) ? True : False;
}

static Bool L_XrpcRelayIsLocal(TXrpcNode *self, UInt32 process_id)
{
    TXrpcProcessObject *proc = TXrpcNodeFindProcess(self, process_id);

    return ((proc != NULL) && (proc->static_desc != NULL)
            && (proc->static_desc->exec_cb != NULL)) ? True : False;
}

static TXrpcRelayRoute *L_XrpcRelayFindRoute(TXrpcNode *self, UInt32 process_id, UInt32 process_key)
{
    TXrpcRelayRoute *result = NULL;
    UInt32 i = 0U;

    for (i = 0U; (i < self->relay.route_count) && (result == NULL); i++)
    {
        TXrpcRelayRoute *route = &self->relay.routes[i];

        if ((route->state != kXrpcRelayRouteFree) && (route->process_id == process_id)
            && (route->process_key == process_key))
        {
            result = route;
        }
    }

    return result;
}

static TXrpcRelayRoute *L_XrpcRelayFreeRoute(TXrpcNode *self)
{
    TXrpcRelayRoute *result = NULL;
    UInt32 i = 0U;

    for (i = 0U; (i < self->relay.route_count) && (result == NULL); i++)
    {
        if (self->relay.routes[i].state == kXrpcRelayRouteFree)
        {
            result = &self->relay.routes[i];
        }
    }

    return result;
}

/* 这条链路现在能不能用来询问 */
static Bool L_XrpcRelayPeerUsable(TXrpcNode *self, TXrpcPeerNode *peer, const TXrpcPeerNode *ingress,
                                  UInt32 process_id)
{
    const TXrpcProcessBinding *binding = TXrpcNodeFindBinding(self, process_id);
    Bool result = False;

    if ((peer != ingress) && (IXrpcLinkState(peer->link) == kXrpcLinkStateConnected)
        && ((binding == NULL)
            || (&self->peer_cache->peer_array[binding->link_index] == peer)))
    {
        result = True;
        if ((peer->topology == kXrpcLinkTopologyP2P) && (L_XrpcNodeLock(self) == True))
        {
            result = (peer->peer_node_id != 0U) ? True : False;
            L_XrpcNodeUnlock(self);
        }
    }

    return result;
}

static UInt32 L_XrpcRelayCountPeers(TXrpcNode *self, const TXrpcPeerNode *ingress, UInt32 process_id)
{
    UInt32 result = 0U;
    UInt32 i = 0U;

    for (i = 0U; i < self->peer_cache->peer_count; i++)
    {
        if (L_XrpcRelayPeerUsable(self, &self->peer_cache->peer_array[i], ingress, process_id) == True)
        {
            result++;
        }
    }

    return result;
}

static void L_XrpcRelayReplyKnock(TXrpcNode *self, TXrpcPeerNode *origin_peer, UInt32 origin_node_id,
                                  UInt32 origin_seq, UInt32 process_id, Int32 errcode)
{
    TXrpcMsgKnockRspMem rsp;

    /* 总线上否定应答可能盖过真正的提供者，和普通节点一样不应答 */
    if ((errcode == 1) || (origin_peer->topology == kXrpcLinkTopologyP2P))
    {
        XrpcWirePutU32(rsp.req_seq, origin_seq);
        XrpcWirePutU32(rsp.process_id, process_id);
        XrpcWirePutI32(rsp.errcode, errcode);
        (void)TXrpcNodeRelayForward(self, origin_peer, origin_node_id, kXrpcMsgTypeKnockRsp,
                                    TXrpcNodeNextSeq(self), XRPC_MSG_TTL_DEFAULT,
                                    (const char *)&rsp, sizeof(rsp), False, 0U);
    }
}

static void L_XrpcRelayReplyCallError(TXrpcNode *self, TXrpcPeerNode *origin_peer,
                                      UInt32 origin_node_id, UInt32 origin_seq,
                                      UInt32 process_id, Int32 errcode)
{
    TXrpcMsgCallRspMem rsp;

    XrpcWirePutU32(rsp.req_seq, origin_seq);
    XrpcWirePutU32(rsp.process_id, process_id);
    XrpcWirePutI32(rsp.errcode, errcode);
    rsp.param_cnt = 0U;
    (void)TXrpcNodeRelayForward(self, origin_peer, origin_node_id, kXrpcMsgTypeCallRsp,
                                TXrpcNodeNextSeq(self), XRPC_MSG_TTL_DEFAULT, (const char *)&rsp,
                                (UInt16)(sizeof(rsp) - 1U), False, 0U);
}

static Bool L_XrpcRelayForwardsValid(const TXrpcRelayConfig *config)
{
    Bool result = True;
    UInt32 i = 0U;

    if (((config->forwards == NULL) && (config->forward_count > 0U))
        || ((config->ack_records == NULL) && (config->ack_record_count > 0U)))
    {
        result = False;
    }

    for (i = 0U; (i < config->forward_count) && (result == True); i++)
    {
        if ((config->forwards[i].runtime == NULL)
            || ((config->forwards[i].link_index == NULL) && (config->forwards[i].link_count > 0U)))
        {
            result = False;
        }
    }

    return result;
}

Int32 TXrpcNodeSetRelay(TXrpcNode *node, const TXrpcRelayConfig *config)
{
    Int32 result = kXrpcErrInvalidParam;

    if ((node != NULL) && (config != NULL) && (config->routes != NULL) && (config->calls != NULL)
        && (config->route_count > 0U) && (config->call_count > 0U)
        && (L_XrpcRelayForwardsValid(config) == True))
    {
        UInt32 i = 0U;

        memset(config->routes, 0, sizeof(TXrpcRelayRoute) * config->route_count);
        memset(config->calls, 0, sizeof(TXrpcRelayCall) * config->call_count);
        for (i = 0U; i < config->forward_count; i++)
        {
            memset(config->forwards[i].runtime, 0, sizeof(TXrpcNotifyForwardRuntime));
        }
        if (config->ack_record_count > 0U)
        {
            memset(config->ack_records, 0, sizeof(TXrpcNotifyAckRecord) * config->ack_record_count);
        }
        node->relay = *config;
        if (node->relay.knock_timeout_ms == 0U)
        {
            node->relay.knock_timeout_ms = XRPC_RELAY_KNOCK_TIMEOUT_MS;
        }
        if (node->relay.call_timeout_ms == 0U)
        {
            node->relay.call_timeout_ms = XRPC_RELAY_CALL_TIMEOUT_MS;
        }
        if (node->relay.notify_timeout_ms == 0U)
        {
            node->relay.notify_timeout_ms = XRPC_RELAY_NOTIFY_TIMEOUT_MS;
        }
        node->relay_enabled = True;
        result = kXrpcErrOk;
    }

    return result;
}

Bool TXrpcRelayOnKnockReq(TXrpcNode *self, TXrpcPeerNode *ingress, UInt32 src_node_id,
                          UInt32 req_seq, UInt8 ttl, const TXrpcMsgKnockReqMem *req)
{
    Bool handled = False;
    UInt32 process_id = XrpcWireGetU32(req->process_id);

    if ((self->relay_enabled == True) && (L_XrpcRelayIsLocal(self, process_id) == False))
    {
        UInt32 process_key = XrpcWireGetU32(req->process_key);
        UInt32 origin_seq = req_seq;
        UInt32 candidates = L_XrpcRelayCountPeers(self, ingress, process_id);
        Bool reply_ok = False;
        Bool reply_expired = False;
        Bool reply_full = False;
        Bool flood = False;
        UInt32 seq = 0U;

        if (L_XrpcNodeLock(self) == True)
        {
            TXrpcRelayRoute *route = L_XrpcRelayFindRoute(self, process_id, process_key);

            if ((route == NULL) && (candidates > 0U)
                && (L_XrpcRelayCanForward(ttl) == False))
            {
                TXrpcNodeStatusRecordNoLock(self, kXrpcStatusTtlExpired, ingress, src_node_id,
                                            process_id, 0U, ttl);
                reply_expired = True;
                handled = True;
            }
            else if (route == NULL)
            {
                route = L_XrpcRelayFreeRoute(self);
                if ((route != NULL) && (candidates > 0U))
                {
                    seq = TXrpcNodeNextSeqNoLock(self);
                    route->process_id = process_id;
                    route->process_key = process_key;
                    route->seq = seq;
                    route->origin_peer = ingress;
                    route->origin_node_id = src_node_id;
                    route->origin_seq = origin_seq;
                    route->start_tick = XrpcGetSysTick();
                    route->replies_left = candidates;
                    route->negative_code = kXrpcErrProcessNotFound;
                    route->state = kXrpcRelayRoutePending;
                    flood = True;
                    handled = True;
                }
                else if (candidates > 0U)
                {
                    /* 过程可能存在，只是路由表放不下，不能回"不存在" */
                    TXrpcNodeStatusRecordNoLock(self, kXrpcStatusRouteTableFull, ingress,
                                                src_node_id, process_id,
                                                self->relay.route_count + 1U,
                                                self->relay.route_count);
                    reply_full = True;
                    handled = True;
                }
            }
            else if (route->state == kXrpcRelayRouteReady)
            {
                if (route->peer_node != ingress)
                {
                    reply_ok = True;
                    handled = True;
                }
            }
            else if (route->origin_peer == ingress)
            {
                /* 询问方重试：等结果出来按最新的序号回复 */
                route->origin_node_id = src_node_id;
                route->origin_seq = origin_seq;
                handled = True;
            }
            L_XrpcNodeUnlock(self);
        }

        if (reply_ok == True)
        {
            L_XrpcRelayReplyKnock(self, ingress, src_node_id, origin_seq, process_id, 1);
        }
        else if (reply_expired == True)
        {
            L_XrpcRelayReplyKnock(self, ingress, src_node_id, origin_seq, process_id,
                                  kXrpcErrTtlExpired);
        }
        else if (reply_full == True)
        {
            L_XrpcRelayReplyKnock(self, ingress, src_node_id, origin_seq, process_id,
                                  kXrpcErrNoFreeSlot);
        }

        if (flood == True)
        {
            TXrpcMsgKnockReqMem ask;
            UInt32 i = 0U;

            XrpcWirePutU32(ask.process_id, process_id);
            XrpcWirePutU32(ask.process_key, process_key);
            for (i = 0U; i < self->peer_cache->peer_count; i++)
            {
                TXrpcPeerNode *peer = &self->peer_cache->peer_array[i];

                if (L_XrpcRelayPeerUsable(self, peer, ingress, process_id) == True)
                {
                    const TXrpcProcessBinding *binding = TXrpcNodeFindBinding(self, process_id);
                    UInt32 dst_node_id = ((binding != NULL) && (binding->node_id != 0U))
                                       ? binding->node_id : kXrpcNodeIdBroadcast;

                    (void)TXrpcNodeRelayForward(self, peer, dst_node_id, kXrpcMsgTypeKnockReq,
                                                seq, (UInt8)(ttl - 1U),
                                                (const char *)&ask, sizeof(ask), False, 0U);
                }
            }
        }
    }

    return handled;
}

Bool TXrpcRelayOnKnockRsp(TXrpcNode *self, TXrpcPeerNode *peer_node, UInt32 src_node_id,
                          const TXrpcMsgKnockRspMem *rsp)
{
    Bool handled = False;

    if (self->relay_enabled == True)
    {
        TXrpcPeerNode *origin_peer = NULL;
        UInt32 origin_node_id = 0U;
        UInt32 origin_seq = 0U;
        UInt32 req_seq = XrpcWireGetU32(rsp->req_seq);
        UInt32 process_id = XrpcWireGetU32(rsp->process_id);
        Int32 errcode = XrpcWireGetI32(rsp->errcode);
        Bool reply = False;
        const TXrpcProcessBinding *binding = TXrpcNodeFindBinding(self, process_id);
        Bool binding_ok = ((binding == NULL) || (binding->node_id == 0U)
                           || (binding->node_id == src_node_id)) ? True : False;

        if (L_XrpcNodeLock(self) == True)
        {
            UInt32 i = 0U;

            for (i = 0U; i < self->relay.route_count; i++)
            {
                TXrpcRelayRoute *route = &self->relay.routes[i];

                if ((route->state == kXrpcRelayRoutePending) && (route->seq == req_seq)
                    && (route->process_id == process_id))
                {
                    handled = True;
                    if (binding_ok == False)
                    {
                        /* 绑定了别的提供者，这条应答不算数 */
                    }
                    else if (errcode == 1)
                    {
                        route->state = kXrpcRelayRouteReady;
                        route->peer_node = peer_node;
                        route->provider_node_id = src_node_id;
                        reply = True;
                    }
                    else
                    {
                        if (errcode == 0)
                        {
                            route->negative_code = 0;
                        }
                        route->replies_left--;
                        if (route->replies_left == 0U)
                        {
                            errcode = route->negative_code;
                            route->state = kXrpcRelayRouteFree;
                            reply = True;
                        }
                    }

                    if (reply == True)
                    {
                        origin_peer = route->origin_peer;
                        origin_node_id = route->origin_node_id;
                        origin_seq = route->origin_seq;
                    }
                }
            }
            L_XrpcNodeUnlock(self);
        }

        if (reply == True)
        {
            L_XrpcRelayReplyKnock(self, origin_peer, origin_node_id, origin_seq, process_id, errcode);
        }
    }

    return handled;
}

Bool TXrpcRelayOnCallReq(TXrpcNode *self, TXrpcPeerNode *ingress, UInt32 src_node_id,
                         UInt32 req_seq, UInt8 ttl, const char *payload, UInt16 payload_len)
{
    Bool handled = False;
    const TXrpcMsgCallReqMem *req = (const TXrpcMsgCallReqMem *)payload;
    UInt32 process_id = XrpcWireGetU32(req->process_id);

    if ((self->relay_enabled == True) && (L_XrpcRelayIsLocal(self, process_id) == False))
    {
        TXrpcPeerNode *target = NULL;
        TXrpcRelayCall *call = NULL;
        UInt32 provider_node_id = 0U;
        UInt32 seq = 0U;
        UInt32 origin_seq = req_seq;
        Int32 result = kXrpcErrOk;

        if (L_XrpcNodeLock(self) == True)
        {
            TXrpcRelayRoute *route = L_XrpcRelayFindRoute(self, process_id,
                                                          XrpcWireGetU32(req->process_key));

            if ((route != NULL) && (route->state == kXrpcRelayRouteReady)
                && (route->peer_node != ingress) && (L_XrpcRelayCanForward(ttl) == False))
            {
                TXrpcNodeStatusRecordNoLock(self, kXrpcStatusTtlExpired, ingress, src_node_id,
                                            process_id, 0U, ttl);
                handled = True;
                result = kXrpcErrTtlExpired;
            }
            else if ((route != NULL) && (route->state == kXrpcRelayRouteReady)
                     && (route->peer_node != ingress))
            {
                UInt32 i = 0U;

                handled = True;
                target = route->peer_node;
                provider_node_id = route->provider_node_id;
                for (i = 0U; (i < self->relay.call_count) && (call == NULL); i++)
                {
                    if (self->relay.calls[i].used == False)
                    {
                        call = &self->relay.calls[i];
                    }
                }

                if (call != NULL)
                {
                    seq = TXrpcNodeNextSeqNoLock(self);
                    call->used = True;
                    call->process_id = process_id;
                    call->seq = seq;
                    call->target_peer = target;
                    call->origin_peer = ingress;
                    call->origin_node_id = src_node_id;
                    call->origin_seq = origin_seq;
                    call->start_tick = XrpcGetSysTick();
                    call->fail_code = 0;
                }
                else
                {
                    TXrpcNodeStatusRecordNoLock(self, kXrpcStatusRelayCallFull, ingress, src_node_id,
                                                process_id, self->relay.call_count + 1U,
                                                self->relay.call_count);
                    result = kXrpcErrNoFreeSlot;
                }
            }
            L_XrpcNodeUnlock(self);
        }

        if (call != NULL)
        {
            result = TXrpcNodeRelayForward(self, target, provider_node_id, kXrpcMsgTypeCallReq,
                                           seq, (UInt8)(ttl - 1U),
                                           payload, payload_len, False, 0U);
            if ((result < 0) && (L_XrpcNodeLock(self) == True))
            {
                call->used = False;
                L_XrpcNodeUnlock(self);
            }
        }

        if ((handled == True) && (result < 0))
        {
            L_XrpcRelayReplyCallError(self, ingress, src_node_id, origin_seq, process_id, result);
        }
    }

    return handled;
}

Bool TXrpcRelayOnCallRsp(TXrpcNode *self, TXrpcPeerNode *peer_node,
                         const char *payload, UInt16 payload_len)
{
    Bool handled = False;
    const TXrpcMsgCallRspMem *rsp = (const TXrpcMsgCallRspMem *)payload;

    if (self->relay_enabled == True)
    {
        TXrpcPeerNode *origin_peer = NULL;
        UInt32 origin_node_id = 0U;
        UInt32 origin_seq = 0U;
        UInt32 req_seq = XrpcWireGetU32(rsp->req_seq);
        UInt32 process_id = XrpcWireGetU32(rsp->process_id);
        Bool matched = False;

        if (L_XrpcNodeLock(self) == True)
        {
            UInt32 i = 0U;

            for (i = 0U; (i < self->relay.call_count) && (matched == False); i++)
            {
                TXrpcRelayCall *call = &self->relay.calls[i];

                if ((call->used == True) && (call->fail_code == 0)
                    && (call->seq == req_seq) && (call->process_id == process_id)
                    && (call->target_peer == peer_node))
                {
                    origin_peer = call->origin_peer;
                    origin_node_id = call->origin_node_id;
                    origin_seq = call->origin_seq;
                    call->used = False;
                    matched = True;
                }
            }

            /* 提供者已经没有这个过程：路由作废，调用方收到 -8 后重新发现 */
            if ((matched == True) && (XrpcWireGetI32(rsp->errcode) == kXrpcErrProcessNotFound))
            {
                for (i = 0U; i < self->relay.route_count; i++)
                {
                    TXrpcRelayRoute *route = &self->relay.routes[i];

                    if ((route->state == kXrpcRelayRouteReady) && (route->process_id == process_id)
                        && (route->peer_node == peer_node))
                    {
                        route->state = kXrpcRelayRouteFree;
                    }
                }
            }
            L_XrpcNodeUnlock(self);
        }

        if (matched == True)
        {
            Int32 result = TXrpcNodeRelayForward(self, origin_peer, origin_node_id,
                                                 kXrpcMsgTypeCallRsp, TXrpcNodeNextSeq(self),
                                                 XRPC_MSG_TTL_DEFAULT,
                                                 payload, payload_len, True, origin_seq);

            handled = True;

            if (result < 0)
            {
                L_XrpcRelayReplyCallError(self, origin_peer, origin_node_id, origin_seq,
                                          process_id, result);
            }
        }
    }

    return handled;
}

static const TXrpcNotifyForward *L_XrpcRelayFindForward(const TXrpcNode *self, UInt32 notify_id)
{
    const TXrpcNotifyForward *result = NULL;
    UInt32 i = 0U;

    for (i = 0U; (i < self->relay.forward_count) && (result == NULL); i++)
    {
        if (self->relay.forwards[i].notify_id == notify_id)
        {
            result = &self->relay.forwards[i];
        }
    }

    return result;
}

static TXrpcNotifyAckRecord *L_XrpcRelayFindRecord(TXrpcNode *self, UInt32 notify_id,
                                                   UInt32 publisher, UInt32 seq)
{
    TXrpcNotifyAckRecord *result = NULL;
    UInt32 i = 0U;

    for (i = 0U; (i < self->relay.ack_record_count) && (result == NULL); i++)
    {
        TXrpcNotifyAckRecord *record = &self->relay.ack_records[i];

        if ((record->used == True) && (record->notify_id == notify_id)
            && (record->publisher == publisher) && (record->seq == seq))
        {
            result = record;
        }
    }

    return result;
}

/* 调用方持节点锁。表满时选一项淘汰：优先最老的已转发过 Ack 的，它多半已经收齐；
 * 中继不知道有多少订阅者，只能到时释放，不淘汰的话容量会变成"速率 x 保留时长" */
static TXrpcNotifyAckRecord *L_XrpcRelayEvictRecord(TXrpcNode *self)
{
    TXrpcNotifyAckRecord *result = NULL;
    UInt32 now_tick = XrpcGetSysTick();
    UInt32 i = 0U;

    for (i = 0U; i < self->relay.ack_record_count; i++)
    {
        TXrpcNotifyAckRecord *record = &self->relay.ack_records[i];

        if ((result == NULL) || ((record->acked == True) && (result->acked == False))
            || ((record->acked == result->acked)
                && ((now_tick - record->start_tick) > (now_tick - result->start_tick))))
        {
            result = record;
        }
    }

    return result;
}

/* 调用方持节点锁。发布方补发时记录已经在表里，只刷新 */
static void L_XrpcRelayRememberEventNoLock(TXrpcNode *self, TXrpcPeerNode *ingress,
                                           UInt32 src_node_id, UInt32 notify_id,
                                           UInt32 publisher, UInt32 seq)
{
    TXrpcNotifyAckRecord *record = L_XrpcRelayFindRecord(self, notify_id, publisher, seq);
    Bool is_new = (record == NULL) ? True : False;
    UInt32 i = 0U;

    for (i = 0U; (i < self->relay.ack_record_count) && (record == NULL); i++)
    {
        if (self->relay.ack_records[i].used == False)
        {
            record = &self->relay.ack_records[i];
        }
    }

    if (record == NULL)
    {
        record = L_XrpcRelayEvictRecord(self);
        /* 还没收到过 Ack 的记录被淘汰，这个事件的 Ack 可能回不去，发布方超时后补发 */
        if ((record != NULL) && (record->acked == False))
        {
            TXrpcNodeStatusRecordNoLock(self, kXrpcStatusNotifyRecordFull, ingress, publisher,
                                        notify_id, self->relay.ack_record_count + 1U,
                                        self->relay.ack_record_count);
        }
    }

    if (record != NULL)
    {
        if (is_new == True)
        {
            record->acked = False;
        }
        record->used = True;
        record->notify_id = notify_id;
        record->publisher = publisher;
        record->seq = seq;
        record->origin_peer = ingress;
        record->origin_node_id = src_node_id;
        record->start_tick = XrpcGetSysTick();
    }
}

void TXrpcRelayOnNotifyEvent(TXrpcNode *self, TXrpcPeerNode *ingress, UInt32 src_node_id, UInt8 ttl,
                             const char *payload, UInt16 payload_len)
{
    const TXrpcMsgNotifyEventMem *event = (const TXrpcMsgNotifyEventMem *)payload;
    UInt32 notify_id = XrpcWireGetU32(event->notify_id);
    UInt32 publisher = XrpcWireGetU32(event->origin_node);
    UInt32 seq = XrpcWireGetU32(event->seq);
    const TXrpcNotifyForward *forward = (self->relay_enabled == True)
                                      ? L_XrpcRelayFindForward(self, notify_id) : NULL;

    /* 自己发布的事件绕回来了，不再转发 */
    if ((forward != NULL) && (publisher != self->node_id) && (L_XrpcNodeLock(self) == True))
    {
        TXrpcNotifyForwardRuntime *rt = forward->runtime;
        Bool go = True;
        UInt16 i = 0U;

        /* 同一事件从别的链路再到达是绕回来的，从同一条链路再到达是发布方补发 */
        if ((rt->has_event == True) && (rt->origin_node == publisher) && (rt->seq == seq)
            && (rt->ingress != ingress))
        {
            go = False;
        }
        else if (L_XrpcRelayCanForward(ttl) == False)
        {
            TXrpcNodeStatusRecordNoLock(self, kXrpcStatusTtlExpired, ingress, publisher, notify_id,
                                        0U, ttl);
            go = False;
        }
        else
        {
            rt->has_event = True;
            rt->origin_node = publisher;
            rt->seq = seq;
            rt->ingress = ingress;
            if ((event->flags & XRPC_NOTIFY_FLAG_NEED_ACK) != 0U)
            {
                L_XrpcRelayRememberEventNoLock(self, ingress, src_node_id, notify_id, publisher, seq);
            }
        }
        L_XrpcNodeUnlock(self);

        for (i = 0U; (i < forward->link_count) && (go == True); i++)
        {
            TXrpcPeerNode *peer = TXrpcPeerCachePeerNode(self->peer_cache, forward->link_index[i]);

            if ((peer != NULL) && (peer != ingress)
                && (IXrpcLinkState(peer->link) == kXrpcLinkStateConnected))
            {
                (void)TXrpcNodeRelayForward(self, peer, kXrpcNodeIdBroadcast, kXrpcMsgTypeNotifyEvent,
                                            TXrpcNodeNextSeq(self), (UInt8)(ttl - 1U),
                                            payload, payload_len, False, 0U);
            }
        }
    }
}

Bool TXrpcRelayOnNotifyAck(TXrpcNode *self, const char *payload, UInt16 payload_len)
{
    Bool handled = False;
    const TXrpcMsgNotifyAckMem *ack = (const TXrpcMsgNotifyAckMem *)payload;
    UInt32 publisher = XrpcWireGetU32(ack->publisher);

    if ((self->relay_enabled == True) && (publisher != self->node_id))
    {
        TXrpcPeerNode *origin_peer = NULL;
        UInt32 origin_node_id = 0U;

        /* 找不到记录也不用本地处理：这条 Ack 不是给本节点的 */
        handled = True;
        if (L_XrpcNodeLock(self) == True)
        {
            TXrpcNotifyAckRecord *record = L_XrpcRelayFindRecord(self,
                                                                 XrpcWireGetU32(ack->notify_id),
                                                                 publisher,
                                                                 XrpcWireGetU32(ack->seq));

            if (record != NULL)
            {
                record->acked = True;
                origin_peer = record->origin_peer;
                origin_node_id = record->origin_node_id;
            }
            L_XrpcNodeUnlock(self);
        }

        if (origin_peer != NULL)
        {
            (void)TXrpcNodeRelayForward(self, origin_peer, origin_node_id, kXrpcMsgTypeNotifyAck,
                                        TXrpcNodeNextSeq(self), XRPC_MSG_TTL_DEFAULT,
                                        payload, payload_len, False, 0U);
        }
    }

    return handled;
}

void TXrpcRelayPoll(TXrpcNode *self)
{
    if (self->relay_enabled == True)
    {
        UInt32 now_tick = XrpcGetSysTick();
        UInt32 i = 0U;

        if (L_XrpcNodeLock(self) == True)
        {
            for (i = 0U; i < self->relay.ack_record_count; i++)
            {
                TXrpcNotifyAckRecord *record = &self->relay.ack_records[i];

                /* 不知道有多少订阅者，只能到时释放 */
                if ((record->used == True)
                    && ((now_tick - record->start_tick) >= self->relay.notify_timeout_ms))
                {
                    record->used = False;
                }
            }
            L_XrpcNodeUnlock(self);
        }

        for (i = 0U; i < self->relay.route_count; i++)
        {
            TXrpcRelayRoute *route = &self->relay.routes[i];
            TXrpcPeerNode *origin_peer = NULL;
            UInt32 origin_node_id = 0U;
            UInt32 origin_seq = 0U;
            UInt32 process_id = 0U;
            Int32 code = 0;

            if (L_XrpcNodeLock(self) == True)
            {
                if ((route->state == kXrpcRelayRoutePending)
                    && ((now_tick - route->start_tick) >= self->relay.knock_timeout_ms))
                {
                    origin_peer = route->origin_peer;
                    origin_node_id = route->origin_node_id;
                    origin_seq = route->origin_seq;
                    process_id = route->process_id;
                    code = route->negative_code;
                    route->state = kXrpcRelayRouteFree;
                }
                L_XrpcNodeUnlock(self);
            }

            if (origin_peer != NULL)
            {
                L_XrpcRelayReplyKnock(self, origin_peer, origin_node_id, origin_seq, process_id, code);
            }
        }

        for (i = 0U; i < self->relay.call_count; i++)
        {
            TXrpcRelayCall *call = &self->relay.calls[i];
            TXrpcPeerNode *origin_peer = NULL;
            UInt32 origin_node_id = 0U;
            UInt32 origin_seq = 0U;
            UInt32 process_id = 0U;
            Int32 code = 0;

            if (L_XrpcNodeLock(self) == True)
            {
                if (call->used == True)
                {
                    code = call->fail_code;
                    if ((code == 0) && ((now_tick - call->start_tick) >= self->relay.call_timeout_ms))
                    {
                        code = kXrpcErrTimeout;
                    }

                    if (code != 0)
                    {
                        origin_peer = call->origin_peer;
                        origin_node_id = call->origin_node_id;
                        origin_seq = call->origin_seq;
                        process_id = call->process_id;
                        call->used = False;
                    }
                }
                L_XrpcNodeUnlock(self);
            }

            if (origin_peer != NULL)
            {
                L_XrpcRelayReplyCallError(self, origin_peer, origin_node_id, origin_seq,
                                          process_id, code);
            }
        }
    }
}

void TXrpcRelayOnLinkDownNoLock(TXrpcNode *self, TXrpcPeerNode *peer_node)
{
    if (self->relay_enabled == True)
    {
        UInt32 i = 0U;

        for (i = 0U; i < self->relay.route_count; i++)
        {
            TXrpcRelayRoute *route = &self->relay.routes[i];

            if (((route->state == kXrpcRelayRouteReady) && (route->peer_node == peer_node))
                || ((route->state == kXrpcRelayRoutePending) && (route->origin_peer == peer_node)))
            {
                route->state = kXrpcRelayRouteFree;
            }
        }

        for (i = 0U; i < self->relay.ack_record_count; i++)
        {
            if ((self->relay.ack_records[i].used == True)
                && (self->relay.ack_records[i].origin_peer == peer_node))
            {
                self->relay.ack_records[i].used = False;
            }
        }

        for (i = 0U; i < self->relay.forward_count; i++)
        {
            if (self->relay.forwards[i].runtime->ingress == peer_node)
            {
                self->relay.forwards[i].runtime->has_event = False;
            }
        }

        for (i = 0U; i < self->relay.call_count; i++)
        {
            TXrpcRelayCall *call = &self->relay.calls[i];

            if ((call->used == True) && (call->origin_peer == peer_node))
            {
                call->used = False;
            }
            else if ((call->used == True) && (call->target_peer == peer_node))
            {
                /* 这里持着节点锁不能发送，下一轮 RunOnce 回给调用方 */
                call->fail_code = kXrpcErrPeerStatusInvalid;
            }
        }
    }
}
