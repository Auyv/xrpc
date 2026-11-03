/**
 * @file        dpt_encoder.h
 * @brief       向字节缓冲按指定字节序写入整数和数据块，供参数列表编码使用。
 * @copyright   Copyright (c) 2026 AuyvJpz
 */

#ifndef _DPT_ENCODER_H_
#define _DPT_ENCODER_H_

#include "xrpc/xrpc_types.h"
#include <string.h>

/* 函数都是 static，每个包含它的 .c 各有一份拷贝 */

typedef struct TDptEncoderTag
{
    Byte *buf;
    Int32 size;
    Int32 pos;
} TDptEncoder;

static Bool TDptEncoderCreate(TDptEncoder *self, Byte *buf, Int32 size)
{
    Bool result = False;

    if ((self != NULL) && (buf != NULL) && (size >= 0))
    {
        self->buf = buf;
        self->size = size;
        self->pos = 0;
        result = True;
    }

    return result;
}

static void TDptEncoderDestroy(TDptEncoder *self)
{
    (void)self;
}

static Bool TDptEncoderWriteMem(TDptEncoder *self, const void *data, Int32 size)
{
    Bool result = False;

    if ((self != NULL) && (data != NULL) && (size >= 0)
        && ((self->pos + size) <= self->size))
    {
        memcpy(&self->buf[self->pos], data, (UInt32)size);
        self->pos += size;
        result = True;
    }

    return result;
}

static Bool TDptEncoderWriteUInt8(TDptEncoder *self, UInt8 value)
{
    return TDptEncoderWriteMem(self, &value, sizeof(value));
}

static Bool TDptEncoderWriteUInt16(TDptEncoder *self, UInt16 value, Bool big_endian)
{
    Byte data[2];

    if (big_endian)
    {
        data[0] = (Byte)(value >> 8);
        data[1] = (Byte)value;
    }
    else
    {
        data[0] = (Byte)value;
        data[1] = (Byte)(value >> 8);
    }

    return TDptEncoderWriteMem(self, data, sizeof(data));
}

static Int32 TDptEncoderWritePosition(TDptEncoder *self)
{
    Int32 result = 0;

    if (self != NULL)
    {
        result = self->pos;
    }

    return result;
}

#endif
