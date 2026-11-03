/**
 * @file        xrpc_delivery.c
 * @brief       投递状态机实现。
 * @copyright   Copyright (c) 2026 AuyvJpz
 */

#include "xrpc/xrpc_delivery.h"

void TXrpcDeliveryStart(TXrpcDelivery *self, UInt32 seq, UInt32 now_tick,
                        UInt32 timeout_ms, UInt8 retry)
{
    self->seq = seq;
    self->wait_tick = now_tick;
    self->timeout_ms = timeout_ms;
    self->retry_left = retry;
    self->attempts = 1U;
}

TXrpcDeliveryAction TXrpcDeliveryStep(TXrpcDelivery *self, UInt32 now_tick)
{
    TXrpcDeliveryAction result = kXrpcDeliveryWait;

    if ((now_tick - self->wait_tick) >= self->timeout_ms)
    {
        if (self->retry_left > 0U)
        {
            self->retry_left--;
            self->attempts++;
            self->wait_tick = now_tick;
            result = kXrpcDeliveryResend;
        }
        else
        {
            result = kXrpcDeliveryExpired;
        }
    }

    return result;
}
