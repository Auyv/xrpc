/**
 * @addtogroup xrpc_msg_param_list
 * xrpc_msg_param_list
 * @{
 */
/**
 * @file        xrpc_msg_param_list.c
 * @brief       参数列表对象实现：基于 XrpcMsgParamItem 编解码对参数列表缓冲区进行 add/get/encode 操作
 * @note        缓冲区中每个参数项按如下顺序排列：
 *              UInt8 param_id | UInt8 param_type | UInt16 param_len(be) | param_len bytes
 * @copyright   Copyright (c) 2026 AuyvJpz
 */

#include "xrpc/xrpc_msg_param_list.h"
#include "xrpc/xrpc_common.h"
#include "dpt_decoder.h"
#include "dpt_encoder.h"

#include <string.h>

static Bool L_DecoderReadValuePtr(TDptDecoder *decoder, void **value, Int32 size)
{
    Bool result = False;

    if ((decoder != NULL) && (value != NULL) && (size >= 0)
        && (TDptDecoderReadAble(decoder) >= size))
    {
        *value = (void *)&decoder->buf[decoder->pos];
        decoder->pos += size;
        result = True;
    }

    return result;
}


/**
 * @brief   创建参数列表
 * @param *self 参数列表对象指针
 * @param param_cnt 参数个数。从数据类型限制最多 255
 * @param *buffer   参数列表数据缓冲区
 * @param buffer_size   参数列表数据缓冲区长度
 * @return = 0 成功，< 0 失败
 * @note: param_cnt = 0 时，encode_buffer_used = 0;
 *        param_cnt > 0 时，需要主动解析buffer内容，确定真实param_cnt和encode_buffer_used的值
 */
Int32 TXrpcMsgParamListCreate(TXrpcMsgParamList *self, UInt8 param_cnt, char *buffer, UInt16 buffer_size)
{
    Int32 result = kXrpcErrInvalidParam;

    if (self == NULL || buffer == NULL || buffer_size == 0)
    {
        return kXrpcErrInvalidParam;
    }

    /* 初始化成员 */
    self->encode_buffer_addr = buffer;
    self->encode_buffer_size = buffer_size;
    self->encode_buffer_used = 0;
    self->param_cnt = 0;

    if (param_cnt == 0)
    {
        /* param_cnt = 0 时，缓冲区为空 */
        result = kXrpcErrOk;
    }
    else
    {
        /* param_cnt > 0 时，主动解析 buffer 内容，确定真实 param_cnt 与 encode_buffer_used */
        TDptDecoder decoder;
        if (TDptDecoderCreate(&decoder, (const UInt8 *)buffer, (UInt32)buffer_size))
        {
            UInt8 actual_cnt = 0;
            Bool ok = True;
            Int32 pos = 0;

            while (ok && actual_cnt < param_cnt)
            {
                UInt8 param_id = 0;
                UInt8 param_type = 0;
                UInt16 param_len = 0;

                /* 项数还没到,数据却不够一个头部:声明的项数与内容不符 */
                if (TDptDecoderReadAble(&decoder) < kXrpcParamItemHeaderSize)
                {
                    ok = False;
                    break;
                }

                if (!TDptDecoderReadUInt8(&decoder, &param_id))
                {
                    ok = False;
                    break;
                }
                if (!TDptDecoderReadUInt8(&decoder, &param_type))
                {
                    ok = False;
                    break;
                }
                if (!TDptDecoderReadUInt16(&decoder, &param_len, True))
                {
                    ok = False;
                    break;
                }

                /* 检查 value 区段是否越界 */
                if ((Int32)param_len > TDptDecoderReadAble(&decoder))
                {
                    ok = False;
                    break;
                }

                /* 跳过 value 区段 */
                if (param_len > 0)
                {
                    void *dummy = NULL;
                    if (!L_DecoderReadValuePtr(&decoder, &dummy, (Int32)param_len))
                    {
                        ok = False;
                        break;
                    }
                }

                actual_cnt++;
            }

            pos = TDptDecoderReadPosition(&decoder);
            TDptDecoderDestroy(&decoder);

            if (ok)
            {
                self->param_cnt = actual_cnt;
                self->encode_buffer_used = (UInt16)pos;
                result = kXrpcErrOk;
            }
            else
            {
                /* 解析失败：保留已识别的部分结果 */
                self->param_cnt = actual_cnt;
                self->encode_buffer_used = (UInt16)pos;
                result = kXrpcErrBufferOverflow;
            }
        }
    }

    return result;
}


/**
 * @brief   析构函数，释放参数列表占用的资源
 * @param *self
 * @note: 仅重置内部状态，不释放外部传入的 buffer
 */
void TXrpcMsgParamListDestroy(TXrpcMsgParamList *self)
{
    if (self != NULL)
    {
        self->encode_buffer_addr = NULL;
        self->encode_buffer_size = 0;
        self->encode_buffer_used = 0;
        self->param_cnt = 0;
    }
}


/**
 * @brief   清空参数列表，重置参数列表状态
 * @param *self
 * @note: 执行 param_cnt = 0 ; encode_buffer_used = 0;
 */
void TXrpcMsgParamListClear(TXrpcMsgParamList *self)
{
    if (self != NULL)
    {
        self->encode_buffer_used = 0;
        self->param_cnt = 0;
    }
}


/**
 * @brief   获取参数列表中参数的个数
 * @param *self
 * @return self==NULL 时返回 0(与"count=0"语义等价,且不会与任何错误码混淆);
 *         有效时返回当前 param_cnt(0~255)
 */
UInt8 TXrpcMsgParamListCount(TXrpcMsgParamList *self)
{
    if (self == NULL)
    {
        return 0U;
    }
    return self->param_cnt;
}


/**
 * @brief   增加参数到参数列表并编码到参数列表数据缓冲区
 * @param *self
 * @param *param_data 参数数据结构指针，包含参数ID、参数类型、参数值等信息
 * @return = 0 成功，< 0 失败
 */
Int32 TXrpcMsgParamListAdd(TXrpcMsgParamList *self, const TXrpcMsgParamItem *param_data)
{
    Int32 result = kXrpcErrInvalidParam;

    if (self == NULL || param_data == NULL || self->encode_buffer_addr == NULL
        || ((param_data->param_value == NULL) && (param_data->param_len > 0)))
    {
        return kXrpcErrInvalidParam;
    }

    /* 参数个数已达上限 */
    if (self->param_cnt >= kXrpcParamListMaxCnt)
    {
        return kXrpcErrListFull;
    }

    /* 剩余空间不足 */
    {
        UInt32 need = (UInt32)kXrpcParamItemHeaderSize + (UInt32)param_data->param_len;
        if ((UInt32)self->encode_buffer_used + need > (UInt32)self->encode_buffer_size)
        {
            return kXrpcErrBufferOverflow;
        }
    }

    {
        TDptEncoder encoder;
        UInt16 remain = (UInt16)(self->encode_buffer_size - self->encode_buffer_used);
        UInt8 *cur = (UInt8 *)(self->encode_buffer_addr + self->encode_buffer_used);

        if (TDptEncoderCreate(&encoder, cur, (Int32)remain))
        {
            Bool ok = TDptEncoderWriteUInt8(&encoder, param_data->param_id)
                   && TDptEncoderWriteUInt8(&encoder, param_data->param_type)
                   && TDptEncoderWriteUInt16(&encoder, param_data->param_len, True)
                   && ((param_data->param_len == 0)
                       || TDptEncoderWriteMem(&encoder, param_data->param_value,
                                              (Int32)param_data->param_len));

            if (ok)
            {
                Int32 written = TDptEncoderWritePosition(&encoder);
                TDptEncoderDestroy(&encoder);

                self->encode_buffer_used = (UInt16)(self->encode_buffer_used + written);
                self->param_cnt++;
                result = kXrpcErrOk;
            }
            else
            {
                TDptEncoderDestroy(&encoder);
                result = kXrpcErrBufferOverflow;
            }
        }
        else
        {
            result = kXrpcErrBufferOverflow;
        }
    }

    return result;
}


/**
 * @brief   解析参数列表数据缓冲区内容，获取参数列表中所有参数的数据
 * @param *self
 * @param *param_data 参数数据结构指针，包含参数ID、参数类型、参数值等信息。调用前需要分配足够的内存空间以存储所有参数数据
 * @param *param_cnt 参数数据结构的个数。调用前需要设置为param_data指针指向的内存空间可以存储的最大参数个数。调用后返回实际解析出的参数个数
 * @return  = 0 成功，< 0 失败
 */
Int32 TXrpcMsgParamListGetAllItem(TXrpcMsgParamList *self, TXrpcMsgParamItem *param_data, UInt8 *param_cnt)
{
    Int32 result = kXrpcErrInvalidParam;

    if (self == NULL || param_data == NULL || param_cnt == NULL)
    {
        return kXrpcErrInvalidParam;
    }

    /* 缓冲区为空：直接返回 0 项 */
    if (self->encode_buffer_addr == NULL || self->encode_buffer_used == 0)
    {
        *param_cnt = 0;
        return kXrpcErrOk;
    }

    {
        UInt8 max_cnt = *param_cnt;
        UInt8 actual_cnt = 0;
        Bool ok = True;
        TDptDecoder decoder;

        if (TDptDecoderCreate(&decoder, (const UInt8 *)self->encode_buffer_addr, (UInt32)self->encode_buffer_used))
        {
            while (ok && actual_cnt < max_cnt)
            {
                UInt8 param_id = 0;
                UInt8 param_type = 0;
                UInt16 param_len = 0;

                if (TDptDecoderReadAble(&decoder) < kXrpcParamItemHeaderSize)
                {
                    break;
                }

                if (!TDptDecoderReadUInt8(&decoder, &param_id))
                {
                    ok = False;
                    break;
                }
                if (!TDptDecoderReadUInt8(&decoder, &param_type))
                {
                    ok = False;
                    break;
                }
                if (!TDptDecoderReadUInt16(&decoder, &param_len, True))
                {
                    ok = False;
                    break;
                }

                if ((Int32)param_len > TDptDecoderReadAble(&decoder))
                {
                    ok = False;
                    break;
                }

                param_data[actual_cnt].param_id = param_id;
                param_data[actual_cnt].param_type = param_type;
                param_data[actual_cnt].param_len = param_len;

                if (param_len > 0)
                {
                    void *ptr = NULL;
                    if (!L_DecoderReadValuePtr(&decoder, &ptr, (Int32)param_len))
                    {
                        ok = False;
                        break;
                    }
                    param_data[actual_cnt].param_value = ptr;
                }
                else
                {
                    param_data[actual_cnt].param_value = NULL;
                }

                actual_cnt++;
            }

            TDptDecoderDestroy(&decoder);

            *param_cnt = actual_cnt;
            result = ok ? kXrpcErrOk : kXrpcErrBufferOverflow;
        }
        else
        {
            result = kXrpcErrBufferOverflow;
        }
    }

    return result;
}


/**
 * @brief   根据参数ID获取参数列表中的参数数据
 * @param *self
 * @param param_id 需要获取的参数ID
 * @param *param_data 参数数据结构指针，包含参数ID、参数类型、参数值等信息。调用前需要分配足够的内存空间以存储参数数据
 * @return = 0 成功，< 0 失败
 * @note: param_data->param_value 指向缓冲区内部数据，不复制；调用方需保证 self 在使用期间不被修改
 */
Int32 TXrpcMsgParamListGetItemById(TXrpcMsgParamList *self, Int32 param_id, TXrpcMsgParamItem *param_data)
{
    Int32 result = kXrpcErrInvalidParam;

    if (self == NULL || param_data == NULL || self->encode_buffer_addr == NULL)
    {
        return kXrpcErrInvalidParam;
    }

    if (self->encode_buffer_used == 0)
    {
        return kXrpcErrItemNotFound;
    }

    {
        TDptDecoder decoder;
        if (TDptDecoderCreate(&decoder, (const UInt8 *)self->encode_buffer_addr, (UInt32)self->encode_buffer_used))
        {
            Bool found = False;
            Bool ok = True;

            while (ok)
            {
                UInt8 cur_id = 0;
                UInt8 param_type = 0;
                UInt16 param_len = 0;

                if (TDptDecoderReadAble(&decoder) < kXrpcParamItemHeaderSize)
                {
                    break;
                }

                if (!TDptDecoderReadUInt8(&decoder, &cur_id))
                {
                    ok = False;
                    break;
                }
                if (!TDptDecoderReadUInt8(&decoder, &param_type))
                {
                    ok = False;
                    break;
                }
                if (!TDptDecoderReadUInt16(&decoder, &param_len, True))
                {
                    ok = False;
                    break;
                }

                if ((Int32)cur_id == param_id)
                {
                    param_data->param_id = cur_id;
                    param_data->param_type = param_type;
                    param_data->param_len = param_len;

                    if (param_len > 0)
                    {
                        void *ptr = NULL;
                        if (TDptDecoderReadAble(&decoder) < (Int32)param_len
                            || !L_DecoderReadValuePtr(&decoder, &ptr, (Int32)param_len))
                        {
                            ok = False;
                            break;
                        }
                        param_data->param_value = ptr;
                    }
                    else
                    {
                        param_data->param_value = NULL;
                    }

                    found = True;
                    break;
                }
                else
                {
                    /* 非目标项，跳过 value */
                    if (param_len > 0)
                    {
                        void *dummy = NULL;
                        if (TDptDecoderReadAble(&decoder) < (Int32)param_len
                            || !L_DecoderReadValuePtr(&decoder, &dummy, (Int32)param_len))
                        {
                            ok = False;
                            break;
                        }
                    }
                }
            }

            TDptDecoderDestroy(&decoder);

            if (found)
            {
                result = kXrpcErrOk;
            }
            else if (ok)
            {
                result = kXrpcErrItemNotFound; /* 未找到 */
            }
            else
            {
                result = kXrpcErrBufferOverflow; /* 解析失败 */
            }
        }
        else
        {
            result = kXrpcErrBufferOverflow;
        }
    }

    return result;
}


/**
 * @brief   获取参数列表编码后的数据缓冲区指针和长度
 * @param *self
 * @param *len 参数列表编码后的数据长度 encode_buffer_used
 * @return 编码后的数据缓冲区指针
 */
char *TXrpcMsgParamListEncodeBuffer(TXrpcMsgParamList *self, Int32 *len)
{
    if (self == NULL || self->encode_buffer_addr == NULL)
    {
        if (len != NULL)
        {
            *len = 0;
        }
        return NULL;
    }

    if (len != NULL)
    {
        *len = (Int32)self->encode_buffer_used;
    }
    return self->encode_buffer_addr;
}


/**
 * @}
 */
