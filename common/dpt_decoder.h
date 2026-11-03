/**
 * @file        dpt_decoder.h
 * @brief       从字节缓冲按指定字节序读取整数和数据块，供参数列表解析使用。
 * @copyright   Copyright (c) 2026 AuyvJpz
 */

#ifndef _DPT_DECODER_H_
#define _DPT_DECODER_H_

#include "xrpc/xrpc_types.h"
#include <string.h>

/* 函数都是 static，每个包含它的 .c 各有一份拷贝 */

typedef struct TDptDecoderTag
{
    const Byte *buf;
    Int32 size;
    Int32 pos;
} TDptDecoder;

static Bool TDptDecoderCreate(TDptDecoder *self, const Byte *buf, UInt32 size)
{
    Bool result = False;

    if ((self != NULL) && (buf != NULL))
    {
        self->buf = buf;
        self->size = (Int32)size;
        self->pos = 0;
        result = True;
    }

    return result;
}

static void TDptDecoderDestroy(TDptDecoder *self)
{
    (void)self;
}

static Int32 TDptDecoderReadAble(TDptDecoder *self)
{
    Int32 result = 0;

    if ((self != NULL) && (self->size >= self->pos))
    {
        result = self->size - self->pos;
    }

    return result;
}

static Bool TDptDecoderReadMem(TDptDecoder *self, void *data, Int32 size)
{
    Bool result = False;

    if ((self != NULL) && (data != NULL) && (size >= 0)
        && (TDptDecoderReadAble(self) >= size))
    {
        memcpy(data, &self->buf[self->pos], (UInt32)size);
        self->pos += size;
        result = True;
    }

    return result;
}

static Bool TDptDecoderReadUInt8(TDptDecoder *self, UInt8 *value)
{
    return TDptDecoderReadMem(self, value, sizeof(*value));
}

static Bool TDptDecoderReadUInt16(TDptDecoder *self, UInt16 *value, Bool big_endian)
{
    Bool result = False;
    Byte data[2];

    if ((value != NULL) && TDptDecoderReadMem(self, data, sizeof(data)))
    {
        if (big_endian)
        {
            *value = (UInt16)(((UInt16)data[0] << 8) | data[1]);
        }
        else
        {
            *value = (UInt16)(((UInt16)data[1] << 8) | data[0]);
        }
        result = True;
    }

    return result;
}

static Int32 TDptDecoderReadPosition(TDptDecoder *self)
{
    Int32 result = 0;

    if (self != NULL)
    {
        result = self->pos;
    }

    return result;
}

#endif
