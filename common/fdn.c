/**
 * @file        fdn.c
 * @brief       FullDataNotify 功能实现
 * @copyright   Copyright (c) 2026 AuyvJpz
 */

#include "xrpc/fdn.h"

#include "xrpc_port.h"

#include <string.h>


/** 默认连续错误上限:连续解码错误累计跳过该字节数后,上报一次并重新计数 */
#define FDN_DEFAULT_MAX_ERROR_SKIP  (32)


/**
 * @brief 消费缓存区前 consume_size 个字节(左移剩余数据)。
 * @param self         FDN 实例
 * @param consume_size 待消费的字节数;若 >= m_recv_size,直接清空缓存。
 */
static void L_TFdnConsumeData(TFdnData *self, Int32 consume_size)
{
    Int32 remaining = self->m_recv_size - consume_size;

    if ((remaining > 0) && (consume_size > 0))
    {
        memmove(self->m_buff_start, self->m_buff_start + consume_size,
                (size_t)remaining);
        self->m_recv_size = remaining;
    }
    else
    {
        self->m_recv_size = 0;
    }
}


/**
 * @brief 喂入一整段数据,反复调用解码器直到数据不完整或缓存清空。
 * @param self            FDN 实例
 * @param data            待追加的数据
 * @param data_size       数据字节数
 * @param user_data_farme 透传给解码器/通知回调
 * @return 实际拷贝到内部缓存的字节数(0 表示缓存区已满,未消费任何字节)
 */
static Int32 L_TFdnFeedData(TFdnData *self, const char *data, Int32 data_size,
                         void *user_data_farme)
{
    Int32 copy_size = 0;
    Int32 decode_result = 0;

    /* 先把能装的都装进去,再反复解析 */
    copy_size = self->m_buff_size - self->m_recv_size;
    if (data_size < copy_size)
    {
        copy_size = data_size;
    }

    if (copy_size > 0)
    {
        memcpy(&self->m_buff_start[self->m_recv_size], data,
                (size_t)copy_size);
        self->m_recv_size += copy_size;
    }

    /* 在循环内反复调用解码器,直到数据不完整或缓存清空 */
    while (self->m_recv_size > 0)
    {
        decode_result = self->m_decode_method(self->m_buff_start,
                                                self->m_recv_size,
                                                user_data_farme);

        if (decode_result > 0)
        {
            /* 成功:通知回调,左移已消费数据 */
            self->m_notify_callback(self->m_buff_start, decode_result,
                                    user_data_farme);
            L_TFdnConsumeData(self, decode_result);
            self->m_error_count = 0;
        }
        else if (decode_result == 0)
        {
            /* 数据不完整, 等待下次补料 */

            /* 如果此时已经没有缓存空间,则表示永远不可能解析成功,
             * 丢弃当前已缓存的全部字节并 XRPC_LOG_E 上报,
             * 便于上层排查链路层是否在喂入过短/异常分包 */
            if (self->m_recv_size >= self->m_buff_size)
            {
                XRPC_LOG_E("Node(0x00000000) Fdn buff full but frame incomplete, drop %d bytes (buff=%d)",
                       (Int32)self->m_recv_size, (Int32)self->m_buff_size);
                self->m_recv_size = 0;
                self->m_error_count = 0;
            }
            break;
        }
        else
        {
            /* 解码错误:丢 1 字节继续。每个错误都消费字节,不会死循环;
             * 达到上限只上报并重新计数,不能清缓存,噪声后面可能就是有效帧 */
            self->m_error_count++;
            L_TFdnConsumeData(self, 1);
            if ((self->m_max_error_skip > 0)
                && (self->m_error_count >= self->m_max_error_skip))
            {
                XRPC_LOG_W("Node(0x00000000) Fdn skipped %d noise bytes",
                       (Int32)self->m_error_count);
                self->m_error_count = 0;
            }
        }
    }

    return copy_size;
}


Int32 TFdnCreate(TFdnData *self,
                 char *buff,
                 Int32 buff_size,
                 TFdnDecodeMethod decode_method,
                 TFdnNotifyCallback notify_callback)
{
    Int32 result = 0;

    if ((self == NULL) || (buff == NULL) || (buff_size <= 0)
        || (decode_method == NULL) || (notify_callback == NULL))
    {
        result = (Int32)kFdnErrCodeParam;
    }
    else
    {
        self->m_buff_start = buff;
        self->m_buff_size = buff_size;
        self->m_recv_size = 0;
        self->m_error_count = 0;
        self->m_max_error_skip = FDN_DEFAULT_MAX_ERROR_SKIP;
        self->m_decode_method = decode_method;
        self->m_notify_callback = notify_callback;
    }

    return result;
}


void TFdnDestroy(TFdnData *self)
{
    if (self != NULL)
    {
        /* 不释放外部传入的 m_buff_start,只复位自身状态 */
        self->m_recv_size = 0;
        self->m_error_count = 0;
        self->m_decode_method = NULL;
        self->m_notify_callback = NULL;
    }
}


void TFdnReset(TFdnData *self)
{
    if (self != NULL)
    {
        self->m_recv_size = 0;
        self->m_error_count = 0;
    }
}


Int32 TFdnRecvData(TFdnData *self,
                   const char *data,
                   Int32 size,
                   void *user_data_farme)
{
    Int32 result = 0;
    Int32 consumed = 0;
    Int32 once = 0;

    if ((self == NULL) || (data == NULL) || (size <= 0))
    {
        result = (Int32)kFdnErrCodeParam;
    }
    else
    {
        /* 分批喂入:每次从 data[consumed] 开始,剩余 size-consumed 字节。
         * 缓存区满时 L_TFdnFeedData 返回 0,直接 break 避免外层死循环。 */
        while (consumed < size)
        {
            once = L_TFdnFeedData(self, data + consumed, size - consumed,
                               user_data_farme);
            if (once <= 0)
            {
                break;
            }
            consumed += once;
        }
        result = consumed;
    }

    return result;
}


/**
 * @brief 读取当前连续错误计数。
 * @param self FDN 实例
 * @return 当前 m_error_count;self 为 NULL 时返回 0
 */
UInt16 TFdnErrorCount(TFdnData *self)
{
    UInt16 result = 0;

    if (self != NULL)
    {
        result = self->m_error_count;
    }

    return result;
}


/**
 * @brief 读取当前连续错误上限。
 * @param self FDN 实例
 * @return 当前 m_max_error_skip;self 为 NULL 时返回 0
 */
UInt16 TFdnMaxErrorSkip(TFdnData *self)
{
    UInt16 result = 0;

    if (self != NULL)
    {
        result = self->m_max_error_skip;
    }

    return result;
}


/**
 * @brief 设置连续错误上限。
 * @param self            FDN 实例
 * @param max_error_skip  新的连续错误上限;0 表示禁用自动清空
 */
void TFdnSetMaxErrorSkip(TFdnData *self, UInt16 max_error_skip)
{
    if (self != NULL)
    {
        self->m_max_error_skip = max_error_skip;
    }
}


Int32 TFdnBuffSize(TFdnData *self)
{
    Int32 result = 0;

    if (self != 0 && self->m_notify_callback != NULL)
        result = self->m_buff_size;

    return result;
}
