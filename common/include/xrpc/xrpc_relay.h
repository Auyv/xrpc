/**
 * @addtogroup xrpc_relay
 * xrpc_relay
 * @{
 */
/**
 * @file        xrpc_relay.h
 * @brief       服务中继：本节点没有的过程，替请求方到其他链路上发现并转发调用。
 * @note        A - B - C 中 B 开启中继后，A 把 B 当作提供者，C 把 B 当作调用方，
 *              A、C 不需要任何改动，线上帧也和普通调用完全相同。
 *              中继在 RunOnce 线程里完成，只转发和记账，不阻塞。
 *              事件通知不走中继，需要跨跳时在中间节点订阅后重新发布。
 *              拓扑不能有环路。设计说明见 docs/XRPC设计文档.md 第 11 章。
 * @copyright   Copyright (c) 2026 AuyvJpz
 */

#ifndef _XRPC_RELAY_H_
#define _XRPC_RELAY_H_

#include "xrpc/xrpc_types.h"
#include "xrpc/xrpc_common.h"
#include "xrpc/xrpc_peer.h"

#ifdef __cplusplus
extern "C"
{
#endif

struct TXrpcNodeTag;

/** 路由状态 */
typedef enum TXrpcRelayRouteStateTag
{
    kXrpcRelayRouteFree    = 0,
    kXrpcRelayRoutePending = 1,     ///< 已向其他链路询问，等待应答
    kXrpcRelayRouteReady   = 2,     ///< 已找到提供者
} TXrpcRelayRouteState;

/** 一条路由。数组由调用方提供，内容由库管理 */
typedef struct TXrpcRelayRouteTag TXrpcRelayRoute;
struct TXrpcRelayRouteTag
{
    TXrpcRelayRouteState state;
    UInt32 process_id;
    UInt32 process_key;
    TXrpcPeerNode *peer_node;       ///< 提供者所在的链路
    UInt32 provider_node_id;
    UInt32 seq;                     ///< 询问时使用的中继序号
    TXrpcPeerNode *origin_peer;     ///< 询问方所在的链路
    UInt32 origin_node_id;
    UInt32 origin_seq;
    UInt32 start_tick;
    UInt32 replies_left;            ///< 还没应答的链路数
    Int32 negative_code;            ///< 全部否定时回给询问方的结果
};

/** 一次在途的转发调用。数组由调用方提供，内容由库管理 */
typedef struct TXrpcRelayCallTag TXrpcRelayCall;
struct TXrpcRelayCallTag
{
    Bool used;
    UInt32 process_id;
    UInt32 seq;
    TXrpcPeerNode *target_peer;
    TXrpcPeerNode *origin_peer;
    UInt32 origin_node_id;
    UInt32 origin_seq;
    UInt32 start_tick;
    Int32 fail_code;                ///< 非 0 表示已失败，下一轮 RunOnce 回给调用方
};

/** 转发表项的运行时数据：记住最近转发的事件，防止成环时事件风暴。内容由库管理 */
typedef struct TXrpcNotifyForwardRuntimeTag TXrpcNotifyForwardRuntime;
struct TXrpcNotifyForwardRuntimeTag
{
    Bool has_event;
    UInt32 origin_node;
    UInt32 seq;
    const TXrpcPeerNode *ingress;   ///< 事件来自哪条链路
};

/** 一个通知转发到哪些链路，收到它的那条链路自动排除。可放 Flash */
typedef struct TXrpcNotifyForwardTag TXrpcNotifyForward;
struct TXrpcNotifyForwardTag
{
    UInt32 notify_id;
    UInt16 link_count;
    const UInt16 *link_index;
    TXrpcNotifyForwardRuntime *runtime;
};

/** 一个待应答的事件，Ack 靠它原路返回。数组由调用方提供，内容由库管理 */
typedef struct TXrpcNotifyAckRecordTag TXrpcNotifyAckRecord;
struct TXrpcNotifyAckRecordTag
{
    Bool used;
    Bool acked;                     ///< 已转发过 Ack，表满时优先淘汰
    UInt32 notify_id;
    UInt32 publisher;
    UInt32 seq;
    TXrpcPeerNode *origin_peer;     ///< 事件来自哪条链路
    UInt32 origin_node_id;          ///< 上一跳节点
    UInt32 start_tick;
};

/** 中继配置 */
typedef struct TXrpcRelayConfigTag TXrpcRelayConfig;
struct TXrpcRelayConfigTag
{
    TXrpcRelayRoute *routes;        ///< 路由表，容量决定最多中继几个过程
    UInt32 route_count;
    TXrpcRelayCall *calls;          ///< 转发调用表，容量决定最多同时转发几个调用
    UInt32 call_count;
    UInt32 knock_timeout_ms;        ///< 询问其他链路的等待时长；0 表示 500ms
    UInt32 call_timeout_ms;         ///< 等待提供者应答的时长；0 表示 5000ms
    const TXrpcNotifyForward *forwards;     ///< 事件转发表；NULL 表示不转发事件
    UInt32 forward_count;
    TXrpcNotifyAckRecord *ack_records;      ///< Ack 记录表，容量决定同时有几个待应答的事件，
                                            ///< 表满时淘汰最老的记录
    UInt32 ack_record_count;
    UInt32 notify_timeout_ms;       ///< Ack 记录保留时长，发布方补发时刷新；0 表示 1000ms
};

/**
 * @brief 开启服务中继。不调用时节点不转发任何请求，与旧行为完全一致。
 * @param node   节点对象
 * @param config 中继配置，表由调用方提供，生命周期不短于节点
 * @return  成功返回 kXrpcErrOk；参数非法返回 kXrpcErrInvalidParam
 * @note    必须在开始 RunOnce 之前调用。本地有实现的过程优先本地执行。
 *          过程绑定表同样约束中继：绑定了的过程只到指定链路和节点上询问。
 *          转发事件时 ack_records 可以为 NULL，这时 Ack 回不去，只适合不要求应答的事件。
 */
Int32 TXrpcNodeSetRelay(struct TXrpcNodeTag *node, const TXrpcRelayConfig *config);

#ifdef __cplusplus
}  ///< extern "C"
#endif

#endif  ///< _XRPC_RELAY_H_

/**
 * @}
 */
