/**
 * @file        xrpc_node_inner.c
 * @brief       节点内部实现：帧收发、消息分发、握手、发现和调用状态机。
 * @copyright   Copyright (c) 2026 AuyvJpz
 */

#include "xrpc_node_inner.h"
#include "xrpc_notify_inner.h"
#include "xrpc_relay_inner.h"

#include "xrpc_port.h"

#define XRPC_PROCESS_AUTO_KNOCK_END_MS         (1000U)
/* 自动额度耗尽后转为慢速重试,对端晚启动时不需要业务层参与 */
#define XRPC_PROCESS_SLOW_KNOCK_MS             (5000U)


static Int32 L_XrpcNodeSendCallRspNoLock(TXrpcNode *self, TXrpcPeerNode *peer_node,
                                          UInt32 dst_node_id, UInt32 req_seq, UInt32 process_id,
                                          TXrpcMsgParamList *output_params,
                                          Int32 errcode,
                                          Bool *retried, Int32 *first_result);

static const char *L_XrpcMsgTypeName(UInt16 type)
{
    const char *result = "Unknown";

    switch (type)
    {
    case kXrpcMsgTypeHelloReq:
        result = "HelloReq";
        break;
    case kXrpcMsgTypeHelloRsp:
        result = "HelloRsp";
        break;
    case kXrpcMsgTypeKnockReq:
        result = "KnockReq";
        break;
    case kXrpcMsgTypeKnockRsp:
        result = "KnockRsp";
        break;
    case kXrpcMsgTypeCallReq:
        result = "CallReq";
        break;
    case kXrpcMsgTypeCallRsp:
        result = "CallRsp";
        break;
    case kXrpcMsgTypeNotifyEvent:
        result = "NotifyEvent";
        break;
    case kXrpcMsgTypeNotifyAck:
        result = "NotifyAck";
        break;
    case kXrpcMsgTypeAddressed:
        result = "Addressed";
        break;
    default:
        break;
    }

    return result;
}


static UInt16 L_XrpcPayloadLen(const TXrpcMsgParamList *params)
{
    UInt16 result = 0U;

    if (params != NULL)
    {
        result = params->encode_buffer_used;
    }

    return result;
}

static UInt16 L_XrpcRxPayloadLen(const TXrpcMsgHeadMem *msg)
{
    UInt16 result = 0U;
    UInt16 frame_len = (msg != NULL) ? XrpcWireGetU16(msg->frame_len) : 0U;

    if (frame_len >= XRPC_MSG_FRAME_LEN_MIN)
    {
        result = (UInt16)(frame_len - XRPC_MSG_FRAME_LEN_MIN);
    }

    return result;
}

static Bool L_XrpcFrameUsesEnvelope(const TXrpcPeerNode *peer_node, UInt16 type)
{
    return ((peer_node->topology == kXrpcLinkTopologyBus)
            && (type != kXrpcMsgTypeNotifyEvent)) ? True : False;
}

/* 载荷在 send_buff 里的偏移:总线上要给信封头留位置 */
static UInt16 L_XrpcTxPayloadOffset(const TXrpcPeerNode *peer_node, UInt16 type)
{
    UInt16 result = XRPC_MSG_HEAD_SIZE;

    if (L_XrpcFrameUsesEnvelope(peer_node, type) == True)
    {
        result = XRPC_MSG_HEAD_SIZE + XRPC_MSG_ADDRESSED_HEAD_SIZE;
    }

    return result;
}

static UInt32 L_XrpcNodeId(const TXrpcNode *self)
{
    return self->node_id;
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

static Bool L_XrpcPeerSendLock(TXrpcPeerNode *peer_node)
{
    Bool result = True;

    if ((peer_node != NULL) && (peer_node->send_lock_ops != NULL))
    {
        result = peer_node->send_lock_ops->lock(peer_node->send_lock_ops->user_data);
    }

    return result;
}

static void L_XrpcPeerSendUnlock(TXrpcPeerNode *peer_node)
{
    if ((peer_node != NULL) && (peer_node->send_lock_ops != NULL))
    {
        (void)peer_node->send_lock_ops->unlock(peer_node->send_lock_ops->user_data);
    }
}

UInt32 TXrpcNodeNextSeqNoLock(TXrpcNode *self)
{
    self->tx_seq++;

    return self->tx_seq;
}

UInt32 TXrpcNodeNextSeq(TXrpcNode *self)
{
    UInt32 result = 0U;

    if (L_XrpcNodeLock(self) == True)
    {
        result = TXrpcNodeNextSeqNoLock(self);
        L_XrpcNodeUnlock(self);
    }

    return result;
}

static UInt32 L_XrpcPeerNodeIdLocked(TXrpcNode *self, TXrpcPeerNode *peer_node)
{
    UInt32 result = 0U;

    if (L_XrpcNodeLock(self) == True)
    {
        result = peer_node->peer_node_id;
        L_XrpcNodeUnlock(self);
    }

    return result;
}

static Bool L_XrpcBindingAcceptsPeer(const TXrpcNode *self, UInt32 process_id,
                                     const TXrpcPeerNode *peer_node);

/* 调用方持节点锁。慢速重试一次只有一个额度，轮询又总是从第 0 条链路开始，
 * 不轮转的话额度永远被第 0 条用掉，晚上线的提供者再也问不到 */
static Bool L_XrpcProcessSlowTurnNoLock(TXrpcNode *self, TXrpcProcessObject *proc,
                                        const TXrpcPeerNode *peer_node)
{
    Bool result = False;
    Bool found = False;
    UInt32 count = self->peer_cache->peer_count;
    UInt32 step = 0U;

    for (step = 0U; (step < count) && (found == False); step++)
    {
        UInt32 index = (proc->runtime_data->knock_cursor + step) % count;
        const TXrpcPeerNode *peer = &self->peer_cache->peer_array[index];

        /* 没握手的链路不会轮询 Knock，排上它就卡住了 */
        if ((peer->link_up == True)
            && ((peer->peer_node_id != 0U) || (peer->topology == kXrpcLinkTopologyBus))
            && (L_XrpcBindingAcceptsPeer(self, proc->static_desc->process_id, peer) == True))
        {
            found = True;
            if (peer == peer_node)
            {
                proc->runtime_data->knock_cursor = index + 1U;
                result = True;
            }
        }
    }

    return result;
}

static Bool L_XrpcProcessNeedKnockLocked(TXrpcNode *self, TXrpcProcessObject *proc,
                                         const TXrpcPeerNode *peer_node)
{
    Bool need_knock = False;
    UInt32 now_tick = XrpcGetSysTick();

    if ((proc != NULL) && (proc->static_desc != NULL)
        && (proc->runtime_data != NULL)
        && (proc->static_desc->exec_cb == NULL)
        && (L_XrpcNodeLock(self) == True))
    {
        /* WaitRsp / Notify 属于在途调用,Knock 不能抢占,否则调用方拿不到结果 */
        if ((proc->runtime_data->provider_node_id == 0U)
            && (proc->runtime_data->call_step != kXrpcProcessCallStepWaitRsp)
            && (proc->runtime_data->call_step != kXrpcProcessCallStepNotify)
            && ((proc->runtime_data->call_step != kXrpcProcessCallStepDetecting)
                || ((now_tick - proc->runtime_data->last_knock_tick)
                    >= XRPC_PROCESS_AUTO_KNOCK_END_MS)))
        {
            if (proc->runtime_data->knock_left_cnt > 0U)
            {
                need_knock = True;
            }
            else if (((now_tick - proc->runtime_data->last_knock_tick)
                      >= XRPC_PROCESS_SLOW_KNOCK_MS)
                     && (L_XrpcProcessSlowTurnNoLock(self, proc, peer_node) == True))
            {
                proc->runtime_data->knock_left_cnt = 1U;
                need_knock = True;
            }
        }
        L_XrpcNodeUnlock(self);
    }

    return need_knock;
}

/* 调用方持节点锁。恢复为未发现，自动 Knock 额度重新计算 */
static void L_XrpcProcessForgetProviderNoLock(TXrpcProcessObject *proc)
{
    proc->runtime_data->provider_node_id = 0U;
    proc->runtime_data->knock_left_cnt = XRPC_PROCESS_AUTO_KNOCK_INIT_CNT;
    proc->runtime_data->last_knock_tick = 0U;
}

/* 调用方持节点锁 */
static void L_XrpcProcessPeerLostNoLock(TXrpcProcessObject *proc)
{
    TXrpcMsgParamList *curr_output = proc->runtime_data->curr_output;

    L_XrpcProcessForgetProviderNoLock(proc);
    if (proc->runtime_data->call_step == kXrpcProcessCallStepWaitRsp)
    {
        /* 走 Notify 唤醒调用方，由 ProcessExec 负责复位 */
        if (curr_output != NULL)
        {
            curr_output->encode_buffer_used = 0U;
            curr_output->param_cnt = 0U;
        }
        proc->runtime_data->curr_errcode = kXrpcErrPeerStatusInvalid;
        proc->runtime_data->call_step = kXrpcProcessCallStepNotify;
    }
    else if (proc->runtime_data->call_step != kXrpcProcessCallStepNotify)
    {
        proc->runtime_data->call_step = kXrpcProcessCallStepInvalid;
    }
}

const TXrpcProcessBinding *TXrpcNodeFindBinding(const TXrpcNode *self, UInt32 process_id)
{
    const TXrpcProcessBinding *result = NULL;
    UInt32 i = 0U;

    for (i = 0U; (i < self->binding_count) && (result == NULL); i++)
    {
        if (self->bindings[i].process_id == process_id)
        {
            result = &self->bindings[i];
        }
    }

    return result;
}

/* 绑定了提供者节点时，其他节点的应答不算数 */
static Bool L_XrpcBindingAcceptsNode(const TXrpcNode *self, UInt32 process_id, UInt32 node_id)
{
    const TXrpcProcessBinding *binding = TXrpcNodeFindBinding(self, process_id);

    return ((binding == NULL) || (binding->node_id == 0U)
            || (binding->node_id == node_id)) ? True : False;
}

/* 过程是否可以在这条链路上发现 */
static Bool L_XrpcBindingAcceptsPeer(const TXrpcNode *self, UInt32 process_id,
                                     const TXrpcPeerNode *peer_node)
{
    const TXrpcProcessBinding *binding = TXrpcNodeFindBinding(self, process_id);

    return ((binding == NULL)
            || (&self->peer_cache->peer_array[binding->link_index] == peer_node)) ? True : False;
}

/* 调用方持节点锁。查总线远端节点表，查不到返回 NULL */
static TXrpcRemoteNode *L_XrpcFindRemote(TXrpcPeerNode *peer_node, UInt32 node_id)
{
    TXrpcRemoteNode *result = NULL;
    UInt32 i = 0U;

    for (i = 0U; (i < peer_node->remote_count) && (result == NULL); i++)
    {
        if (peer_node->remotes[i].node_id == node_id)
        {
            result = &peer_node->remotes[i];
        }
    }

    return result;
}

/* 总线上每收到一帧都按帧头发送方学习远端节点；表满时不再学习 */
static void L_XrpcLearnRemote(TXrpcNode *self, TXrpcPeerNode *peer_node, UInt32 node_id)
{
    if ((peer_node->topology == kXrpcLinkTopologyBus) && (node_id != 0U)
        && (L_XrpcNodeLock(self) == True))
    {
        if ((L_XrpcFindRemote(peer_node, node_id) == NULL)
            && (peer_node->remote_count < peer_node->remote_capacity))
        {
            peer_node->remotes[peer_node->remote_count].node_id = node_id;
            peer_node->remotes[peer_node->remote_count].max_recv_frame = 0U;
            peer_node->remote_count++;
        }
        else if (L_XrpcFindRemote(peer_node, node_id) == NULL)
        {
            TXrpcNodeStatusRecordNoLock(self, kXrpcStatusRemoteTableFull, peer_node, node_id, 0U,
                                        peer_node->remote_capacity + 1U,
                                        peer_node->remote_capacity);
        }
        L_XrpcNodeUnlock(self);
    }
}

/* 记录对端在 Hello 里声明的接收上限 */
static void L_XrpcHandleHelloCap(TXrpcNode *self, TXrpcPeerNode *peer_node, UInt32 src_node_id,
                                 const char *raw_payload, UInt16 payload_len)
{
    if ((payload_len >= sizeof(TXrpcMsgHelloCapMem)) && (L_XrpcNodeLock(self) == True))
    {
        UInt32 max_recv = XrpcWireGetU32(((const TXrpcMsgHelloCapMem *)raw_payload)->max_recv_frame);

        if (peer_node->topology == kXrpcLinkTopologyP2P)
        {
            peer_node->peer_max_recv_frame = max_recv;
        }
        else
        {
            TXrpcRemoteNode *remote = L_XrpcFindRemote(peer_node, src_node_id);

            if (remote != NULL)
            {
                remote->max_recv_frame = max_recv;
            }
        }
        L_XrpcNodeUnlock(self);
    }
}

/* 发送前检查链路和对端的帧长上限 */
static Bool L_XrpcFrameFits(TXrpcNode *self, TXrpcPeerNode *peer_node, UInt32 dst_node_id,
                            UInt32 send_size, UInt32 *limit_out)
{
    Bool result = True;

    if ((peer_node->max_frame > 0U) && (send_size > peer_node->max_frame))
    {
        *limit_out = peer_node->max_frame;
        result = False;
    }
    else if (L_XrpcNodeLock(self) == True)
    {
        UInt32 limit = peer_node->peer_max_recv_frame;

        if (peer_node->topology == kXrpcLinkTopologyBus)
        {
            TXrpcRemoteNode *remote = L_XrpcFindRemote(peer_node, dst_node_id);

            limit = (remote != NULL) ? remote->max_recv_frame : 0U;
        }
        result = ((limit == 0U) || (send_size <= limit)) ? True : False;
        *limit_out = limit;
        L_XrpcNodeUnlock(self);
    }

    return result;
}

static TXrpcProcessObject *L_XrpcNodeFindProcessWithKey(TXrpcNode *self,
                                                        UInt32 process_id,
                                                        UInt32 process_key)
{
    TXrpcProcessObject *result = NULL;
    TXrpcProcessObject *proc = TXrpcNodeFindProcess(self, process_id);

    if ((proc != NULL) && (proc->static_desc != NULL)
        && (proc->static_desc->process_key == process_key))
    {
        result = proc;
    }

    return result;
}

static Bool L_XrpcProcessHasLocalExec(const TXrpcProcessObject *proc)
{
    Bool result = False;

    if ((proc != NULL) && (proc->static_desc != NULL)
        && (proc->static_desc->exec_cb != NULL))
    {
        result = True;
    }

    return result;
}

static Int32 L_XrpcCreateRxParamList(TXrpcMsgParamList *param_list,
                                     UInt8 param_cnt,
                                     char *param_data,
                                     UInt16 param_data_size)
{
    Int32 result = kXrpcErrInvalidParam;

    if (param_list != NULL)
    {
        if (param_cnt == 0U)
        {
            result = TXrpcMsgParamListCreate(param_list, 0U, param_data, 1U);
        }
        else if ((param_data != NULL) && (param_data_size > 0U))
        {
            result = TXrpcMsgParamListCreate(param_list, param_cnt,
                                             param_data, param_data_size);
        }
    }

    return result;
}

static void L_XrpcNodeHandleKnockRsp(TXrpcNode *self, TXrpcPeerNode *peer_node,
                                     UInt32 src_node_id, TXrpcMsgKnockRspMem *payload)
{
    TXrpcProcessObject *process = NULL;
    UInt32 peer_node_id = 0U;
    UInt32 process_id = 0U;
    UInt32 process_seq = 0U;
    Int32 errcode = kXrpcErrInvalidParam;


    if ((self != NULL) && (peer_node != NULL) && (payload != NULL))
    {
        process_id = XrpcWireGetU32(payload->process_id);
        process_seq = XrpcWireGetU32(payload->req_seq);
        errcode = XrpcWireGetI32(payload->errcode);

        if (L_XrpcNodeLock(self) == True)
        {
            peer_node_id = (peer_node->topology == kXrpcLinkTopologyBus)
                         ? src_node_id : peer_node->peer_node_id;
            process = TXrpcNodeFindProcess(self, process_id);
            if ((process != NULL) && (process->runtime_data != NULL)
                && (process->runtime_data->call_step == kXrpcProcessCallStepDetecting)
                && (process->runtime_data->process_seq == process_seq)
                && (L_XrpcBindingAcceptsNode(self, process_id, src_node_id) == True))
            {
                if (errcode == 1)
                {
                    process->runtime_data->provider_node_id = peer_node_id;
                    process->runtime_data->curr_errcode = kXrpcErrOk;
                    process->runtime_data->call_step = kXrpcProcessCallStepIdle;
                }
                else
                {
                    process->runtime_data->curr_errcode = errcode;
                    process->runtime_data->call_step = kXrpcProcessCallStepInvalid;
                }
            }
            L_XrpcNodeUnlock(self);
        }
        
        XRPC_LOG_T("Node(%d) KnockRsp deal: process=0x%08x, seq=%u, err=%d, peer=0x%08x",
               L_XrpcNodeId(self), process_id, process_seq, errcode, peer_node_id);
    }
}

static void L_XrpcNodeHandleCallReq(TXrpcNode *self, TXrpcPeerNode *peer_node,
                                    UInt32 src_node_id, UInt32 req_seq,
                                    char *raw_payload, UInt16 payload_len)
{
    if ((self != NULL) && (peer_node != NULL) && (raw_payload != NULL)
        && (payload_len >= (sizeof(TXrpcMsgCallReqMem) - 1U)))
    {
        TXrpcMsgCallReqMem *payload = (TXrpcMsgCallReqMem *)raw_payload;
        UInt32 process_id = XrpcWireGetU32(payload->process_id);
        UInt32 process_key = XrpcWireGetU32(payload->process_key);
        TXrpcProcessObject *proc = L_XrpcNodeFindProcessWithKey(self, process_id, process_key);
        Int32 errcode = kXrpcErrProcessNotFound;
        UInt32 peer_node_id = L_XrpcPeerNodeIdLocked(self, peer_node);

        XRPC_LOG_T("Node(%d) CallReq recv: process=0x%08x, seq=%u, key=0x%08x, param_cnt=%u, payload=%u, peer=0x%08x",
               L_XrpcNodeId(self), process_id, req_seq, process_key,
               payload->param_cnt, payload_len, peer_node_id);
        if ((proc != NULL) && (L_XrpcProcessHasLocalExec(proc) == True))
        {
            UInt16 param_data_size = (UInt16)(payload_len - (sizeof(TXrpcMsgCallReqMem) - 1U));
            TXrpcMsgParamList input_param;
            TXrpcMsgParamList output_param;
            UInt16 output_offset =
                (UInt16)(L_XrpcTxPayloadOffset(peer_node, kXrpcMsgTypeCallRsp)
                         + sizeof(TXrpcMsgCallRspMem) - 1U);
            UInt16 output_size = 0U;
            Int32 in_ret = L_XrpcCreateRxParamList(&input_param,
                                                   payload->param_cnt,
                                                   payload->param_list,
                                                   param_data_size);
            Int32 out_ret = kXrpcErrInvalidParam;
            Int32 rsp_result = kXrpcErrInvalidParam;
            Bool rsp_retried = False;
            Int32 rsp_first_result = kXrpcErrInvalidParam;

            
            if ((peer_node->send_buff_ptr != NULL)
                && (peer_node->send_buff_size > output_offset))
            {
                output_size = (UInt16)(peer_node->send_buff_size - output_offset);
                out_ret = TXrpcMsgParamListCreate(&output_param, 0U,
                                                (char *)&peer_node->send_buff_ptr[output_offset],
                                                output_size);
            }

            XRPC_LOG_T("Node(%d) CallReq param create: process=0x%08x, in_ret=%d, out_ret=%d, in_size=%u, out_size=%u",
                L_XrpcNodeId(self), process_id, in_ret, out_ret,
                param_data_size, output_size);
            if ((in_ret == kXrpcErrOk) && (out_ret == kXrpcErrOk))
            {
                XRPC_LOG_T("Node(%d) CallReq exec begin: process=0x%08x, seq=%u",
                    L_XrpcNodeId(self), process_id, req_seq);
                if (L_XrpcPeerSendLock(peer_node) == True)
                {
                    errcode = XrpcProcessExec(proc, &input_param, &output_param);
                    
                    rsp_result = L_XrpcNodeSendCallRspNoLock(self, peer_node, src_node_id, req_seq, process_id,
                                                             &output_param, errcode,
                                                             &rsp_retried, &rsp_first_result);
                    L_XrpcPeerSendUnlock(peer_node);
                    
                    if (rsp_result < 0)
                    {
                        XRPC_LOG_E("Node(%d) CallReq send CallRsp failed: process=0x%08x, seq=%u, err=%d, rsp_rc=%d, retried=%d, first_rc=%d",
                               L_XrpcNodeId(self), process_id, req_seq,
                               errcode, rsp_result, rsp_retried, rsp_first_result);
                    }
					else
					{
						XRPC_LOG_T("Node(%d) CallReq exec end: process=0x%08x, seq=%u, err=%d, out_cnt=%u, out_used=%u, rsp_rc=%d, rsp_retried=%d, rsp_first_rc=%d",
                                        L_XrpcNodeId(self), process_id, req_seq, errcode,
                                        output_param.param_cnt, output_param.encode_buffer_used,
                                        rsp_result, rsp_retried, rsp_first_result);
					}
                }
                else
                {
                    XRPC_LOG_E("Node(%d) CallReq peer send lock failed: process=0x%08x",
                        L_XrpcNodeId(self), process_id);
                }
            }
            else
            {
                XRPC_LOG_E("Node(%d) CallReq invalid params: process=0x%08x, in_ret=%d, out_ret=%d",
                    L_XrpcNodeId(self), process_id, in_ret, out_ret);
                TXrpcNodeSendCallRsp(self, peer_node, src_node_id, req_seq, process_id, NULL,
                                 kXrpcErrInvalidParam);
            }
        }
        else
        {
            XRPC_LOG_E("Node(%d) CallReq process unavailable: process=0x%08x, key=0x%08x, proc=%p",
                   L_XrpcNodeId(self), process_id, process_key, (void *)proc);
            TXrpcNodeSendCallRsp(self, peer_node, src_node_id, req_seq, process_id, NULL, errcode);
        }
    }
    else
    {
        XRPC_LOG_E("Node(%d) CallReq drop: payload_len=%u",
               L_XrpcNodeId(self), payload_len);
    }
}

/* 调用方持节点锁。把应答交给匹配的调用槽，没有匹配返回 False */
static Bool L_XrpcCallSlotOnRspNoLock(TXrpcNode *self, UInt32 process_id, UInt32 req_seq,
                                      Int32 errcode, const TXrpcMsgParamList *output_param)
{
    Bool result = False;
    UInt32 i = 0U;

    for (i = 0U; (i < self->call_slot_count) && (result == False); i++)
    {
        TXrpcCallSlot *slot = &self->call_slots[i];

        if ((slot->step == kXrpcCallSlotWait) && (slot->process_id == process_id)
            && (slot->delivery.seq == req_seq))
        {
            slot->errcode = errcode;
            if (slot->output != NULL)
            {
                UInt16 cpy_size = output_param->encode_buffer_used;

                slot->output->encode_buffer_used = 0U;
                slot->output->param_cnt = 0U;
                if (errcode >= 0)
                {
                    if (slot->output->encode_buffer_size < cpy_size)
                    {
                        slot->errcode = kXrpcErrRxBufferOverflow;
                    }
                    else
                    {
                        memcpy(slot->output->encode_buffer_addr,
                               output_param->encode_buffer_addr, cpy_size);
                        slot->output->encode_buffer_used = cpy_size;
                        slot->output->param_cnt = output_param->param_cnt;
                    }
                }
            }
            slot->step = kXrpcCallSlotDone;
            result = True;
        }
    }

    return result;
}

static void L_XrpcNodeHandleCallRsp(TXrpcNode *self, char *raw_payload, UInt16 payload_len)
{
    if ((self != NULL) && (raw_payload != NULL)
        && (payload_len >= (sizeof(TXrpcMsgCallRspMem) - 1U)))
    {
        TXrpcMsgCallRspMem *payload = (TXrpcMsgCallRspMem *)raw_payload;
        UInt32 process_id = XrpcWireGetU32(payload->process_id);
        UInt32 req_seq = XrpcWireGetU32(payload->req_seq);
        Int32 errcode = XrpcWireGetI32(payload->errcode);
        TXrpcProcessObject *proc = TXrpcNodeFindProcess(self, process_id);

        if (proc != NULL)
        {
            UInt16 param_data_size = (UInt16)(payload_len - (sizeof(TXrpcMsgCallRspMem) - 1U));
            TXrpcMsgParamList output_param;
            Int32 result = L_XrpcCreateRxParamList(&output_param,
                                                   payload->param_cnt,
                                                   payload->param_list,
                                                   param_data_size);

            if (result == kXrpcErrOk)
            {
                if (L_XrpcNodeLock(self) == True)
                {
                    Bool matched = L_XrpcCallSlotOnRspNoLock(self, process_id, req_seq, errcode,
                                                             &output_param);

                    if (matched == False)
                    {
                        XrpcProcessNotifyCallRsp(proc, payload, &output_param);
                        matched = ((proc->runtime_data != NULL)
                                   && (proc->runtime_data->call_step == kXrpcProcessCallStepNotify)
                                   && (proc->runtime_data->process_seq == req_seq))
                                ? True : False;
                    }

                    /* 对端已没有这个过程:忘掉 provider,下次重新发现。
                     * 业务返回值恰好是 -8 时只是多一次 Knock,没有其他影响 */
                    if ((matched == True) && (proc->runtime_data != NULL)
                        && (errcode == kXrpcErrProcessNotFound))
                    {
                        L_XrpcProcessForgetProviderNoLock(proc);
                    }
                    L_XrpcNodeUnlock(self);
                }
            }
            else
            {
                XRPC_LOG_E("Node(%d) CallRsp param create failed: process=0x%08x, rc=%d",
                       L_XrpcNodeId(self), process_id, result);
            }
        }
        else
        {
            XRPC_LOG_E("Node(%d) CallRsp process not found: process=0x%08x",
                   L_XrpcNodeId(self), process_id);
        }
    }
}

/* 发送缓冲放不下。应答和转发在 RunOnce 里发送，没有调用方能收到错误码 */
static void L_XrpcReportTxOverflow(TXrpcNode *self, const TXrpcPeerNode *peer_node,
                                   UInt32 dst_node_id, UInt16 type, UInt32 send_size)
{
    if (self != NULL)
    {
        TXrpcNodeStatusRecord(self, kXrpcStatusTxBufferOverflow, peer_node, dst_node_id, type,
                              send_size, peer_node->send_buff_size);
    }
}

static Int32 L_XrpcNodeSendMsgFrameNoLock(TXrpcNode *self, TXrpcPeerNode *peer_node,
                                          UInt32 dst_node_id, UInt16 type, UInt32 frame_seq,
                                          UInt8 ttl, UInt8 *payload_data, UInt16 payload_size)
{
    Int32 result = kXrpcErrInvalidParam;
    Bool use_envelope = ((peer_node != NULL)
                         && (L_XrpcFrameUsesEnvelope(peer_node, type) == True)) ? True : False;
    UInt16 envelope_size = (use_envelope == True) ? XRPC_MSG_ADDRESSED_HEAD_SIZE : 0U;
    UInt32 send_size = XRPC_MSG_CRC_START_OFFSET + XRPC_MSG_FRAME_LEN_MIN
                     + envelope_size + payload_size;
    UInt16 frame_len = (UInt16)(XRPC_MSG_FRAME_LEN_MIN + envelope_size + payload_size);
    UInt16 frame_crc = 0U;

    if ((peer_node != NULL) && (send_size > peer_node->send_buff_size))
    {
        L_XrpcReportTxOverflow(self, peer_node, dst_node_id, type, send_size);
        result = kXrpcErrBufferOverflow;
    }
    else if ((self != NULL) && (peer_node != NULL) && (peer_node->link != NULL)
        && (peer_node->send_buff_ptr != NULL)
        && ((payload_data != NULL) || (payload_size == 0U)))
    {
        TXrpcMsgHeadMem *msg = (TXrpcMsgHeadMem *)peer_node->send_buff_ptr;
        char *payload_dst = &msg->payload[envelope_size];
        UInt32 frame_limit = 0U;

        if (L_XrpcFrameFits(self, peer_node, dst_node_id, send_size, &frame_limit) == False)
        {
            TXrpcNodeStatusRecord(self, kXrpcStatusTxFrameTooLarge, peer_node, dst_node_id, type,
                                  send_size, frame_limit);
            result = kXrpcErrFrameTooLarge;
        }
        else
        {
            /* 载荷可能已在 send_buff 的最终位置,先搬载荷再写头部 */
            if ((payload_size > 0U) && (payload_dst != (char *)payload_data))
            {
                memmove(payload_dst, payload_data, payload_size);
            }
            memcpy(msg->tag, XRPC_MSG_HEAD_TAG, sizeof(msg->tag));
            XrpcWirePutU16(msg->frame_crc, 0U);
            XrpcWirePutU16(msg->frame_len, frame_len);
            XrpcWirePutU32(msg->node_id, self->node_id);
            XrpcWirePutU16(msg->version, self->version);
            XrpcWirePutU16(msg->type, type);
            XrpcWirePutU32(msg->user_tag, self->user_tag);
            XrpcWirePutU32(msg->seq, frame_seq);
            msg->ttl = ttl;
            msg->flags = 0U;
            memset(msg->reserved, 0, sizeof(msg->reserved));
            if (use_envelope == True)
            {
                TXrpcMsgAddressedMem *envelope = (TXrpcMsgAddressedMem *)msg->payload;

                XrpcWirePutU16(msg->type, kXrpcMsgTypeAddressed);
                XrpcWirePutU32(envelope->dst_node, dst_node_id);
                XrpcWirePutU16(envelope->inner_type, type);
            }
            result = IXrpcCrc16Calc(self->crc16,
                                    &peer_node->send_buff_ptr[XRPC_MSG_CRC_START_OFFSET],
                                    frame_len, &frame_crc);
            if (result == kXrpcErrOk)
            {
                XrpcWirePutU16(msg->frame_crc, frame_crc);
                result = IXrpcLinkSend(peer_node->link, (const char *)msg, (Int32)send_size);
                /* 帧必须一次完整写入:返回 0 或短写都算失败,由调用方决定是否重发 */
                if (result != (Int32)send_size)
                {
                    result = kXrpcErrLinkSendFailed;
                }
            }
        }
    }

    return result;
}

static Int32 L_XrpcNodeSendFrameLocked(TXrpcNode *self, TXrpcPeerNode *peer_node,
                                       UInt32 dst_node_id, UInt16 type,
                                       UInt8 *payload_data, UInt16 payload_size)
{
    Int32 result = kXrpcErrInvalidParam;

    if (L_XrpcPeerSendLock(peer_node) == True)
    {
        result = L_XrpcNodeSendMsgFrameNoLock(self, peer_node, dst_node_id, type,
                                              TXrpcNodeNextSeq(self), XRPC_MSG_TTL_DEFAULT,
                                              payload_data, payload_size);
        L_XrpcPeerSendUnlock(peer_node);
    }
    else
    {
        XRPC_LOG_E("Node(%d) TX frame lock failed: type=0x%04x(%s), payload=%u",
               L_XrpcNodeId(self), type, L_XrpcMsgTypeName(type), payload_size);
    }

    if (result < 0)
    {
        XRPC_LOG_E("Node(%d) TX frame failed: type=0x%04x(%s), rc=%d, payload=%u",
               L_XrpcNodeId(self), type, L_XrpcMsgTypeName(type), result, payload_size);
    }

    return result;
}

Int32 TXrpcNodeSendMsgFrame(TXrpcNode *self, TXrpcPeerNode *peer_node,
                            UInt16 type, UInt8 *payload_data, UInt16 payload_size)
{
    UInt32 dst_node_id = (peer_node != NULL) ? L_XrpcPeerNodeIdLocked(self, peer_node) : 0U;

    return L_XrpcNodeSendFrameLocked(self, peer_node, dst_node_id, type,
                                     payload_data, payload_size);
}

/**
 * @brief 帧发送骨架 + 10ms 失败重试封装。
 * @param self         节点对象
 * @param peer_node    目标 peer
 * @param type         消息类型
 * @param frame_seq    帧头 seq,重发时不变
 * @param ttl          帧头 ttl
 * @param payload_data payload 数据(允许 NULL,此时 payload_size 必须为 0)
 * @param payload_size payload 字节数
 * @return  >= 0 实际发送字节数;< 0 失败错误码(两次都失败)
 * @note    第一次失败后 sleep 10ms 再试一次,仍失败返回最后一次的错误码。
 *          适用于"链路偶发忙"的场景;不保证事务幂等,业务方需自行考虑。
 */
static Int32 L_XrpcNodeSendWithRetry(TXrpcNode *self, TXrpcPeerNode *peer_node,
                                     UInt32 dst_node_id, UInt16 type, UInt32 frame_seq,
                                     UInt8 ttl, UInt8 *payload_data, UInt16 payload_size,
                                     Bool *retried, Int32 *first_result)
{
    Int32 result = L_XrpcNodeSendMsgFrameNoLock(self, peer_node, dst_node_id, type, frame_seq, ttl,
                                                payload_data, payload_size);

    if (retried != NULL)
    {
        *retried = False;
    }
    if (first_result != NULL)
    {
        *first_result = result;
    }

    /* 只有链路发送失败才值得重发,帧过大、缓冲不足重发也没用 */
    if (result == kXrpcErrLinkSendFailed)
    {
        if (retried != NULL)
        {
            *retried = True;
        }
        XrpcSleepMs(10U);
        result = L_XrpcNodeSendMsgFrameNoLock(self, peer_node, dst_node_id, type, frame_seq, ttl,
                                              payload_data, payload_size);
    }

    return result;
}

/* Hello 只在链路配置了通告时才带载荷 */
static Int32 L_XrpcNodeSendHello(TXrpcNode *self, TXrpcPeerNode *peer_node,
                                 UInt32 dst_node_id, UInt16 type)
{
    Int32 result = kXrpcErrInvalidParam;

    if (peer_node != NULL)
    {
        TXrpcMsgHelloCapMem payload;
        UInt8 *payload_data = NULL;
        UInt16 payload_size = 0U;

        if (peer_node->advertise_max_recv == True)
        {
            XrpcWirePutU32(payload.max_recv_frame,
                           (peer_node->fdn_recv.m_buff_start != NULL)
                               ? (UInt32)peer_node->fdn_recv.m_buff_size
                               : peer_node->max_frame);
            payload_data = (UInt8 *)&payload;
            payload_size = sizeof(payload);
        }
        result = L_XrpcNodeSendFrameLocked(self, peer_node, dst_node_id, type,
                                           payload_data, payload_size);
    }

    return result;
}

Int32 TXrpcNodeSendHelloReq(TXrpcNode *self, TXrpcPeerNode *peer_node)
{
    /* 总线上是广播宣告;P2P 不用信封,目的地址用不到 */
    return L_XrpcNodeSendHello(self, peer_node, kXrpcNodeIdBroadcast, kXrpcMsgTypeHelloReq);
}

Int32 TXrpcNodeSendHelloRsp(TXrpcNode *self, TXrpcPeerNode *peer_node, UInt32 dst_node_id)
{
    return L_XrpcNodeSendHello(self, peer_node, dst_node_id, kXrpcMsgTypeHelloRsp);
}

Int32 TXrpcNodeSendKnockReq(TXrpcNode *self, TXrpcPeerNode *peer_node, UInt32 process_id)
{
    Int32 result = kXrpcErrInvalidParam;
    TXrpcProcessObject *process = TXrpcNodeFindProcess(self, process_id);
    Bool send_retried = False;
    Int32 first_send_result = kXrpcErrInvalidParam;
    TXrpcMsgKnockReqMem payload;
    UInt32 frame_seq = 0U;
    Bool state_prepared = False;
    UInt32 knock_left_cnt = 0U;
    UInt32 last_knock_tick = 0U;
    TXrpcProcessCallStep call_step = kXrpcProcessCallStepInvalid;

    if ((process != NULL) && (process->static_desc != NULL)
        && (process->runtime_data != NULL))
    {
        XrpcWirePutU32(payload.process_id, process->static_desc->process_id);
        XrpcWirePutU32(payload.process_key, process->static_desc->process_key);

        if (L_XrpcNodeLock(self) == True)
        {
            UInt32 now_tick = XrpcGetSysTick();

            call_step = process->runtime_data->call_step;
            knock_left_cnt = process->runtime_data->knock_left_cnt;
            if ((call_step == kXrpcProcessCallStepDetecting)
                && ((now_tick - process->runtime_data->last_knock_tick)
                    < XRPC_PROCESS_AUTO_KNOCK_END_MS))
            {
                result = kXrpcErrProcessStatus;
            }
            else if (knock_left_cnt == 0U)
            {
                result = kXrpcErrProcessStatus;
            }
            else
            {
                UInt32 next_seq = TXrpcNodeNextSeqNoLock(self);

                knock_left_cnt = knock_left_cnt - 1U;

                process->runtime_data->process_seq = next_seq;
                process->runtime_data->call_step = kXrpcProcessCallStepDetecting;
                process->runtime_data->curr_errcode = kXrpcErrTimeout;
                process->runtime_data->knock_left_cnt = knock_left_cnt;
                process->runtime_data->last_knock_tick = now_tick;
                last_knock_tick = now_tick;
                frame_seq = next_seq;
                state_prepared = True;
            }
            L_XrpcNodeUnlock(self);
        }

        if (state_prepared == False)
        {
            /* result keeps lock failure or kXrpcErrProcessStatus. */
            if (result == kXrpcErrProcessStatus)
            {
                XRPC_LOG_T("Node(%d) TX KnockReq skipped: process=0x%08x, step=%u, left_cnt=%u",
                       L_XrpcNodeId(self), process_id, call_step, knock_left_cnt);
            }
        }
        else if (L_XrpcPeerSendLock(peer_node) == True)
        {
            const TXrpcProcessBinding *binding = TXrpcNodeFindBinding(self, process_id);
            UInt32 dst_node_id = ((binding != NULL) && (binding->node_id != 0U))
                               ? binding->node_id : kXrpcNodeIdBroadcast;

            result = L_XrpcNodeSendWithRetry(self, peer_node, dst_node_id, kXrpcMsgTypeKnockReq,
                                             frame_seq, XRPC_MSG_TTL_DEFAULT,
                                             (UInt8 *)&payload, sizeof(payload),
                                             &send_retried, &first_send_result);
            L_XrpcPeerSendUnlock(peer_node);
        }
        else
        {
            result = kXrpcErrInvalidParam;
        }

        if ((result < 0) && (result != kXrpcErrProcessStatus))
        {
            if (L_XrpcNodeLock(self) == True)
            {
                process->runtime_data->call_step = kXrpcProcessCallStepIdle;
                process->runtime_data->curr_errcode = result;
                L_XrpcNodeUnlock(self);
            }
        }
        else if (result >= 0)
        {
            XRPC_LOG_T("Node(%d) TX KnockReq: process=0x%08x, seq=%u, key=%d, left_cnt=%u, last_tick=%u, retried=%d, first_rc=%d, rc=%d",
               L_XrpcNodeId(self), process_id, frame_seq, process->static_desc->process_key,
               knock_left_cnt, last_knock_tick, send_retried, first_send_result, result);
        }

        if ((result < 0) && (result != kXrpcErrProcessStatus))
        {
            XRPC_LOG_E("Node(%d) TX KnockReq failed: process=0x%08x, seq=%u, key=%d, left_cnt=%u, last_tick=%u, retried=%d, first_rc=%d, rc=%d",
                   L_XrpcNodeId(self), process_id, frame_seq, process->static_desc->process_key,
                   knock_left_cnt, last_knock_tick, send_retried, first_send_result, result);
        }
    }

    return result;
}

Int32 TXrpcNodeSendKnockRsp(TXrpcNode *self, TXrpcPeerNode *peer_node,
                            UInt32 dst_node_id, UInt32 req_seq,
                            const TXrpcMsgKnockReqMem *req_msg)
{
    Int32 result = kXrpcErrInvalidParam;

    if (req_msg != NULL)
    {
        TXrpcMsgKnockRspMem payload;
        UInt32 process_id = XrpcWireGetU32(req_msg->process_id);
        TXrpcProcessObject *process = TXrpcNodeFindProcess(self, process_id);
        Int32 errcode = kXrpcErrProcessNotFound;

        if ((process != NULL) && (process->static_desc != NULL)
            && (process->static_desc->exec_cb != NULL))
        {
            errcode = (process->static_desc->process_key == XrpcWireGetU32(req_msg->process_key))
                    ? 1 : 0;
        }
        XrpcWirePutU32(payload.req_seq, req_seq);
        XrpcWirePutU32(payload.process_id, process_id);
        XrpcWirePutI32(payload.errcode, errcode);
        XRPC_LOG_T("Node(%d) TX KnockRsp: process=0x%08x, seq=%u, err=%d, has_exec=%d",
               L_XrpcNodeId(self), process_id, req_seq, errcode,
               ((process != NULL) && (process->static_desc != NULL)
                && (process->static_desc->exec_cb != NULL)) ? 1 : 0);
        if (L_XrpcPeerSendLock(peer_node) == True)
        {
            result = L_XrpcNodeSendMsgFrameNoLock(self, peer_node, dst_node_id, kXrpcMsgTypeKnockRsp,
                                                  TXrpcNodeNextSeq(self), XRPC_MSG_TTL_DEFAULT,
                                                  (UInt8 *)&payload, sizeof(payload));
            L_XrpcPeerSendUnlock(peer_node);
        }
        else
        {
            XRPC_LOG_E("Node(%d) TX KnockRsp lock failed: process=0x%08x, seq=%u",
                   L_XrpcNodeId(self), process_id, req_seq);
        }

        if (result < 0)
        {
            XRPC_LOG_E("Node(%d) TX KnockRsp failed: process=0x%08x, seq=%u, err=%d, rc=%d",
                   L_XrpcNodeId(self), process_id, req_seq, errcode, result);
        }
    }

    return result;
}

static Int32 L_XrpcNodeSendCallFrameEx(TXrpcNode *self, TXrpcPeerNode *peer_node,
                                       TXrpcProcessObject *process, UInt32 seq,
                                       UInt32 dst_node_id, const TXrpcMsgParamList *input,
                                       Bool *retried, Int32 *first_result)
{
    Int32 result = kXrpcErrInvalidParam;
    UInt16 param_len = L_XrpcPayloadLen(input);
    UInt16 payload_size = (UInt16)(sizeof(TXrpcMsgCallReqMem) - 1U + param_len);
    UInt16 payload_offset = (peer_node != NULL)
                          ? L_XrpcTxPayloadOffset(peer_node, kXrpcMsgTypeCallReq)
                          : XRPC_MSG_HEAD_SIZE;
    UInt32 send_size = payload_offset + payload_size;

    if (L_XrpcPeerSendLock(peer_node) == True)
    {
        if ((peer_node != NULL) && (peer_node->send_buff_ptr != NULL)
            && (send_size <= peer_node->send_buff_size))
        {
            TXrpcMsgCallReqMem *payload =
                (TXrpcMsgCallReqMem *)&peer_node->send_buff_ptr[payload_offset];

            XrpcWirePutU32(payload->process_id, process->static_desc->process_id);
            XrpcWirePutU32(payload->process_key, process->static_desc->process_key);
            payload->param_cnt = (input != NULL) ? input->param_cnt : 0U;
            if (param_len > 0U)
            {
                memcpy(payload->param_list, input->encode_buffer_addr, param_len);
            }
            result = L_XrpcNodeSendWithRetry(self, peer_node, dst_node_id, kXrpcMsgTypeCallReq,
                                             seq, XRPC_MSG_TTL_DEFAULT,
                                             (UInt8 *)payload, payload_size,
                                             retried, first_result);
        }
        else
        {
            if (peer_node != NULL)
            {
                L_XrpcReportTxOverflow(self, peer_node, dst_node_id, kXrpcMsgTypeCallReq, send_size);
            }
            result = kXrpcErrTxBufferOverflow;
        }
        L_XrpcPeerSendUnlock(peer_node);
    }

    return result;
}

Int32 TXrpcNodeSendCallFrame(TXrpcNode *self, TXrpcPeerNode *peer_node,
                             TXrpcProcessObject *process, UInt32 seq,
                             UInt32 dst_node_id, const TXrpcMsgParamList *input)
{
    Int32 result = kXrpcErrInvalidParam;

    if ((process != NULL) && (process->static_desc != NULL))
    {
        result = L_XrpcNodeSendCallFrameEx(self, peer_node, process, seq, dst_node_id,
                                           input, NULL, NULL);
    }

    return result;
}

Int32 TXrpcNodeSendCallReq(TXrpcNode *self, TXrpcPeerNode *peer_node,
                           UInt32 process_id, TXrpcMsgParamList *input_params,
                           TXrpcMsgParamList *output_params)
{
    Int32 result = kXrpcErrInvalidParam;
    TXrpcProcessObject *process = TXrpcNodeFindProcess(self, process_id);
    UInt32 process_seq = 0U;
    Bool runtime_occupied = False;
    Bool send_retried = False;
    Int32 first_send_result = kXrpcErrInvalidParam;

    if ((process != NULL) && (process->static_desc != NULL)
        && (process->runtime_data != NULL))
    {
        UInt16 param_len = L_XrpcPayloadLen(input_params);
        UInt32 dst_node_id = 0U;

        if (L_XrpcNodeLock(self) == True)
        {
            if (process->runtime_data->call_step != kXrpcProcessCallStepIdle)
            {
                result = kXrpcErrProcessStatus;
            }
            else
            {
                process_seq = TXrpcNodeNextSeqNoLock(self);
                dst_node_id = process->runtime_data->provider_node_id;
                process->runtime_data->curr_output = output_params;
                process->runtime_data->curr_errcode = kXrpcErrOk;
                process->runtime_data->process_seq = process_seq;
                process->runtime_data->call_step = kXrpcProcessCallStepWaitRsp;
                runtime_occupied = True;
            }
            L_XrpcNodeUnlock(self);
        }

        if (runtime_occupied == False)
        {
            /* result keeps kXrpcErrInvalidParam or kXrpcErrProcessStatus. */
        }
        else
        {
            result = L_XrpcNodeSendCallFrameEx(self, peer_node, process, process_seq, dst_node_id,
                                               input_params, &send_retried, &first_send_result);
            /* 没发出去:必须复位 call_step,否则 process 永久卡在 WaitRsp,
             * 后续 ProcessExec 会在第一段 while 循环上等到超时 */
            if ((result < 0) && (L_XrpcNodeLock(self) == True))
            {
                process->runtime_data->call_step = kXrpcProcessCallStepIdle;
                process->runtime_data->curr_errcode = result;
                process->runtime_data->curr_output = NULL;
                L_XrpcNodeUnlock(self);
            }
        }

        if (result >= 0)
        {
            XRPC_LOG_T("Node(%d) TX CallReq: process=0x%08x, seq=%u, param_len=%u, retried=%d, first_rc=%d, rc=%d",
                   L_XrpcNodeId(self), process_id, process_seq, param_len,
                   send_retried, first_send_result, result);
        }
        else
        {
            XRPC_LOG_E("Node(%d) TX CallReq failed: process=0x%08x, seq=%u, param_len=%u, buff=%u, retried=%d, first_rc=%d, rc=%d",
                   L_XrpcNodeId(self), process_id, process_seq, param_len,
                   (peer_node != NULL) ? peer_node->send_buff_size : 0U,
                   send_retried, first_send_result, result);
        }
    }
    else
    {
        XRPC_LOG_E("Node(%d) TX CallReq process invalid: process=0x%08x, proc=%p",
               L_XrpcNodeId(self), process_id, (void *)process);
    }

    return result;
}

static Int32 L_XrpcNodeSendCallRspNoLock(TXrpcNode *self, TXrpcPeerNode *peer_node,
                                          UInt32 dst_node_id, UInt32 req_seq, UInt32 process_id,
                                          TXrpcMsgParamList *output_params,
                                          Int32 errcode,
                                          Bool *retried, Int32 *first_result)
{
    Int32 result = kXrpcErrInvalidParam;

    if (peer_node != NULL)
    {
        UInt16 param_len = L_XrpcPayloadLen(output_params);
        UInt16 payload_size = (UInt16)(sizeof(TXrpcMsgCallRspMem) - 1U + param_len);
        UInt16 payload_offset = (peer_node != NULL)
                              ? L_XrpcTxPayloadOffset(peer_node, kXrpcMsgTypeCallRsp)
                              : XRPC_MSG_HEAD_SIZE;
        UInt32 send_size = payload_offset + payload_size;

        if ((peer_node != NULL) && (peer_node->send_buff_ptr != NULL)
            && (send_size <= peer_node->send_buff_size))
        {
            TXrpcMsgCallRspMem *payload =
                (TXrpcMsgCallRspMem *)&peer_node->send_buff_ptr[payload_offset];

            XrpcWirePutU32(payload->req_seq, req_seq);
            XrpcWirePutU32(payload->process_id, process_id);
            XrpcWirePutI32(payload->errcode, errcode);
            payload->param_cnt = (output_params != NULL) ? output_params->param_cnt : 0U;
            /* 处理 CallReq 时输出参数就建在 send_buff 的 param_list 位置,此时无需拷贝 */
            if ((param_len > 0U) && (payload->param_list != output_params->encode_buffer_addr))
            {
                memcpy(payload->param_list, output_params->encode_buffer_addr, param_len);
            }
            result = L_XrpcNodeSendWithRetry(self, peer_node, dst_node_id, kXrpcMsgTypeCallRsp,
                                             TXrpcNodeNextSeq(self), XRPC_MSG_TTL_DEFAULT,
                                             (UInt8 *)payload, payload_size,
                                             retried, first_result);
        }
        else
        {
            if (peer_node != NULL)
            {
                L_XrpcReportTxOverflow(self, peer_node, dst_node_id, kXrpcMsgTypeCallRsp, send_size);
            }
            result = kXrpcErrTxBufferOverflow;
        }
    }

    return result;
}

Int32 TXrpcNodeSendCallRsp(TXrpcNode *self, TXrpcPeerNode *peer_node,
                           UInt32 dst_node_id, UInt32 req_seq, UInt32 process_id,
                           TXrpcMsgParamList *output_params, Int32 errcode)
{
    Int32 result = kXrpcErrInvalidParam;
    Bool send_retried = False;
    Int32 first_send_result = kXrpcErrInvalidParam;
    UInt32 process_seq = req_seq;
    UInt16 param_len = L_XrpcPayloadLen(output_params);

    if (L_XrpcPeerSendLock(peer_node) == True)
    {
        result = L_XrpcNodeSendCallRspNoLock(self, peer_node, dst_node_id, req_seq, process_id,
                                             output_params, errcode,
                                             &send_retried, &first_send_result);
        L_XrpcPeerSendUnlock(peer_node);
    }
    else
    {
        XRPC_LOG_E("Node(%d) TX CallRsp lock failed: process=0x%08x, seq=%u",
               L_XrpcNodeId(self), process_id, process_seq);
    }

    if (result >= 0)
    {
        XRPC_LOG_T("Node(%d) TX CallRsp: process=0x%08x, seq=%u, err=%d, param_len=%u, retried=%d, first_rc=%d, rc=%d",
               L_XrpcNodeId(self), process_id, process_seq, errcode, param_len,
               send_retried, first_send_result, result);
    }
    else
    {
        XRPC_LOG_E("Node(%d) TX CallRsp failed: process=0x%08x, seq=%u, err=%d, param_len=%u, buff=%u, retried=%d, first_rc=%d, rc=%d",
               L_XrpcNodeId(self), process_id, process_seq, errcode, param_len,
               (peer_node != NULL) ? peer_node->send_buff_size : 0U,
               send_retried, first_send_result, result);
    }

    return result;
}

Int32 TXrpcNodeRelayForward(TXrpcNode *self, TXrpcPeerNode *peer_node, UInt32 dst_node_id,
                            UInt16 type, UInt32 frame_seq, UInt8 ttl,
                            const char *payload, UInt16 payload_len,
                            Bool patch_seq, UInt32 req_seq)
{
    Int32 result = kXrpcErrInvalidParam;

    if (L_XrpcPeerSendLock(peer_node) == True)
    {
        UInt16 payload_offset = L_XrpcTxPayloadOffset(peer_node, type);
        UInt32 send_size = (UInt32)payload_offset + payload_len;

        if (send_size > peer_node->send_buff_size)
        {
            L_XrpcReportTxOverflow(self, peer_node, dst_node_id, type, send_size);
            result = kXrpcErrTxBufferOverflow;
        }
        else
        {
            UInt8 *dst = &peer_node->send_buff_ptr[payload_offset];

            memcpy(dst, payload, payload_len);
            if (patch_seq == True)
            {
                XrpcWirePutU32(dst, req_seq);
            }
            result = L_XrpcNodeSendWithRetry(self, peer_node, dst_node_id, type, frame_seq, ttl,
                                             dst, payload_len, NULL, NULL);
        }
        L_XrpcPeerSendUnlock(peer_node);
    }

    return result;
}

Int32 TXrpcNodeSendNotifyEvent(TXrpcNode *self, TXrpcPeerNode *peer_node,
                               TXrpcNotifyPubObject *pub, UInt32 *sent_seq)
{
    Int32 result = kXrpcErrInvalidParam;

    if (L_XrpcPeerSendLock(peer_node) == True)
    {
        TXrpcMsgNotifyEventMem *payload =
            (TXrpcMsgNotifyEventMem *)&peer_node->send_buff_ptr[XRPC_MSG_HEAD_SIZE];
        UInt16 payload_size = 0U;
        Bool ready = False;

        if (L_XrpcNodeLock(self) == True)
        {
            TXrpcNotifyPubRuntime *rt = pub->runtime;

            payload_size = (UInt16)(sizeof(TXrpcMsgNotifyEventMem) - 1U + rt->param_len);
            if ((UInt32)(XRPC_MSG_HEAD_SIZE + payload_size) <= peer_node->send_buff_size)
            {
                XrpcWirePutU32(payload->notify_id, pub->desc->notify_id);
                XrpcWirePutU32(payload->notify_key, pub->desc->notify_key);
                XrpcWirePutU32(payload->origin_node, self->node_id);
                XrpcWirePutU32(payload->seq, rt->seq);
                payload->flags = ((rt->opt.need_ack == True) && (pub->desc->ack_node_count > 0U))
                               ? XRPC_NOTIFY_FLAG_NEED_ACK : 0U;
                payload->param_cnt = rt->param_cnt;
                if (rt->param_len > 0U)
                {
                    memcpy(payload->param_list, pub->desc->slot_buf, rt->param_len);
                }
                *sent_seq = rt->seq;
                ready = True;
            }
            else
            {
                TXrpcNodeStatusRecordNoLock(self, kXrpcStatusTxBufferOverflow, peer_node,
                                            kXrpcNodeIdBroadcast, pub->desc->notify_id,
                                            XRPC_MSG_HEAD_SIZE + (UInt32)payload_size,
                                            peer_node->send_buff_size);
                result = kXrpcErrBufferOverflow;
            }
            L_XrpcNodeUnlock(self);
        }

        if (ready == True)
        {
            result = L_XrpcNodeSendWithRetry(self, peer_node, kXrpcNodeIdBroadcast,
                                             kXrpcMsgTypeNotifyEvent, TXrpcNodeNextSeq(self),
                                             XRPC_MSG_TTL_DEFAULT,
                                             (UInt8 *)payload, payload_size, NULL, NULL);
        }
        L_XrpcPeerSendUnlock(peer_node);
    }

    return result;
}

Int32 TXrpcNodeSendNotifyAck(TXrpcNode *self, TXrpcPeerNode *peer_node, UInt32 dst_node_id,
                             UInt32 notify_id, UInt32 publisher, UInt32 seq, Int32 ack_result)
{
    TXrpcMsgNotifyAckMem payload;

    XrpcWirePutU32(payload.notify_id, notify_id);
    XrpcWirePutU32(payload.origin_node, self->node_id);
    XrpcWirePutU32(payload.publisher, publisher);
    XrpcWirePutU32(payload.seq, seq);
    XrpcWirePutI32(payload.result, ack_result);

    return L_XrpcNodeSendFrameLocked(self, peer_node, dst_node_id, kXrpcMsgTypeNotifyAck,
                                     (UInt8 *)&payload, sizeof(payload));
}

/* 调用方持节点锁。这条链路上原来的对端不在了：经它学到的提供者和路由作废，在途调用结束 */
static void L_XrpcNodePeerGoneNoLock(TXrpcNode *self, TXrpcPeerNode *peer_node)
{
    UInt32 i = 0U;

    for (i = 0U; i < self->process_table.count; i++)
    {
        TXrpcProcessObject *proc = self->process_table.process_table[i];

        if ((proc != NULL) && (proc->runtime_data != NULL)
            && (TXrpcPeerNodeOwnsNode(peer_node,
                                      proc->runtime_data->provider_node_id) == True))
        {
            L_XrpcProcessPeerLostNoLock(proc);
        }
    }
    for (i = 0U; i < self->call_slot_count; i++)
    {
        TXrpcCallSlot *slot = &self->call_slots[i];

        if ((slot->step == kXrpcCallSlotWait) && (slot->peer_node == peer_node))
        {
            if (slot->output != NULL)
            {
                slot->output->encode_buffer_used = 0U;
                slot->output->param_cnt = 0U;
            }
            slot->errcode = kXrpcErrPeerStatusInvalid;
            slot->step = kXrpcCallSlotDone;
        }
    }
    TXrpcRelayOnLinkDownNoLock(self, peer_node);
}

/* 点对点链路握手。链路没断但对端换了节点时，按原对端不在了处理 */
static void L_XrpcNodeLearnP2pPeer(TXrpcNode *self, TXrpcPeerNode *peer_node, UInt32 src_node_id)
{
    if (L_XrpcNodeLock(self) == True)
    {
        if ((peer_node->peer_node_id != 0U) && (peer_node->peer_node_id != src_node_id))
        {
            XRPC_LOG_W("Node(%d) peer changed: 0x%08x -> 0x%08x",
                       L_XrpcNodeId(self), peer_node->peer_node_id, src_node_id);
            L_XrpcNodePeerGoneNoLock(self, peer_node);
            peer_node->peer_max_recv_frame = 0U;
        }
        peer_node->peer_node_id = src_node_id;
        peer_node->hello_pending = False;
        L_XrpcNodeUnlock(self);
    }
}

/* 按内层类型分发。P2P 帧和信封内层走同一条路 */
static void L_XrpcNodeDispatch(TXrpcNode *self, TXrpcPeerNode *peer_node, UInt32 src_node_id,
                               UInt16 type, UInt32 frame_seq, UInt8 ttl,
                               char *payload, UInt16 payload_len)
{
    if (type == kXrpcMsgTypeHelloReq)
    {
        if (peer_node->topology == kXrpcLinkTopologyP2P)
        {
            L_XrpcNodeLearnP2pPeer(self, peer_node, src_node_id);
        }
        L_XrpcHandleHelloCap(self, peer_node, src_node_id, payload, payload_len);
        XRPC_LOG_W("Node(%d) RX HelloReq: peer_node=0x%08x, send HelloRsp",
               L_XrpcNodeId(self), src_node_id);
        (void)TXrpcNodeSendHelloRsp(self, peer_node, src_node_id);
    }
    else if (type == kXrpcMsgTypeHelloRsp)
    {
        if (peer_node->topology == kXrpcLinkTopologyP2P)
        {
            /* 对端回 HelloRsp:握手成功 */
            L_XrpcNodeLearnP2pPeer(self, peer_node, src_node_id);
        }
        L_XrpcHandleHelloCap(self, peer_node, src_node_id, payload, payload_len);
        XRPC_LOG_W("Node(%d) RX HelloRsp: peer_node=0x%08x, hello_pending=0",
               L_XrpcNodeId(self), src_node_id);
    }
    else if ((type == kXrpcMsgTypeKnockReq)
             && (payload_len >= sizeof(TXrpcMsgKnockReqMem)))
    {
        TXrpcMsgKnockReqMem *req = (TXrpcMsgKnockReqMem *)payload;
        UInt32 process_id = XrpcWireGetU32(req->process_id);

        XRPC_LOG_T("Node(%d) RX KnockReq: process=0x%08x, seq=%u, key=0x%08x",
               L_XrpcNodeId(self), process_id, frame_seq, XrpcWireGetU32(req->process_key));
        if (TXrpcRelayOnKnockReq(self, peer_node, src_node_id, frame_seq, ttl, req) == True)
        {
            /* 中继已接手:问到其他链路,或者路由已知 */
        }
        /* 总线上广播询问时,没有这个过程的节点不应答,否则会盖过真正的提供者 */
        else if ((peer_node->topology == kXrpcLinkTopologyP2P)
                 || (L_XrpcProcessHasLocalExec(TXrpcNodeFindProcess(self, process_id)) == True))
        {
            (void)TXrpcNodeSendKnockRsp(self, peer_node, src_node_id, frame_seq, req);
        }
    }
    else if ((type == kXrpcMsgTypeKnockRsp)
             && (payload_len >= sizeof(TXrpcMsgKnockRspMem)))
    {
        TXrpcMsgKnockRspMem *rsp = (TXrpcMsgKnockRspMem *)payload;

        XRPC_LOG_T("Node(%d) RX KnockRsp: process=0x%08x, seq=%u, err=%d",
               L_XrpcNodeId(self), XrpcWireGetU32(rsp->process_id), XrpcWireGetU32(rsp->req_seq),
               XrpcWireGetI32(rsp->errcode));
        if (TXrpcRelayOnKnockRsp(self, peer_node, src_node_id, rsp) == False)
        {
            L_XrpcNodeHandleKnockRsp(self, peer_node, src_node_id, rsp);
        }
    }
    else if (type == kXrpcMsgTypeCallReq)
    {
        XRPC_LOG_T("Node(%d) RX CallReq", L_XrpcNodeId(self));
        if ((payload_len < (sizeof(TXrpcMsgCallReqMem) - 1U))
            || (TXrpcRelayOnCallReq(self, peer_node, src_node_id, frame_seq, ttl,
                                    payload, payload_len) == False))
        {
            L_XrpcNodeHandleCallReq(self, peer_node, src_node_id, frame_seq, payload, payload_len);
        }
    }
    else if (type == kXrpcMsgTypeCallRsp)
    {
        XRPC_LOG_T("Node(%d) RX CallRsp", L_XrpcNodeId(self));
        if ((payload_len < (sizeof(TXrpcMsgCallRspMem) - 1U))
            || (TXrpcRelayOnCallRsp(self, peer_node, payload, payload_len) == False))
        {
            L_XrpcNodeHandleCallRsp(self, payload, payload_len);
        }
    }
    else if (type == kXrpcMsgTypeNotifyEvent)
    {
        if (payload_len >= (sizeof(TXrpcMsgNotifyEventMem) - 1U))
        {
            TXrpcRelayOnNotifyEvent(self, peer_node, src_node_id, ttl, payload, payload_len);
        }
        TXrpcNotifyHandleEvent(self, peer_node, src_node_id, payload, payload_len);
    }
    else if (type == kXrpcMsgTypeNotifyAck)
    {
        if ((payload_len < sizeof(TXrpcMsgNotifyAckMem))
            || (TXrpcRelayOnNotifyAck(self, payload, payload_len) == False))
        {
            TXrpcNotifyHandleAck(self, payload, payload_len);
        }
    }
    else
    {
        XRPC_LOG_E("Node(%d) RX frame ignored: type=0x%04x(%s), payload=%u",
               L_XrpcNodeId(self), type, L_XrpcMsgTypeName(type), payload_len);
    }
}

void TXrpcNodeHandleRxFrame(TXrpcNode *self, TXrpcPeerNode *peer_node, TXrpcMsgHeadMem *msg)
{
    if ((self != NULL) && (peer_node != NULL) && (msg != NULL))
    {
        UInt16 payload_len = L_XrpcRxPayloadLen(msg);
        UInt32 src_node_id = XrpcWireGetU32(msg->node_id);
        UInt16 version = XrpcWireGetU16(msg->version);
        UInt16 type = XrpcWireGetU16(msg->type);
        UInt32 user_tag = XrpcWireGetU32(msg->user_tag);
        UInt32 frame_seq = XrpcWireGetU32(msg->seq);

        if (version != self->version)
        {
            XRPC_LOG_E("Node(%d) RX frame drop: version mismatch, msg=0x%04x, self=0x%04x",
                   L_XrpcNodeId(self), version, self->version);
            TXrpcNodeStatusRecord(self, kXrpcStatusVersionMismatch, peer_node, src_node_id, type,
                                  self->version, version);
        }
        else if (user_tag != self->user_tag)
        {
            /* 别的系统的帧,不学习也不应答 */
            TXrpcNodeStatusRecord(self, kXrpcStatusUserTagMismatch, peer_node, src_node_id, type,
                                  self->user_tag, user_tag);
        }
        else if ((peer_node->topology == kXrpcLinkTopologyBus)
                 && (src_node_id == self->node_id))
        {
            /* 总线不回显,收到自己的 ID 说明别的节点配重了,这种帧不能当成对端的 */
            TXrpcNodeStatusRecord(self, kXrpcStatusNodeIdConflict, peer_node, src_node_id, type,
                                  0U, 0U);
        }
        else if (type == kXrpcMsgTypeAddressed)
        {
            L_XrpcLearnRemote(self, peer_node, src_node_id);
            if (payload_len >= XRPC_MSG_ADDRESSED_HEAD_SIZE)
            {
                TXrpcMsgAddressedMem *envelope = (TXrpcMsgAddressedMem *)msg->payload;
                UInt32 dst_node = XrpcWireGetU32(envelope->dst_node);

                /* 不是发给自己的直接丢弃,这是总线上的常态,不记错误 */
                if ((dst_node == self->node_id) || (dst_node == kXrpcNodeIdBroadcast))
                {
                    L_XrpcNodeDispatch(self, peer_node, src_node_id,
                                       XrpcWireGetU16(envelope->inner_type), frame_seq, msg->ttl,
                                       envelope->inner_payload,
                                       (UInt16)(payload_len - XRPC_MSG_ADDRESSED_HEAD_SIZE));
                }
            }
        }
        else
        {
            L_XrpcLearnRemote(self, peer_node, src_node_id);
            L_XrpcNodeDispatch(self, peer_node, src_node_id, type, frame_seq, msg->ttl,
                               msg->payload, payload_len);
        }
    }
}

void TXrpcNodePeerLinkDown(TXrpcNode *self, TXrpcPeerNode *peer_node)
{
    if ((self != NULL) && (peer_node != NULL))
    {
        if (peer_node->fdn_recv.m_buff_start != NULL)
        {
            TFdnReset(&peer_node->fdn_recv);
        }

        if (L_XrpcNodeLock(self) == True)
        {
            L_XrpcNodePeerGoneNoLock(self, peer_node);
            peer_node->peer_node_id = peer_node->static_peer_node_id;
            peer_node->peer_max_recv_frame = 0U;
            peer_node->remote_count = 0U;
            peer_node->hello_pending = False;
            peer_node->hello_tick = 0U;
            peer_node->knock_tick = 0U;
            L_XrpcNodeUnlock(self);
        }

        XRPC_LOG_W("Node(%d) peer link down", L_XrpcNodeId(self));
    }
}

Int32 TXrpcNodeProcessKnockDeal(TXrpcNode *self, TXrpcPeerNode *peer_node)
{
    Int32 result = kXrpcErrOk;
    UInt32 i = 0U;

    if ((self == NULL) || (peer_node == NULL)
        || (self->process_table.process_table == NULL))
    {
        result = kXrpcErrInvalidParam;
    }
    else
    {
        /* 单 peer 视角:遍历 process_table,对所有尚未发现 provider 的 process
         * 向该 peer 发 KnockReq。PeerRunOnce 会对每个 peer 各调一次本函数,
         * 实现"每个 process × 每个 peer" 二维发现。 */
        for (i = 0U; i < self->process_table.count; i++)
        {
            TXrpcProcessObject *proc = self->process_table.process_table[i];

            if ((proc != NULL) && (proc->static_desc != NULL)
                && (L_XrpcBindingAcceptsPeer(self, proc->static_desc->process_id, peer_node) == True)
                && (L_XrpcProcessNeedKnockLocked(self, proc, peer_node) == True))
            {
                (void)TXrpcNodeSendKnockReq(self, peer_node, proc->static_desc->process_id);
            }
        }
    }

    return result;
}
