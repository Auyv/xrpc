/**
 * @file        xrpc_process.c
 * @brief       过程执行，以及按类型读写参数的实现。
 * @copyright   Copyright (c) 2026 AuyvJpz
 */

#include "xrpc/xrpc_process.h"
#include <string.h>


/* ============================================================
 *  通用 helper
 *  - L_FindItemById:从 param_list 中按 id 取一项,并校验类型与长度
 *  - L_BeWrite / L_BeRead:大端定长整数与缓冲的互转,按移位实现,与宿主字节序无关
 *  - 所有定长整数(UInt8/16/32/64 + Int8/16/32/64)在 wire 上
 *    一律 big-endian,见 XRPC_PROCESS_PARAM_BE。
 * ============================================================ */

static Int32 L_FindItemById(const TXrpcMsgParamList *param_list,
                            UInt8 param_id,
                            UInt8 expect_type,
                            UInt16 expect_len,
                            TXrpcMsgParamItem *out_item)
{
    Int32 result;

    if ((param_list == NULL) || (out_item == NULL))
    {
        return kXrpcErrInvalidParam;
    }

    result = TXrpcMsgParamListGetItemById((TXrpcMsgParamList *)param_list,
                                          (Int32)param_id,
                                          out_item);
    if (result != kXrpcErrOk)
    {
        return result;
    }

    if ((out_item->param_type != expect_type)
        || (out_item->param_len != expect_len))
    {
        return kXrpcErrInvalidParam;
    }

    return kXrpcErrOk;
}


static void L_BeWrite(UInt8 *dst, UInt64 value, UInt16 size)
{
    UInt16 i;

    for (i = 0U; i < size; i++)
    {
        dst[size - 1U - i] = (UInt8)((value >> (8U * i)) & 0xFFU);
    }
}

static UInt64 L_BeRead(const UInt8 *src, UInt16 size)
{
    UInt64 result = 0U;
    UInt16 i;

    for (i = 0U; i < size; i++)
    {
        result = (result << 8) | src[i];
    }

    return result;
}


/* ============================================================
 *  XrpcProcessExec / XrpcProcessNotifyCallRsp
 * ============================================================ */

Int32 XrpcProcessExec(TXrpcProcessObject *proc,
                      const TXrpcMsgParamList *input_param,
                      TXrpcMsgParamList *output_param)
{
    Int32 result = kXrpcErrInvalidParam;

    if ((proc != NULL) && (proc->static_desc != NULL)
        && (proc->static_desc->exec_cb != NULL))
    {
        result = proc->static_desc->exec_cb(input_param, output_param,
                                            proc->static_desc->user_data);
    }

    return result;
}

/* 调用方必须持有节点锁:这里读写过程运行状态,并写入调用线程提供的 output */
void XrpcProcessNotifyCallRsp(TXrpcProcessObject *proc,
                              TXrpcMsgCallRspMem *rsp_msg,
                              TXrpcMsgParamList *output_param)
{
    if ((proc != NULL) && (proc->runtime_data != NULL) && (rsp_msg != NULL))
    {
        TXrpcMsgParamList *curr_output = proc->runtime_data->curr_output;

        if ((proc->runtime_data->call_step == kXrpcProcessCallStepWaitRsp)
            && (proc->runtime_data->process_seq == XrpcWireGetU32(rsp_msg->req_seq)))
        {
            Int32 errcode = XrpcWireGetI32(rsp_msg->errcode);

            proc->runtime_data->curr_errcode = errcode;
            /* 【B3】errcode<0(对端执行失败)时禁止复制 output,
             * 避免把垃圾 / 未填充的 payload 拷给上层调用方;
             * 同时把 curr_output 复位,避免上层拿到旧引用 */
            if ((errcode >= 0)
                && (curr_output != NULL)
                && (output_param != NULL))
            {
                UInt16 cpy_size = output_param->encode_buffer_used;

                if (curr_output->encode_buffer_size < cpy_size)
                {
                    /* 放不下时不留半截 TLV,否则项数与内容不一致 */
                    curr_output->encode_buffer_used = 0U;
                    curr_output->param_cnt = 0U;
                    proc->runtime_data->curr_errcode = kXrpcErrRxBufferOverflow;
                }
                else
                {
                    memcpy(curr_output->encode_buffer_addr,
                           output_param->encode_buffer_addr,
                           cpy_size);
                    curr_output->encode_buffer_used = cpy_size;
                    curr_output->param_cnt = output_param->param_cnt;
                }
            }
            else
            {
                /* 失败 / 无 output / 无 curr_output:不留旧引用 */
                if (curr_output != NULL)
                {
                    curr_output->encode_buffer_used = 0U;
                    curr_output->param_cnt = 0U;
                }
            }
            proc->runtime_data->call_step = kXrpcProcessCallStepNotify;
        }
    }
}


/* ============================================================
 *  定长整数 Get / Add 薄壳
 *  - Get:从 param_list 取 item,校验 type/len,然后按大端读出
 *  - Add:把 host 端原生字节按大端写入 item.value
 * ============================================================ */

#define XRPC_DEFINE_GET_SCALAR(NAME, TYPE, PARAM_TYPE, SIZE)                \
Int32 XrpcProcessParamGet##NAME(const TXrpcMsgParamList *param_list,        \
                                UInt8 param_id, TYPE *value)                \
{                                                                           \
    Int32 result;                                                           \
    TXrpcMsgParamItem item;                                                 \
                                                                            \
    if (value == NULL)                                                      \
    {                                                                       \
        return kXrpcErrInvalidParam;                                        \
    }                                                                       \
    result = L_FindItemById(param_list, param_id,                           \
                            (UInt8)(PARAM_TYPE), (UInt16)(SIZE), &item);    \
    if (result == kXrpcErrOk)                                               \
    {                                                                       \
        *value = (TYPE)L_BeRead((const UInt8 *)item.param_value,            \
                                (UInt16)(SIZE));                            \
    }                                                                       \
    return result;                                                          \
}

#define XRPC_DEFINE_ADD_SCALAR(NAME, TYPE, PARAM_TYPE, SIZE)                \
Int32 XrpcProcessParamAdd##NAME(TXrpcMsgParamList *param_list,             \
                                UInt8 param_id, TYPE value)                  \
{                                                                           \
    Int32 result;                                                           \
    TXrpcMsgParamItem item;                                                 \
    UInt8 be_buf[SIZE];                                                     \
                                                                            \
    L_BeWrite(be_buf, (UInt64)value, (UInt16)(SIZE));                        \
    item.param_id    = param_id;                                            \
    item.param_type  = (UInt8)(PARAM_TYPE);                                 \
    item.param_len   = (UInt16)(SIZE);                                      \
    item.param_value = be_buf;                                              \
    result = TXrpcMsgParamListAdd(param_list, &item);                       \
    return result;                                                          \
}


XRPC_DEFINE_GET_SCALAR(UInt8 , UInt8 ,  XRPC_PROCESS_PARAM_TYPE_UINT8 , 1)
XRPC_DEFINE_GET_SCALAR(Int8  , Int8  ,  XRPC_PROCESS_PARAM_TYPE_INT8  , 1)
XRPC_DEFINE_GET_SCALAR(UInt16, UInt16,  XRPC_PROCESS_PARAM_TYPE_UINT16, 2)
XRPC_DEFINE_GET_SCALAR(Int16 , Int16 ,  XRPC_PROCESS_PARAM_TYPE_INT16 , 2)
XRPC_DEFINE_GET_SCALAR(UInt32, UInt32,  XRPC_PROCESS_PARAM_TYPE_UINT32, 4)
XRPC_DEFINE_GET_SCALAR(Int32 , Int32 ,  XRPC_PROCESS_PARAM_TYPE_INT32 , 4)
XRPC_DEFINE_GET_SCALAR(UInt64, UInt64,  XRPC_PROCESS_PARAM_TYPE_UINT64, 8)
XRPC_DEFINE_GET_SCALAR(Int64 , Int64 ,  XRPC_PROCESS_PARAM_TYPE_INT64 , 8)

XRPC_DEFINE_ADD_SCALAR(UInt8 , UInt8 ,  XRPC_PROCESS_PARAM_TYPE_UINT8 , 1)
XRPC_DEFINE_ADD_SCALAR(Int8  , Int8  ,  XRPC_PROCESS_PARAM_TYPE_INT8  , 1)
XRPC_DEFINE_ADD_SCALAR(UInt16, UInt16,  XRPC_PROCESS_PARAM_TYPE_UINT16, 2)
XRPC_DEFINE_ADD_SCALAR(Int16 , Int16 ,  XRPC_PROCESS_PARAM_TYPE_INT16 , 2)
XRPC_DEFINE_ADD_SCALAR(UInt32, UInt32,  XRPC_PROCESS_PARAM_TYPE_UINT32, 4)
XRPC_DEFINE_ADD_SCALAR(Int32 , Int32 ,  XRPC_PROCESS_PARAM_TYPE_INT32 , 4)
XRPC_DEFINE_ADD_SCALAR(UInt64, UInt64,  XRPC_PROCESS_PARAM_TYPE_UINT64, 8)
XRPC_DEFINE_ADD_SCALAR(Int64 , Int64 ,  XRPC_PROCESS_PARAM_TYPE_INT64 , 8)


/* ============================================================
 *  BYTES / STRING 通用 Get / Add
 *  - BYTES 与 STRING 在 wire 上同构(都是 length-prefixed 二进制),
 *    仅类型码不同;此处 Get 函数允许 STRING 读成 BYTES(便于业务
 *    透传二进制负载),但 GetString 严格只接受 STRING。
 * ============================================================ */

Int32 XrpcProcessParamGetBytes(const TXrpcMsgParamList *param_list,
                               UInt8 param_id,
                               const char **value, UInt16 *len)
{
    Int32 result;
    TXrpcMsgParamItem item;

    if ((value == NULL) || (len == NULL))
    {
        return kXrpcErrInvalidParam;
    }
    result = TXrpcMsgParamListGetItemById((TXrpcMsgParamList *)param_list,
                                          (Int32)param_id, &item);
    if (result != kXrpcErrOk)
    {
        return result;
    }
    if ((item.param_type != XRPC_PROCESS_PARAM_TYPE_BYTES)
        && (item.param_type != XRPC_PROCESS_PARAM_TYPE_STRING))
    {
        return kXrpcErrInvalidParam;
    }
    *value = (const char *)item.param_value;
    *len   = item.param_len;
    return kXrpcErrOk;
}

Int32 XrpcProcessParamAddBytes(TXrpcMsgParamList *param_list,
                               UInt8 param_id,
                               const char *value, UInt16 len)
{
    TXrpcMsgParamItem item;

    item.param_id    = param_id;
    item.param_type  = XRPC_PROCESS_PARAM_TYPE_BYTES;
    item.param_len   = len;
    item.param_value = (void *)value;
    return TXrpcMsgParamListAdd(param_list, &item);
}

Int32 XrpcProcessParamGetString(const TXrpcMsgParamList *param_list,
                                UInt8 param_id,
                                const char **value, UInt16 *len)
{
    Int32 result;
    TXrpcMsgParamItem item;

    if ((value == NULL) || (len == NULL))
    {
        return kXrpcErrInvalidParam;
    }
    result = TXrpcMsgParamListGetItemById((TXrpcMsgParamList *)param_list,
                                          (Int32)param_id, &item);
    if (result != kXrpcErrOk)
    {
        return result;
    }
    /* STRING 不允许误读成 BYTES */
    if (item.param_type != XRPC_PROCESS_PARAM_TYPE_STRING)
    {
        return kXrpcErrInvalidParam;
    }
    *value = (const char *)item.param_value;
    *len   = item.param_len;
    return kXrpcErrOk;
}

Int32 XrpcProcessParamAddString(TXrpcMsgParamList *param_list,
                                UInt8 param_id,
                                const char *value, UInt16 len)
{
    TXrpcMsgParamItem item;

    item.param_id    = param_id;
    item.param_type  = XRPC_PROCESS_PARAM_TYPE_STRING;
    item.param_len   = len;
    item.param_value = (void *)value;
    return TXrpcMsgParamListAdd(param_list, &item);
}
