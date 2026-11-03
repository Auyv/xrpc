/**
 * @file        xrpc_msg.h
 * @brief       Xrpc 协议 v2 消息结构体
 * @note        手工维护,不再由 dpt 工具生成。
 *              多字节字段都是字节数组,内容为大端,只能经 xrpc_wire.h 读写,
 *              漏掉转换时编译不过。
 * @copyright   Copyright (c) 2026 AuyvJpz
 */

#ifndef _XRPC_MSG_H_
#define _XRPC_MSG_H_

#include "xrpc/xrpc_types.h"
#include "xrpc/xrpc_common.h"   /* 统一消息类型枚举 TXrpcMsgType(2026/06/21 合并) */
#include "xrpc/xrpc_wire.h"

#ifdef __cplusplus
extern "C"
{
#endif

#pragma pack(push, 1)

/** 帧头。frame_crc 与 frame_len 的计算范围都从 node_id 到帧尾 */
typedef struct TXrpcMsgHeadMemTag TXrpcMsgHeadMem;
struct TXrpcMsgHeadMemTag
{
    char tag[4];            // 帧标识
    UInt8 frame_crc[2];     // 帧校验
    UInt8 frame_len[2];     // 帧长度
    UInt8 node_id[4];       // 发送方节点
    UInt8 version[2];       // 协议版本,决定帧头之后的解析方式
    UInt8 type[2];          // 消息类型
    UInt8 user_tag[4];      // 应用标识,不同的值互相隔离
    UInt8 seq[4];           // 本节点发送的数据包流水号
    UInt8 ttl;              // 剩余跳数,每过一个中继减 1
    UInt8 flags;            // 发送写 0,未知位接收忽略
    UInt8 reserved[2];      // 发送写 0,接收忽略
    char payload[1];        // 消息数据,长度 frame_len - 20
};

/** 0x0101 KnockReq:询问过程是否存在,请求序号就是帧头 seq */
typedef struct TXrpcMsgKnockReqMemTag TXrpcMsgKnockReqMem;
struct TXrpcMsgKnockReqMemTag
{
    UInt8 process_id[4];    // 过程 ID
    UInt8 process_key[4];   // 过程 KEY,防止重名
};

/** 0xF101 KnockRsp */
typedef struct TXrpcMsgKnockRspMemTag TXrpcMsgKnockRspMem;
struct TXrpcMsgKnockRspMemTag
{
    UInt8 req_seq[4];       // 对应请求的帧头 seq
    UInt8 process_id[4];    // 过程 ID
    UInt8 errcode[4];       // 0 表示存在但 KEY 无效;1 表示可用;< 0 表示不存在
};

/** 0x0111 CallReq:请求序号就是帧头 seq */
typedef struct TXrpcMsgCallReqMemTag TXrpcMsgCallReqMem;
struct TXrpcMsgCallReqMemTag
{
    UInt8 process_id[4];    // 过程 ID
    UInt8 process_key[4];   // 过程 KEY
    UInt8 param_cnt;        // 参数个数
    char param_list[1];     // 参数列表
};

/** 0xF111 CallRsp */
typedef struct TXrpcMsgCallRspMemTag TXrpcMsgCallRspMem;
struct TXrpcMsgCallRspMemTag
{
    UInt8 req_seq[4];       // 对应请求的帧头 seq
    UInt8 process_id[4];    // 过程 ID
    UInt8 errcode[4];       // >= 0 表示执行成功,其他为错误码
    UInt8 param_cnt;        // 参数个数
    char param_list[1];     // 参数列表
};

#pragma pack(pop)

/** 解码后的参数项,不是线上结构 */
typedef struct TXrpcMsgParamItemTag TXrpcMsgParamItem;
struct TXrpcMsgParamItemTag
{
    UInt8 param_id;     // 参数 id,每个过程的输入、输出参数各自独立
    UInt8 param_type;   // 0x00: U8; 0x01: I8; 0x02: U16; 0x03: I16; 0x04: U32; 0x05: I32; 0x06: U64; 0x07: I64; 0x10: Bytes; 0x11: String
    UInt16 param_len;   // 参数长度
    void *param_value;  // 参数值
};

#ifdef __cplusplus
}  ///< extern "C"
#endif

#endif  ///< _XRPC_MSG_H_

