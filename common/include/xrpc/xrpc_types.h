/**
 * @file        xrpc_types.h
 * @brief       核心库和生成代码使用的基础类型。
 * @note        只放核心库真正用到的名字。下面的检查让类型宽度不符的平台编译失败。
 * @copyright   Copyright (c) 2026 AuyvJpz
 */

#ifndef _XRPC_TYPES_H_
#define _XRPC_TYPES_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

#ifndef False
#define False   0
#endif

#ifndef True
#define True    1
#endif

typedef unsigned char       Bool;       ///< 1 字节，取值 False、True

typedef signed char         Int8;
typedef signed short        Int16;
typedef signed int          Int32;
typedef long long           Int64;

typedef unsigned char       UInt8;
typedef unsigned short      UInt16;
typedef unsigned int        UInt32;
typedef unsigned long long  UInt64;

typedef unsigned char       Byte;

typedef char TXrpcCheckInt16[(sizeof(Int16) == 2U) ? 1 : -1];
typedef char TXrpcCheckInt32[(sizeof(Int32) == 4U) ? 1 : -1];
typedef char TXrpcCheckInt64[(sizeof(Int64) == 8U) ? 1 : -1];

#ifdef __cplusplus
}  ///< extern "C"
#endif

#endif  ///< _XRPC_TYPES_H_
