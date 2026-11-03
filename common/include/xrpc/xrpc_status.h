/**
 * @addtogroup xrpc_status
 * xrpc_status
 * @{
 */
/**
 * @file        xrpc_status.h
 * @brief       运行时状态上报：缓存不足、表项用尽、配置不一致等异常都从这里报给应用。
 * @note        发现异常的线程只把次数和最近一次的详情记入节点的状态表，
 *              RunOnce 再扫描状态表并回调，所以回调固定在 RunOnce 线程、节点锁之外执行，
 *              不需要动态内存。同一状态的回调间隔不小于配置的间隔，这期间的次数累加。
 * @copyright   Copyright (c) 2026 AuyvJpz
 */

#ifndef _XRPC_STATUS_H_
#define _XRPC_STATUS_H_

#include "xrpc/xrpc_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

struct TXrpcNodeTag;

/** 没有对应链路时的 link_index */
#define XRPC_STATUS_LINK_NONE  (0xFFFFU)

/** 状态码。need、actual 的含义见各项说明 */
typedef enum TXrpcStatusCodeTag
{
    kXrpcStatusRxFrameTooLarge  = 0,    ///< 收到的帧放不下。need 帧长，actual 接收缓存大小
    kXrpcStatusTxBufferOverflow = 1,    ///< 发送缓冲不足。need 帧长，actual 缓冲大小
    kXrpcStatusTxFrameTooLarge  = 2,    ///< 超过链路或对端的帧长上限。need 帧长，actual 上限
    kXrpcStatusRouteTableFull   = 3,    ///< 中继路由表满。actual 表项数
    kXrpcStatusRelayCallFull    = 4,    ///< 中继转发槽满。actual 槽数
    kXrpcStatusRemoteTableFull  = 5,    ///< 总线远端节点表满。actual 表项数
    kXrpcStatusVersionMismatch  = 6,    ///< 版本不一致。need 本节点版本，actual 帧里的版本
    kXrpcStatusUserTagMismatch  = 7,    ///< user_tag 不一致。need 本节点的值，actual 帧里的值
    kXrpcStatusNodeIdConflict   = 8,    ///< 总线上有节点与本节点 ID 相同
    kXrpcStatusTtlExpired       = 9,    ///< 跳数耗尽，帧不再转发。object_id 为过程 ID 或通知 ID
    kXrpcStatusNotifySeqGap     = 10,   ///< 事件序号缺口。need 期望序号，actual 实际序号
    kXrpcStatusNotifyKeyMismatch = 11,  ///< 事件 key 不匹配，事件丢弃。actual 序号
    kXrpcStatusNotifyWrongPublisher = 12,   ///< 事件来源不是期望的发布方，事件丢弃。actual 序号
    kXrpcStatusNotifyBadParams  = 13,   ///< 事件参数解析失败，事件丢弃。actual 序号
    kXrpcStatusNotifyRecordFull = 14,   ///< 中继淘汰了还没收到 Ack 的记录，这个事件的 Ack 可能回不去。actual 表项数
    kXrpcStatusCount
} TXrpcStatusCode;

/** 一种状态的记录 */
typedef struct TXrpcStatusInfoTag TXrpcStatusInfo;
struct TXrpcStatusInfoTag
{
    TXrpcStatusCode code;
    UInt16 link_index;      ///< 相关链路，XRPC_STATUS_LINK_NONE 表示没有
    UInt32 node_id;         ///< 相关节点，0 表示没有
    UInt32 object_id;       ///< 相关对象：过程 ID、通知 ID 或消息类型，0 表示没有
    UInt32 count;           ///< 启动以来累计次数
    UInt32 need;            ///< 最近一次需要的值
    UInt32 actual;          ///< 最近一次实际的值
};

/**
 * @brief 状态回调。在 RunOnce 线程、节点锁之外执行，不得做阻塞或耗时操作。
 * @param info 最近一次的详情，只在回调内有效
 */
typedef void (*TXrpcStatusCb)(struct TXrpcNodeTag *node, const TXrpcStatusInfo *info,
                              void *user_data);

/** 状态表的一项，内容由库管理 */
typedef struct TXrpcStatusEntryTag TXrpcStatusEntry;
struct TXrpcStatusEntryTag
{
    TXrpcStatusInfo info;
    UInt32 reported_count;  ///< 上次回调时的 count
    UInt32 report_tick;     ///< 上次回调的时刻
};

/**
 * @brief 设置状态回调。
 * @param node        节点对象
 * @param cb          回调，NULL 表示不回调，状态表照常计数
 * @param interval_ms 同一状态两次回调的最小间隔，0 表示每次 RunOnce 都可以回调
 * @param user_data   透传给回调的上下文
 * @return  成功返回 kXrpcErrOk；node 为 NULL 返回 kXrpcErrInvalidParam
 * @note    启动 RunOnce 之前设置。
 */
Int32 TXrpcNodeSetStatusCb(struct TXrpcNodeTag *node, TXrpcStatusCb cb, UInt32 interval_ms,
                           void *user_data);

/**
 * @brief 取某种状态的累计次数，不用回调时可以主动查询。
 * @return  累计次数；参数非法返回 0
 */
UInt32 TXrpcNodeGetStatusCount(struct TXrpcNodeTag *node, TXrpcStatusCode code);

#ifdef __cplusplus
}  ///< extern "C"
#endif

#endif  ///< _XRPC_STATUS_H_

/**
 * @}
 */
