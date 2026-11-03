/**
 * @addtogroup xrpc_crc16
 * xrpc_crc16
 * @{
 */
/**
 * @file        xrpc_crc16.h
 * @brief       CRC16 计算虚接口。核心库不实现算法,由部署方通过适配层提供,
 *              经 TXrpcNodeSetCrc16 注入节点。
 * @note        frame_crc 是线上协议的一部分:实现必须与现网设备的算法字节级一致,
 *              计算范围由核心库决定(从 node_id 到帧尾)。
 * @copyright   Copyright (c) 2026 AuyvJpz
 */

#ifndef _XRPC_CRC16_H_
#define _XRPC_CRC16_H_

#include "xrpc/xrpc_types.h"

#ifdef __cplusplus
extern "C"
{
#endif


typedef struct IXrpcCrc16Tag  IXrpcCrc16;

/**
 * @brief CRC16 计算事件回调(vtable OnCalc_)。
 * @param sender 调用者对象指针(CRC16 接口实例)
 * @param data   待计算数据
 * @param size   待计算字节数
 * @return  CRC16 结果
 * @note    可能同时被轮询线程和调用线程调用,实现必须可重入。
 */
typedef UInt16(*IXrpcCrc16CalcEvent)(IXrpcCrc16 *sender, const UInt8 *data, UInt32 size);


/**
 * @brief CRC16 虚函数表结构体。
 * @note  实现方提供 OnCalc_ 回调,并通过 IXrpcCrc16* 暴露给节点。
 */
struct IXrpcCrc16Tag
{
    IXrpcCrc16CalcEvent OnCalc_;    ///< CRC16 计算事件
};


/**
 * @brief 计算 CRC16。
 * @param sender CRC16 接口指针
 * @param data   待计算数据
 * @param size   待计算字节数
 * @param crc    输出计算结果
 * @return  成功返回 kXrpcErrOk;任一指针为 NULL 返回 kXrpcErrInvalidParam
 */
Int32 IXrpcCrc16Calc(IXrpcCrc16 *sender, const UInt8 *data, UInt32 size, UInt16 *crc);


#ifdef __cplusplus
}  ///< extern "C"
#endif

#endif  ///< _XRPC_CRC16_H_

/**
 * @}
 */
