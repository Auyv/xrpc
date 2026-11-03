/**
 * @addtogroup xrpc
 * xrpc
 * @{
 */
/**
 * @file        xrpc.h
 * @brief       XRPC 系统适配层。声明 2 个由部署方实现的系统函数:
 *              XrpcSleepMs / XrpcGetSysTick。CRC16 走虚接口,见 xrpc_crc16.h。
 * @note        进程级单例入口与节点级 API 不在本头:
 *              - 部署代码可用 tools/xrpc_port_gen.py 生成;
 *              - 多实例部署请直接 include xrpc_node.h 使用
 *                TXrpcNodeCreate / SetPeerCache / SetProcessTable / RunOnce。
 *              本头仅暴露平台相关的 2 个函数原型,供部署方在自己的
 *              .c 中提供实现。
 * @copyright   Copyright (c) 2026 AuyvJpz
 */

#ifndef _XRPC_H_
#define _XRPC_H_

#include "xrpc/xrpc_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

// ======================== 系统函数定义。外部实现时自行实现 ===============

// 休眠指定毫秒数。由部署方实现。
void XrpcSleepMs(UInt32 time_ms);

// 获取当前系统 tick。单位 ms。由部署方实现。
UInt32 XrpcGetSysTick(void);

#ifdef __cplusplus
}  ///< extern "C"
#endif

#endif  ///< _XRPC_H_
