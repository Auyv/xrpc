/**
 * @addtogroup xrpc_notify
 * xrpc_notify
 * @{
 */
/**
 * @file        xrpc_notify.h
 * @brief       事件通知（订阅发布）。订阅关系编译期写死，发布异步完成。
 * @note        发布方配置"发布到哪些链路"和"必须应答的节点"两张独立的表；
 *              订阅方写明期望的发布方。所有回调都在 RunOnce 线程、节点锁之外执行，
 *              不得在回调里做阻塞或耗时操作。
 *              设计说明见 docs/XRPC设计文档.md 第 11 章。
 * @copyright   Copyright (c) 2026 AuyvJpz
 */

#ifndef _XRPC_NOTIFY_H_
#define _XRPC_NOTIFY_H_

#include "xrpc/xrpc_types.h"
#include "xrpc/xrpc_common.h"
#include "xrpc/xrpc_msg_param_list.h"

#ifdef __cplusplus
extern "C"
{
#endif

struct TXrpcNodeTag;

/** ack_work 需要的字数，n 为 ack_node_count：两张位图加两张节点列表 */
#define XRPC_NOTIFY_ACK_WORK_WORDS(n)  \
    ((2U * (((UInt32)(n) + 31U) / 32U)) + (2U * (UInt32)(n)))

/** 发布结果 */
typedef enum TXrpcNotifyResultCodeTag
{
    kXrpcNotifyOk         = 0,  ///< 发出（无需应答）或应答收齐
    kXrpcNotifyAckTimeout = 1,  ///< 补发次数用尽，仍有节点未应答
    kXrpcNotifyRejected   = 2,  ///< 应答收齐，但有节点拒绝（如 key 不匹配）
    kXrpcNotifySendFailed = 3,  ///< 链路发送失败
    kXrpcNotifyLinkDown   = 4,  ///< 所有链路都未连接
    kXrpcNotifyReplaced   = 5,  ///< 槽位忙时被新数据替换
    kXrpcNotifyTooLarge   = 6,  ///< 超过链路或对端的帧长上限
} TXrpcNotifyResultCode;

/** 完成回调携带的结果 */
typedef struct TXrpcNotifyResultTag TXrpcNotifyResult;
struct TXrpcNotifyResultTag
{
    UInt32 notify_id;
    UInt32 seq;
    TXrpcNotifyResultCode code;
    UInt8  attempts;                            ///< 实际发送次数
    UInt32 elapsed_ms;                          ///< 从受理到完成的耗时
    UInt32 replaced_count;                      ///< code 为 Replaced 时：累计被替换的包数，seq 是最后一包
    UInt16 acked_count;
    UInt16 missing_count;
    const UInt32 *acked;                        ///< 已确认的节点，只在回调内有效
    const UInt32 *missing;                      ///< 未确认的节点，只在回调内有效
};

typedef void (*TXrpcNotifyDoneCb)(struct TXrpcNodeTag *node,
                                  const TXrpcNotifyResult *result, void *user_data);

/** 每次发布的选项 */
typedef struct TXrpcNotifyOptTag TXrpcNotifyOpt;
struct TXrpcNotifyOptTag
{
    Bool   need_ack;        ///< False：发出即完成
    UInt32 timeout_ms;      ///< 每次等待应答的时长
    UInt8  retry;           ///< 超时后补发的次数
};

/** 发布方配置，每个通知一条，可放 Flash */
typedef struct TXrpcNotifyPubDescTag TXrpcNotifyPubDesc;
struct TXrpcNotifyPubDescTag
{
    UInt32 notify_id;
    UInt32 notify_key;
    UInt16 link_count;
    const UInt16 *link_index;                   ///< 发布到这些链路，共 link_count 项
    UInt16 ack_node_count;
    const UInt32 *ack_node_id;                  ///< 必须应答的节点，共 ack_node_count 项
    UInt32 *ack_work;                           ///< 应答工作区，放在 RAM，
                                                ///< 共 XRPC_NOTIFY_ACK_WORK_WORDS(ack_node_count) 字
    UInt8  *slot_buf;                           ///< 快照缓冲，由调用方静态提供
    UInt16 slot_size;
};

/** 发布槽状态 */
typedef enum TXrpcNotifySlotStateTag
{
    kXrpcNotifySlotFree    = 0,
    kXrpcNotifySlotPending = 1,     ///< 等待发出
    kXrpcNotifySlotWaitAck = 2,     ///< 已发出，等待应答
} TXrpcNotifySlotState;

/** 发布方运行时数据，内容由库管理 */
typedef struct TXrpcNotifyPubRuntimeTag TXrpcNotifyPubRuntime;
struct TXrpcNotifyPubRuntimeTag
{
    TXrpcNotifySlotState state;
    UInt32 seq;
    TXrpcNotifyOpt opt;
    TXrpcNotifyDoneCb done_cb;
    void *user_data;
    UInt8  param_cnt;
    UInt16 param_len;
    UInt8  retry_left;
    UInt8  attempts;
    UInt32 accept_tick;
    UInt32 wait_tick;
    TXrpcNotifyResultCode send_code;    ///< 最近一次发送的结果
    TXrpcNotifyDoneCb replaced_cb;      ///< 待上报的替换记录
    void *replaced_user;
    UInt32 replaced_seq;
    UInt32 replaced_count;
};

typedef struct TXrpcNotifyPubObjectTag TXrpcNotifyPubObject;
struct TXrpcNotifyPubObjectTag
{
    const TXrpcNotifyPubDesc *desc;
    TXrpcNotifyPubRuntime *runtime;
};

/**
 * @brief 事件回调。
 * @param params 事件参数，指向接收缓存，只在回调内有效
 */
typedef void (*TXrpcNotifyCb)(struct TXrpcNodeTag *node, UInt32 notify_id,
                              UInt32 publisher_node_id, UInt32 seq,
                              const TXrpcMsgParamList *params, void *user_data);

/** 订阅方配置，每个通知一条，可放 Flash */
typedef struct TXrpcNotifySubDescTag TXrpcNotifySubDesc;
struct TXrpcNotifySubDescTag
{
    UInt32 notify_id;
    UInt32 notify_key;
    UInt32 publisher_node_id;   ///< 期望的发布方；0 表示接受任意发布方（观察者用）
    Bool   send_ack;            ///< 观察者为 False
    TXrpcNotifyCb cb;
    void  *user_data;
};

typedef struct TXrpcNotifySubRuntimeTag TXrpcNotifySubRuntime;
struct TXrpcNotifySubRuntimeTag
{
    Bool   has_seq;
    UInt32 last_seq;
    UInt32 last_publisher;
};

typedef struct TXrpcNotifySubObjectTag TXrpcNotifySubObject;
struct TXrpcNotifySubObjectTag
{
    const TXrpcNotifySubDesc *desc;
    TXrpcNotifySubRuntime *runtime;
};

/**
 * @brief 设置发布表。
 * @param node     节点对象
 * @param objects  发布对象数组
 * @param count    项数
 * @param seq_seed 序号初值。取与启动相关的值（tick、随机数或掉电保存的计数），
 *                 否则发布方重启后序号可能恰好等于订阅方记住的值，事件被当成重复丢弃
 * @return  成功返回 kXrpcErrOk；参数非法返回 kXrpcErrInvalidParam
 */
Int32 TXrpcNodeSetNotifyPubs(struct TXrpcNodeTag *node, TXrpcNotifyPubObject **objects,
                             UInt32 count, UInt32 seq_seed);

/**
 * @brief 设置订阅表。
 * @return  成功返回 kXrpcErrOk；参数非法返回 kXrpcErrInvalidParam
 */
Int32 TXrpcNodeSetNotifySubs(struct TXrpcNodeTag *node, TXrpcNotifySubObject **objects,
                             UInt32 count);

/**
 * @brief 发布一个事件。非阻塞：数据拷入静态槽位后立即返回，成功只表示已受理。
 * @param node      节点对象
 * @param notify_id 通知 ID，必须在发布表中
 * @param params    参数列表，返回后可以立刻修改或释放；NULL 表示无参数
 * @param opt       发布选项，不可为 NULL
 * @param done_cb   完成回调，成功、失败都回调；可为 NULL
 * @param user_data 透传给回调的上下文
 * @return  kXrpcErrOk 已受理；kXrpcErrItemNotFound 通知不在发布表；
 *          kXrpcErrTxBufferOverflow 超过槽位大小；kXrpcErrInvalidParam 参数非法
 * @note    槽位忙时新数据替换旧数据，被替换的包回调 kXrpcNotifyReplaced。
 *          连续替换时合并成一次回调，replaced_count 给出总包数，done_cb 取最后一包的。
 */
Int32 XrpcNotifyPublish(struct TXrpcNodeTag *node, UInt32 notify_id,
                        const TXrpcMsgParamList *params,
                        const TXrpcNotifyOpt *opt,
                        TXrpcNotifyDoneCb done_cb, void *user_data);

#ifdef __cplusplus
}  ///< extern "C"
#endif

#endif  ///< _XRPC_NOTIFY_H_

/**
 * @}
 */
