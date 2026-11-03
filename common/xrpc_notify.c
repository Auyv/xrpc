/**
 * @file        xrpc_notify.c
 * @brief       事件通知（订阅发布）实现。
 * @note        发布方运行时数据由节点锁保护；订阅方运行时数据只在 RunOnce 线程访问。
 * @copyright   Copyright (c) 2026 AuyvJpz
 */

#include "xrpc_node_inner.h"
#include "xrpc_notify_inner.h"

#include "xrpc_port.h"

#include <string.h>

/* 序号差落在后半区间，视为发布方重启 */
#define XRPC_NOTIFY_SEQ_HALF  (0x80000000U)

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

/* ack_work 的布局：已确认位图、已拒绝位图、已确认列表、未确认列表 */
static UInt32 L_XrpcAckMaskWords(const TXrpcNotifyPubDesc *desc)
{
    return ((UInt32)desc->ack_node_count + 31U) / 32U;
}

static Bool L_XrpcAckBitGet(const UInt32 *mask, UInt32 index)
{
    return ((mask[index / 32U] & (1UL << (index % 32U))) != 0U) ? True : False;
}

static void L_XrpcAckBitSet(UInt32 *mask, UInt32 index)
{
    mask[index / 32U] |= (1UL << (index % 32U));
}

/* 调用方持节点锁 */
static void L_XrpcAckClearNoLock(const TXrpcNotifyPubDesc *desc)
{
    if (desc->ack_node_count > 0U)
    {
        memset(desc->ack_work, 0, 2U * L_XrpcAckMaskWords(desc) * sizeof(UInt32));
    }
}

static TXrpcNotifyPubObject *L_XrpcFindPub(TXrpcNode *self, UInt32 notify_id)
{
    TXrpcNotifyPubObject *result = NULL;
    UInt32 i = 0U;

    for (i = 0U; (i < self->notify_pub_count) && (result == NULL); i++)
    {
        if (self->notify_pubs[i]->desc->notify_id == notify_id)
        {
            result = self->notify_pubs[i];
        }
    }

    return result;
}

Int32 TXrpcNodeSetNotifyPubs(TXrpcNode *node, TXrpcNotifyPubObject **objects,
                             UInt32 count, UInt32 seq_seed)
{
    Int32 result = kXrpcErrInvalidParam;
    UInt32 i = 0U;

    if ((node != NULL) && (objects != NULL) && (count > 0U))
    {
        result = kXrpcErrOk;
        for (i = 0U; i < count; i++)
        {
            if ((objects[i] == NULL) || (objects[i]->desc == NULL)
                || (objects[i]->runtime == NULL)
                || ((objects[i]->desc->link_index == NULL) && (objects[i]->desc->link_count > 0U))
                || (((objects[i]->desc->ack_node_id == NULL) || (objects[i]->desc->ack_work == NULL))
                    && (objects[i]->desc->ack_node_count > 0U))
                || ((objects[i]->desc->slot_buf == NULL) && (objects[i]->desc->slot_size > 0U)))
            {
                result = kXrpcErrInvalidParam;
            }
        }

        for (i = 0U; (result == kXrpcErrOk) && (i < count); i++)
        {
            memset(objects[i]->runtime, 0, sizeof(*objects[i]->runtime));
            objects[i]->runtime->seq = seq_seed;
        }

        if (result == kXrpcErrOk)
        {
            node->notify_pubs = objects;
            node->notify_pub_count = count;
        }
    }

    return result;
}

Int32 TXrpcNodeSetNotifySubs(TXrpcNode *node, TXrpcNotifySubObject **objects, UInt32 count)
{
    Int32 result = kXrpcErrInvalidParam;
    UInt32 i = 0U;

    if ((node != NULL) && (objects != NULL) && (count > 0U))
    {
        result = kXrpcErrOk;
        for (i = 0U; i < count; i++)
        {
            if ((objects[i] == NULL) || (objects[i]->desc == NULL)
                || (objects[i]->runtime == NULL) || (objects[i]->desc->cb == NULL))
            {
                result = kXrpcErrInvalidParam;
            }
        }

        for (i = 0U; (result == kXrpcErrOk) && (i < count); i++)
        {
            memset(objects[i]->runtime, 0, sizeof(*objects[i]->runtime));
        }

        if (result == kXrpcErrOk)
        {
            node->notify_subs = objects;
            node->notify_sub_count = count;
        }
    }

    return result;
}

Int32 XrpcNotifyPublish(TXrpcNode *node, UInt32 notify_id,
                        const TXrpcMsgParamList *params,
                        const TXrpcNotifyOpt *opt,
                        TXrpcNotifyDoneCb done_cb, void *user_data)
{
    Int32 result = kXrpcErrInvalidParam;
    TXrpcNotifyPubObject *pub = NULL;

    if ((node != NULL) && (opt != NULL))
    {
        UInt16 param_len = (params != NULL) ? params->encode_buffer_used : 0U;

        pub = L_XrpcFindPub(node, notify_id);
        if (pub == NULL)
        {
            result = kXrpcErrItemNotFound;
        }
        else if (param_len > pub->desc->slot_size)
        {
            result = kXrpcErrTxBufferOverflow;
        }
        else if (L_XrpcNodeLock(node) == True)
        {
            TXrpcNotifyPubRuntime *rt = pub->runtime;

            if (rt->state != kXrpcNotifySlotFree)
            {
                rt->replaced_cb = rt->done_cb;
                rt->replaced_user = rt->user_data;
                rt->replaced_seq = rt->seq;
                rt->replaced_count++;
            }

            if (param_len > 0U)
            {
                memcpy(pub->desc->slot_buf, params->encode_buffer_addr, param_len);
            }
            rt->param_len = param_len;
            rt->param_cnt = (params != NULL) ? params->param_cnt : 0U;
            rt->seq = rt->seq + 1U;
            rt->opt = *opt;
            rt->done_cb = done_cb;
            rt->user_data = user_data;
            rt->retry_left = opt->retry;
            rt->attempts = 0U;
            rt->accept_tick = XrpcGetSysTick();
            L_XrpcAckClearNoLock(pub->desc);
            rt->send_code = kXrpcNotifyOk;
            rt->state = kXrpcNotifySlotPending;
            L_XrpcNodeUnlock(node);
            result = kXrpcErrOk;
        }
    }

    return result;
}

static void L_XrpcNotifyFillResult(const TXrpcNotifyPubObject *pub, TXrpcNotifyResultCode code,
                                   TXrpcNotifyResult *result)
{
    const TXrpcNotifyPubRuntime *rt = pub->runtime;
    UInt16 i = 0U;

    memset(result, 0, sizeof(*result));
    result->notify_id = pub->desc->notify_id;
    result->seq = rt->seq;
    result->code = code;
    result->attempts = rt->attempts;
    result->elapsed_ms = XrpcGetSysTick() - rt->accept_tick;

    if ((rt->opt.need_ack == True) && (pub->desc->ack_node_count > 0U))
    {
        UInt32 words = L_XrpcAckMaskWords(pub->desc);
        UInt32 *acked = &pub->desc->ack_work[2U * words];
        UInt32 *missing = &acked[pub->desc->ack_node_count];

        /* 列表只在 RunOnce 线程写，回调期间不会变 */
        result->acked = acked;
        result->missing = missing;
        for (i = 0U; i < pub->desc->ack_node_count; i++)
        {
            if (L_XrpcAckBitGet(pub->desc->ack_work, i) == True)
            {
                acked[result->acked_count] = pub->desc->ack_node_id[i];
                result->acked_count++;
            }
            else
            {
                missing[result->missing_count] = pub->desc->ack_node_id[i];
                result->missing_count++;
            }
        }
    }
}

/* 调用方持节点锁 */
static Bool L_XrpcNotifyAllAckedNoLock(const TXrpcNotifyPubObject *pub)
{
    Bool result = True;
    UInt16 i = 0U;

    for (i = 0U; (i < pub->desc->ack_node_count) && (result == True); i++)
    {
        result = L_XrpcAckBitGet(pub->desc->ack_work, i);
    }

    return result;
}

/* 调用方持节点锁 */
static Bool L_XrpcNotifyAnyRejectedNoLock(const TXrpcNotifyPubObject *pub)
{
    Bool result = False;
    UInt32 words = L_XrpcAckMaskWords(pub->desc);
    UInt32 i = 0U;

    for (i = 0U; (i < words) && (result == False); i++)
    {
        result = (pub->desc->ack_work[words + i] != 0U) ? True : False;
    }

    return result;
}

/* 发到配置的每条链路，返回这一轮的结果 */
static TXrpcNotifyResultCode L_XrpcNotifySendAll(TXrpcNode *self, TXrpcNotifyPubObject *pub,
                                                 UInt32 *sent_seq)
{
    TXrpcNotifyResultCode result = kXrpcNotifyLinkDown;
    UInt16 i = 0U;
    Bool any_ok = False;

    for (i = 0U; i < pub->desc->link_count; i++)
    {
        TXrpcPeerNode *peer = TXrpcPeerCachePeerNode(self->peer_cache, pub->desc->link_index[i]);

        if ((peer != NULL) && (IXrpcLinkState(peer->link) == kXrpcLinkStateConnected))
        {
            Int32 ret = TXrpcNodeSendNotifyEvent(self, peer, pub, sent_seq);

            if (ret >= 0)
            {
                any_ok = True;
            }
            else if ((ret == kXrpcErrFrameTooLarge) || (ret == kXrpcErrBufferOverflow))
            {
                result = kXrpcNotifyTooLarge;
            }
            else if (result != kXrpcNotifyTooLarge)
            {
                result = kXrpcNotifySendFailed;
            }
        }
    }

    return (any_ok == True) ? kXrpcNotifyOk : result;
}

static void L_XrpcNotifyPollOne(TXrpcNode *self, TXrpcNotifyPubObject *pub)
{
    TXrpcNotifyPubRuntime *rt = pub->runtime;
    TXrpcNotifyDoneCb done_cb = NULL;
    void *user_data = NULL;
    TXrpcNotifyResult result;
    Bool need_send = False;
    Bool report = False;
    UInt32 sent_seq = 0U;

    /* 先上报被替换的包，它们比当前这包更早 */
    if (L_XrpcNodeLock(self) == True)
    {
        if (rt->replaced_count > 0U)
        {
            L_XrpcNotifyFillResult(pub, kXrpcNotifyReplaced, &result);
            result.seq = rt->replaced_seq;
            result.replaced_count = rt->replaced_count;
            result.acked_count = 0U;
            result.missing_count = 0U;
            done_cb = rt->replaced_cb;
            user_data = rt->replaced_user;
            report = True;
            rt->replaced_count = 0U;
            rt->replaced_cb = NULL;
            rt->replaced_user = NULL;
        }
        L_XrpcNodeUnlock(self);
    }

    if ((report == True) && (done_cb != NULL))
    {
        done_cb(self, &result, user_data);
    }
    report = False;

    if (L_XrpcNodeLock(self) == True)
    {
        if (rt->state == kXrpcNotifySlotPending)
        {
            need_send = True;
        }
        else if (rt->state == kXrpcNotifySlotWaitAck)
        {
            if (L_XrpcNotifyAllAckedNoLock(pub) == True)
            {
                report = True;
                L_XrpcNotifyFillResult(pub, (L_XrpcNotifyAnyRejectedNoLock(pub) == True)
                                            ? kXrpcNotifyRejected : kXrpcNotifyOk, &result);
            }
            else if ((XrpcGetSysTick() - rt->wait_tick) >= rt->opt.timeout_ms)
            {
                if (rt->retry_left > 0U)
                {
                    rt->retry_left--;
                    need_send = True;
                }
                else
                {
                    report = True;
                    L_XrpcNotifyFillResult(pub, (rt->send_code != kXrpcNotifyOk)
                                                ? rt->send_code : kXrpcNotifyAckTimeout, &result);
                }
            }
        }

        if (report == True)
        {
            done_cb = rt->done_cb;
            user_data = rt->user_data;
            rt->state = kXrpcNotifySlotFree;
        }
        L_XrpcNodeUnlock(self);
    }

    if (report == True)
    {
        if (done_cb != NULL)
        {
            done_cb(self, &result, user_data);
        }
    }
    else if (need_send == True)
    {
        TXrpcNotifyResultCode send_code = L_XrpcNotifySendAll(self, pub, &sent_seq);

        if (L_XrpcNodeLock(self) == True)
        {
            /* 发送期间又有新发布：这一轮作废，下一轮发新数据 */
            if ((rt->seq == sent_seq) || (sent_seq == 0U))
            {
                rt->attempts++;
                rt->send_code = send_code;
                if ((rt->opt.need_ack == True) && (pub->desc->ack_node_count > 0U))
                {
                    rt->wait_tick = XrpcGetSysTick();
                    rt->state = kXrpcNotifySlotWaitAck;
                }
                else
                {
                    report = True;
                    L_XrpcNotifyFillResult(pub, send_code, &result);
                    done_cb = rt->done_cb;
                    user_data = rt->user_data;
                    rt->state = kXrpcNotifySlotFree;
                }
            }
            L_XrpcNodeUnlock(self);
        }

        if ((report == True) && (done_cb != NULL))
        {
            done_cb(self, &result, user_data);
        }
    }
}

void TXrpcNotifyPoll(TXrpcNode *self)
{
    UInt32 i = 0U;

    for (i = 0U; i < self->notify_pub_count; i++)
    {
        L_XrpcNotifyPollOne(self, self->notify_pubs[i]);
    }
}

void TXrpcNotifyHandleAck(TXrpcNode *self,
                          const char *payload, UInt16 payload_len)
{
    if ((self->notify_pub_count > 0U) && (payload_len >= sizeof(TXrpcMsgNotifyAckMem)))
    {
        const TXrpcMsgNotifyAckMem *ack = (const TXrpcMsgNotifyAckMem *)payload;
        UInt32 origin_node = XrpcWireGetU32(ack->origin_node);
        UInt32 ack_seq = XrpcWireGetU32(ack->seq);
        Int32 ack_result = XrpcWireGetI32(ack->result);
        TXrpcNotifyPubObject *pub = (XrpcWireGetU32(ack->publisher) == self->node_id)
                                  ? L_XrpcFindPub(self, XrpcWireGetU32(ack->notify_id)) : NULL;

        if ((pub != NULL) && (L_XrpcNodeLock(self) == True))
        {
            UInt16 i = 0U;

            if ((pub->runtime->state != kXrpcNotifySlotFree) && (pub->runtime->seq == ack_seq))
            {
                for (i = 0U; i < pub->desc->ack_node_count; i++)
                {
                    if (pub->desc->ack_node_id[i] == origin_node)
                    {
                        L_XrpcAckBitSet(pub->desc->ack_work, i);
                        if (ack_result < 0)
                        {
                            L_XrpcAckBitSet(&pub->desc->ack_work[L_XrpcAckMaskWords(pub->desc)], i);
                        }
                    }
                }
            }
            L_XrpcNodeUnlock(self);
        }
    }
}

/* 同一通知可以按发布方配多条：先找写明发布方的，再找通配的 */
static TXrpcNotifySubObject *L_XrpcFindSub(TXrpcNode *self, UInt32 notify_id, UInt32 src_node_id,
                                           Bool *id_known)
{
    TXrpcNotifySubObject *result = NULL;
    TXrpcNotifySubObject *any = NULL;
    UInt32 i = 0U;

    *id_known = False;
    for (i = 0U; i < self->notify_sub_count; i++)
    {
        TXrpcNotifySubObject *sub = self->notify_subs[i];

        if (sub->desc->notify_id == notify_id)
        {
            *id_known = True;
            if (sub->desc->publisher_node_id == src_node_id)
            {
                result = sub;
            }
            else if (sub->desc->publisher_node_id == 0U)
            {
                any = sub;
            }
        }
    }

    return (result != NULL) ? result : any;
}

void TXrpcNotifyHandleEvent(TXrpcNode *self, TXrpcPeerNode *peer_node, UInt32 src_node_id,
                            char *payload, UInt16 payload_len)
{
    if ((self->notify_sub_count > 0U)
        && (payload_len >= (sizeof(TXrpcMsgNotifyEventMem) - 1U)))
    {
        TXrpcMsgNotifyEventMem *event = (TXrpcMsgNotifyEventMem *)payload;
        UInt32 notify_id = XrpcWireGetU32(event->notify_id);
        UInt32 publisher = XrpcWireGetU32(event->origin_node);
        UInt32 seq = XrpcWireGetU32(event->seq);
        Bool need_ack = ((event->flags & XRPC_NOTIFY_FLAG_NEED_ACK) != 0U) ? True : False;
        Bool id_known = False;
        TXrpcNotifySubObject *sub = L_XrpcFindSub(self, notify_id, publisher, &id_known);

        if (sub == NULL)
        {
            /* 没有订阅这个通知很正常，只有订阅了但来源不对才算异常 */
            if (id_known == True)
            {
                TXrpcNodeStatusRecord(self, kXrpcStatusNotifyWrongPublisher, peer_node, publisher,
                                      notify_id, 0U, seq);
            }
        }
        else
        {
            Int32 ack_result = kXrpcErrOk;
            Bool deliver = False;
            TXrpcMsgParamList params;
            UInt16 param_size = (UInt16)(payload_len - (sizeof(TXrpcMsgNotifyEventMem) - 1U));

            if (XrpcWireGetU32(event->notify_key) != sub->desc->notify_key)
            {
                ack_result = kXrpcErrProcessNotFound;
                TXrpcNodeStatusRecord(self, kXrpcStatusNotifyKeyMismatch, peer_node, publisher,
                                      notify_id, 0U, seq);
            }
            else if (((event->param_cnt > 0U) && (param_size == 0U))
                     || (TXrpcMsgParamListCreate(&params, event->param_cnt, event->param_list,
                                                 (param_size > 0U) ? param_size : 1U) != kXrpcErrOk))
            {
                ack_result = kXrpcErrInvalidParam;
                TXrpcNodeStatusRecord(self, kXrpcStatusNotifyBadParams, peer_node, publisher,
                                      notify_id, 0U, seq);
            }
            else
            {
                TXrpcNotifySubRuntime *rt = sub->runtime;

                deliver = True;
                if ((rt->has_seq == True) && (rt->last_publisher == publisher))
                {
                    UInt32 diff = seq - rt->last_seq;

                    if (diff == 0U)
                    {
                        /* 补发：只补 Ack，不重复回调 */
                        deliver = False;
                    }
                    else if ((diff > 1U) && (diff < XRPC_NOTIFY_SEQ_HALF))
                    {
                        TXrpcNodeStatusRecord(self, kXrpcStatusNotifySeqGap, peer_node, publisher,
                                              notify_id, rt->last_seq + 1U, seq);
                    }
                }

                if (deliver == True)
                {
                    rt->has_seq = True;
                    rt->last_seq = seq;
                    rt->last_publisher = publisher;
                    sub->desc->cb(self, notify_id, publisher, seq, &params,
                                  sub->desc->user_data);
                }
            }

            /* 应答发给相邻节点，经中继时由它带回去 */
            if ((need_ack == True) && (sub->desc->send_ack == True))
            {
                (void)TXrpcNodeSendNotifyAck(self, peer_node, src_node_id, notify_id, publisher, seq,
                                             ack_result);
            }
        }
    }
}
