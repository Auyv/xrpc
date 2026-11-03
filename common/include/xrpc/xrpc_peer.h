/**
 * @addtogroup xrpc_peer
 * xrpc_peer
 * @{
 */
/**
 * @file        xrpc_peer.h
 * @brief       对端(peer)连接缓存表。管理多个 peer 节点对应的 IXrpcLink 和
 *              流式帧拼接缓存(TFdnData),由 TXrpcNodeRunOnce 统一驱动。
 * @note        并发保护采用锁回调注入:调用方通过 TXrpcNodeSetLockMethod 注入
 *              pthread mutex / critical section / 关中断 等实现;
 *              lock_ops=NULL 时全部 no-op,沿用「调用方单线程」默认约定。
 * @copyright   Copyright (c) 2026 AuyvJpz
 */

#ifndef _XRPC_PEER_H_
#define _XRPC_PEER_H_

#include "xrpc/xrpc_types.h"
#include "xrpc/fdn.h"
#include "xrpc/xrpc_common.h"
#include "xrpc/xrpc_link.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** @brief 链路拓扑 */
enum TXrpcLinkTopologyTag
{
    kXrpcLinkTopologyP2P = 0,   ///< 点对点:一条链路一个对端,帧不带目的地址(默认)
    kXrpcLinkTopologyBus = 1,   ///< 多点总线:单播消息使用 Addressed 信封
};
typedef enum TXrpcLinkTopologyTag TXrpcLinkTopology;

/** @brief 总线上的一个远端节点 */
typedef struct TXrpcRemoteNodeTag TXrpcRemoteNode;
struct TXrpcRemoteNodeTag
{
    UInt32 node_id;             ///< 远端节点 ID
    UInt32 max_recv_frame;      ///< 对端声明的最大接收帧长;0 表示未知
};

/**
 * @brief 链路属性。不配置时等同于 P2P、无帧长上限、不通告能力,与旧行为完全一致。
 */
typedef struct TXrpcLinkConfigTag TXrpcLinkConfig;
struct TXrpcLinkConfigTag
{
    TXrpcLinkTopology topology;     ///< 拓扑
    UInt32 max_frame;               ///< 本链路单帧上限;0 表示不限制
    TXrpcRemoteNode *remotes;       ///< 总线远端节点表,由调用方提供;P2P 为 NULL
    UInt32 remote_capacity;         ///< 远端节点表容量
    Bool advertise_max_recv;        ///< Hello 是否携带本端最大接收帧长
};

/** @brief 单个 peer 节点:持有远端链路实例与拼接缓存 */
typedef struct TXrpcPeerNodeTag TXrpcPeerNode;
struct TXrpcPeerNodeTag
{
    IXrpcLink *link;               ///< 远端链路实例(4 个 vtable 回调必须齐全)
    UInt32 peer_node_id;           ///< 远端节点 ID(0 表示尚未完成 Hello 握手)
    UInt32 static_peer_node_id;    ///< 注册时配置的远端 ID;链路断开后恢复到这个值
    Bool link_up;                  ///< 链路上一轮是否已连接,用于检测断开(仅轮询线程访问)
    Bool hello_pending;            ///< HelloReq 已发出但尚未收到对端 HelloRsp 时为 True
    UInt32 hello_tick;             ///< 上次 HelloReq 发送时刻(单位 ms,用于 1s 节流)
    UInt32 knock_tick;             ///< 本 peer 上次发 KnockReq 的 tick(用于 1s 节流)
    TFdnData fdn_recv;             ///< 流式帧拼接缓存;fdn_buff_size=0 时不支持拼接,
                                   ///< 直接把单次 recv 数据当完整帧处理
    UInt8 *send_buff_ptr;          ///< 发送数据编码缓存指针; 不能为NULL
    UInt32 send_buff_size;         ///< 发送数据编码缓存容量
    const TXrpcLockOps *send_lock_ops; ///< send_buff_ptr 专用锁;NULL 表示 no-op
    TXrpcLinkTopology topology;    ///< 链路拓扑
    UInt32 max_frame;              ///< 本链路单帧上限;0 表示不限制
    Bool advertise_max_recv;       ///< Hello 是否携带本端最大接收帧长
    UInt32 peer_max_recv_frame;    ///< P2P 对端声明的最大接收帧长;0 表示未知
    TXrpcRemoteNode *remotes;      ///< 总线远端节点表
    UInt32 remote_capacity;        ///< 远端节点表容量
    UInt32 remote_count;           ///< 已学习到的远端节点数
    /* Lock configuration rule:
     * send_lock_ops must use a lock object different from TXrpcNode.lock_ops
     * and from the logging backend internal lock. Reusing the same mutex can
     * self-deadlock when send paths produce logs while holding the send lock.
     */
};

/** @brief peer 缓存表对象:批量管理多个 peer */
typedef struct TXrpcPeerCacheTag TXrpcPeerCache;
struct TXrpcPeerCacheTag
{
    TXrpcPeerNode *peer_array;     ///< peer 数组(由调用方提供内存)
    UInt32 max_peer_count;         ///< peer 数组容量
    UInt32 peer_count;             ///< 当前已注册 peer 数
    void *owner;                   ///< 拥有者句柄(通常为 TXrpcNode*),方便反向访问;
                                   ///< 通过 owner->lock_ops 间接获取锁配置
};


/**
 * @brief 创建 PeerCache 并绑定调用方提供的 peer 数组。
 * @param self           PeerCache 对象指针
 * @param peer_cache     调用方提供的 peer 数组(外部拥有,生命周期由调用方管理)
 * @param max_peer_count peer 数组容量(>0)
 * @return  成功返回 kXrpcErrOk;参数非法返回 kXrpcErrInvalidParam
 * @note    peer_cache 指向的内存由调用方拥有,PeerCacheDestroy 不会释放它
 */
Int32 TXrpcPeerCacheCreate(TXrpcPeerCache *self, TXrpcPeerNode *peer_cache, UInt32 max_peer_count);

/**
 * @brief 反初始化 PeerCache:对所有 peer 的 fdn_recv 复位并清空自身状态。
 * @param self PeerCache 对象指针;为 NULL 时安全 no-op
 * @note    不释放 peer 数组内存(由调用方拥有);不重置 owner 指向的对象;
 *          当 lock_ops 非 NULL 时,临界区由 lock/unlock 回调保护(阻塞等待)
 */
void  TXrpcPeerCacheDestroy(TXrpcPeerCache *self);

/**
 * @brief 向 PeerCache 注册一个 peer(链路 + 远端节点 ID + 可选拼接缓存)。
 * @param self           PeerCache 对象指针
 * @param link           链路对象指针(必须实现全部 4 个 vtable 回调)
 * @param peer_node_id   远端节点 ID
 * @param fdn_buff_ptr   流式帧拼接缓存指针;NULL 表示禁用拼接功能
 * @param fdn_buff_size  拼接缓存容量(字节);fdn_buff_ptr=NULL 时本参数被忽略
 * @param send_buff_ptr   发送数据编码缓存指针; 不能为NULL
 * @param send_buff_size  发送数据编码缓存容量
 * @return  成功返回 kXrpcErrOk;
 *          参数非法返回 kXrpcErrInvalidParam;容量已满返回 kXrpcErrListFull;
 *          fdn 初始化失败透传 kXrpcErrInvalidParam
 * @note    当 lock_ops 非 NULL 时,临界区由 lock/unlock 回调保护(阻塞等待)
 */
Int32 TXrpcPeerCacheAddLink(TXrpcPeerCache *self, IXrpcLink *link, UInt32 peer_node_id, 
                            UInt8 *fdn_buff_ptr, UInt32 fdn_buff_size, 
                            UInt8 *send_buff_ptr, UInt32 send_buff_size,
                            const TXrpcLockOps *send_lock_ops);

/**
 * @brief 设置已注册链路的属性。
 * @param self   PeerCache 对象指针
 * @param index  链路序号(按 AddLink 顺序,从 0 开始)
 * @param config 链路属性
 * @return  成功返回 kXrpcErrOk;参数非法返回 kXrpcErrInvalidParam
 * @note    必须在开始 RunOnce 之前调用。总线链路必须提供远端节点表。
 */
Int32 TXrpcPeerCacheSetLinkConfig(TXrpcPeerCache *self, UInt32 index,
                                  const TXrpcLinkConfig *config);

/**
 * @brief 获取 PeerCache 中已注册的 peer 数量。
 * @param self PeerCache 对象指针
 * @return  已注册 peer 数(>=0);self 为 NULL 时返回 0
 */
Int32 TXrpcPeerCachePeerCount(TXrpcPeerCache *self);

/**
 * @brief 按索引获取已注册 peer 对应的链路对象。
 * @param self  PeerCache 对象指针
 * @param index peer 索引(0 ~ peer_count-1)
 * @return  链路对象指针;self 为 NULL 或 index 越界时返回 NULL
 */
IXrpcLink *TXrpcPeerCachePeerLink(TXrpcPeerCache *self, UInt32 index);

TXrpcPeerNode *TXrpcPeerCachePeerNode(TXrpcPeerCache *self, UInt32 index);

/**
 * @brief 轮询所有 peer:检查链路状态,拉取一帧数据,经 fdn 拼接后通知上层。
 * @note  本函数已下沉到 xrpc_peer_inner.h,作为节点 TXrpcNodeRunOnce 的内部封装。
 *        用户禁止直接调用;通过节点层 TXrpcNodeRunOnce 统一调度。
 */

/* TXrpcPeerCachePeerRunOnce 声明已移到 xrpc_peer_inner.h,详见该头。 */

#ifdef __cplusplus
}  ///< extern "C"
#endif

#endif  ///< _XRPC_PEER_H_
