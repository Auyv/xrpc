/**
 * @addtogroup xrpc_wire
 * xrpc_wire
 * @{
 */
/**
 * @file        xrpc_wire.h
 * @brief       线上字段读写。线上的多字节字段一律为网络字节序（大端）。
 * @note        线上结构体的多字节字段都是字节数组，只能经这里读写，
 *              按移位实现，与宿主字节序和对齐要求无关。
 * @copyright   Copyright (c) 2026 AuyvJpz
 */

#ifndef _XRPC_WIRE_H_
#define _XRPC_WIRE_H_

#include "xrpc/xrpc_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

UInt16 XrpcWireGetU16(const void *src);
UInt32 XrpcWireGetU32(const void *src);
Int32 XrpcWireGetI32(const void *src);

void XrpcWirePutU16(void *dst, UInt16 value);
void XrpcWirePutU32(void *dst, UInt32 value);
void XrpcWirePutI32(void *dst, Int32 value);

#ifdef __cplusplus
}  ///< extern "C"
#endif

#endif  ///< _XRPC_WIRE_H_

/**
 * @}
 */
