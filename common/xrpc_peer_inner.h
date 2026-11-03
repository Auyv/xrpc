/**
 * @file        xrpc_peer_inner.h
 * @brief       PeerCache 内部分层函数:仅供 common 内部 .c 调用,用户层禁止 include。
 * @note        本头收纳"节点视角下才合理调用"的函数:
 *              - TXrpcPeerCacheSetOwner:假设 owner 为 TXrpcNode*
 *              - TXrpcPeerCachePeerRunOnce:被 TXrpcNodeRunOnce 封装,节点统一调度入口
 *              物理位置刻意放在 common/ 平铺目录(不在 include/xrpc/ 下),
 *              用户通过标准 include 路径搜不到,实现物理隔离。
 * @copyright   Copyright (c) 2026 AuyvJpz
 */

#ifndef _XRPC_PEER_INNER_H_
#define _XRPC_PEER_INNER_H_

#include "xrpc/xrpc_peer.h"   /* 取 TXrpcPeerCache typedef */

#ifdef __cplusplus
extern "C"
{
#endif


/**
 * @brief 设置 PeerCache 的 owner(节点视角下绑定的反向引用句柄)。
 * @param self  PeerCache 对象指针
 * @param owner 拥有者句柄;实际语义上应为 TXrpcNode*,签名保留 void*
 *              以保持"非头 API 表面契约"
 * @return  成功返回 kXrpcErrOk;self 为 NULL 返回 kXrpcErrInvalidParam
 * @note    内部函数。仅 TXrpcNodeSetPeerCache 内部调用,用户层禁止 include 此头。
 */
Int32 TXrpcPeerCacheSetOwner(TXrpcPeerCache *self, void *owner);


/**
 * @brief 判断节点是否在这条链路上。调用方持节点锁。
 * @param peer    peer 对象
 * @param node_id 节点 ID;0 永远返回 False
 * @return  P2P 比较对端 ID,总线查远端节点表
 */
Bool TXrpcPeerNodeOwnsNode(const TXrpcPeerNode *peer, UInt32 node_id);


/**
 * @brief 轮询所有 peer:检查链路状态,拉取一帧数据,经 fdn 拼接后通知上层。
 * @param self                PeerCache 对象指针
 * @param link_recv_buff_ptr  链路层单次读取使用的临时缓冲区(由调用方提供)
 * @param link_recv_buff_size 临时缓冲区容量(字节,>0)
 * @return  成功返回 kXrpcErrOk;参数非法返回 kXrpcErrInvalidParam
 * @note    内部函数。被 TXrpcNodeRunOnce 封装,用户应通过 TXrpcNodeRunOnce
 *          统一调度,禁止直接 include 此头调用。
 *          当 lock_ops 非 NULL 时,临界区由 lock/unlock 回调保护(阻塞等待);
 *          每个 peer 共用同一临时缓冲区,顺序处理。
 */
Int32 TXrpcPeerCachePeerRunOnce(TXrpcPeerCache *self,
                                UInt8 *link_recv_buff_ptr,
                                UInt32 link_recv_buff_size);



#ifdef __cplusplus
}  ///< extern "C"
#endif

#endif  ///< _XRPC_PEER_INNER_H_
