/**
 * @file        xrpc_peer.c
 * @brief       TXrpcPeerCache 缓存表实现：管理多个对端节点（peer）对应的
 *              IXrpcLink 实例及流式帧拼接缓存（TFdnData）。
 * @note        非线程安全；同一 peer_cache 的并发访问需外层串行化。
 * @copyright   Copyright (c) 2026 AuyvJpz
 */

#include "xrpc/xrpc_peer.h"
#include "xrpc/xrpc_node.h"
#include "xrpc/xrpc_common.h"           /* 错误码 kXrpcErrListFull 等 */
#include "xrpc_peer_inner.h"       /* 内部头: 取 SetOwner / PeerRunOnce 声明 */
#include "xrpc_node_inner.h"
#include "xrpc/xrpc_msg.h"
#include <string.h>



typedef struct TXrpcMsgFrameNotifyUserParamsTag TXrpcMsgFrameNotifyUserParams;
struct TXrpcMsgFrameNotifyUserParamsTag
{
    TXrpcNode *node;
    TXrpcPeerNode *peer_node;
};



/* 帧放不下。这时 CRC 还没校验,噪声也可能走到这里,所以 node_id 只作参考 */
static void L_XrpcPeerReportTooLarge(const TXrpcMsgFrameNotifyUserParams *params,
                                     const TXrpcMsgHeadMem *msg, UInt16 frame_len, UInt32 actual)
{
    if (params->node != NULL)
    {
        TXrpcNodeStatusRecord(params->node, kXrpcStatusRxFrameTooLarge, params->peer_node,
                              XrpcWireGetU32(msg->node_id), XrpcWireGetU16(msg->type),
                              (UInt32)XRPC_MSG_CRC_START_OFFSET + frame_len, actual);
    }
}

/**
 * @brief 流式帧解码回调占位（fdn -> TFdnDecodeMethod 签名）。
 * @param data          已拼接缓存中的数据
 * @param size          缓存中当前未消费字节数
 * @param user_data_frame 透传上下文（实际为 TXrpcMsgFrameNotifyUserParams*）
 * @return  > 0 完整帧字节数(头部+payload 整帧长度);
 *          = 0 数据不完整,等待下次补料;
 *          < 0 帧头 / CRC 校验失败,fdn 内部按 m_max_error_skip 自动丢字节
 * @note    完整解码逻辑:校验 tag、frame_len 下界、frame_crc 校验。
 *          本函数仅做合法性判定,业务分发由 L_XrpcPeerNotifyFrame 完成。
 */
static Int32 L_XrpcPeerDecodeFrame(const char *data, Int32 size,
                                    void *user_data_frame)
{
    Int32 result = 0;
    TXrpcMsgHeadMem *msg = (TXrpcMsgHeadMem *)data;
    TXrpcMsgFrameNotifyUserParams *params = (TXrpcMsgFrameNotifyUserParams *)user_data_frame;
    UInt16 frame_crc = 0U;
    UInt16 frame_len = 0U;

    if (size >= XRPC_MSG_HEAD_SIZE)
    {
        frame_len = XrpcWireGetU16(msg->frame_len);
    }

    /* 帧头最小长度不足:留作不完整 */
    if (size < XRPC_MSG_HEAD_SIZE)
    {
        result = 0;
    }
    /* tag 不匹配或 frame_len 下界不合法:丢帧 */
    else if ((memcmp(msg->tag, XRPC_MSG_HEAD_TAG, 4) != 0)
             || (frame_len < XRPC_MSG_FRAME_LEN_MIN))
    {
        result = -1;
    }
    /* 比拼帧缓存还大的帧不可能收齐,不等缓存填满就当错误,以免卡住后面的有效帧 */
    else if ((params->peer_node->fdn_recv.m_buff_start != NULL)
             && ((XRPC_MSG_CRC_START_OFFSET + (Int32)frame_len)
                 > params->peer_node->fdn_recv.m_buff_size))
    {
        L_XrpcPeerReportTooLarge(params, msg, frame_len,
                                 (UInt32)params->peer_node->fdn_recv.m_buff_size);
        result = -1;
    }
    /* payload 还没到齐:留作不完整 */
    else if (size < (XRPC_MSG_CRC_START_OFFSET + frame_len))
    {
        /* 单帧模式没有后续补料,这一帧到此为止 */
        if (params->peer_node->fdn_recv.m_buff_start == NULL)
        {
            L_XrpcPeerReportTooLarge(params, msg, frame_len, (UInt32)size);
        }
        result = 0;
    }
    /* CRC 校验:失败丢帧,成功返回整帧长度;未注入 CRC 接口按校验失败处理 */
    else if ((params->node != NULL)
             && (IXrpcCrc16Calc(params->node->crc16,
                                (const UInt8 *)&data[XRPC_MSG_CRC_START_OFFSET],
                                frame_len, &frame_crc) == kXrpcErrOk)
             && (XrpcWireGetU16(msg->frame_crc) == frame_crc))
    {
        result = XRPC_MSG_CRC_START_OFFSET + frame_len;
    }
    else
    {
        result = -1;
    }

    return result;
}


/**
 * @brief 完整帧通知回调(fdn -> TFdnNotifyCallback 签名)。
 * @param data          完整帧数据起始位置(fdn 内部缓存,回调返回后即可能被覆盖)
 * @param size          完整帧字节数
 * @param user_data_frame 透传上下文(实际为 TXrpcMsgFrameNotifyUserParams*)
 * @note    调用 TXrpcNodeHandleRxFrame 进行帧头解析与业务分发
 *          (CallRsp 匹配 pending context / KnockRsp 更新 provider / HelloRsp 更新 peer_node_id)。
 *          owner 在 TXrpcNodeSetPeerCache 中自动绑定,可能为 NULL
 *          (用户未走节点装配路径),此时直接丢弃帧,避免悬空指针。
 */
static void L_XrpcPeerNotifyFrame(const char *data, Int32 size,
                                   void *user_data_frame)
{
    TXrpcMsgFrameNotifyUserParams *params = (TXrpcMsgFrameNotifyUserParams *)user_data_frame;
    TXrpcMsgHeadMem *msg = (TXrpcMsgHeadMem *)data;
    TXrpcNode *node = params->node;
    TXrpcPeerNode *peer_node = params->peer_node;

    (void)size;

    TXrpcNodeHandleRxFrame(node, peer_node, msg);
}


/**
 * @brief 创建 PeerCache 缓存表并初始化底层 peer 数组。
 * @param self          PeerCache 对象指针
 * @param peer_cache    调用方提供的 peer 数组（外部拥有,生命周期由调用方管理）
 * @param max_peer_count peer 数组容量(>0)
 * @return  成功返回 kXrpcErrOk;参数非法返回 kXrpcErrInvalidParam
 * @note    peer_cache 指向的内存由调用方拥有,PeerCacheDestroy 不会释放它
 */
Int32 TXrpcPeerCacheCreate(TXrpcPeerCache *self, TXrpcPeerNode *peer_cache,
                            UInt32 max_peer_count)
{
    Int32 result = kXrpcErrOk;

    if ((self == NULL) || (peer_cache == NULL) || (max_peer_count == 0))
    {
        result = kXrpcErrInvalidParam;
    }
    else
    {
        /* 清零 peer 数组：link/peer_node_id/fdn_recv 均归零 */
        memset(peer_cache, 0, sizeof(TXrpcPeerNode) * max_peer_count);

        self->peer_array = peer_cache;
        self->max_peer_count = max_peer_count;
        self->peer_count = 0;
        self->owner = NULL;
    }

    return result;
}


/**
 * @brief 反初始化 PeerCache:对所有 peer 的 fdn_recv 复位并清空自身状态。
 * @param self PeerCache 对象指针
 * @note    不释放 peer 数组内存(由调用方拥有);不重置 owner 指向的对象
 */
void TXrpcPeerCacheDestroy(TXrpcPeerCache *self)
{
    UInt32 i = 0;
    TXrpcNode *node = NULL;
    const TXrpcLockOps *lock_ops = NULL;
    TXrpcLockOpsMethod unlock_cb = NULL;
    void *unlock_user_data = NULL;

    if (self != NULL)
    {
        /* 通过 owner(TXrpcNode*) 间接获取 lock_ops;
         * owner 非 NULL 且 lock_ops 非 NULL 时,临界区由回调保护 */
        node = (TXrpcNode *)self->owner;
        if (node != NULL)
        {
            lock_ops = node->lock_ops;
        }
        if (lock_ops != NULL)
        {
            (void)lock_ops->lock(lock_ops->user_data);
            unlock_cb = lock_ops->unlock;
            unlock_user_data = lock_ops->user_data;
        }

        if (self->peer_array != NULL)
        {
            /* 遍历所有已注册 peer, 反初始化 fdn_recv */
            for (i = 0; i < self->peer_count; i++)
            {
                TFdnDestroy(&self->peer_array[i].fdn_recv);
            }
        }

        self->peer_array = NULL;
        self->max_peer_count = 0;
        self->peer_count = 0;
        self->owner = NULL;

        if (unlock_cb != NULL)
        {
            (void)unlock_cb(unlock_user_data);
        }
    }
}


/**
 * @brief 设置 PeerCache 拥有者句柄(通常是 TXrpcNode*)。
 * @param self  PeerCache 对象指针
 * @param owner 拥有者句柄(可为 NULL 表示清空)
 * @return  成功返回 kXrpcErrOk;self 为 NULL 时返回 kXrpcErrInvalidParam
 * @note    owner 仅为 void* 指针,PeerCache 不解释其类型;
 *          通常由 TXrpcNodeSetPeerCache 自动调用
 */
Int32 TXrpcPeerCacheSetOwner(TXrpcPeerCache *self, void *owner)
{
    Int32 result = kXrpcErrOk;

    if (self == NULL)
    {
        result = kXrpcErrInvalidParam;
    }
    else
    {
        self->owner = owner;
    }

    return result;
}


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
                            const TXrpcLockOps *send_lock_ops)
{
    Int32 result = kXrpcErrOk;
    TXrpcPeerNode *peer = NULL;
    TXrpcNode *node = NULL;
    const TXrpcLockOps *lock_ops = NULL;
    TXrpcLockOpsMethod unlock_cb = NULL;
    void *unlock_user_data = NULL;
    Bool lock_ok = True;

    if (self == NULL || send_buff_ptr == NULL || send_buff_size == 0)
    {
        result = kXrpcErrInvalidParam;
    }
    else
    {
        /* 通过 owner(TXrpcNode*) 间接获取 lock_ops */
        node = (TXrpcNode *)self->owner;
        if (node != NULL)
        {
            lock_ops = node->lock_ops;
        }
        if (lock_ops != NULL)
        {
            lock_ok = lock_ops->lock(lock_ops->user_data);
            unlock_cb = lock_ops->unlock;
            unlock_user_data = lock_ops->user_data;
        }

        if (lock_ok == False)
        {
            result = kXrpcErrInvalidParam;
        }
        else if ((self->peer_array == NULL) || (link == NULL)
            || (link->OnConnectSwitch_ == NULL) || (link->OnSend_ == NULL)
            || (link->OnRecv_ == NULL) || (link->OnState_ == NULL))
        {
            result = kXrpcErrInvalidParam;
        }
        else if (self->peer_count >= self->max_peer_count)
        {
            result = kXrpcErrListFull;
        }
        else if (self->peer_array == NULL)
        {
            /* 【C6】防御:Destroy 后 peer_array 被置 NULL,再 AddLink
             * 会拿到 self->peer_array[0] 的野指针;此处拦截。 */
            result = kXrpcErrInvalidParam;
        }
        else
        {
            peer = &self->peer_array[self->peer_count];

            /* 可选:启用流式帧拼接缓存 */
            if ((fdn_buff_ptr != NULL) && (fdn_buff_size > 0))
            {
                result = TFdnCreate(&peer->fdn_recv,
                                    (char *)fdn_buff_ptr,
                                    (Int32)fdn_buff_size,
                                    L_XrpcPeerDecodeFrame,
                                    L_XrpcPeerNotifyFrame);
            }

            if (result == kXrpcErrOk)
            {
                peer->send_buff_ptr = send_buff_ptr;
                peer->send_buff_size = send_buff_size;
                peer->send_lock_ops = send_lock_ops;
                peer->link = link;
                peer->peer_node_id = peer_node_id;
                peer->static_peer_node_id = peer_node_id;
                self->peer_count++;
            }
        }

        if ((lock_ok == True) && (unlock_cb != NULL))
        {
            (void)unlock_cb(unlock_user_data);
        }
    }

    return result;
}


/**
 * @brief 获取 PeerCache 中已注册的 peer 数量。
 * @param self PeerCache 对象指针
 * @return  已注册 peer 数(>=0);self 为 NULL 时返回 0
 */
Int32 TXrpcPeerCachePeerCount(TXrpcPeerCache *self)
{
    Int32 result = 0;

    if (self != NULL)
    {
        result = (Int32)self->peer_count;
    }

    return result;
}


/**
 * @brief 按索引获取已注册 peer 对应的链路对象。
 * @param self  PeerCache 对象指针
 * @param index peer 索引(0 ~ peer_count-1)
 * @return  链路对象指针;self 为 NULL 或 index 越界时返回 NULL
 */
Int32 TXrpcPeerCacheSetLinkConfig(TXrpcPeerCache *self, UInt32 index,
                                  const TXrpcLinkConfig *config)
{
    Int32 result = kXrpcErrInvalidParam;

    if ((self != NULL) && (self->peer_array != NULL) && (config != NULL)
        && (index < self->peer_count)
        && ((config->topology == kXrpcLinkTopologyP2P)
            || ((config->remotes != NULL) && (config->remote_capacity > 0U))))
    {
        TXrpcPeerNode *peer = &self->peer_array[index];

        peer->topology = config->topology;
        peer->max_frame = config->max_frame;
        peer->advertise_max_recv = config->advertise_max_recv;
        peer->remotes = config->remotes;
        peer->remote_capacity = config->remote_capacity;
        peer->remote_count = 0U;
        result = kXrpcErrOk;
    }

    return result;
}

Bool TXrpcPeerNodeOwnsNode(const TXrpcPeerNode *peer, UInt32 node_id)
{
    Bool result = False;
    UInt32 i = 0U;

    if ((peer != NULL) && (node_id != 0U))
    {
        if (peer->topology == kXrpcLinkTopologyP2P)
        {
            result = (peer->peer_node_id == node_id) ? True : False;
        }
        else
        {
            for (i = 0U; (i < peer->remote_count) && (result == False); i++)
            {
                result = (peer->remotes[i].node_id == node_id) ? True : False;
            }
        }
    }

    return result;
}

IXrpcLink *TXrpcPeerCachePeerLink(TXrpcPeerCache *self, UInt32 index)
{
    IXrpcLink *result = NULL;

    if ((self != NULL) && (self->peer_array != NULL) && (index < self->peer_count))
    {
        result = self->peer_array[index].link;
    }

    return result;
}

TXrpcPeerNode *TXrpcPeerCachePeerNode(TXrpcPeerCache *self, UInt32 index)
{
    TXrpcPeerNode *result = NULL;

    if ((self != NULL) && (self->peer_array != NULL) && (index < self->peer_count))
    {
        result = &self->peer_array[index];
    }

    return result;
}


/**
 * @brief 轮询所有 peer:检查链路状态,拉取一帧数据,经 fdn 拼接后通知上层。
 * @param self                PeerCache 对象指针
 * @param link_recv_buff_ptr  链路层单次读取使用的临时缓冲区(由调用方提供)
 * @param link_recv_buff_size 临时缓冲区容量(字节,>0)
 * @return  成功返回 kXrpcErrOk;参数非法返回 kXrpcErrInvalidParam
 * @note    当 lock_ops 非 NULL 时,临界区由 lock/unlock 回调保护(阻塞等待);
 *          每个 peer 共用同一临时缓冲区,顺序处理
 */
Int32 TXrpcPeerCachePeerRunOnce(TXrpcPeerCache *self,
                                 UInt8 *link_recv_buff_ptr,
                                 UInt32 link_recv_buff_size)
{
    Int32 result = kXrpcErrOk;
    UInt32 i = 0;
    TXrpcPeerNode *peer = NULL;
    IXrpcLink *link = NULL;
    Int32 recv_size = 0;
    TXrpcLinkState state = kXrpcLinkStateInvalid;
    TXrpcNode *node = NULL;
    UInt32 now_tick = 0U;
    /* HelloReq 节流周期(单位 ms);调整这里即可变更握手节拍 */
    const UInt32 kHelloReqPeriodMs = 1000U;
    /* HelloReq 发出后等待应答的时长,超时后重发 */
    const UInt32 kHelloRspTimeoutMs = 3000U;
    /* 总线上 Hello 是广播宣告,周期发送,让后加入的节点也能学习到 */
    const UInt32 kBusHelloPeriodMs = 3000U;

    if ((self == NULL) || (self->peer_array == NULL)
        || (link_recv_buff_ptr == NULL) || (link_recv_buff_size == 0))
    {
        result = kXrpcErrInvalidParam;
    }
    else
    {
        /* 通过 owner(TXrpcNode*) 间接获取 lock_ops */
        node = (TXrpcNode *)self->owner;
        {
            TXrpcMsgFrameNotifyUserParams fdn_user_param = {0};
            fdn_user_param.node = node;

            now_tick = XrpcGetSysTick();

            for (i = 0; i < self->peer_count; i++)
            {
                peer = &self->peer_array[i];
                link = peer->link;
                fdn_user_param.peer_node = peer;

                /* 链路未连接:区分状态处理
                 *   Disconnect   → 立即发起连接
                 *   TimeOut      → 重连(链路层上次连接失败已 settle)
                 *   ConnectPending→ 等连接回调,本轮不做动作
                 *   Connected    → 进入收发分支
                 *   Busy / Invalid → 本轮跳过 */
                state = IXrpcLinkState(link);
                if (state != kXrpcLinkStateConnected)
                {
                    /* Busy 只是链路暂时忙,不算断开 */
                    if ((peer->link_up == True) && (state != kXrpcLinkStateBusy))
                    {
                        peer->link_up = False;
                        TXrpcNodePeerLinkDown(node, peer);
                    }

                    if ((state == kXrpcLinkStateDisconnect)
                        || (state == kXrpcLinkStateTimeOut))
                    {
                        (void)IXrpcLinkConnectSwitch(link, True);
                    }
                    continue;
                }
                peer->link_up = True;

                /* 已连接:先做握手节流(HelloReq 1s 节拍);
                 * HelloReq 与 Knock/业务收发解耦,即使 provider_node_id==0 也照发,
                 * 收包与 knock 都按 peer 自己的 tick 节拍执行 */
                {
                    Bool need_hello_req = False;
                    const TXrpcLockOps *node_lock_ops = (node != NULL) ? node->lock_ops : NULL;
                    Bool node_lock_ok = True;

                    if (node_lock_ops != NULL)
                    {
                        node_lock_ok = node_lock_ops->lock(node_lock_ops->user_data);
                    }

                    if ((node_lock_ok == True)
                        && (peer->topology == kXrpcLinkTopologyBus))
                    {
                        if ((now_tick - peer->hello_tick) >= kBusHelloPeriodMs)
                        {
                            peer->hello_tick = now_tick;
                            need_hello_req = True;
                        }
                    }
                    else if ((node_lock_ok == True) && (peer->peer_node_id == 0U))
                    {
                        UInt32 hello_elapsed = now_tick - peer->hello_tick;

                        if (((peer->hello_pending == False)
                             && (hello_elapsed >= kHelloReqPeriodMs))
                            || ((peer->hello_pending == True)
                                && (hello_elapsed >= kHelloRspTimeoutMs)))
                        {
                            peer->hello_tick = now_tick;
                            peer->hello_pending = True;
                            need_hello_req = True;
                        }
                    }

                    if ((node_lock_ops != NULL) && (node_lock_ok == True))
                    {
                        (void)node_lock_ops->unlock(node_lock_ops->user_data);
                    }

                    if (need_hello_req == True)
                    {
                        (void)TXrpcNodeSendHelloReq(node, peer);
                    }
                }
                /* hello_pending=True 时等对端 HelloRsp 清标志,超时后重发 */

                /* 已连接:拉取一帧数据(无论 hello_pending 与否,本轮都允许收包) */
                recv_size = IXrpcLinkRecv(link, (char *)link_recv_buff_ptr,
                                           (Int32)link_recv_buff_size);
                if (recv_size > 0)
                {
                    if (peer->fdn_recv.m_buff_start != NULL)
                    {
                        /* 启用拼接模式:喂入 fdn,由回调驱动上层路由 */
                        TFdnRecvData(&peer->fdn_recv,
                                     (const char *)link_recv_buff_ptr,
                                     recv_size,
                                     (void *)&fdn_user_param);
                    }
                    else
                    {
                        /* 单帧模式:无 fdn 缓存时,recv 数据必须本身是一个完整有效帧 */
                        Int32 frame_size = L_XrpcPeerDecodeFrame((const char *)link_recv_buff_ptr,
                                                                 recv_size,
                                                                 (void *)&fdn_user_param);

                        if (frame_size > 0)
                        {
                            L_XrpcPeerNotifyFrame((const char *)link_recv_buff_ptr,
                                                  frame_size,
                                                  (void *)&fdn_user_param);
                        }
                    }
                }
                else if (recv_size < 0)
                {
                    (void)IXrpcLinkConnectSwitch(link, False);
                    peer->link_up = False;
                    TXrpcNodePeerLinkDown(node, peer);
                    /* 链路已断,本轮不再发 Knock,以免额度耗在断开的链路上 */
                    continue;
                }

                /* knock 按 peer 自己的 knock_tick 节流(1s),与 hello 解耦 */
                {
                    Bool need_knock_deal = False;
                    const TXrpcLockOps *node_lock_ops = (node != NULL) ? node->lock_ops : NULL;
                    Bool node_lock_ok = True;

                    if (node_lock_ops != NULL)
                    {
                        node_lock_ok = node_lock_ops->lock(node_lock_ops->user_data);
                    }

                    /* provider 取自 peer_node_id,握手完成前 Knock 没有意义 */
                    if ((node_lock_ok == True)
                        && ((peer->peer_node_id != 0U)
                            || (peer->topology == kXrpcLinkTopologyBus))
                        && ((now_tick - peer->knock_tick) >= 1000U))
                    {
                        peer->knock_tick = now_tick;
                        need_knock_deal = True;
                    }

                    if ((node_lock_ops != NULL) && (node_lock_ok == True))
                    {
                        (void)node_lock_ops->unlock(node_lock_ops->user_data);
                    }

                    if (need_knock_deal == True)
                    {
                        (void)TXrpcNodeProcessKnockDeal(node, peer);
                    }
                }
            }
        }
    }

    return result;
}
