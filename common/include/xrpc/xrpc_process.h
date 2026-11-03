/**
 * @file        xrpc_process.h
 * @brief       过程对象、过程表，以及按类型读写参数的接口。
 * @copyright   Copyright (c) 2026 AuyvJpz
 */

#ifndef _XRPC_PROCESS_H_
#define _XRPC_PROCESS_H_

#include "xrpc/xrpc_types.h"
#include "xrpc/xrpc_msg_param_list.h"
#include "xrpc/xrpc_common.h"

#ifdef __cplusplus
extern "C" {
#endif
#define XRPC_PROCESS_PARAM_TYPE_UINT8   (0U)
#define XRPC_PROCESS_PARAM_TYPE_INT8    (1U)
#define XRPC_PROCESS_PARAM_TYPE_UINT16  (2U)
#define XRPC_PROCESS_PARAM_TYPE_INT16   (3U)
#define XRPC_PROCESS_PARAM_TYPE_UINT32  (4U)
#define XRPC_PROCESS_PARAM_TYPE_INT32   (5U)
#define XRPC_PROCESS_PARAM_TYPE_UINT64  (6U)
#define XRPC_PROCESS_PARAM_TYPE_INT64   (7U)
#define XRPC_PROCESS_PARAM_TYPE_BYTES   (0x10U)
#define XRPC_PROCESS_PARAM_TYPE_STRING  (0x11U)

typedef struct TXrpcProcessObjectTag TXrpcProcessObject;

typedef struct TXrpcProcessParamDescTag
{
    UInt8  param_id;
    UInt8  param_type;
    UInt16 param_len;
} TXrpcProcessParamDesc;

typedef Int32 (*TXrpcProcessExecCb)(const TXrpcMsgParamList *input_param,
                                    TXrpcMsgParamList *output_param,
                                    void *user_data);

typedef struct TXrpcProcessStaticDescTag
{
    UInt32              process_id;
    UInt32              process_key;
    const char         *process_name;
    TXrpcProcessExecCb  exec_cb;
    void               *user_data;
} TXrpcProcessStaticDesc;

typedef enum TXrpcProcessCallStepTag
{
    kXrpcProcessCallStepInvalid   = 0,
    kXrpcProcessCallStepDetecting = 1,
    kXrpcProcessCallStepIdle      = 2,
    kXrpcProcessCallStepWaitRsp   = 3,
    kXrpcProcessCallStepNotify    = 4,
} TXrpcProcessCallStep;

typedef struct TXrpcProcessRuntimeDataTag
{
    UInt32               provider_node_id;
    UInt32               process_seq;
    TXrpcProcessCallStep call_step;
    UInt32               knock_left_cnt;
    UInt32               last_knock_tick;
    UInt32               knock_cursor;      ///< 慢速 Knock 下一次从哪条链路开始
    TXrpcMsgParamList   *curr_output;
    Int32                curr_errcode;
} TXrpcProcessRuntimeData;

struct TXrpcProcessObjectTag
{
    const TXrpcProcessStaticDesc *static_desc;
    TXrpcProcessRuntimeData      *runtime_data;
};

typedef struct TXrpcProcessTableTag TXrpcProcessTable;
struct TXrpcProcessTableTag
{
    TXrpcProcessObject **process_table;
    UInt32               count;
};

Int32 XrpcProcessExec(TXrpcProcessObject *proc,
                      const TXrpcMsgParamList *input_param,
                      TXrpcMsgParamList *output_param);

void XrpcProcessNotifyCallRsp(TXrpcProcessObject *proc,
                              TXrpcMsgCallRspMem *rsp_msg,
                              TXrpcMsgParamList *output_param);

/* ------------------------------------------------------------
 *  参数定长整数字节序:与 wire 一致,统一 big-endian
 *  (对端应按相同规则解析;改动需同步两端)
 * ------------------------------------------------------------ */
#define XRPC_PROCESS_PARAM_BE   (True)

/* ============================================================
 *  参数 Get / Add 通用 API
 *  - Get 函数在类型/长度不匹配时返回 kXrpcErrInvalidParam;
 *  - Add 函数内部走 TXrpcMsgParamListAdd,缓冲区不足返回 kXrpcErrBufferOverflow,
 *    参数个数达上限返回 kXrpcErrListFull。
 *  - 定长整数(UInt8/16/32/64 与 Int8/16/32/64)按 XRPC_PROCESS_PARAM_BE
 *    统一字节序(默认 big-endian)。
 *  - BYTES 与 STRING 均为 length-prefixed 二进制,
 *    BYTES param_len 由调用方提供;STRING 与 BYTES 同构,
 *    仅类型码不同。
 *  - 本头内的 API 实现位于 framework\xrpc\common\xrpc_process.c,
 *    工具不负责生成实现。
 * ============================================================ */
Int32 XrpcProcessParamGetUInt8 (const TXrpcMsgParamList *param_list, UInt8 param_id, UInt8  *value);
Int32 XrpcProcessParamAddUInt8 (TXrpcMsgParamList *param_list, UInt8 param_id, UInt8  value);
Int32 XrpcProcessParamGetInt8  (const TXrpcMsgParamList *param_list, UInt8 param_id, Int8   *value);
Int32 XrpcProcessParamAddInt8  (TXrpcMsgParamList *param_list, UInt8 param_id, Int8   value);
Int32 XrpcProcessParamGetUInt16(const TXrpcMsgParamList *param_list, UInt8 param_id, UInt16 *value);
Int32 XrpcProcessParamAddUInt16(TXrpcMsgParamList *param_list, UInt8 param_id, UInt16 value);
Int32 XrpcProcessParamGetInt16 (const TXrpcMsgParamList *param_list, UInt8 param_id, Int16  *value);
Int32 XrpcProcessParamAddInt16 (TXrpcMsgParamList *param_list, UInt8 param_id, Int16  value);
Int32 XrpcProcessParamGetUInt32(const TXrpcMsgParamList *param_list, UInt8 param_id, UInt32 *value);
Int32 XrpcProcessParamAddUInt32(TXrpcMsgParamList *param_list, UInt8 param_id, UInt32 value);
Int32 XrpcProcessParamGetInt32 (const TXrpcMsgParamList *param_list, UInt8 param_id, Int32  *value);
Int32 XrpcProcessParamAddInt32 (TXrpcMsgParamList *param_list, UInt8 param_id, Int32  value);
Int32 XrpcProcessParamGetUInt64(const TXrpcMsgParamList *param_list, UInt8 param_id, UInt64 *value);
Int32 XrpcProcessParamAddUInt64(TXrpcMsgParamList *param_list, UInt8 param_id, UInt64 value);
Int32 XrpcProcessParamGetInt64 (const TXrpcMsgParamList *param_list, UInt8 param_id, Int64  *value);
Int32 XrpcProcessParamAddInt64 (TXrpcMsgParamList *param_list, UInt8 param_id, Int64  value);
Int32 XrpcProcessParamGetBytes (const TXrpcMsgParamList *param_list, UInt8 param_id, const char **value, UInt16 *len);
Int32 XrpcProcessParamAddBytes (TXrpcMsgParamList *param_list, UInt8 param_id, const char *value, UInt16 len);
Int32 XrpcProcessParamGetString(const TXrpcMsgParamList *param_list, UInt8 param_id, const char **value, UInt16 *len);
Int32 XrpcProcessParamAddString(TXrpcMsgParamList *param_list, UInt8 param_id, const char *value, UInt16 len);

#ifdef __cplusplus
}
#endif

#endif
