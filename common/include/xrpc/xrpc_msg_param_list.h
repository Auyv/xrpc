/**
 * @addtogroup xrpc_msg_param_list
 * xrpc_msg_param_list
 * @{
 */
/**
 * @file        xrpc_msg_param_list.h
 * @brief       参数列表的 TLV 编解码。
 * @copyright   Copyright (c) 2026 AuyvJpz
 */

#ifndef _XRPC_MSG_PARAM_LIST_H_
#define _XRPC_MSG_PARAM_LIST_H_

#include "xrpc/xrpc_types.h"
#include "xrpc/xrpc_msg.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct TXrpcMsgParamListTag TXrpcMsgParamList;
struct TXrpcMsgParamListTag
{
    char *encode_buffer_addr;    // 参数列表数据缓冲区
    UInt16 encode_buffer_size;    // 参数列表数据缓冲区长度
    UInt16 encode_buffer_used;    // 参数列表数据缓冲区已使用长度

    UInt8 param_cnt;    // 参数个数
};

/**
 * 创建参数列表
 * @param *self 参数列表对象指针
 * @param param_cnt 参数个数。从数据类型限制最多 255
 * @param *buffer   参数列表数据缓冲区
 * @param buffer_size   参数列表数据缓冲区长度
 * @return = 0 成功，< 0 失败
 * @note: param_cnt = 0 时，encode_buffer_used = 0;
 *        param_cnt > 0 时，需要主动解析buffer内容，确定真实param_cnt和encode_buffer_used的值
 */
Int32 TXrpcMsgParamListCreate(TXrpcMsgParamList *self, UInt8 param_cnt, char *buffer, UInt16 buffer_size);


/**
 * 析构函数，释放参数列表占用的资源
 * @param *self
 * @return 
 * @note:xxxxx
 */
void  TXrpcMsgParamListDestroy(TXrpcMsgParamList *self);

/**
 * 清空参数列表，重置参数列表状态
 * @param *self
 * @return 
 * @note: 执行 param_cnt = 0 ; encode_buffer_used = 0;
 */
void  TXrpcMsgParamListClear(TXrpcMsgParamList *self);

/**
 * 获取参数列表中参数的个数
 * @param *self
 * @return 有效参数个数
 */
UInt8 TXrpcMsgParamListCount(TXrpcMsgParamList *self);


/**
 * 增加参数到参数列表并编码到参数列表数据缓冲区
 * @param *self     
 * @param *param_data   参数数据结构指针，包含参数ID、参数类型、参数值等信息
 * @return = 0 成功，< 0 失败
 * @note:xxxxx
 */
Int32 TXrpcMsgParamListAdd(TXrpcMsgParamList *self, const TXrpcMsgParamItem *param_data);


/**
 * 解析参数列表数据缓冲区内容，获取参数列表中所有参数的数据
 * @param *self
 * @param *param_data   参数数据结构指针，包含参数ID、参数类型、参数值等信息。调用前需要分配足够的内存空间以存储所有参数数据
 * @param *param_cnt    参数数据结构的个数。调用前需要设置为param_data指针指向的内存空间可以存储的最大参数个数。调用后返回实际解析出的参数个数
 * @return  = 0 成功，< 0 失败
 * @note:xxxxx
 */
Int32 TXrpcMsgParamListGetAllItem(TXrpcMsgParamList *self, TXrpcMsgParamItem *param_data, UInt8 *param_cnt);


/**
 * 根据参数ID获取参数列表中的参数数据
 * @param *self
 * @param param_id  需要获取的参数ID
 * @param *param_data   参数数据结构指针，包含参数ID、参数类型、参数值等信息。调用前需要分配足够的内存空间以存储参数数据
 * @return = 0 成功，< 0 失败
 * @note:xxxxx
 */
Int32 TXrpcMsgParamListGetItemById(TXrpcMsgParamList *self, Int32 param_id, TXrpcMsgParamItem *param_data);

/**
 * 获取参数列表编码后的数据缓冲区指针和长度
 * @param *self 
 * @param *len 参数列表编码后的数据长度 encode_buffer_used
 * @return 编码后的数据缓冲区指针
 * @note:xxxxx
 */
char *TXrpcMsgParamListEncodeBuffer(TXrpcMsgParamList *self, Int32 *len);




#ifdef __cplusplus
}  ///< extern "C"
#endif

#endif  ///< _XRPC_MSG_PARAM_LIST_H_


