/**
 * @file        xrpc_status.c
 * @brief       运行时状态上报实现。
 * @note        状态表由节点锁保护。
 * @copyright   Copyright (c) 2026 AuyvJpz
 */

#include "xrpc_node_inner.h"

#include <string.h>

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

Int32 TXrpcNodeSetStatusCb(TXrpcNode *node, TXrpcStatusCb cb, UInt32 interval_ms, void *user_data)
{
    Int32 result = kXrpcErrInvalidParam;

    if (node != NULL)
    {
        node->status_cb = cb;
        node->status_interval_ms = interval_ms;
        node->status_user = user_data;
        result = kXrpcErrOk;
    }

    return result;
}

UInt32 TXrpcNodeGetStatusCount(TXrpcNode *node, TXrpcStatusCode code)
{
    UInt32 result = 0U;

    if ((node != NULL) && ((UInt32)code < (UInt32)kXrpcStatusCount)
        && (L_XrpcNodeLock(node) == True))
    {
        result = node->status[code].info.count;
        L_XrpcNodeUnlock(node);
    }

    return result;
}

void TXrpcNodeStatusRecordNoLock(TXrpcNode *self, TXrpcStatusCode code,
                                 const TXrpcPeerNode *peer_node, UInt32 node_id,
                                 UInt32 object_id, UInt32 need, UInt32 actual)
{
    TXrpcStatusInfo *info = &self->status[code].info;

    info->code = code;
    info->link_index = (peer_node != NULL)
                     ? (UInt16)(peer_node - self->peer_cache->peer_array)
                     : (UInt16)XRPC_STATUS_LINK_NONE;
    info->node_id = node_id;
    info->object_id = object_id;
    info->count++;
    info->need = need;
    info->actual = actual;
}

void TXrpcNodeStatusRecord(TXrpcNode *self, TXrpcStatusCode code,
                           const TXrpcPeerNode *peer_node, UInt32 node_id,
                           UInt32 object_id, UInt32 need, UInt32 actual)
{
    if (L_XrpcNodeLock(self) == True)
    {
        TXrpcNodeStatusRecordNoLock(self, code, peer_node, node_id, object_id, need, actual);
        L_XrpcNodeUnlock(self);
    }
}

void TXrpcNodeStatusFlush(TXrpcNode *self)
{
    if (self->status_cb != NULL)
    {
        UInt32 now_tick = XrpcGetSysTick();
        UInt32 i = 0U;

        for (i = 0U; i < (UInt32)kXrpcStatusCount; i++)
        {
            TXrpcStatusEntry *entry = &self->status[i];
            TXrpcStatusInfo info;
            Bool report = False;

            if (L_XrpcNodeLock(self) == True)
            {
                /* 第一次出现立即报，之后受间隔限制 */
                if ((entry->info.count != entry->reported_count)
                    && ((entry->reported_count == 0U)
                        || ((now_tick - entry->report_tick) >= self->status_interval_ms)))
                {
                    info = entry->info;
                    entry->reported_count = entry->info.count;
                    entry->report_tick = now_tick;
                    report = True;
                }
                L_XrpcNodeUnlock(self);
            }

            if (report == True)
            {
                self->status_cb(self, &info, self->status_user);
            }
        }
    }
}
