/**
 * @addtogroup xrpc_common
 * xrpc_common
 * @{
 */
/**
 * @file        xrpc_common.h
 * @brief       Xrpc 通用定义：错误码、配置宏
 * @note        用于 xrpc 系列模块共享的错误码与可配置项；避免在源码中直接使用魔数
 * @copyright   Copyright (c) 2026 AuyvJpz
 */

#ifndef _XRPC_COMMON_H_
#define _XRPC_COMMON_H_

#include "xrpc/xrpc_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* ============================================================
 * 错误码定义
 * 注：调用方使用返回值时，按 < 0 视为失败这一约定判断即可
 * ============================================================ */

#define kXrpcErrOk                 (0)     ///< 成功
#define kXrpcErrInvalidParam       (-1)    ///< 参数无效（NULL 指针 / 非法入参）
#define kXrpcErrBufferOverflow     (-2)    ///< 缓冲区不足 / 数据解析失败
#define kXrpcErrListFull           (-3)    ///< 参数列表已满，无法再添加
#define kXrpcErrItemNotFound       (-4)    ///< 未找到指定 ID 的参数
#define kXrpcErrTimeout            (-5)       // 超时
#define kXrpcErrLinkSendFailed     (-6)       // 发送失败
#define kXrpcErrTxBufferOverflow   (-7)    ///< 发送缓冲区不足 / 数据解析失败
#define kXrpcErrProcessNotFound    (-8)    ///< process没有找到
#define kXrpcErrProcessStatus      (-9)    ///< process状态不允许
#define kXrpcErrPeerNodeUnknow     (-10)    ///< 对端node id无效
#define kXrpcErrRxBufferOverflow   (-11)    ///< 接收缓冲区不足 / 数据解析失败
#define kXrpcErrProviderUnknow     (-12)    ///< provider id无效
#define kXrpcErrPeerStatusInvalid  (-13)    ///< 对端状态无效
#define kXrpcErrNotSupported       (-14)    ///< 当前节点不支持该 process 能力
#define kXrpcErrFrameTooLarge      (-15)    ///< 帧超过链路或对端声明的接收上限
#define kXrpcErrNoFreeSlot         (-16)    ///< 调用槽已用完
#define kXrpcErrTtlExpired         (-17)    ///< 跳数耗尽，中继不再转发

/* ============================================================
 * 配置项
 * ============================================================ */

/* 单个参数项的固定头部长度：param_id(1) + param_type(1) + param_len(2) */
#define kXrpcParamItemHeaderSize   (4)

#define XRPC_MSG_HEAD_SIZE         (28)
#define XRPC_MSG_HEAD_TAG          "XRPC"
#define XRPC_MSG_CRC_START_OFFSET  (8)
#define XRPC_MSG_FRAME_LEN_MIN     (20)

/* 新帧的跳数。大于正常网络的跳数，只用来拦截绕圈的帧 */
#define XRPC_MSG_TTL_DEFAULT       (255U)

/* 组件版本，只在这里写。和协议版本无关，不上线 */
#define XRPC_VERSION_MAJOR         (0)
#define XRPC_VERSION_MINOR         (9)
#define XRPC_VERSION_PATCH         (0)

/* 协议版本：高字节是主版本，决定帧格式 */
#define XRPC_PROTOCOL_VERSION      (0x0200U)

/* 参数列表最大项数（由 UInt8 限制） */
#define kXrpcParamListMaxCnt       (255)


/* 保留的节点 ID：0 表示未知，全 1 表示广播 */
#define kXrpcNodeIdBroadcast       (0xFFFFFFFFU)





/* ============================================================
 * 默认值
 * ============================================================ */

#define XRPC_DEFAULT_PROCESS_TIMEOUT_MS  (5000U)


/**
 * @brief 锁操作 vtable。用户提供一组回调,由库在临界区前后调用。
 * @note  所有字段均可为 NULL;为 NULL 时该操作视为 no-op。
 *        锁回调应是非递归的(若需要重入保护,请使用 TXrpcNodeRunOnce
 *        的单线程调度约定,而非依赖锁的重入能力)。
 *        lock 回调返回 False 时,被保护的函数将返回 kXrpcErrInvalidParam;
 *        unlock 回调返回值被忽略。
 *        过程与 peer 的运行状态只靠节点锁保护,多线程使用时节点锁必须注入。
 */
/* Lock configuration rule:
 * - TXrpcNode.lock_ops protects node/process runtime state.
 * - TXrpcPeerNode.send_lock_ops protects one peer send buffer.
 * These lock objects must not share the same underlying mutex, and must not be
 * the same mutex used internally by the logging backend. Otherwise nested xrpc
 * calls or log output may self-deadlock when the lock implementation is not
 * recursive.
 * Lock order: the node lock may be taken while holding a peer send lock,
 * never the other way round.
 */
typedef struct TXrpcLockOpsTag TXrpcLockOps;
typedef Bool(*TXrpcLockOpsMethod)(void *user_data);
struct TXrpcLockOpsTag
{
    TXrpcLockOpsMethod lock;    ///< 加锁回调
    TXrpcLockOpsMethod unlock;  ///< 解锁回调
    void *user_data;            ///< 透传给两个回调的上下文
};

/* xrpc 消息类型，全部消息见 docs/XRPC协议规范.md 第 3 章 */
enum TXrpcMsgTypeTag
{
    kXrpcMsgTypeHelloReq  = 0x0102,  // 无payload数据,用于向对端发送自身node_id
    kXrpcMsgTypeHelloRsp  = 0xF102,  // 无payload数据,用于向对端回复自身node_id

    kXrpcMsgTypeKnockReq  = 0x0101,  // 向对端询问是否存在期望的process功能
    kXrpcMsgTypeKnockRsp  = 0xF101,  // 应答对应结果

    /* 0x0110 曾是 OneWay，已移除，不要复用 */
    kXrpcMsgTypeCallReq   = 0x0111,  // 需要等待对端回复调用结果
    kXrpcMsgTypeCallRsp   = 0xF111,  // 回复对端调用结果

    kXrpcMsgTypeNotifyEvent = 0x0120,  // 通知事件
    kXrpcMsgTypeNotifyAck   = 0xF120,  // 通知应答
    kXrpcMsgTypeAddressed   = 0x0130,  // 寻址信封,总线链路上的单播消息使用
};
typedef enum TXrpcMsgTypeTag TXrpcMsgType;




#ifdef __cplusplus
}  ///< extern "C"
#endif

#endif  ///< _XRPC_COMMON_H_

/**
 * @}
 */
