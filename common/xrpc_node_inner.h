/**
 * @addtogroup xrpc_node_inner
 * xrpc_node_inner
 * @{
 */
/**
 * @file        xrpc_node_inner.h
 * @brief       TXrpcNode 节点对象内部 msg 发送 API:仅供 common 内部 .c 调用。
 *              对外通过 xrpc_node.h 的 TXrpcNodeCall / TXrpcNodeCallAsync
 *              等高层 API 接入。
 * @note        本头收纳"按 process_id 收发的低层函数":
 *              - TXrpcNodeSendMsgFrame:通用帧发送骨架(其他 SendXxx 复用)
 *              - TXrpcNodeSendHelloReq/Rsp:握手 ping/pong
 *              - TXrpcNodeSendKnockReq/Rsp:函数发现
 *              - TXrpcNodeSendCallReq / SendCallRsp:过程调用
 *              物理位置刻意放在 common/ 平铺目录(不在 include/xrpc/ 下),
 *              用户通过标准 include 路径搜不到,实现物理隔离。
 *              接收侧入口已上移至 xrpc_node.c 的 TXrpcNodeHandleRxFrame
 *              (2026/06/21 v1.2 起 inner 升格为权威发送路径,
 *              原 TXrpcNodeRecvMsgFrameNotify 删除)。
 * @copyright   Copyright (c) 2026 AuyvJpz
 */

#ifndef _XRPC_NODE_INNER_H_
#define _XRPC_NODE_INNER_H_

#include "xrpc/xrpc_node.h"          /* TXrpcNode / TXrpcProcessObject */
#include "xrpc/xrpc.h"               /* XrpcSleepMs / XrpcGetSysTick */
#include "xrpc/xrpc_peer.h"          /* TXrpcPeerNode(公开头) */
#include "xrpc_peer_inner.h"         /* 内部头:仅 .c 可见,提供 SetOwner / PeerRunOnce */
#include "xrpc/xrpc_msg.h"           /* TXrpcMsgHeadMem / Knock / Call 帧结构 */
#include "xrpc/xrpc_msg_param_list.h"     /* TXrpcMsgParamList */
#include "xrpc/xrpc_msg_ext.h"            /* 信封 / 通知 / Hello 能力 */
#include <string.h>


#define XRPC_PROCESS_AUTO_KNOCK_INIT_CNT       (3U)


#ifdef __cplusplus
extern "C"
{
#endif


/* ============================================================
 *  消息发送
 *  全部以 peer_node->send_buff_ptr / send_buff_size 为编码缓冲,
 *  最终通过 IXrpcLinkSend 推到底层链路。
 * ============================================================ */

/**
 * @brief 发送一帧完整的 xrpc 消息:头部(tag/frame_crc/frame_len/node_id/version/type/user_tag/seq/ttl)
 *        由本函数填充, payload 由调用方预先写到 peer_node->send_buff_ptr 的
 *        payload 段。
 * @param self         节点对象指针
 * @param peer_node    目标 peer;send_buff_ptr / send_buff_size 必须已配置
 * @param type         消息类型(@ref TXrpcMsgType)
 * @param payload_data payload 数据指针(允许为 NULL,此时 payload_size 必须为 0)
 * @param payload_size payload 字节数
 * @return  >= 0 实际发送字节数;< 0 失败错误码
 *          - kXrpcErrInvalidParam    参数非法
 *          - kXrpcErrBufferOverflow  send_buff 容量不足
 *          - kXrpcErrLinkSendFailed  链路层发送失败
 * @note    本函数是所有 TXrpcNodeSendXxx 的共同骨架,直接对外暴露便于
 *          部署层 / 生成代码发送自定义 payload 时复用。
 */
Int32 TXrpcNodeSendMsgFrame(TXrpcNode *self, TXrpcPeerNode *peer_node,
                            UInt16 type, UInt8 *payload_data, UInt16 payload_size);


/**
 * @brief 发送 HelloReq(自身 node_id 通告,无 payload)。
 * @param self      节点对象指针
 * @param peer_node 目标 peer
 * @return  >= 0 发送字节数;< 0 错误码
 * @note    kXrpcMsgTypeHelloReq
 */
Int32 TXrpcNodeSendHelloReq(TXrpcNode *self, TXrpcPeerNode *peer_node);


/**
 * @brief 发送 HelloRsp(应答对端 HelloReq,无 payload)。
 * @param self      节点对象指针
 * @param peer_node 目标 peer
 * @param dst_node_id 目的节点,总线链路上写入信封
 * @return  >= 0 发送字节数;< 0 错误码
 * @note    kXrpcMsgTypeHelloRsp
 */
Int32 TXrpcNodeSendHelloRsp(TXrpcNode *self, TXrpcPeerNode *peer_node, UInt32 dst_node_id);


/**
 * @brief 发送 KnockReq:询问对端是否存在指定 process。
 * @param self       节点对象指针
 * @param peer_node  目标 peer
 * @param process_id 待询问的 process_id
 * @return  >= 0 发送字节数;< 0 错误码
 *          - kXrpcErrInvalidParam    process_id 未在 process_table 中找到
 *          - kXrpcErrBufferOverflow  send_buff 容量不足
 *          - kXrpcErrLinkSendFailed  链路层发送失败
 * @note    kXrpcMsgTypeKnockReq
 */
Int32 TXrpcNodeSendKnockReq(TXrpcNode *self, TXrpcPeerNode *peer_node, UInt32 process_id);


/**
 * @brief 发送 KnockRsp:应答对端 KnockReq。
 * @param self      节点对象指针
 * @param peer_node 目标 peer
 * @param dst_node_id 目的节点,总线链路上写入信封
 * @param req_seq   请求帧头的 seq
 * @param req_msg   对端发来的 KnockReq
 * @return  >= 0 发送字节数;< 0 错误码
 * @note    errcode 语义:
 *          - kXrpcErrProcessNotFound   process_id 未在本地注册
 *          - 0                        process_id 已注册但 process_key 不匹配
 *          - 1                        process_id + process_key 完全匹配
 *          kXrpcMsgTypeKnockRsp
 */
Int32 TXrpcNodeSendKnockRsp(TXrpcNode *self, TXrpcPeerNode *peer_node,
                            UInt32 dst_node_id, UInt32 req_seq,
                            const TXrpcMsgKnockReqMem *req_msg);


/**
 * @brief 发送 CallReq:需要等待对端回复的同步调用请求。
 * @param self          节点对象指针
 * @param peer_node     目标 peer
 * @param process_id    目标 process_id
 * @param input_params  已编码好的入参列表
 * @return  >= 0 发送字节数;< 0 错误码
 * @note    本帧的帧头 seq 记入 process->runtime_data->process_seq,用来匹配应答。
 *          kXrpcMsgTypeCallReq
 */
Int32 TXrpcNodeSendCallReq(TXrpcNode *self, TXrpcPeerNode *peer_node,
                           UInt32 process_id, TXrpcMsgParamList *input_params,
                           TXrpcMsgParamList *output_params);


/**
 * @brief 发送 CallRsp:对 CallReq 的应答。
 * @param self          节点对象指针
 * @param peer_node     目标 peer
 * @param dst_node_id   目的节点,总线链路上写入信封
 * @param req_seq       请求帧头的 seq
 * @param process_id    过程 ID
 * @param output_params 已编码好的出参列表(允许为 NULL 表示无出参)
 * @param errcode       执行结果(>= 0 成功;< 0 失败错误码);
 *                      当 errcode < 0 或 output_params == NULL 时仅发送头部与 errcode
 * @return  >= 0 发送字节数;< 0 错误码
 * @note    kXrpcMsgTypeCallRsp
 */
Int32 TXrpcNodeSendCallRsp(TXrpcNode *self, TXrpcPeerNode *peer_node,
                           UInt32 dst_node_id, UInt32 req_seq, UInt32 process_id,
                           TXrpcMsgParamList *output_params, Int32 errcode);


/**
 * @brief 发送一帧 NotifyEvent,载荷取自发布槽。
 * @param self      节点对象指针
 * @param peer_node 目标 peer
 * @param pub       发布对象
 * @param sent_seq  输出本次发送的序号
 * @return  >= 0 发送字节数;< 0 错误码
 * @note    加锁顺序:先发送锁,后节点锁。在发送锁内读快照,补发时内容与序号一致。
 */
Int32 TXrpcNodeSendNotifyEvent(TXrpcNode *self, TXrpcPeerNode *peer_node,
                               TXrpcNotifyPubObject *pub, UInt32 *sent_seq);


/**
 * @brief 发送 NotifyAck。
 * @return  >= 0 发送字节数;< 0 错误码
 */
Int32 TXrpcNodeSendNotifyAck(TXrpcNode *self, TXrpcPeerNode *peer_node, UInt32 dst_node_id,
                             UInt32 notify_id, UInt32 publisher, UInt32 seq, Int32 ack_result);


/* ============================================================
 *  消息接收
 * ============================================================ */


void TXrpcNodeHandleRxFrame(TXrpcNode *self, TXrpcPeerNode *peer_node, TXrpcMsgHeadMem *msg);





/* ============================================================
 *  process Knock
 * ============================================================ */


Int32 TXrpcNodeProcessKnockDeal(TXrpcNode *self, TXrpcPeerNode *peer_node);


/**
 * @brief 链路断开时重置 peer 与依赖它的过程状态。
 * @param self      节点对象指针
 * @param peer_node 断开的 peer
 * @note  恢复为未握手状态(静态配置的 peer ID 保留),清空拼帧缓存;
 *        provider 为该 peer 的过程恢复为未发现,在途调用立即以
 *        kXrpcErrPeerStatusInvalid 结束,不必等到超时。
 *        只能在轮询线程调用,FDN 缓存没有锁保护。
 */
void TXrpcNodePeerLinkDown(TXrpcNode *self, TXrpcPeerNode *peer_node);


/**
 * @brief 查找过程的链路绑定。
 * @return  绑定表项;没有绑定返回 NULL
 */
const TXrpcProcessBinding *TXrpcNodeFindBinding(const TXrpcNode *self, UInt32 process_id);


/**
 * @brief 把一段现成的载荷发到指定 peer,供服务中继使用。
 * @param self        节点对象指针
 * @param peer_node   目标 peer
 * @param dst_node_id 目的节点,总线链路上写入信封
 * @param type        消息类型
 * @param frame_seq   帧头 seq
 * @param ttl         帧头 ttl
 * @param payload     载荷
 * @param payload_len 载荷字节数
 * @param patch_seq   是否改写载荷开头的 req_seq
 * @param req_seq     新的 req_seq
 * @return  >= 0 发送字节数;< 0 错误码
 */
Int32 TXrpcNodeRelayForward(TXrpcNode *self, TXrpcPeerNode *peer_node, UInt32 dst_node_id,
                            UInt16 type, UInt32 frame_seq, UInt8 ttl,
                            const char *payload, UInt16 payload_len,
                            Bool patch_seq, UInt32 req_seq);


/**
 * @brief 记录一次状态。调用方持节点锁
 * @param peer_node 相关 peer,没有传 NULL
 */
void TXrpcNodeStatusRecordNoLock(TXrpcNode *self, TXrpcStatusCode code,
                                 const TXrpcPeerNode *peer_node, UInt32 node_id,
                                 UInt32 object_id, UInt32 need, UInt32 actual);


/**
 * @brief 记录一次状态,内部取节点锁。
 */
void TXrpcNodeStatusRecord(TXrpcNode *self, TXrpcStatusCode code,
                           const TXrpcPeerNode *peer_node, UInt32 node_id,
                           UInt32 object_id, UInt32 need, UInt32 actual);


/**
 * @brief 扫描状态表并回调。只在 RunOnce 线程调用
 */
void TXrpcNodeStatusFlush(TXrpcNode *self);


/**
 * @brief 取下一个帧头 seq。调用方持节点锁
 * @note  节点内唯一,所以应答只凭 req_seq 就能找到请求。
 *        发送前要记下序号时用这个,以免应答先于记录到达。
 */
UInt32 TXrpcNodeNextSeqNoLock(TXrpcNode *self);


/**
 * @brief 取下一个帧头 seq,内部取节点锁。
 */
UInt32 TXrpcNodeNextSeq(TXrpcNode *self);


/**
 * @brief 发送一帧 CallReq,不改动过程状态。
 * @param self        节点对象指针
 * @param peer_node   目标 peer
 * @param process     目标过程
 * @param seq         流水号
 * @param dst_node_id 目的节点,总线链路上写入信封
 * @param input       入参列表,可为 NULL
 * @return  >= 0 发送字节数;< 0 错误码
 */
Int32 TXrpcNodeSendCallFrame(TXrpcNode *self, TXrpcPeerNode *peer_node,
                             TXrpcProcessObject *process, UInt32 seq,
                             UInt32 dst_node_id, const TXrpcMsgParamList *input);


#ifdef __cplusplus
}  ///< extern "C"
#endif

#endif  ///< _XRPC_NODE_INNER_H_

/**
 * @}
 */

