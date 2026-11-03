/**
 * @file        xrpc_wire.c
 * @brief       线上字段读写实现。
 * @copyright   Copyright (c) 2026 AuyvJpz
 */

#include "xrpc/xrpc_wire.h"

UInt16 XrpcWireGetU16(const void *src)
{
    const UInt8 *bytes = (const UInt8 *)src;

    return (UInt16)(((UInt16)bytes[0] << 8) | (UInt16)bytes[1]);
}

UInt32 XrpcWireGetU32(const void *src)
{
    const UInt8 *bytes = (const UInt8 *)src;

    return ((UInt32)bytes[0] << 24) | ((UInt32)bytes[1] << 16)
         | ((UInt32)bytes[2] << 8) | (UInt32)bytes[3];
}

Int32 XrpcWireGetI32(const void *src)
{
    return (Int32)XrpcWireGetU32(src);
}

void XrpcWirePutU16(void *dst, UInt16 value)
{
    UInt8 *bytes = (UInt8 *)dst;

    bytes[0] = (UInt8)(value >> 8);
    bytes[1] = (UInt8)(value & 0xFFU);
}

void XrpcWirePutU32(void *dst, UInt32 value)
{
    UInt8 *bytes = (UInt8 *)dst;

    bytes[0] = (UInt8)(value >> 24);
    bytes[1] = (UInt8)((value >> 16) & 0xFFU);
    bytes[2] = (UInt8)((value >> 8) & 0xFFU);
    bytes[3] = (UInt8)(value & 0xFFU);
}

void XrpcWirePutI32(void *dst, Int32 value)
{
    XrpcWirePutU32(dst, (UInt32)value);
}
