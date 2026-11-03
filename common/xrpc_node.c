/**
 * @file        xrpc_node.c
 * @brief       节点的公开接口：创建与装配、同步和异步调用、轮询。
 * @copyright   Copyright (c) 2026 AuyvJpz
 */

#include "xrpc/xrpc_node.h"
#include "xrpc/xrpc.h"
#include "xrpc_peer_inner.h"
#include "xrpc_node_inner.h"
#include "xrpc_notify_inner.h"
#include "xrpc_relay_inner.h"

#include "xrpc_port.h"

#include <string.h>

#define XRPC_PROCESS_AUTO_KNOCK_END_MS         (1000U)


static const char *L_XrpcCallStepName(TXrpcProcessCallStep step)
{
    const char *result = "Unknown";

    switch (step)
    {
    case kXrpcProcessCallStepInvalid:
        result = "Invalid";
        break;
    case kXrpcProcessCallStepDetecting:
        result = "Detecting";
        break;
    case kXrpcProcessCallStepIdle:
        result = "Idle";
        break;
    case kXrpcProcessCallStepWaitRsp:
        result = "WaitRsp";
        break;
    case kXrpcProcessCallStepNotify:
        result = "Notify";
        break;
    default:
        break;
    }

    return result;
}

static UInt32 L_XrpcNodeId(const TXrpcNode *self)
{
    UInt32 result = 0U;

    if (self != NULL)
    {
        result = self->node_id;
    }

    return result;
}

static Bool L_XrpcNodeLock(TXrpcNode *self)
{
    Bool result = True;

    if ((self != NULL) && (self->lock_ops != NULL))
    {
        result = self->lock_ops->lock(self->lock_ops->user_data);
    }

    return result;
}

static void L_XrpcNodeUnlock(TXrpcNode *self)
{
    if ((self != NULL) && (self->lock_ops != NULL))
    {
        (void)self->lock_ops->unlock(self->lock_ops->user_data);
    }
}

/* 函数名后缀 Locked 表示内部自行加节点锁，NoLock 表示调用方已持节点锁 */

/* 加锁失败按"忙"处理：等待方继续等到超时，不会误判状态已变化 */
static TXrpcProcessCallStep L_XrpcProcessCallStepLocked(TXrpcNode *self,
                                                        TXrpcProcessObject *proc)
{
    TXrpcProcessCallStep result = kXrpcProcessCallStepWaitRsp;

    if (L_XrpcNodeLock(self) == True)
    {
        result = proc->runtime_data->call_step;
        L_XrpcNodeUnlock(self);
    }

    return result;
}

static UInt32 L_XrpcProcessProviderNodeIdLocked(TXrpcNode *self,
                                                TXrpcProcessObject *proc)
{
    UInt32 result = 0U;

    if (L_XrpcNodeLock(self) == True)
    {
        result = proc->runtime_data->provider_node_id;
        L_XrpcNodeUnlock(self);
    }

    return result;
}

static Bool L_XrpcProcessAutoKnockEndedNoLock(TXrpcProcessObject *proc,
                                              UInt32 now_tick)
{
    Bool result = False;

    if ((proc->runtime_data->knock_left_cnt == 0U)
        && ((now_tick - proc->runtime_data->last_knock_tick)
            >= XRPC_PROCESS_AUTO_KNOCK_END_MS))
    {
        result = True;
    }

    return result;
}

static Bool L_XrpcPeerNodeIsReadyLocked(TXrpcNode *self, TXrpcPeerNode *peer_node)
{
    Bool ready = False;

    if ((peer_node->link != NULL)
        && (IXrpcLinkState(peer_node->link) == kXrpcLinkStateConnected)
        && (L_XrpcNodeLock(self) == True))
    {
        /* 总线上没有一对一握手,能找到 peer 就说明远端已学习到 */
        if ((peer_node->topology == kXrpcLinkTopologyBus)
            || ((peer_node->hello_pending == False)
                && (peer_node->peer_node_id != 0U)))
        {
            ready = True;
        }
        L_XrpcNodeUnlock(self);
    }

    return ready;
}

static void L_XrpcCallSlotsPoll(TXrpcNode *self);

Int32 TXrpcNodeCreate(TXrpcNode *self, UInt32 node_id, UInt16 version)
{
    Int32 result = kXrpcErrInvalidParam;

    if ((self != NULL) && ((version >> 8) == (XRPC_PROTOCOL_VERSION >> 8)))
    {
        memset(self, 0, sizeof(*self));
        self->node_id = node_id;
        self->version = version;
        result = kXrpcErrOk;
    }

    return result;
}

void TXrpcNodeDestroy(TXrpcNode *self)
{
    if (self != NULL)
    {
        memset(self, 0, sizeof(*self));
    }
}

Int32 TXrpcNodeSetPeerCache(TXrpcNode *self, TXrpcPeerCache *peer_cache)
{
    Int32 result = kXrpcErrInvalidParam;

    if ((self != NULL) && (peer_cache != NULL))
    {
        self->peer_cache = peer_cache;
        result = TXrpcPeerCacheSetOwner(peer_cache, self);
    }

    return result;
}

Int32 TXrpcNodeSetProcessTable(TXrpcNode *self, TXrpcProcessTable *process_table)
{
    Int32 result = kXrpcErrInvalidParam;

    if ((self != NULL) && (process_table != NULL)
        && (process_table->process_table != NULL)
        && (process_table->count > 0U))
    {
        UInt32 i = 0U;

        self->process_table = *process_table;
        if (L_XrpcNodeLock(self) == True)
        {
            for (i = 0U; i < self->process_table.count; i++)
            {
                TXrpcProcessObject *proc = self->process_table.process_table[i];

                if ((proc != NULL) && (proc->static_desc != NULL)
                    && (proc->runtime_data != NULL)
                    && (proc->static_desc->exec_cb == NULL))
                {
                    proc->runtime_data->provider_node_id = 0U;
                    proc->runtime_data->call_step = kXrpcProcessCallStepInvalid;
                    proc->runtime_data->knock_left_cnt = XRPC_PROCESS_AUTO_KNOCK_INIT_CNT;
                    proc->runtime_data->last_knock_tick = 0U;
                }
            }
            L_XrpcNodeUnlock(self);
            result = kXrpcErrOk;
        }
    }

    return result;
}

Int32 TXrpcNodeSetLockMethod(TXrpcNode *self, const TXrpcLockOps *lock_ops)
{
    Int32 result = kXrpcErrInvalidParam;

    if (self != NULL)
    {
        if ((lock_ops == NULL)
            || ((lock_ops->lock != NULL) && (lock_ops->unlock != NULL)))
        {
            self->lock_ops = lock_ops;
            result = kXrpcErrOk;
        }
    }

    return result;
}

Int32 TXrpcNodeSetCrc16(TXrpcNode *self, IXrpcCrc16 *crc16)
{
    Int32 result = kXrpcErrInvalidParam;

    if ((self != NULL) && (crc16 != NULL) && (crc16->OnCalc_ != NULL))
    {
        self->crc16 = crc16;
        result = kXrpcErrOk;
    }

    return result;
}

Int32 TXrpcNodeSetProcessBindings(TXrpcNode *self, const TXrpcProcessBinding *bindings,
                                  UInt32 count)
{
    Int32 result = kXrpcErrInvalidParam;

    if ((self != NULL) && (bindings != NULL))
    {
        self->bindings = bindings;
        self->binding_count = count;
        result = kXrpcErrOk;
    }

    return result;
}

Int32 TXrpcNodeRunOnce(TXrpcNode *self, UInt8 *link_recv_buff_ptr,
                       UInt32 link_recv_buff_size)
{
    Int32 result = kXrpcErrInvalidParam;

    if ((self != NULL) && (self->peer_cache != NULL)
        && (link_recv_buff_ptr != NULL) && (link_recv_buff_size > 0U))
    {
        result = TXrpcPeerCachePeerRunOnce(self->peer_cache,
                                           link_recv_buff_ptr,
                                           link_recv_buff_size);
        L_XrpcCallSlotsPoll(self);
        TXrpcNotifyPoll(self);
        TXrpcRelayPoll(self);
        TXrpcNodeStatusFlush(self);
    }

    return result;
}

UInt32 TXrpcNodeGetNodeId(TXrpcNode *self)
{
    UInt32 result = 0U;

    if (self != NULL)
    {
        result = self->node_id;
    }

    return result;
}

UInt16 TXrpcNodeGetVersion(TXrpcNode *self)
{
    UInt16 result = 0U;

    if (self != NULL)
    {
        result = self->version;
    }

    return result;
}

Int32 TXrpcNodeSetUserTag(TXrpcNode *self, UInt32 user_tag)
{
    Int32 result = kXrpcErrInvalidParam;

    if (self != NULL)
    {
        self->user_tag = user_tag;
        result = kXrpcErrOk;
    }

    return result;
}

UInt32 TXrpcNodeGetUserTag(TXrpcNode *self)
{
    UInt32 result = 0U;

    if (self != NULL)
    {
        result = self->user_tag;
    }

    return result;
}

UInt32 TXrpcNodeGetUserTagDrops(TXrpcNode *self)
{
    return TXrpcNodeGetStatusCount(self, kXrpcStatusUserTagMismatch);
}

IXrpcLink *TXrpcNodeGetPeerLinkByNodeId(TXrpcNode *self, UInt32 peer_node_id)
{
    IXrpcLink *result = NULL;
    TXrpcPeerNode *peer_node = TXrpcNodeGetPeerNodeByNodeId(self, peer_node_id);

    if (peer_node != NULL)
    {
        result = peer_node->link;
    }

    return result;
}

TXrpcPeerNode *TXrpcNodeGetPeerNodeByNodeId(TXrpcNode *self, UInt32 peer_node_id)
{
    TXrpcPeerNode *result = NULL;
    UInt32 i = 0U;

    if ((self != NULL) && (self->peer_cache != NULL)
        && (self->peer_cache->peer_array != NULL)
        && (L_XrpcNodeLock(self) == True))
    {
        for (i = 0U; (i < self->peer_cache->peer_count) && (result == NULL); i++)
        {
            if (TXrpcPeerNodeOwnsNode(&self->peer_cache->peer_array[i], peer_node_id) == True)
            {
                result = &self->peer_cache->peer_array[i];
            }
        }
        L_XrpcNodeUnlock(self);
    }

    return result;
}

TXrpcPeerCache *TXrpcNodeGetPeerCache(TXrpcNode *self)
{
    TXrpcPeerCache *result = NULL;

    if (self != NULL)
    {
        result = self->peer_cache;
    }

    return result;
}

TXrpcProcessObject *TXrpcNodeFindProcess(TXrpcNode *self, UInt32 process_id)
{
    TXrpcProcessObject *result = NULL;
    UInt32 i = 0U;

    if ((self != NULL) && (self->process_table.process_table != NULL))
    {
        for (i = 0U; i < self->process_table.count; i++)
        {
            TXrpcProcessObject *proc = self->process_table.process_table[i];

            if ((proc != NULL) && (proc->static_desc != NULL)
                && (proc->static_desc->process_id == process_id))
            {
                result = proc;
                break;
            }
        }
    }

    return result;
}

/* 确认远端调用的前置条件。provider 未知时按需补一次 Knock 额度 */
static Int32 L_XrpcNodeResolveRemote(TXrpcNode *self, TXrpcProcessObject *proc, UInt32 process_id,
                                     TXrpcPeerNode **peer_node_out, UInt32 *provider_out)
{
    Int32 result = kXrpcErrOk;
    TXrpcPeerNode *peer_node = NULL;
    UInt32 provider_node_id = L_XrpcProcessProviderNodeIdLocked(self, proc);

    /* provider==0:主动 knock 一次触发发现,但本次调用则返回 kXrpcErrProviderUnknow*/
    if (provider_node_id == 0U)
    {
        UInt32 now_tick = XrpcGetSysTick();
        UInt32 left_cnt = 0U;
        UInt32 last_tick = 0U;
        Bool knock_granted = False;

        result = kXrpcErrProviderUnknow;
        if (L_XrpcNodeLock(self) == True)
        {
            left_cnt = proc->runtime_data->knock_left_cnt;
            last_tick = proc->runtime_data->last_knock_tick;
            if (L_XrpcProcessAutoKnockEndedNoLock(proc, now_tick) == True)
            {
                left_cnt = left_cnt + 1U;
                proc->runtime_data->knock_left_cnt = left_cnt;
                proc->runtime_data->curr_errcode = result;
                proc->runtime_data->call_step = kXrpcProcessCallStepIdle;
                knock_granted = True;
            }
            L_XrpcNodeUnlock(self);
        }

        if (knock_granted == True)
        {
            XRPC_LOG_W("Node(%d) ProcessExec grant KnockReq: process=0x%08x, left_cnt=%u",
                   L_XrpcNodeId(self), process_id, left_cnt);
        }
        else
        {
            XRPC_LOG_T("Node(%d) ProcessExec provider unknown: process=0x%08x, left_cnt=%u, last_tick=%u",
                   L_XrpcNodeId(self), process_id, left_cnt, last_tick);
        }
    }

    if (result == kXrpcErrOk)
    {
        peer_node = TXrpcNodeGetPeerNodeByNodeId(self, provider_node_id);

        if (peer_node == NULL)
        {
            XRPC_LOG_E("Node(%d) ProcessExec provider peer not found: process=0x%08x, provider=0x%08x",
                   L_XrpcNodeId(self), process_id, provider_node_id);
            result = kXrpcErrPeerNodeUnknow;
        }
        else
        {
            Bool peer_ready = L_XrpcPeerNodeIsReadyLocked(self, peer_node);

            XRPC_LOG_W("Node(%d) ProcessExec provider peer found: process=0x%08x, provider=0x%08x, hello_pending=%d",
                   L_XrpcNodeId(self), process_id,
                   provider_node_id,
                   peer_ready == True ? 0 : 1);

           /* 【A3】peer 已找到但 hello_pending 仍 True:
            *   表示上一轮 HelloReq 已发但对端 HelloRsp 还没到,
            *   若强行发 CallReq 可能因对端尚未接受握手而丢失。
            *   此处直接返回失败 提示状态无效。*/
            if (peer_ready == False)
            {
                result = kXrpcErrPeerStatusInvalid;
            }
        }
    }

    *peer_node_out = peer_node;
    *provider_out = provider_node_id;

    return result;
}

/* 申请调用槽并发出请求。失败时槽已归还 */
static Int32 L_XrpcCallSlotStart(TXrpcNode *self, TXrpcProcessObject *proc, UInt32 process_id,
                                 TXrpcPeerNode *peer_node, UInt32 provider_node_id,
                                 const TXrpcMsgParamList *input, TXrpcMsgParamList *output,
                                 UInt32 timeout_ms, UInt8 retry,
                                 TXrpcCallDoneCb done_cb, void *user_data,
                                 TXrpcCallSlot **slot_out)
{
    Int32 result = kXrpcErrNoFreeSlot;
    TXrpcCallSlot *slot = NULL;
    UInt32 seq = 0U;
    UInt32 i = 0U;

    if (L_XrpcNodeLock(self) == True)
    {
        for (i = 0U; (i < self->call_slot_count) && (slot == NULL); i++)
        {
            if (self->call_slots[i].step == kXrpcCallSlotFree)
            {
                slot = &self->call_slots[i];
            }
        }

        if (slot != NULL)
        {
            seq = TXrpcNodeNextSeqNoLock(self);
            proc->runtime_data->process_seq = seq;
            slot->process_id = process_id;
            TXrpcDeliveryStart(&slot->delivery, seq, XrpcGetSysTick(), timeout_ms, retry);
            slot->proc = proc;
            slot->peer_node = peer_node;
            slot->provider_node_id = provider_node_id;
            slot->input = input;
            slot->output = output;
            slot->errcode = kXrpcErrOk;
            slot->done_cb = done_cb;
            slot->user_data = user_data;
            slot->step = kXrpcCallSlotWait;
        }
        L_XrpcNodeUnlock(self);
    }

    if (slot != NULL)
    {
        result = TXrpcNodeSendCallFrame(self, peer_node, proc, seq, provider_node_id, input);
        if (result >= 0)
        {
            result = kXrpcErrOk;
            *slot_out = slot;
        }
        else if (L_XrpcNodeLock(self) == True)
        {
            slot->step = kXrpcCallSlotFree;
            L_XrpcNodeUnlock(self);
        }
    }

    return result;
}

/* 调用方持节点锁。要重发时取新序号，迟到的旧应答对不上，直接丢弃 */
static TXrpcDeliveryAction L_XrpcCallSlotStepNoLock(TXrpcNode *self, TXrpcCallSlot *slot,
                                                    UInt32 *seq_out)
{
    TXrpcDeliveryAction result = TXrpcDeliveryStep(&slot->delivery, XrpcGetSysTick());

    if (result == kXrpcDeliveryResend)
    {
        *seq_out = TXrpcNodeNextSeqNoLock(self);
        slot->delivery.seq = *seq_out;
    }

    return result;
}

/* 发送失败不用处理：下一次超时再发，用尽后以超时结束 */
static void L_XrpcCallSlotResend(TXrpcNode *self, const TXrpcCallSlot *slot, UInt32 seq)
{
    (void)TXrpcNodeSendCallFrame(self, slot->peer_node, slot->proc, seq,
                                 slot->provider_node_id, slot->input);
}

/* 同步调用走调用槽：申请、等待完成、归还 */
static Int32 L_XrpcNodeCallSync(TXrpcNode *self, TXrpcProcessObject *proc, UInt32 process_id,
                                TXrpcPeerNode *peer_node, UInt32 provider_node_id,
                                const TXrpcMsgParamList *input, TXrpcMsgParamList *output,
                                UInt32 timeout_ms, UInt8 retry)
{
    TXrpcCallSlot *slot = NULL;
    Int32 result = L_XrpcCallSlotStart(self, proc, process_id, peer_node, provider_node_id,
                                       input, output, timeout_ms, retry, NULL, NULL, &slot);

    if (result == kXrpcErrOk)
    {
        Bool done = False;
        Bool node_locked = False;

        while (done == False)
        {
            /* 取不到锁时无法判断，按到期处理，不能卡在这里 */
            TXrpcDeliveryAction action = kXrpcDeliveryExpired;
            UInt32 seq = 0U;

            if (L_XrpcNodeLock(self) == True)
            {
                done = (slot->step == kXrpcCallSlotDone) ? True : False;
                if (done == False)
                {
                    action = L_XrpcCallSlotStepNoLock(self, slot, &seq);
                }
                L_XrpcNodeUnlock(self);
            }

            if (done == False)
            {
                if (action == kXrpcDeliveryExpired)
                {
                    break;
                }
                if (action == kXrpcDeliveryResend)
                {
                    L_XrpcCallSlotResend(self, slot, seq);
                }
                XrpcSleepMs(3U);
            }
        }

        /* 取结果和归还放在同一临界区:归还之后迟到的应答找不到槽,不会再写 output */
        node_locked = L_XrpcNodeLock(self);
        result = (slot->step == kXrpcCallSlotDone) ? slot->errcode : kXrpcErrTimeout;
        slot->output = NULL;
        slot->step = kXrpcCallSlotFree;
        if (node_locked == True)
        {
            L_XrpcNodeUnlock(self);
        }
    }

    return result;
}

Int32 TXrpcNodeSetCallSlots(TXrpcNode *self, TXrpcCallSlot *slots, UInt32 count)
{
    Int32 result = kXrpcErrInvalidParam;

    if ((self != NULL) && (slots != NULL) && (count > 0U))
    {
        memset(slots, 0, sizeof(TXrpcCallSlot) * count);
        self->call_slots = slots;
        self->call_slot_count = count;
        result = kXrpcErrOk;
    }

    return result;
}

Int32 TXrpcNodeCallAsync(TXrpcNode *self, UInt32 process_id,
                         const TXrpcMsgParamList *input,
                         TXrpcMsgParamList *output,
                         UInt32 timeout_ms,
                         TXrpcCallDoneCb done_cb, void *user_data)
{
    TXrpcCallOpt opt;

    opt.timeout_ms = timeout_ms;
    opt.retry = 0U;

    return TXrpcNodeCallAsyncOpt(self, process_id, input, output, &opt, done_cb, user_data);
}

Int32 TXrpcNodeCallAsyncOpt(TXrpcNode *self, UInt32 process_id,
                            const TXrpcMsgParamList *input,
                            TXrpcMsgParamList *output,
                            const TXrpcCallOpt *opt,
                            TXrpcCallDoneCb done_cb, void *user_data)
{
    Int32 result = kXrpcErrInvalidParam;
    TXrpcProcessObject *proc = TXrpcNodeFindProcess(self, process_id);

    if ((self != NULL) && (proc != NULL) && (proc->static_desc != NULL)
        && (proc->runtime_data != NULL) && (done_cb != NULL) && (opt != NULL))
    {
        if ((self->call_slots == NULL) || (proc->static_desc->exec_cb != NULL))
        {
            result = kXrpcErrNotSupported;
        }
        else
        {
            TXrpcPeerNode *peer_node = NULL;
            TXrpcCallSlot *slot = NULL;
            UInt32 provider_node_id = 0U;

            result = L_XrpcNodeResolveRemote(self, proc, process_id, &peer_node, &provider_node_id);
            if (result == kXrpcErrOk)
            {
                result = L_XrpcCallSlotStart(self, proc, process_id, peer_node, provider_node_id,
                                             input, output,
                                             (opt->timeout_ms == 0U)
                                                 ? XRPC_DEFAULT_PROCESS_TIMEOUT_MS
                                                 : opt->timeout_ms,
                                             opt->retry, done_cb, user_data, &slot);
            }
        }
    }

    return result;
}

/* RunOnce 线程：结束超时的异步调用，并在锁外回调 */
static void L_XrpcCallSlotsPoll(TXrpcNode *self)
{
    UInt32 i = 0U;

    for (i = 0U; i < self->call_slot_count; i++)
    {
        TXrpcCallSlot *slot = &self->call_slots[i];
        TXrpcCallDoneCb done_cb = NULL;
        UInt32 process_id = 0U;
        Int32 errcode = 0;
        TXrpcMsgParamList *output = NULL;
        void *user_data = NULL;
        TXrpcDeliveryAction action = kXrpcDeliveryWait;
        UInt32 seq = 0U;

        if (L_XrpcNodeLock(self) == True)
        {
            if ((slot->done_cb != NULL) && (slot->step == kXrpcCallSlotWait))
            {
                action = L_XrpcCallSlotStepNoLock(self, slot, &seq);
            }

            if (action == kXrpcDeliveryExpired)
            {
                if (slot->output != NULL)
                {
                    slot->output->encode_buffer_used = 0U;
                    slot->output->param_cnt = 0U;
                }
                slot->errcode = kXrpcErrTimeout;
                slot->step = kXrpcCallSlotDone;
            }

            if ((slot->done_cb != NULL) && (slot->step == kXrpcCallSlotDone))
            {
                done_cb = slot->done_cb;
                process_id = slot->process_id;
                errcode = slot->errcode;
                output = slot->output;
                user_data = slot->user_data;
                slot->done_cb = NULL;
                slot->output = NULL;
                slot->step = kXrpcCallSlotFree;
            }
            L_XrpcNodeUnlock(self);
        }

        if (action == kXrpcDeliveryResend)
        {
            L_XrpcCallSlotResend(self, slot, seq);
        }

        if (done_cb != NULL)
        {
            done_cb(self, process_id, errcode, output, user_data);
        }
    }
}

Int32 TXrpcNodeProcessExec(TXrpcNode *self, UInt32 process_id,
                           const TXrpcMsgParamList *input,
                           TXrpcMsgParamList *output,
                           UInt32 timeout_ms)
{
    TXrpcCallOpt opt;

    opt.timeout_ms = timeout_ms;
    opt.retry = 0U;

    return TXrpcNodeProcessExecOpt(self, process_id, input, output, &opt);
}

Int32 TXrpcNodeProcessExecOpt(TXrpcNode *self, UInt32 process_id,
                              const TXrpcMsgParamList *input,
                              TXrpcMsgParamList *output,
                              const TXrpcCallOpt *opt)
{
    Int32 result = kXrpcErrInvalidParam;
    TXrpcProcessObject *proc = TXrpcNodeFindProcess(self, process_id);
    UInt32 wait_ms = (opt != NULL) ? opt->timeout_ms : 0U;
    Bool do_remote_call = False;  ///< 本机 exec_cb==NULL 时改走 CallReq

    if (wait_ms == 0U)
    {
        wait_ms = XRPC_DEFAULT_PROCESS_TIMEOUT_MS;
    }

    XRPC_LOG_T("Node(%d) ProcessExec enter: process=0x%08x, timeout=%u, key=0x%08x",
           L_XrpcNodeId(self), process_id, wait_ms,
           ((proc != NULL) && (proc->static_desc != NULL)) ? proc->static_desc->process_key : 0U);

    if ((self != NULL) && (proc != NULL) && (opt != NULL)
        && (proc->static_desc != NULL))
    {
        if (proc->static_desc->exec_cb != NULL)
        {
            XRPC_LOG_T("Node(%d) ProcessExec local exec: process=0x%08x",
                   L_XrpcNodeId(self), process_id);
            result = proc->static_desc->exec_cb(input, output, proc->static_desc->user_data);
        }
        else if ((opt->retry > 0U) && (self->call_slots == NULL))
        {
            /* 无槽路径没有地方记重发状态，不能静默按不重发处理 */
            result = kXrpcErrNotSupported;
        }
        else if (proc->runtime_data != NULL)
        {
            UInt32 provider_node_id = L_XrpcProcessProviderNodeIdLocked(self, proc);
            TXrpcProcessCallStep call_step = L_XrpcProcessCallStepLocked(self, proc);

            /* exec_cb==NULL 表示本机不实现,语义为"远端调用"。
             *  - provider 已知 → 直接走 CallReq
             *  - provider 未知 → 主动发一次 KnockReq 触发发现,
             *    拿到 provider 后再走 CallReq
             * 这样 mpu 端把远端 process 静态注册进来即可使用,无需
             * 业务方手工管理 knock 流程。*/
            do_remote_call = True;
            XRPC_LOG_T("Node(%d) ProcessExec remote: process=0x%08x, provider=0x%08x, step=%d(%s)",
                   L_XrpcNodeId(self), process_id,
                   provider_node_id,
                   call_step,
                   L_XrpcCallStepName(call_step));
        }
        else
        {
            result = kXrpcErrProcessStatus;
        }

        if (do_remote_call == True)
        {
            TXrpcPeerNode *peer_node = NULL;
            UInt32 begin_tick = XrpcGetSysTick();
            UInt32 provider_node_id = 0U;

            result = L_XrpcNodeResolveRemote(self, proc, process_id, &peer_node, &provider_node_id);

            /* 启用调用槽后走槽,否则保持原有的"每过程一个在途调用" */
            if ((result == kXrpcErrOk) && (self->call_slots != NULL))
            {
                result = L_XrpcNodeCallSync(self, proc, process_id, peer_node, provider_node_id,
                                            input, output, wait_ms, opt->retry);
                do_remote_call = False;
            }

            /* call_step 应当已被 KnockRsp 推到 Idle;此处保险判断 */
            if ((result == kXrpcErrOk) && (do_remote_call == True))
            {
                TXrpcProcessCallStep step;

                step = L_XrpcProcessCallStepLocked(self, proc);

                if (step <= kXrpcProcessCallStepDetecting)
                {
                    result = kXrpcErrProcessStatus;
                }
                else
                {
                    while ((step = L_XrpcProcessCallStepLocked(self, proc))
                           != kXrpcProcessCallStepIdle)
                    {
                        if ((XrpcGetSysTick() - begin_tick) >= wait_ms)
                        {
                            XRPC_LOG_E("Node(%d) ProcessExec wait Idle timeout: process=0x%08x, step=%d(%s)",
                                L_XrpcNodeId(self), process_id,
                                step,
                                L_XrpcCallStepName(step));
                            result = kXrpcErrTimeout;
                            break;
                        }
                        else
                        {
                            XrpcSleepMs(3U);
                        }
                    }
                }
            }

            if ((result == kXrpcErrOk) && (do_remote_call == True))
            {
                Bool call_sent = False;
                Bool call_owned = False;
                Bool node_locked = False;

                XRPC_LOG_T("Node(%d) ProcessExec send CallReq: process=0x%08x, provider=0x%08x",
                       L_XrpcNodeId(self), process_id, provider_node_id);
                result = TXrpcNodeSendCallReq(self, peer_node, process_id,
                                              (TXrpcMsgParamList *)input,
                                              output);
                XRPC_LOG_W("Node(%d) ProcessExec send CallReq done: process=0x%08x, result=%d",
                       L_XrpcNodeId(self), process_id, result);

                /* ProcessStatus 表示过程正被别的调用占用,运行状态不属于本次调用 */
                call_owned = (result != kXrpcErrProcessStatus) ? True : False;

                if (result >= 0)
                {
                    call_sent = True;
                    result = kXrpcErrTimeout;
                    while (True)
                    {
                        TXrpcProcessCallStep step = L_XrpcProcessCallStepLocked(self, proc);

                        if (step != kXrpcProcessCallStepWaitRsp)
                        {
                            break;
                        }

                        if ((XrpcGetSysTick() - begin_tick) >= wait_ms)
                        {
                            XRPC_LOG_E("Node(%d) ProcessExec wait CallRsp timeout: process=0x%08x, step=%d(%s)",
                                   L_XrpcNodeId(self), process_id,
                                   step,
                                   L_XrpcCallStepName(step));
                            break;
                        }
                        XrpcSleepMs(3U);
                    }
                }

                /* 取结果和复位放在同一临界区:解锁之后迟到的应答只会看到 Idle,
                 * 不会再写调用方的 output。无论成功 / 失败 / 超时,都把 call_step
                 * 拉回 Idle,防止 SendCallReq 已做复位但后续二次进入时残留 */
                if (call_owned == True)
                {
                    node_locked = L_XrpcNodeLock(self);
                    if ((call_sent == True)
                        && (proc->runtime_data->call_step == kXrpcProcessCallStepNotify))
                    {
                        result = proc->runtime_data->curr_errcode;
                    }
                    proc->runtime_data->curr_output = NULL;
                    proc->runtime_data->call_step = kXrpcProcessCallStepIdle;
                    if (node_locked == True)
                    {
                        L_XrpcNodeUnlock(self);
                    }
                }
            }
        }
    }
    else
    {
        XRPC_LOG_E("Node(%d) ProcessExec invalid entry: self=%p, proc=%p, process=0x%08x",
               L_XrpcNodeId(self), (void *)self, (void *)proc, process_id);
    }

    XRPC_LOG_T("Node(%d) ProcessExec leave: process=0x%08x, rc=%d",
           L_XrpcNodeId(self), process_id, result);

    return result;
}
