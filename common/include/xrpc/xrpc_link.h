/**
 * @addtogroup xrpc_link
 * xrpc_link
 * @{
 */
/**
 * @file        xrpc_link.h
 * @brief       link 接口定义。封装多种数据链路方式（串口、SOCKET 等）的统一抽象。
 *              上层通过 IXrpcLink* 虚函数表调用 ConnectSwitch/State/Send/Recv 四个
 *              分发函数，无需关心底层是哪种物理链路。
 * @note        IXrpcLink 仅声明接口，链路实现由调用方提供（如串口 link、SOCKET link）。
 *              同一进程可以注册多个不同链路实例，每个实例独立管理连接状态。
 * @copyright   Copyright (c) 2026 AuyvJpz
 */

#ifndef _XRPC_LINK_H_
#define _XRPC_LINK_H_

#include "xrpc/xrpc_types.h"

#ifdef __cplusplus
extern "C"
{
#endif


/**
 * @brief 总线链路状态枚举。
 * @note  链路实现方在 OnState_ 回调中返回当前状态；
 *        kXrpcLinkStateInvalid 表示链路对象未初始化或被销毁。
 */
enum TXrpcLinkStateTag
{
    kXrpcLinkStateInvalid         = 0,    ///< 无效（未初始化或已销毁）
    kXrpcLinkStateDisconnect      = 1,    ///< 未连接（创建后默认状态）
    kXrpcLinkStateConnectPending  = 2,    ///< 等待连接结果（连接请求已下发）
    kXrpcLinkStateTimeOut         = 3,    ///< 连接超时（等待再次发起连接）
    kXrpcLinkStateConnected       = 4,    ///< 已连接（可收发数据）
    kXrpcLinkStateBusy            = 5,    ///< 总线正忙（链路数据正在发送，暂缓新数据）
};
typedef enum TXrpcLinkStateTag TXrpcLinkState;



typedef struct IXrpcLinkTag  IXrpcLink;

/**
 * @brief 发送事件回调（vtable OnSend_）。
 * @param sender 调用者对象指针（链路实例）
 * @param data   待发送数据缓冲区
 * @param size   待发送数据字节数
 * @return  > 0 实际写入链路层的数据字节数;小于 size 按发送失败处理,
 *              实现需要保证整帧写入
 *          = 0 未发送任何字节（链路忙/未连接），按发送失败处理
 *          < 0 失败（参数非法或链路异常）
 */
typedef Int32(*IXrpcLinkSendEvent)(IXrpcLink *sender, const char *data, Int32 size);

/**
 * @brief 接收事件回调（vtable OnRecv_）。
 * @param sender 调用者对象指针（链路实例）
 * @param buf    接收数据缓冲区（由调用方提供）
 * @param size   缓冲区容量（字节）
 * @return  > 0 实际读取到 buf 的字节数
 *          = 0 本次无可读数据
 *          < 0 失败（参数非法或链路异常）
 */
typedef Int32(*IXrpcLinkRecvEvent)(IXrpcLink *sender, char *buf, Int32 size);

/**
 * @brief 连接开关事件回调（vtable OnConnectSwitch_）。
 * @param sender         调用者对象指针（链路实例）
 * @param connect_switch True  执行连接；False 断开连接
 * @return  True  切换命令已成功下发；
 *          False 命令下发失败或参数非法
 */
typedef Bool(*IXrpcLinkConnectSwitchEvent)(IXrpcLink *sender, Bool connect_switch);

/**
 * @brief 链路状态查询事件回调（vtable OnState_）。
 * @param sender 调用者对象指针（链路实例）
 * @return  当前链路状态，参见 TXrpcLinkState
 */
typedef TXrpcLinkState(*IXrpcLinkStateEvent)(IXrpcLink *sender);


/**
 * @brief 链路虚函数表结构体。
 * @note  链路实现方需实现 OnConnectSwitch_ / OnSend_ / OnRecv_ / OnState_ 四个回调，
 *        并通过 IXrpcLink* 暴露给上层使用。
 */
struct IXrpcLinkTag
{
    IXrpcLinkConnectSwitchEvent OnConnectSwitch_;     ///< 连接/断开切换事件
    IXrpcLinkSendEvent    OnSend_;              ///< 数据发送事件
    IXrpcLinkRecvEvent    OnRecv_;              ///< 数据接收事件
    IXrpcLinkStateEvent   OnState_;             ///< 状态查询事件
};


/**
 * @brief 触发链路连接或断开。
 * @param sender         链路对象指针
 * @param connect_switch True 执行连接；False 断开连接
 * @return  链路实现回调的返回值；任一指针为 NULL 时返回 False
 * @note  非线程安全；同一 sender 的并发调用需外层串行化
 */
Bool IXrpcLinkConnectSwitch(IXrpcLink *sender, Bool connect_switch);

/**
 * @brief 查询链路当前状态。
 * @param sender 链路对象指针
 * @return  当前链路状态；sender 或 OnState_ 为 NULL 时返回 kXrpcLinkStateInvalid
 */
TXrpcLinkState IXrpcLinkState(IXrpcLink *sender);

/**
 * @brief 通过链路发送一帧数据。
 * @param sender 链路对象指针
 * @param data   待发送数据缓冲区
 * @param size   待发送字节数（>0）
 * @return  链路实现回调的返回值；参数非法时返回 kXrpcErrInvalidParam
 */
Int32 IXrpcLinkSend(IXrpcLink *sender, const char *data, Int32 size);

/**
 * @brief 从链路读取一帧数据到调用方缓冲区。
 * @param sender 链路对象指针
 * @param buf    接收数据缓冲区（由调用方提供）
 * @param size   缓冲区容量（字节，>0）
 * @return  链路实现回调的返回值；参数非法时返回 kXrpcErrInvalidParam
 * @note  本函数名为 IXrpcLinkRecv，区别于函数指针 typedef IXrpcLinkRecvEvent
 */
Int32 IXrpcLinkRecv(IXrpcLink *sender, char *buf, Int32 size);


#ifdef __cplusplus
}  ///< extern "C"
#endif

#endif  ///< _XRPC_LINK_H_
