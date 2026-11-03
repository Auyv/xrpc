/**
 * @file        xrpc_link.c
 * @brief       IXrpcLink 虚函数表分发实现：ConnectSwitch / State / Send / Recv。
 *              链路实现方只负责提供 vtable 回调，本文件负责把上层 API 调用转发给回调。
 * @note        非线程安全；同一链路实例的并发分发需外层串行化。
 * @copyright   Copyright (c) 2026 AuyvJpz
 */

#include "xrpc/xrpc_link.h"
#include "xrpc/xrpc_common.h"


/**
 * @brief 触发链路连接或断开。NULL 安全：sender 或 OnConnectSwitch_ 为 NULL 时返回 False。
 * @param sender         链路对象指针
 * @param connect_switch True 执行连接；False 断开连接
 * @return  链路回调返回值；任一指针为 NULL 时返回 False
 */
Bool IXrpcLinkConnectSwitch(IXrpcLink *sender, Bool connect_switch)
{
    Bool result = False;

    if ((sender != NULL) && (sender->OnConnectSwitch_ != NULL))
    {
        result = sender->OnConnectSwitch_(sender, connect_switch);
    }

    return result;
}


/**
 * @brief 查询链路当前状态。NULL 安全：sender 或 OnState_ 为 NULL 时返回 kXrpcLinkStateInvalid。
 * @param sender 链路对象指针
 * @return  当前链路状态
 */
TXrpcLinkState IXrpcLinkState(IXrpcLink *sender)
{
    TXrpcLinkState result = kXrpcLinkStateInvalid;

    if ((sender != NULL) && (sender->OnState_ != NULL))
    {
        result = sender->OnState_(sender);
    }

    return result;
}


/**
 * @brief 通过链路发送一帧数据。
 * @param sender 链路对象指针
 * @param data   待发送数据缓冲区
 * @param size   待发送字节数（>0）
 * @return  链路回调返回值；参数非法时返回 kXrpcErrInvalidParam
 */
Int32 IXrpcLinkSend(IXrpcLink *sender, const char *data, Int32 size)
{
    Int32 result = kXrpcErrInvalidParam;

    if ((sender != NULL) && (data != NULL) && (size > 0)
        && (sender->OnSend_ != NULL))
    {
        result = sender->OnSend_(sender, data, size);
    }

    return result;
}


/**
 * @brief 从链路读取一帧数据到调用方缓冲区。
 * @param sender 链路对象指针
 * @param buf    接收数据缓冲区
 * @param size   缓冲区容量（字节，>0）
 * @return  链路回调返回值；参数非法时返回 kXrpcErrInvalidParam
 */
Int32 IXrpcLinkRecv(IXrpcLink *sender, char *buf, Int32 size)
{
    Int32 result = kXrpcErrInvalidParam;

    if ((sender != NULL) && (buf != NULL) && (size > 0)
        && (sender->OnRecv_ != NULL))
    {
        result = sender->OnRecv_(sender, buf, size);
    }

    return result;
}
