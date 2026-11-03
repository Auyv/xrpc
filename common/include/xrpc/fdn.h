/**
 * @addtogroup fdn
 * fdn
 * @{
 */
/**
 * @file        fdn.h
 * @brief       FullDataNotify 完整数据通知
 * @note        解析流式数据时,需要使用缓存区来存储数据,FullDataNotify 功能可以在数据解析完成后通知应用层,应用层可以通过回调函数获取完整的数据进行处理。
 *              当数据不完整时,数据在缓存中保存,直到数据完整后才会通知应用层。
 * @copyright   Copyright (c) 2026 AuyvJpz
 */

#ifndef _FDN_H_
#define _FDN_H_


/* 基础类型由 xrpc_types.h 提供, 避免重复定义 */
#include "xrpc/xrpc_types.h"


#ifdef __cplusplus
extern "C"
{
#endif


/**
 * @note 本组件非线程安全。若需要在多线程或中断上下文使用,调用方需自行串行化(例如临界区、关中断)。
 */
typedef struct TFdnDataTag TFdnData;
//

/**
 * 数据解码方法类型定义
 * @param *data 接收到的数据。指向数据缓存区的指针,数据存储在该缓存区中。
 * @param size 数据大小
 * @param *user_data_farme 目标数据帧结构体指针。可能会是临时变量,或者是全局变量,或者是用户自定义的结构体指针。用户可以通过该指针将解析后的数据存储到目标数据帧中,以便后续处理。
 * @return > 0 解析成功,返回解析后的数据大小;
 *         = 0 解析失败,数据不完整,暂存缓存数据,下次调度补充数据再执行编码;
 *         < 0 解析失败,发生错误。
 * @note 解码器内部可维护状态机;若需跨帧状态,建议把状态保存在 user_data_farme 指向的结构里。
 */
typedef Int32 (* TFdnDecodeMethod)(const char *data, Int32 size, void *user_data_farme);

/**
 * 完整数据通知回调函数类型定义
 * @param *data 指向已解析完整帧的缓存区起始位置(本组件内部缓存,回调返回后即可能被覆盖)
 * @param size 完整帧大小
 * @param *user_data_farme 同 @ref TFdnDecodeMethod
 */
typedef void (* TFdnNotifyCallback)(const char *data, Int32 size, void *user_data_farme);

/**
 * @brief FDN 错误码定义
 */
enum TFdnErrCodeTag
{
    kFdnErrCodeParam = -1    ///< 参数错误(size<=0 或 self/data 为空)
};
typedef enum TFdnErrCodeTag TFdnErrCode;

struct TFdnDataTag
{
    char             *m_buff_start;       ///< 数据缓存区(外部拥有,生命周期由调用方管理)
    Int32             m_buff_size;        ///< 缓存区容量(字节)
    Int32             m_recv_size;        ///< 当前已接收未消费字节数
    UInt16            m_error_count;      ///< 连续解析错误计数(达到 m_max_error_skip 时上报并重新计数)
    UInt16            m_max_error_skip;   ///< 连续错误字节上报阈值(=0 表示不上报)
    TFdnDecodeMethod  m_decode_method;    ///< 数据解码方法
    TFdnNotifyCallback m_notify_callback; ///< 数据通知回调函数
};


/**
 * @brief 初始化一个 FDN 实例。
 * @param *self TFdnData 结构体指针
 * @param *buff 外部提供的缓存区,生命周期由调用方管理
 * @param buff_size 缓存区容量(字节)
 * @param decode_method 解码回调,不可为 NULL
 * @param notify_callback 通知回调,不可为 NULL
 * @return 0 成功;-1 参数错误(参见 @ref kFdnErrCodeParam)
 * @note buff 的所有权归调用方,TFdnDestroy / TFdnReset 不会释放它。
 */
Int32 TFdnCreate(TFdnData *self,
                 char *buff,
                 Int32 buff_size,
                 TFdnDecodeMethod decode_method,
                 TFdnNotifyCallback notify_callback);


/**
 * @brief 反初始化 FDN 实例:复位自身状态,但不释放外部传入的 buff。
 * @param *self TFdnData 结构体指针
 * @note 若想清空已缓存未消费数据,使用 @ref TFdnReset。
 */
void TFdnDestroy(TFdnData *self);


UInt16 TFdnErrorCount(TFdnData *self);

UInt16 TFdnMaxErrorSkip(TFdnData *self);

void TFdnSetMaxErrorSkip(TFdnData *self, UInt16 max_error_skip);


/**
 * @brief 复位接收状态:清空已缓存未消费数据(但不修改 buff 绑定关系)。
 * @param *self TFdnData 结构体指针
 */
void TFdnReset(TFdnData *self);


/**
 * @brief 将接收到的数据追加到缓存区中,并调用解码方法进行解析,如果解析成功则调用通知回调函数通知应用层。
 * @param *self TFdnData 结构体指针,包含数据缓存区、解码方法和通知回调函数等信息。
 * @param *data 接收到的数据。
 * @param size  数据大小
 * @param *user_data_farme 目标数据帧结构体指针(透传给解码器和通知回调)。
 * @return > 0 实际消费的字节数(已拷贝到内部缓存);
 *         = 0 缓存区已满或未消费任何字节,数据可能被截断/丢弃;
 *         -1 参数错误(参见 @ref kFdnErrCodeParam)
 * @note 调用方需保证 self 和 data 非空(此处显式做空指针检查以便早发现 bug)。
 */
Int32 TFdnRecvData(TFdnData *self, const char *data, Int32 size, void *user_data_farme);


/**
 * 查看缓存总大小
 * @param *self
 * @return 
 * @note: <= 0 表示对象没有初始化
 */
Int32 TFdnBuffSize(TFdnData *self);

#ifdef __cplusplus
}  ///< extern "C"
#endif

#endif  ///< _FDN_H_
