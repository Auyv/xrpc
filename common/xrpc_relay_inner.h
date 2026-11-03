/**
 * @file        xrpc_relay_inner.h
 * @brief       服务中继的内部接口，仅供 common 内部 .c 调用。
 * @note        TXrpcRelayOnXxx 都在 RunOnce 线程调用，返回 True 表示这帧已由中继处理。
 * @copyright   Copyright (c) 2026 AuyvJpz
 */

#ifndef _XRPC_RELAY_INNER_H_
#define _XRPC_RELAY_INNER_H_

#include "xrpc/xrpc_node.h"

#ifdef __cplusplus
extern "C"
{
#endif

Bool TXrpcRelayOnKnockReq(TXrpcNode *self, TXrpcPeerNode *ingress, UInt32 src_node_id,
                          UInt32 req_seq, UInt8 ttl, const TXrpcMsgKnockReqMem *req);

Bool TXrpcRelayOnKnockRsp(TXrpcNode *self, TXrpcPeerNode *peer_node, UInt32 src_node_id,
                          const TXrpcMsgKnockRspMem *rsp);

Bool TXrpcRelayOnCallReq(TXrpcNode *self, TXrpcPeerNode *ingress, UInt32 src_node_id,
                         UInt32 req_seq, UInt8 ttl, const char *payload, UInt16 payload_len);

Bool TXrpcRelayOnCallRsp(TXrpcNode *self, TXrpcPeerNode *peer_node,
                         const char *payload, UInt16 payload_len);

/** @brief 按转发表转发事件。本地投递不受影响，调用方继续处理 */
void TXrpcRelayOnNotifyEvent(TXrpcNode *self, TXrpcPeerNode *ingress, UInt32 src_node_id, UInt8 ttl,
                             const char *payload, UInt16 payload_len);

/** @brief 这条 Ack 属于别的发布方时原路送回。返回 False 表示由本节点处理 */
Bool TXrpcRelayOnNotifyAck(TXrpcNode *self, const char *payload, UInt16 payload_len);

/** @brief 结束超时的询问与转发调用，在 RunOnce 线程调用 */
void TXrpcRelayPoll(TXrpcNode *self);

/** @brief 链路断开时清理路由与转发调用。调用方持节点锁 */
void TXrpcRelayOnLinkDownNoLock(TXrpcNode *self, TXrpcPeerNode *peer_node);

#ifdef __cplusplus
}  ///< extern "C"
#endif

#endif  ///< _XRPC_RELAY_INNER_H_
