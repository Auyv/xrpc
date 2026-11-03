/**
 * @addtogroup xrpc_delivery
 * xrpc_delivery
 * @{
 */
/**
 * @file        xrpc_delivery.h
 * @brief       投递状态机：判断一次投递该继续等、重发还是到期。
 * @note        只做判断，不发送、不回调、不取锁。调用方在节点锁内拿到动作，
 *              解锁后再发送或回调。
 * @copyright   Copyright (c) 2026 AuyvJpz
 */

#ifndef _XRPC_DELIVERY_H_
#define _XRPC_DELIVERY_H_

#include "xrpc/xrpc_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** 一次投递的状态 */
typedef struct TXrpcDeliveryTag TXrpcDelivery;
struct TXrpcDeliveryTag
{
    UInt32 seq;             ///< 最近一次发送的帧头 seq，迟到的旧应答对不上
    UInt32 wait_tick;       ///< 最近一次发送的时刻
    UInt32 timeout_ms;      ///< 每次等待的时长
    UInt8  retry_left;      ///< 还能重发几次
    UInt8  attempts;        ///< 已发送的次数
};

/** 该做什么 */
typedef enum TXrpcDeliveryActionTag
{
    kXrpcDeliveryWait    = 0,   ///< 继续等
    kXrpcDeliveryResend  = 1,   ///< 该重发了，调用方记下新的 seq 后发送
    kXrpcDeliveryExpired = 2,   ///< 次数用尽
} TXrpcDeliveryAction;

/**
 * @brief 开始一次投递，第一次发送也算一次。
 * @param self       投递状态
 * @param seq        帧头 seq
 * @param now_tick   当前时刻
 * @param timeout_ms 每次等待的时长
 * @param retry      超时后重发的次数
 */
void TXrpcDeliveryStart(TXrpcDelivery *self, UInt32 seq, UInt32 now_tick,
                        UInt32 timeout_ms, UInt8 retry);

/**
 * @brief 判断下一步。返回 kXrpcDeliveryResend 时已扣掉一次重发并重新计时。
 * @param self     投递状态
 * @param now_tick 当前时刻，tick 回绕不影响判断
 * @return 下一步动作
 */
TXrpcDeliveryAction TXrpcDeliveryStep(TXrpcDelivery *self, UInt32 now_tick);

#ifdef __cplusplus
}  ///< extern "C"
#endif

#endif  ///< _XRPC_DELIVERY_H_

/**
 * @}
 */
