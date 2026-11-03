/**
 * @addtogroup xrpc_msg_ext
 * xrpc_msg_ext
 * @{
 */
/**
 * @file        xrpc_msg_ext.h
 * @brief       新增消息的线上结构：寻址信封、通知事件、通知应答、Hello 能力字段。
 * @note        字段规则与 xrpc_msg.h 相同：多字节字段是大端字节数组，经 xrpc_wire.h 读写。
 * @copyright   Copyright (c) 2026 AuyvJpz
 */

#ifndef _XRPC_MSG_EXT_H_
#define _XRPC_MSG_EXT_H_

#include "xrpc/xrpc_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** 寻址信封头长度 */
#define XRPC_MSG_ADDRESSED_HEAD_SIZE  (6)

#pragma pack(push, 1)

/** 0x0130 Addressed：把任意单播消息包一层，加上目的节点 */
typedef struct TXrpcMsgAddressedMemTag TXrpcMsgAddressedMem;
struct TXrpcMsgAddressedMemTag
{
    UInt8 dst_node[4];      // 目的节点，kXrpcNodeIdBroadcast 表示广播
    UInt8 inner_type[2];    // 内层消息类型
    char inner_payload[1];  // 内层载荷，格式与原消息完全相同
};

/** 0x0120 NotifyEvent */
typedef struct TXrpcMsgNotifyEventMemTag TXrpcMsgNotifyEventMem;
struct TXrpcMsgNotifyEventMemTag
{
    UInt8 notify_id[4];
    UInt8 notify_key[4];
    UInt8 origin_node[4];   // 最初发布事件的节点，经中继时不变
    UInt8 seq[4];           // 按（通知，发布方）递增，补发不变
    UInt8 flags;            // bit0 = need_ack
    UInt8 param_cnt;
    char param_list[1];
};

/** 0xF120 NotifyAck */
typedef struct TXrpcMsgNotifyAckMemTag TXrpcMsgNotifyAckMem;
struct TXrpcMsgNotifyAckMemTag
{
    UInt8 notify_id[4];
    UInt8 origin_node[4];   // 发出应答的节点，经中继时不变
    UInt8 publisher[4];     // 事件的发布方，中继靠它找到来路
    UInt8 seq[4];
    UInt8 result[4];        // 0 成功，< 0 订阅方拒绝
};

/** Hello 的可选载荷 */
typedef struct TXrpcMsgHelloCapMemTag TXrpcMsgHelloCapMem;
struct TXrpcMsgHelloCapMemTag
{
    UInt8 max_recv_frame[4];  // 本端能接收的最大整帧字节数
};

#pragma pack(pop)

#define XRPC_NOTIFY_FLAG_NEED_ACK  (0x01U)

#ifdef __cplusplus
}  ///< extern "C"
#endif

#endif  ///< _XRPC_MSG_EXT_H_

/**
 * @}
 */
