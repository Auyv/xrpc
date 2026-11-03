/**
 * @addtogroup xrpc_node
 * xrpc_node
 * @{
 */
/**
 * @file        xrpc_node.h
 * @brief       节点对象,标识当前节点 ID 和使用的 xrpc 协议版本,并管理 peer_cache。
 * @note        监控全部 link 接收的数据,收到有效数据时按 process_table 路由分发。
 *              本头保留 TXrpcNodeSetPeerCache 作为节点与 PeerCache 的"装配桥"
 *              (内部封装 owner 反向引用);其他 PeerCache API
 *              (TXrpcPeerCacheCreate / Destroy / AddLink / PeerCount / PeerLink / PeerRunOnce)
 *              请直接 include xrpc_peer.h 调用,本头不再提供 wrapper。
 *              TXrpcPeerCacheSetOwner / TXrpcPeerCachePeerRunOnce 已下沉到 xrpc_peer_inner.h,
 *              用户不应直接调用;PeerRunOnce 由节点 TXrpcNodeRunOnce 统一调度。
 * @copyright   Copyright (c) 2026 AuyvJpz
 */

#ifndef _XRPC_NODE_H_
#define _XRPC_NODE_H_

#include "xrpc/xrpc_types.h"
#include "xrpc/xrpc_common.h"
#include "xrpc/xrpc_crc16.h"
#include "xrpc/xrpc_link.h"
#include "xrpc/xrpc_notify.h"
#include "xrpc/xrpc_status.h"
#include "xrpc/xrpc_delivery.h"
#include "xrpc/xrpc_peer.h"
#include "xrpc/xrpc_process.h"
#include "xrpc/xrpc_relay.h"

#ifdef __cplusplus
extern "C"
{
#endif


/* ============================================================
 *  process 节点级通用调用 API 配套的错误码
 *  注:沿用 xrpc_process_common.h / xrpc_common.h 既有错误码,
 *      同步调用发现未完成时返回 kXrpcErrInvalidParam
 *      (复用 "参数无效" 语义——表示调用方传入的 process 当前不可用)。
 * ============================================================ */


/* ============================================================
 * 锁 / 节点对象
 * ============================================================ */


/**
 * @brief 过程的链路绑定:通过哪条链路完成,总线上可再指定由哪个节点提供。
 * @note  表由调用方提供,只读,可以放 Flash。没有绑定的过程在所有链路上发现。
 */
typedef struct TXrpcProcessBindingTag TXrpcProcessBinding;
struct TXrpcProcessBindingTag
{
    UInt32 process_id;      ///< 过程 ID
    UInt8  link_index;      ///< 链路序号(按 AddLink 顺序)
    UInt32 node_id;         ///< 总线上的提供者节点;0 表示广播询问,先应答者为准
};

/** @brief xrpc 节点对象 */
typedef struct TXrpcNodeTag TXrpcNode;

/**
 * @brief 异步调用完成回调。
 * @param node       节点对象
 * @param process_id 过程 ID
 * @param result     >= 0 业务结果;< 0 错误码(kXrpcErrTimeout、kXrpcErrPeerStatusInvalid 等)
 * @param output     发起调用时传入的出参列表,结果 < 0 时为空列表
 * @param user_data  发起调用时传入的上下文
 * @note  在 RunOnce 线程、节点锁之外执行。可以再发起异步调用,不能做阻塞操作。
 */
typedef void (*TXrpcCallDoneCb)(TXrpcNode *node, UInt32 process_id, Int32 result,
                                TXrpcMsgParamList *output, void *user_data);

/**
 * @brief 带选项的调用使用的选项。
 * @note  组件不判断函数是否幂等。重试时对端可能已经执行过，超时也不代表对端没有执行，
 *        不是幂等的函数把 retry 配成 0。
 */
typedef struct TXrpcCallOptTag TXrpcCallOpt;
struct TXrpcCallOptTag
{
    UInt32 timeout_ms;      ///< 每次等待应答的时长,0 表示 XRPC_DEFAULT_PROCESS_TIMEOUT_MS
    UInt8  retry;           ///< 超时后重发的次数,0 表示不重发
};

/** @brief 调用槽状态 */
typedef enum TXrpcCallSlotStepTag
{
    kXrpcCallSlotFree = 0,      ///< 空闲
    kXrpcCallSlotWait = 1,      ///< 已发出,等待应答
    kXrpcCallSlotDone = 2,      ///< 已有结果,等待取走
} TXrpcCallSlotStep;

/**
 * @brief 一次在途调用。数组由调用方提供(TXrpcNodeSetCallSlots),内容由库管理。
 */
typedef struct TXrpcCallSlotTag TXrpcCallSlot;
struct TXrpcCallSlotTag
{
    TXrpcCallSlotStep step;
    UInt32 process_id;
    TXrpcDelivery delivery;     ///< 序号、超时与重发次数
    TXrpcProcessObject *proc;
    TXrpcPeerNode *peer_node;
    UInt32 provider_node_id;
    const TXrpcMsgParamList *input;     ///< 重发时重新编码
    TXrpcMsgParamList *output;
    Int32 errcode;
    TXrpcCallDoneCb done_cb;    ///< NULL 表示同步调用占用的槽
    void *user_data;
};
struct TXrpcNodeTag
{
    UInt32 node_id;                       ///< 节点 ID(用于消息头中的 source/dest)
    UInt16 version;                       ///< 协议版本(用于校验远端消息头 version)
    UInt32 user_tag;                      ///< 应用标识,写入帧头,不一致的帧被丢弃
    UInt32 tx_seq;                        ///< 帧头 seq 计数,节点内全部链路共用
    TXrpcPeerCache *peer_cache;           ///< 节点绑定的 PeerCache(由 TXrpcNodeSetPeerCache 装配)
    TXrpcProcessTable process_table;   ///< process 函数定义表
    const TXrpcLockOps *lock_ops;         ///< 锁操作 vtable;NULL 表示不启用锁(no-op)
    IXrpcCrc16 *crc16;                    ///< CRC16 计算接口(由 TXrpcNodeSetCrc16 注入)
    const TXrpcProcessBinding *bindings;  ///< 过程绑定表;NULL 表示没有绑定
    UInt32 binding_count;                 ///< 过程绑定表项数
    TXrpcCallSlot *call_slots;            ///< 调用槽数组;NULL 表示不启用
    UInt32 call_slot_count;               ///< 调用槽个数
    TXrpcNotifyPubObject **notify_pubs;   ///< 发布表;NULL 表示本节点不发布
    UInt32 notify_pub_count;
    TXrpcNotifySubObject **notify_subs;   ///< 订阅表;NULL 表示本节点不订阅
    UInt32 notify_sub_count;
    TXrpcStatusCb status_cb;              ///< 状态回调;NULL 表示不回调
    UInt32 status_interval_ms;            ///< 同一状态两次回调的最小间隔
    void *status_user;
    TXrpcStatusEntry status[kXrpcStatusCount];  ///< 状态表
    Bool relay_enabled;                   ///< 是否开启服务中继(TXrpcNodeSetRelay)
    TXrpcRelayConfig relay;               ///< 中继配置
};


/* ============================================================
 * 节点生命周期与装配 API
 * ============================================================ */

/**
 * @brief 创建节点对象并初始化基础字段。
 * @param self    节点对象指针
 * @param node_id 当前节点 ID
 * @param version xrpc 协议版本号,主版本必须与 XRPC_PROTOCOL_VERSION 相同
 * @return  成功返回 kXrpcErrOk;self 为 NULL 返回 kXrpcErrInvalidParam
 * @note    创建后 peer_cache 与 process_table 均为 NULL,需后续 Set 装配
 */
Int32 TXrpcNodeCreate(TXrpcNode *self,
                      UInt32 node_id, UInt16 version);

/**
 * @brief 反初始化节点对象:清空字段,不解绑外部资源。
 * @param self 节点对象指针;为 NULL 时安全 no-op
 * @note    不释放 peer_cache 与 process_table(所有权归调用方)
 */
void  TXrpcNodeDestroy(TXrpcNode *self);

/**
 * @brief 为节点绑定一个已存在的 PeerCache(非首次装配场景, 用于多节点复用同一 cache)。
 * @param self       节点对象指针
 * @param peer_cache PeerCache 对象指针(必须已通过 TXrpcPeerCacheCreate 创建过)
 * @return  成功返回 kXrpcErrOk;任一指针为 NULL 返回 kXrpcErrInvalidParam
 * @note    本函数自动把 owner 指向 self(内部封装 TXrpcPeerCacheSetOwner,
 *          该函数已下沉到 xrpc_peer_inner.h,用户不应直接调用);
 *          如需在装配时同时创建 PeerCache,请先
 *          TXrpcPeerCacheCreate(&cache, peers, max_count) 再
 *          TXrpcNodeSetPeerCache(self, &cache)
 */
Int32 TXrpcNodeSetPeerCache(TXrpcNode *self, TXrpcPeerCache *peer_cache);

/**
 * @brief 为节点绑定 process 路由表。
 * @param self          节点对象指针
 * @param process_table process 路由表指针
 * @return  成功返回 kXrpcErrOk;任一指针为 NULL 返回 kXrpcErrInvalidParam
 */
Int32 TXrpcNodeSetProcessTable(TXrpcNode *self, TXrpcProcessTable *process_table);

/**
 * @brief 为节点配置锁操作 vtable(保护 peer_array 的并发访问)。
 * @param self     节点对象指针
 * @param lock_ops 锁操作 vtable 指针;NULL 表示禁用锁(等价于 no-op);
 *                 若非 NULL,必须同时实现 lock 和 unlock 两个回调,
 *                 否则本函数返回 kXrpcErrInvalidParam 且不修改节点状态
 * @return  成功返回 kXrpcErrOk;self 为 NULL 或 lock_ops 字段不完整
 *          返回 kXrpcErrInvalidParam
 * @note    调用线程与 RunOnce 线程不同时必须配置:过程与 peer 的运行状态
 *          只靠这把锁保护。单线程使用时可以不配置。
 *          设置成功后,绑定到本节点的 peer_cache 通过 owner 反向访问
 *          本节点的 lock_ops,无需额外传播。
 *          典型用法:
 *          ```
 *          static TXrpcLockOps g_my_lock_ops = {
 *              .lock = my_mutex_lock,
 *              .unlock = my_mutex_unlock,
 *              .user_data = &g_my_mutex,
 *          };
 *          TXrpcNodeSetLockMethod(&node, &g_my_lock_ops);
 *          ```
 */
Int32 TXrpcNodeSetLockMethod(TXrpcNode *self, const TXrpcLockOps *lock_ops);

/**
 * @brief 为节点注入 CRC16 计算接口。
 * @param self  节点对象指针
 * @param crc16 CRC16 接口;OnCalc_ 不可为 NULL
 * @return  成功返回 kXrpcErrOk;任一指针为 NULL 返回 kXrpcErrInvalidParam
 * @note    必须在节点开始收发之前调用。未注入时发送返回 kXrpcErrInvalidParam,
 *          接收的帧全部按校验失败丢弃。
 *          算法必须与对端完全一致,否则两端互相丢帧。
 */
Int32 TXrpcNodeSetCrc16(TXrpcNode *self, IXrpcCrc16 *crc16);

/**
 * @brief 设置过程的链路绑定表。
 * @param self     节点对象指针
 * @param bindings 绑定表;生命周期由调用方保证
 * @param count    表项数
 * @return  成功返回 kXrpcErrOk;self 或 bindings 为 NULL 返回 kXrpcErrInvalidParam
 */
Int32 TXrpcNodeSetProcessBindings(TXrpcNode *self, const TXrpcProcessBinding *bindings,
                                  UInt32 count);

/**
 * @brief 启用调用槽。启用后同一过程可以有多个调用同时在途,同步调用也走调用槽。
 * @param self  节点对象指针
 * @param slots 调用槽数组,由调用方提供,生命周期不短于节点
 * @param count 调用槽个数,决定最多同时在途的调用数
 * @return  成功返回 kXrpcErrOk;参数非法返回 kXrpcErrInvalidParam
 * @note    不启用时同步调用保持原有行为:同一过程同一时刻只能一个在途调用。
 *          必须在开始 RunOnce 之前调用。
 */
Int32 TXrpcNodeSetCallSlots(TXrpcNode *self, TXrpcCallSlot *slots, UInt32 count);

/**
 * @brief 发起异步远端调用,发出后立即返回。
 * @param self       节点对象指针
 * @param process_id 目标 process_id
 * @param input      入参列表(允许为 NULL)。返回后可以立刻释放
 * @param output     出参列表(允许为 NULL)。必须保持有效直到完成回调返回
 * @param timeout_ms 超时时长(ms);0 表示使用 XRPC_DEFAULT_PROCESS_TIMEOUT_MS
 * @param done_cb    完成回调,不可为 NULL
 * @param user_data  透传给回调的上下文
 * @return  kXrpcErrOk 已发出,结果一定通过回调通知;< 0 未发出,不会回调
 *          - kXrpcErrNotSupported   未启用调用槽,或目标是本地过程
 *          - kXrpcErrNoFreeSlot     调用槽已用完
 *          - kXrpcErrProviderUnknow / kXrpcErrPeerNodeUnknow / kXrpcErrPeerStatusInvalid
 *            前置条件不满足,含义同 TXrpcNodeProcessExec
 * @note    发送本身可能因链路失败重试而等待 10ms,不会等待应答。
 */
Int32 TXrpcNodeCallAsync(TXrpcNode *self, UInt32 process_id,
                         const TXrpcMsgParamList *input,
                         TXrpcMsgParamList *output,
                         UInt32 timeout_ms,
                         TXrpcCallDoneCb done_cb, void *user_data);

/**
 * @brief 带选项发起异步远端调用。
 * @param opt 调用选项,不可为 NULL
 * @note    其余参数和返回值同 TXrpcNodeCallAsync。最长总耗时为
 *          timeout_ms * (retry + 1)。retry 大于 0 时 input 要保持有效,直到完成回调。
 *          超时短于往返时延时,每次应答都算迟到,重试用尽仍然失败。
 */
Int32 TXrpcNodeCallAsyncOpt(TXrpcNode *self, UInt32 process_id,
                            const TXrpcMsgParamList *input,
                            TXrpcMsgParamList *output,
                            const TXrpcCallOpt *opt,
                            TXrpcCallDoneCb done_cb, void *user_data);

/**
 * @brief 节点单步轮询:复用同一接收缓冲区,顺序驱动各 peer 的 RunOnce。
 * @param self                节点对象指针
 * @param link_recv_buff_ptr  链路层单次读取使用的临时缓冲区
 * @param link_recv_buff_size 临时缓冲区容量(字节,>0)
 * @return  成功返回 kXrpcErrOk;参数非法或 peer_cache 未绑定返回 kXrpcErrInvalidParam
 * @note    非线程安全;单线程主循环中应固定周期调用。
 *          内部封装了 xrpc_peer_inner.h 中的 TXrpcPeerCachePeerRunOnce,
 *          用户应通过本函数统一调度,不要直接 include 内层头。
 *          本函数末尾会顺带做一次"函数发现轮询":
 *          对每个 provider_node_id==0 且未在 knock 等待中的 process,
 *          周期性发送 KnockReq;完成发现后 provider_node_id 即被填充,
 *          后续 TXrpcNodeCall 才能发出完整 CallReq。
 */
Int32 TXrpcNodeRunOnce(TXrpcNode *self, UInt8 *link_recv_buff_ptr, UInt32 link_recv_buff_size);


/* ============================================================
 * 节点侧 helper(由生成代码 / peer 层回调使用,不建议业务方直接调)
 * ============================================================ */

/**
 * @brief 取本节点 node_id(用于组装 msg 头 source 字段)。
 * @param self 节点对象指针;NULL 时返回 0
 */
UInt32 TXrpcNodeGetNodeId(TXrpcNode *self);

/**
 * @brief 取本节点使用的 xrpc 协议版本(用于校验远端 msg 头 version)。
 * @param self 节点对象指针;NULL 时返回 0
 */
UInt16 TXrpcNodeGetVersion(TXrpcNode *self);

/**
 * @brief 设置应用标识。
 * @param self     节点对象指针
 * @param user_tag 应用标识,默认 0
 * @return  kXrpcErrOk;self 为 NULL 时返回 kXrpcErrInvalidParam
 * @note    发送的每一帧都带上这个值,收到不一致的帧直接丢弃并计数。
 *          同一设备上的多套 xrpc 系统用不同的值互相隔离。
 *          启动 RunOnce 之前设置。
 */
Int32 TXrpcNodeSetUserTag(TXrpcNode *self, UInt32 user_tag);

/**
 * @brief 取应用标识。
 * @param self 节点对象指针;NULL 时返回 0
 */
UInt32 TXrpcNodeGetUserTag(TXrpcNode *self);

/**
 * @brief 取因 user_tag 不一致而丢弃的帧数,用于排查配置错误。
 * @param self 节点对象指针;NULL 时返回 0
 */
UInt32 TXrpcNodeGetUserTagDrops(TXrpcNode *self);

/**
 * @brief 按远端节点 ID 查找已注册 peer 的链路对象。
 * @param self         节点对象指针
 * @param peer_node_id 远端节点 ID
 * @return  链路对象指针;未找到返回 NULL
 * @note    供生成代码内 RequestNode / SendNodeAsync 等使用,
 *          业务层不应直接调用
 */
IXrpcLink *TXrpcNodeGetPeerLinkByNodeId(TXrpcNode *self, UInt32 peer_node_id);

/**
 * @brief 按远端节点 ID 查找已注册 peer 的 TXrpcPeerNode 句柄(2026/06/21 新增)。
 * @param self         节点对象指针
 * @param peer_node_id 远端节点 ID
 * @return  TXrpcPeerNode 指针;未找到返回 NULL
 * @note    与 TXrpcNodeGetPeerLinkByNodeId 对称,用于把 link 路径调用
 *          转为 peer_node 路径调用(配合 xrpc_node_inner 的 SendXxx API)。
 *          业务层不应直接调用。
 */
TXrpcPeerNode *TXrpcNodeGetPeerNodeByNodeId(TXrpcNode *self, UInt32 peer_node_id);

/**
 * @brief 取节点绑定的 PeerCache(供生成代码遍历全部 peer 广播 msg)。
 * @param self 节点对象指针;NULL 时返回 NULL
 */
TXrpcPeerCache *TXrpcNodeGetPeerCache(TXrpcNode *self);

/**
 * @brief 节点级 process 同步调用入口:按 process_id 路由到本地 exec_cb 或
 *        通过 peer 发 CallReq 等待 CallRsp,timeout_ms 超时返回。
 *
 * @param self       节点对象
 * @param process_id 目标 process_id(在 process_table 中)
 * @param input      入参列表(允许为 NULL,表示无入参)
 * @param output     出参列表(由业务方提供编码缓冲)
 * @param timeout_ms 超时时长(ms);0 表示使用 XRPC_DEFAULT_PROCESS_TIMEOUT_MS
 * @return  >= 0 业务执行结果;< 0 错误码(详见 xrpc_common.h)
 *          - kXrpcErrPeerNodeUnknow   provider_node_id 未在 peer_cache 找到
 *          - kXrpcErrProcessStatus    provider_node_id==0 或 process 不可用
 *          - kXrpcErrTimeout          超时未收到对端应答
 *          - kXrpcErrLinkSendFailed   链路发送失败(已重试一次仍失败)
 *          - kXrpcErrTxBufferOverflow 发送缓冲区不足
 *
 * @warning 【线程模型约束】本函数内部用 XrpcSleepMs 自旋等待 KnockRsp/CallRsp,
 *          若与 TXrpcNodeRunOnce 在 **同一线程上下文** 调用会形成死锁
 *          (RunOnce 被阻塞,永远收不到对端回包)。
 *          正确用法:
 *          - 双线程 / RTOS 场景:业务线程调 ProcessExec,RX 主线程周期调 RunOnce;
 *          - 单线程场景:改用 TXrpcNodeCallAsync。
 *          违反约束导致的死锁 / 行为异常,由调用方负责。
 *
 * @note    函数内部会等待 hello_pending 清零(对端 HelloRsp)后再走 knock/call,
 *          单 peer 部署下等待时长 < 1s(由 PeerRunOnce 的 HelloReq 节流决定)。
 */
Int32 TXrpcNodeProcessExec(TXrpcNode *self, UInt32 process_id,
                           const TXrpcMsgParamList *input,
                           TXrpcMsgParamList *output,
                           UInt32 timeout_ms);

/**
 * @brief 带选项发起同步调用。
 * @param opt 调用选项,不可为 NULL
 * @return  同 TXrpcNodeProcessExec。retry 大于 0 但没有配置调用槽时返回
 *          kXrpcErrNotSupported,过程在本节点时 retry 不起作用。
 * @note    最长总耗时为 timeout_ms * (retry + 1)。
 */
Int32 TXrpcNodeProcessExecOpt(TXrpcNode *self, UInt32 process_id,
                              const TXrpcMsgParamList *input,
                              TXrpcMsgParamList *output,
                              const TXrpcCallOpt *opt);

/**
 * @brief 按 process_id 在 process_table 中查找 process 对象。
 * @param self       节点对象指针
 * @param process_id 目标 process_id
 * @return  process 对象指针;未找到返回 NULL
 */
TXrpcProcessObject *TXrpcNodeFindProcess(TXrpcNode *self, UInt32 process_id);


#ifdef __cplusplus
}  ///< extern "C"
#endif

#endif  ///< _XRPC_NODE_H_
