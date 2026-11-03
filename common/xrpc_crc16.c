/**
 * @file        xrpc_crc16.c
 * @brief       IXrpcCrc16 虚函数表分发实现。
 * @copyright   Copyright (c) 2026 AuyvJpz
 */

#include "xrpc/xrpc_crc16.h"
#include "xrpc/xrpc_common.h"


Int32 IXrpcCrc16Calc(IXrpcCrc16 *sender, const UInt8 *data, UInt32 size, UInt16 *crc)
{
    Int32 result = kXrpcErrInvalidParam;

    if ((sender != NULL) && (sender->OnCalc_ != NULL)
        && (data != NULL) && (crc != NULL))
    {
        *crc = sender->OnCalc_(sender, data, size);
        result = kXrpcErrOk;
    }

    return result;
}
