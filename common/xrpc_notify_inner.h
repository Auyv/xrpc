/**
 * @file        xrpc_notify_inner.h
 * @brief       通知模块的内部接口，仅供 common 内部 .c 调用。
 * @copyright   Copyright (c) 2026 AuyvJpz
 */

#ifndef _XRPC_NOTIFY_INNER_H_
#define _XRPC_NOTIFY_INNER_H_

#include "xrpc/xrpc_node.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** @brief 处理收到的 NotifyEvent（订阅方），在 RunOnce 线程调用 */
void TXrpcNotifyHandleEvent(TXrpcNode *self, TXrpcPeerNode *peer_node, UInt32 src_node_id,
                            char *payload, UInt16 payload_len);

/** @brief 处理收到的 NotifyAck（发布方），在 RunOnce 线程调用 */
void TXrpcNotifyHandleAck(TXrpcNode *self,
                          const char *payload, UInt16 payload_len);

/** @brief 驱动发布：发送、补发、超时、完成回调，在 RunOnce 线程调用 */
void TXrpcNotifyPoll(TXrpcNode *self);

#ifdef __cplusplus
}  ///< extern "C"
#endif

#endif  ///< _XRPC_NOTIFY_INNER_H_
