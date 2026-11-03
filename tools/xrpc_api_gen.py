#!/usr/bin/env python3
# Copyright (c) 2026 AuyvJpz
"""XRPC 远程调用与发布订阅代码生成工具。

从一份接口定义生成某个节点的代码：

    <module>_xrpc.h       类型、ID、接口声明。每次重新生成
    <module>_xrpc.c       编解码、调用封装、过程表、通知表。每次重新生成
    <module>_xrpc_impl.c  provider 与事件回调的实现框架。归使用者所有，
                          重新生成时不覆盖，只追加新增函数的骨架

    python xrpc_api_gen.py api.yaml --node mcu --out gen/mcu
    python xrpc_api_gen.py api.yaml --list

process_key 由本工具的规范文本计算（见 canonical_text）。要与已有设备互通，
在定义里写 key: 沿用现有的值。
"""

import argparse
import os
import re
import sys
import zlib

from xrpc_yaml import ConfigError, component_version, load_config, write_text

# type: (C 类型, 线上长度, 类型码宏, Add/Get 后缀)
SCALARS = {
    'u8': ('UInt8', 1, 'XRPC_PROCESS_PARAM_TYPE_UINT8', 'UInt8'),
    'i8': ('Int8', 1, 'XRPC_PROCESS_PARAM_TYPE_INT8', 'Int8'),
    'u16': ('UInt16', 2, 'XRPC_PROCESS_PARAM_TYPE_UINT16', 'UInt16'),
    'i16': ('Int16', 2, 'XRPC_PROCESS_PARAM_TYPE_INT16', 'Int16'),
    'u32': ('UInt32', 4, 'XRPC_PROCESS_PARAM_TYPE_UINT32', 'UInt32'),
    'i32': ('Int32', 4, 'XRPC_PROCESS_PARAM_TYPE_INT32', 'Int32'),
    'u64': ('UInt64', 8, 'XRPC_PROCESS_PARAM_TYPE_UINT64', 'UInt64'),
    'i64': ('Int64', 8, 'XRPC_PROCESS_PARAM_TYPE_INT64', 'Int64'),
}
BLOBS = {'bytes': 'Bytes', 'string': 'String'}
TYPE_CODE = {'u8': 0, 'i8': 1, 'u16': 2, 'i16': 3, 'u32': 4, 'i32': 5, 'u64': 6, 'i64': 7,
             'bytes': 0x10, 'string': 0x11}
MAX_LIST_ITEMS = 255
MAX_LIST_BYTES = 65535


def upper_snake(name):
    return re.sub(r'(?<=[a-z0-9])(?=[A-Z])', '_', name).upper()


def check_param_list(params, where):
    """规范化参数列表，返回 [{id, name, type, size}]"""
    result = []
    ids = set()
    names = set()
    if params is None:
        params = []
    if not isinstance(params, list):
        raise ConfigError('%s 应该是列表' % where)
    if len(params) > MAX_LIST_ITEMS:
        raise ConfigError('%s 超过 %d 项' % (where, MAX_LIST_ITEMS))
    for index, param in enumerate(params):
        tag = '%s[%d]' % (where, index)
        if not isinstance(param, dict):
            raise ConfigError('%s 应该是映射' % tag)
        pid = param.get('id', index + 1)
        name = param.get('name')
        ptype = param.get('type')
        if not isinstance(pid, int) or not (0 <= pid <= 255):
            raise ConfigError('%s：id 必须在 0～255' % tag)
        if pid in ids:
            raise ConfigError('%s：参数 id %d 重复' % (tag, pid))
        if not isinstance(name, str) or not re.fullmatch(r'[a-z_][a-z0-9_]*', name):
            raise ConfigError('%s：name 必填，只能用小写字母、数字、下划线' % tag)
        if name in names:
            raise ConfigError('%s：参数名 %s 重复' % (tag, name))
        if ptype not in TYPE_CODE:
            raise ConfigError('%s：type 只能是 %s' % (tag, '、'.join(TYPE_CODE)))
        if ptype in BLOBS:
            size = param.get('max_len')
            if not isinstance(size, int) or not (0 < size <= 65535):
                raise ConfigError('%s：%s 类型必须写 max_len（1～65535）' % (tag, ptype))
        else:
            size = SCALARS[ptype][1]
        ids.add(pid)
        names.add(name)
        result.append({'id': pid, 'name': name, 'type': ptype, 'size': size})
    if list_bytes(result) > MAX_LIST_BYTES:
        raise ConfigError('%s 编码后超过 %d 字节' % (where, MAX_LIST_BYTES))
    return result


def list_bytes(params):
    return sum(4 + param['size'] for param in params)


def canonical_params(params):
    return ','.join('%d:%d:%d' % (p['id'], TYPE_CODE[p['type']], p['size']) for p in params)


def canonical_text(kind, item):
    """计算 key 的规范文本。修改它会让所有未写 key: 的接口换 key，等于改接口版本"""
    if kind == 'process':
        return 'process|0x%08X|in=%s|out=%s' % (item['id'], canonical_params(item['input']),
                                               canonical_params(item['output']))
    return 'notify|0x%08X|params=%s' % (item['id'], canonical_params(item['params']))


def check_common(item, where, used_ids, kind):
    name = item.get('name')
    if not isinstance(name, str) or not re.fullmatch(r'[A-Z][A-Za-z0-9]*', name):
        raise ConfigError('%s：name 必填，用大驼峰，如 GetVersion' % where)
    uid = item.get('id')
    if not isinstance(uid, int) or isinstance(uid, bool) or not (0 < uid <= 0xFFFFFFFF):
        raise ConfigError('%s（%s）：id 必填，且不能是 0' % (where, name))
    if uid in used_ids:
        raise ConfigError('%s（%s）：%s id 0x%X 重复' % (where, name, kind, uid))
    used_ids.add(uid)


def fill_key(item, kind):
    item['canonical'] = canonical_text(kind, item)
    key = item.get('key')
    if key is None:
        item['key'] = zlib.crc32(item['canonical'].encode('ascii')) & 0xFFFFFFFF
        item['key_from'] = 'crc32'
    elif not isinstance(key, int) or not (0 <= key <= 0xFFFFFFFF):
        raise ConfigError('%s：key 必须是 32 位整数' % item['name'])
    else:
        item['key_from'] = 'spec'


def parse_spec(raw):
    if not isinstance(raw, dict):
        raise ConfigError('定义文件顶层应该是映射')
    unknown = set(raw) - {'module', 'processes', 'notifies', 'nodes'}
    if unknown:
        raise ConfigError('不认识的顶层项：%s' % '、'.join(sorted(unknown)))
    module = raw.get('module')
    if not isinstance(module, str) or not re.fullmatch(r'[a-z][a-z0-9_]*', module):
        raise ConfigError('module 必填，只能用小写字母、数字、下划线')

    used = set()
    processes = {}
    for index, proc in enumerate(raw.get('processes') or []):
        where = 'processes[%d]' % index
        check_common(proc, where, used, '过程')
        proc['input'] = check_param_list(proc.get('input'), '%s.input' % proc['name'])
        proc['output'] = check_param_list(proc.get('output'), '%s.output' % proc['name'])
        proc.setdefault('timeout_ms', 0)
        fill_key(proc, 'process')
        processes[proc['name']] = proc

    used = set()
    notifies = {}
    for index, notify in enumerate(raw.get('notifies') or []):
        where = 'notifies[%d]' % index
        check_common(notify, where, used, '通知')
        notify['params'] = check_param_list(notify.get('params'), '%s.params' % notify['name'])
        notify.setdefault('need_ack', False)
        notify.setdefault('timeout_ms', 500)
        notify.setdefault('retry', 2)
        fill_key(notify, 'notify')
        notifies[notify['name']] = notify

    nodes = {}
    for index, node in enumerate(raw.get('nodes') or []):
        name = node.get('name')
        if not isinstance(name, str):
            raise ConfigError('nodes[%d]：name 必填' % index)
        for proc_name in (node.get('provides') or []) + (node.get('calls') or []):
            if proc_name not in processes:
                raise ConfigError('节点 %s：没有定义过程 %s' % (name, proc_name))
        for key in ('publishes', 'subscribes'):
            for entry in node.get(key) or []:
                if entry.get('notify') not in notifies:
                    raise ConfigError('节点 %s：没有定义通知 %s' % (name, entry.get('notify')))
        for entry in node.get('publishes') or []:
            links = entry.get('links', [0])
            acks = entry.get('ack_nodes') or []
            if not links:
                raise ConfigError('节点 %s：%s 的 links 至少要有 1 条' % (name, entry['notify']))
            entry['links'] = links
            entry['ack_nodes'] = acks
        for entry in node.get('subscribes') or []:
            if 'publisher' not in entry:
                raise ConfigError('节点 %s：订阅 %s 必须写 publisher（0 表示任意）'
                                  % (name, entry['notify']))
            entry.setdefault('send_ack', True)
        for entry in node.get('bindings') or []:
            if entry.get('process') not in processes:
                raise ConfigError('节点 %s：绑定了未定义的过程 %s' % (name, entry.get('process')))
        relay = node.get('relay')
        if relay is not None:
            if (not isinstance(relay, dict)
                    or int(relay.get('routes', 8)) <= 0 or int(relay.get('calls', 8)) <= 0):
                raise ConfigError('节点 %s：relay 的 routes、calls 必须大于 0' % name)
            for entry in relay.get('forwards') or []:
                if entry.get('notify') not in notifies:
                    raise ConfigError('节点 %s：转发了未定义的通知 %s' % (name, entry.get('notify')))
                if not entry.get('links'):
                    raise ConfigError('节点 %s：%s 的转发链路至少 1 条' % (name, entry['notify']))
        nodes[name] = node
    return {'module': module, 'processes': processes, 'notifies': notifies, 'nodes': nodes}


# ---------- C 代码片段 ----------

def struct_decl(type_name, params):
    fields = []
    for param in params:
        if param['type'] in BLOBS:
            fields.append('    char %s[%d];' % (param['name'], param['size']))
            fields.append('    UInt16 %s_len;' % param['name'])
        else:
            fields.append('    %s %s;' % (SCALARS[param['type']][0], param['name']))
    return ('typedef struct %sTag %s;\nstruct %sTag\n{\n%s\n};\n\n'
            % (type_name, type_name, type_name, '\n'.join(fields)))


def encode_fn(fn_name, type_name, params):
    steps = []
    for param in params:
        if param['type'] in BLOBS:
            add = 'XrpcProcessParamAdd%s(list, %dU, value->%s, value->%s_len)' % (
                BLOBS[param['type']], param['id'], param['name'], param['name'])
        else:
            add = 'XrpcProcessParamAdd%s(list, %dU, value->%s)' % (
                SCALARS[param['type']][3], param['id'], param['name'])
        steps.append('    if (result == kXrpcErrOk)\n    {\n        result = %s;\n    }\n' % add)
    return ('XRPC_GEN_UNUSED static Int32 %s(TXrpcMsgParamList *list, const %s *value)\n{\n'
            '    Int32 result = kXrpcErrOk;\n\n%s\n    return result;\n}\n\n'
            % (fn_name, type_name, ''.join(steps)))


def decode_fn(fn_name, type_name, params):
    steps = []
    locals_ = ''
    for param in params:
        if param['type'] in BLOBS:
            locals_ = '    const char *data = NULL;\n    UInt16 len = 0U;\n'
            steps.append('''    if (result == kXrpcErrOk)
    {
        result = XrpcProcessParamGet%s(list, %dU, &data, &len);
    }
    if (result == kXrpcErrOk)
    {
        if (len > sizeof(value->%s))
        {
            result = kXrpcErrRxBufferOverflow;
        }
        else
        {
            if (len > 0U)
            {
                memcpy(value->%s, data, len);
            }
            value->%s_len = len;
        }
    }
''' % (BLOBS[param['type']], param['id'], param['name'], param['name'], param['name']))
        else:
            steps.append('    if (result == kXrpcErrOk)\n    {\n'
                         '        result = XrpcProcessParamGet%s(list, %dU, &value->%s);\n    }\n'
                         % (SCALARS[param['type']][3], param['id'], param['name']))
    return ('XRPC_GEN_UNUSED static Int32 %s(const TXrpcMsgParamList *list, %s *value)\n{\n'
            '    Int32 result = kXrpcErrOk;\n%s\n%s\n    return result;\n}\n\n'
            % (fn_name, type_name, locals_, ''.join(steps)))


class Generator:
    def __init__(self, spec, node_name):
        self.spec = spec
        self.module = spec['module']
        self.prefix = ''.join(part.capitalize() for part in self.module.split('_'))
        self.upper = self.module.upper()
        self.node = spec['nodes'][node_name]
        self.provides = list(self.node.get('provides') or [])
        calls = self.node.get('calls')
        if calls is None:
            calls = [name for name in spec['processes'] if name not in self.provides]
        self.calls = [name for name in calls if name not in self.provides]
        self.publishes = list(self.node.get('publishes') or [])
        self.subscribes = list(self.node.get('subscribes') or [])
        self.bindings = list(self.node.get('bindings') or [])
        self.relay = self.node.get('relay')

    def t(self, name, suffix):
        return 'T%s%s%s' % (self.prefix, name, suffix)

    def pid(self, name):
        return '%s_PROCESS_%s_ID' % (self.upper, upper_snake(name))

    def nid(self, name):
        return '%s_NOTIFY_%s_ID' % (self.upper, upper_snake(name))

    def provide_sig(self, proc):
        args = []
        if proc['input']:
            args.append('const %s *input' % self.t(proc['name'], 'In'))
        if proc['output']:
            args.append('%s *output' % self.t(proc['name'], 'Out'))
        return 'Int32 %sProvide%s(%s)' % (self.prefix, proc['name'],
                                          ', '.join(args) if args else 'void')

    def on_sig(self, notify):
        args = ['UInt32 publisher', 'UInt32 seq']
        if notify['params']:
            args.append('const %s *params' % self.t(notify['name'], 'Params'))
        return 'void %sOn%s(%s)' % (self.prefix, notify['name'], ', '.join(args))

    def call_sig(self, proc):
        args = ['TXrpcNode *node']
        if proc['input']:
            args.append('const %s *input' % self.t(proc['name'], 'In'))
        if proc['output']:
            args.append('%s *output' % self.t(proc['name'], 'Out'))
        args += ['UInt32 timeout_ms', 'UInt8 retry']
        return 'Int32 %sProcess%s(%s)' % (self.prefix, proc['name'], ', '.join(args))

    def async_sig(self, proc):
        args = ['TXrpcNode *node']
        if proc['input']:
            args.append('const %s *input' % self.t(proc['name'], 'In'))
        args += ['%s *call' % self.t(proc['name'], 'Call'),
                 '%s done_cb' % self.t(proc['name'], 'DoneCb'),
                 'void *user_data', 'UInt32 timeout_ms', 'UInt8 retry']
        return 'Int32 %sProcess%sAsync(%s)' % (self.prefix, proc['name'], ', '.join(args))

    def publish_sig(self, notify):
        args = ['TXrpcNode *node']
        if notify['params']:
            args.append('const %s *params' % self.t(notify['name'], 'Params'))
        args += ['const TXrpcNotifyOpt *opt', 'TXrpcNotifyDoneCb done_cb', 'void *user_data']
        return 'Int32 %sPublish%s(%s)' % (self.prefix, notify['name'], ', '.join(args))

    # ---------- 头文件 ----------

    def header(self):
        procs = self.spec['processes']
        notifies = self.spec['notifies']
        parts = ['''/**
 * @file        %s_xrpc.h
 * @brief       由 xrpc_api_gen.py 生成，不要手改，改接口定义后重新生成。
 */

#ifndef _%s_XRPC_H_
#define _%s_XRPC_H_

#include "xrpc/xrpc_node.h"

#ifdef __cplusplus
extern "C"
{
#endif

''' % (self.module, self.upper, self.upper)]
        for proc in procs.values():
            parts.append('#define %s  (0x%08XU)\n' % (self.pid(proc['name']), proc['id']))
            parts.append('#define %s_PROCESS_%s_KEY  (0x%08XU)\n'
                         % (self.upper, upper_snake(proc['name']), proc['key']))
        for notify in notifies.values():
            parts.append('#define %s  (0x%08XU)\n' % (self.nid(notify['name']), notify['id']))
            parts.append('#define %s_NOTIFY_%s_KEY  (0x%08XU)\n'
                         % (self.upper, upper_snake(notify['name']), notify['key']))
        parts.append('\n')
        for proc in procs.values():
            if proc['input']:
                parts.append(struct_decl(self.t(proc['name'], 'In'), proc['input']))
            if proc['output']:
                parts.append(struct_decl(self.t(proc['name'], 'Out'), proc['output']))
        for notify in notifies.values():
            if notify['params']:
                parts.append(struct_decl(self.t(notify['name'], 'Params'), notify['params']))

        parts.append('''/**
 * 注册本节点的过程表、绑定表和通知表。在 RunOnce 启动之前调用。
 * @param seq_seed 通知序号初值，取与启动相关的值，否则重启后序号可能和上次撞上
 * @note  运行时数据是静态的，一个进程只能注册一个节点。
 */
Int32 %sXrpcRegister(TXrpcNode *node, UInt32 seq_seed);

''' % self.prefix)

        for name in self.calls:
            proc = procs[name]
            parts.append('/** 同步调用 %s。等待应答，不能在轮询线程里调用。timeout_ms=0 用默认值，retry 是超时后重发的次数，不是幂等的函数填 0 */\n'
                         % name)
            parts.append(self.call_sig(proc) + ';\n\n')
            out_arg = ('const %s *output, ' % self.t(name, 'Out')) if proc['output'] else ''
            parts.append('typedef void (*%s)(Int32 result, %svoid *user_data);\n\n'
                         % (self.t(name, 'DoneCb'), out_arg))
            parts.append(('/** 异步调用的上下文，由调用方提供，必须保持有效直到完成回调返回 */\n'
                          'typedef struct %sTag %s;\nstruct %sTag\n{\n'
                          '    %s done_cb;\n    void *user_data;\n')
                         % ((self.t(name, 'Call'),) * 3 + (self.t(name, 'DoneCb'),)))
            if proc['input']:
                parts.append('    TXrpcMsgParamList input;\n    char input_buff[%d];\n'
                             % list_bytes(proc['input']))
            if proc['output']:
                parts.append('    TXrpcMsgParamList output;\n    char output_buff[%d];\n'
                             % list_bytes(proc['output']))
            parts.append('};\n\n')
            parts.append('/** 异步调用 %s。发出后立即返回，要求节点启用了调用槽。retry 含义同同步调用 */\n' % name)
            parts.append(self.async_sig(proc) + ';\n\n')

        for entry in self.publishes:
            notify = notifies[entry['notify']]
            parts.append('/** 发布 %s。非阻塞，params 随后可以改。opt 为 NULL 用定义里的默认值 */\n'
                         % notify['name'])
            parts.append(self.publish_sig(notify) + ';\n\n')

        if self.provides or self.subscribes:
            parts.append('/* 以下由 %s_xrpc_impl.c 实现，都在轮询线程执行，不能阻塞 */\n\n' % self.module)
        for name in self.provides:
            parts.append(self.provide_sig(procs[name]) + ';\n')
        for name in sorted({entry['notify'] for entry in self.subscribes}):
            parts.append(self.on_sig(notifies[name]) + ';\n')

        parts.append('''
#ifdef __cplusplus
}  ///< extern "C"
#endif

#endif  ///< _%s_XRPC_H_
''' % self.upper)
        return ''.join(parts)

    # ---------- 源文件 ----------

    def codecs(self):
        parts = []
        items = []
        for proc in self.spec['processes'].values():
            items.append((self.t(proc['name'], 'In'), proc['input'], proc['name'] + 'In'))
            items.append((self.t(proc['name'], 'Out'), proc['output'], proc['name'] + 'Out'))
        for notify in self.spec['notifies'].values():
            items.append((self.t(notify['name'], 'Params'), notify['params'],
                          notify['name'] + 'Params'))
        for type_name, params, tag in items:
            if params:
                parts.append(encode_fn('L_Encode%s' % tag, type_name, params))
                parts.append(decode_fn('L_Decode%s' % tag, type_name, params))
        return ''.join(parts)

    def exec_fn(self, proc):
        name = proc['name']
        decl = ''
        pre = ''
        post = ''
        args = []
        if proc['input']:
            decl += '    %s in_value;\n' % self.t(name, 'In')
            pre += ('    memset(&in_value, 0, sizeof(in_value));\n'
                    '    result = L_Decode%sIn(input_param, &in_value);\n' % name)
            args.append('&in_value')
        else:
            pre += '    (void)input_param;\n'
        if proc['output']:
            decl += '    %s out_value;\n' % self.t(name, 'Out')
            pre += '    memset(&out_value, 0, sizeof(out_value));\n'
            args.append('&out_value')
            post = '''    /* 本地调用可以不要输出，这时 output_param 为 NULL */
    if ((result >= 0) && (output_param != NULL))
    {
        Int32 encoded = L_Encode%sOut(output_param, &out_value);

        result = (encoded < 0) ? encoded : result;
    }
''' % name
        else:
            pre += '    (void)output_param;\n'
        return '''static Int32 L_Exec%s(const TXrpcMsgParamList *input_param,
                     TXrpcMsgParamList *output_param, void *user_data)
{
    Int32 result = kXrpcErrOk;
%s
    (void)user_data;
%s    if (result == kXrpcErrOk)
    {
        result = %sProvide%s(%s);
    }
%s
    return result;
}

''' % (name, decl, pre, self.prefix, name, ', '.join(args), post)

    def call_fn(self, proc):
        name = proc['name']
        guard = ['node != NULL']
        decl = ''
        steps = ''
        in_arg = 'NULL'
        out_arg = 'NULL'
        if proc['input']:
            guard.append('input != NULL')
            decl += ('        TXrpcMsgParamList in_list;\n        char in_buff[%d];\n'
                     % list_bytes(proc['input']))
            steps += ('        result = TXrpcMsgParamListCreate(&in_list, 0U, in_buff, sizeof(in_buff));\n'
                      '        if (result == kXrpcErrOk)\n        {\n'
                      '            result = L_Encode%sIn(&in_list, input);\n        }\n' % name)
            in_arg = '&in_list'
        if proc['output']:
            guard.append('output != NULL')
            decl += ('        TXrpcMsgParamList out_list;\n        char out_buff[%d];\n'
                     % list_bytes(proc['output']))
            steps += ('        if (result == kXrpcErrOk)\n        {\n'
                      '            result = TXrpcMsgParamListCreate(&out_list, 0U, out_buff, sizeof(out_buff));\n'
                      '        }\n')
            out_arg = '&out_list'
        decode = ''
        if proc['output']:
            decode = '''        if (result >= 0)
        {
            Int32 decoded = L_Decode%sOut(&out_list, output);

            result = (decoded < 0) ? decoded : result;
        }
''' % name
        guard_text = ' && '.join('(%s)' % item for item in guard)
        return '''%s
{
    Int32 result = kXrpcErrInvalidParam;

    if (%s)
    {
%s
        result = kXrpcErrOk;
%s        if (result == kXrpcErrOk)
        {
            TXrpcCallOpt opt;

            opt.timeout_ms = (timeout_ms != 0U) ? timeout_ms : %dU;
            opt.retry = retry;
            result = TXrpcNodeProcessExecOpt(node, %s, %s, %s, &opt);
        }
%s    }

    return result;
}

''' % (self.call_sig(proc), guard_text, decl, steps, proc['timeout_ms'], self.pid(name), in_arg,
       out_arg, decode)

    def async_fn(self, proc):
        name = proc['name']
        call_t = self.t(name, 'Call')
        guard = ['node != NULL', 'call != NULL', 'done_cb != NULL']
        decl = ''
        steps = ''
        in_arg = 'NULL'
        out_arg = 'NULL'
        if proc['input']:
            guard.append('input != NULL')
            steps += ('        result = TXrpcMsgParamListCreate(&call->input, 0U, call->input_buff,\n'
                      '                                         sizeof(call->input_buff));\n'
                      '        if (result == kXrpcErrOk)\n        {\n'
                      '            result = L_Encode%sIn(&call->input, input);\n        }\n' % name)
            in_arg = '&call->input'
        if proc['output']:
            steps += ('        if (result == kXrpcErrOk)\n        {\n'
                      '            result = TXrpcMsgParamListCreate(&call->output, 0U, call->output_buff,\n'
                      '                                             sizeof(call->output_buff));\n'
                      '        }\n')
            out_arg = '&call->output'
            done_body = '''    %s value;
    Int32 code = result;

    (void)node;
    (void)process_id;
    memset(&value, 0, sizeof(value));
    if (code >= 0)
    {
        Int32 decoded = L_Decode%sOut(output, &value);

        code = (decoded < 0) ? decoded : code;
    }
    call->done_cb(code, &value, call->user_data);
''' % (self.t(name, 'Out'), name)
        else:
            done_body = '''    (void)node;
    (void)process_id;
    (void)output;
    call->done_cb(result, call->user_data);
'''
        return '''static void L_On%sDone(TXrpcNode *node, UInt32 process_id, Int32 result,
                     TXrpcMsgParamList *output, void *user_data)
{
    %s *call = (%s *)user_data;

%s}

%s
{
    Int32 result = kXrpcErrInvalidParam;

    if (%s)
    {
%s
        result = kXrpcErrOk;
        call->done_cb = done_cb;
        call->user_data = user_data;
%s        if (result == kXrpcErrOk)
        {
            TXrpcCallOpt opt;

            opt.timeout_ms = (timeout_ms != 0U) ? timeout_ms : %dU;
            opt.retry = retry;
            result = TXrpcNodeCallAsyncOpt(node, %s, %s, %s, &opt, L_On%sDone, call);
        }
    }

    return result;
}

''' % (name, call_t, call_t, done_body, self.async_sig(proc),
       ' && '.join('(%s)' % item for item in guard), decl, steps, proc['timeout_ms'],
       self.pid(name), in_arg, out_arg, name)

    def publish_fn(self, notify):
        name = notify['name']
        default = '''    TXrpcNotifyOpt default_opt;
    Int32 result = kXrpcErrInvalidParam;

    default_opt.need_ack = %s;
    default_opt.timeout_ms = %dU;
    default_opt.retry = %dU;
''' % ('True' if notify['need_ack'] else 'False', notify['timeout_ms'], notify['retry'])
        size = list_bytes(notify['params'])
        if notify['params']:
            body = '''    if ((node != NULL) && (params != NULL))
    {
        TXrpcMsgParamList list;
        char buff[%d];

        result = TXrpcMsgParamListCreate(&list, 0U, buff, sizeof(buff));
        if (result == kXrpcErrOk)
        {
            result = L_Encode%sParams(&list, params);
        }
        if (result == kXrpcErrOk)
        {
            result = XrpcNotifyPublish(node, %s, &list,
                                       (opt != NULL) ? opt : &default_opt, done_cb, user_data);
        }
    }
''' % (size, name, self.nid(name))
        else:
            body = '''    if (node != NULL)
    {
        result = XrpcNotifyPublish(node, %s, NULL,
                                   (opt != NULL) ? opt : &default_opt, done_cb, user_data);
    }
''' % self.nid(name)
        return '%s\n{\n%s\n%s\n    return result;\n}\n\n' % (self.publish_sig(notify), default, body)

    def sub_fn(self, notify):
        name = notify['name']
        if notify['params']:
            body = '''    %s value;

    (void)node;
    (void)notify_id;
    (void)user_data;
    memset(&value, 0, sizeof(value));
    if (L_Decode%sParams(params, &value) == kXrpcErrOk)
    {
        %sOn%s(publisher, seq, &value);
    }
''' % (self.t(name, 'Params'), name, self.prefix, name)
        else:
            body = '''    (void)node;
    (void)notify_id;
    (void)params;
    (void)user_data;
    %sOn%s(publisher, seq);
''' % (self.prefix, name)
        return '''static void L_Notify%s(TXrpcNode *node, UInt32 notify_id, UInt32 publisher, UInt32 seq,
                     const TXrpcMsgParamList *params, void *user_data)
{
%s}

''' % (name, body)

    def tables(self):
        procs = self.spec['processes']
        notifies = self.spec['notifies']
        parts = []
        names = self.provides + self.calls
        if names:
            parts.append('#define PROCESS_COUNT  (%dU)\n\n' % len(names))
            parts.append('static const TXrpcProcessStaticDesc s_process_descs[PROCESS_COUNT] =\n{\n')
            for name in names:
                exec_cb = 'L_Exec%s' % name if name in self.provides else 'NULL'
                parts.append('    {%s, 0x%08XU, "%s", %s, NULL},\n'
                             % (self.pid(name), procs[name]['key'], name, exec_cb))
            parts.append('};\n\nstatic TXrpcProcessRuntimeData s_process_runtimes[PROCESS_COUNT];\n')
            parts.append('static TXrpcProcessObject s_process_objects[PROCESS_COUNT];\n')
            parts.append('static TXrpcProcessObject *s_process_table[PROCESS_COUNT];\n\n')
        if self.bindings:
            parts.append('static const TXrpcProcessBinding s_bindings[] =\n{\n')
            for entry in self.bindings:
                parts.append('    {%s, %dU, 0x%08XU},\n' % (self.pid(entry['process']),
                                                            entry.get('link', 0),
                                                            entry.get('provider', 0)))
            parts.append('};\n\n')
        if self.relay is not None:
            parts.append('static TXrpcRelayRoute s_relay_routes[%d];\n' % int(self.relay.get('routes', 8)))
            parts.append('static TXrpcRelayCall s_relay_calls[%d];\n\n' % int(self.relay.get('calls', 8)))
            forwards = self.relay.get('forwards') or []
            if forwards:
                for index, entry in enumerate(forwards):
                    parts.append('static const UInt16 s_forward_links_%d[] = {%s};\n'
                                 % (index, ', '.join('%dU' % link for link in entry['links'])))
                parts.append('static TXrpcNotifyForwardRuntime s_forward_runtimes[%d];\n'
                             % len(forwards))
                parts.append('static TXrpcNotifyAckRecord s_ack_records[%d];\n'
                             % max(1, int(self.relay.get('ack_records', 8))))
                parts.append('\nstatic const TXrpcNotifyForward s_forwards[] =\n{\n')
                for index, entry in enumerate(forwards):
                    parts.append('    {%s, %dU, s_forward_links_%d, &s_forward_runtimes[%d]},\n'
                                 % (self.nid(entry['notify']), len(entry['links']), index, index))
                parts.append('};\n\n')
        if self.publishes:
            parts.append('#define PUB_COUNT  (%dU)\n\n' % len(self.publishes))
            for index, entry in enumerate(self.publishes):
                parts.append('static UInt8 s_slot_%d[%d];\n'
                             % (index, max(1, list_bytes(notifies[entry['notify']]['params']))))
                parts.append('static const UInt16 s_pub_links_%d[] = {%s};\n'
                             % (index, ', '.join('%dU' % link for link in entry['links'])))
                if entry['ack_nodes']:
                    parts.append('static const UInt32 s_pub_acks_%d[] = {%s};\n'
                                 % (index, ', '.join('0x%08XU' % node for node in entry['ack_nodes'])))
                    parts.append('static UInt32 s_pub_ack_work_%d[XRPC_NOTIFY_ACK_WORK_WORDS(%dU)];\n'
                                 % (index, len(entry['ack_nodes'])))
            parts.append('\nstatic const TXrpcNotifyPubDesc s_pub_descs[PUB_COUNT] =\n{\n')
            for index, entry in enumerate(self.publishes):
                notify = notifies[entry['notify']]
                acks = 's_pub_acks_%d' % index if entry['ack_nodes'] else 'NULL'
                work = 's_pub_ack_work_%d' % index if entry['ack_nodes'] else 'NULL'
                parts.append('    {%s, 0x%08XU, %dU, s_pub_links_%d, %dU, %s, %s, s_slot_%d, sizeof(s_slot_%d)},\n'
                             % (self.nid(notify['name']), notify['key'], len(entry['links']), index,
                                len(entry['ack_nodes']), acks, work, index, index))
            parts.append('};\n\nstatic TXrpcNotifyPubRuntime s_pub_runtimes[PUB_COUNT];\n')
            parts.append('static TXrpcNotifyPubObject s_pub_objects[PUB_COUNT];\n')
            parts.append('static TXrpcNotifyPubObject *s_pub_table[PUB_COUNT];\n\n')
        if self.subscribes:
            parts.append('#define SUB_COUNT  (%dU)\n\n' % len(self.subscribes))
            parts.append('static const TXrpcNotifySubDesc s_sub_descs[SUB_COUNT] =\n{\n')
            for entry in self.subscribes:
                notify = notifies[entry['notify']]
                parts.append('    {%s, 0x%08XU, 0x%08XU, %s, L_Notify%s, NULL},\n'
                             % (self.nid(notify['name']), notify['key'], entry['publisher'],
                                'True' if entry['send_ack'] else 'False', notify['name']))
            parts.append('};\n\nstatic TXrpcNotifySubRuntime s_sub_runtimes[SUB_COUNT];\n')
            parts.append('static TXrpcNotifySubObject s_sub_objects[SUB_COUNT];\n')
            parts.append('static TXrpcNotifySubObject *s_sub_table[SUB_COUNT];\n\n')
        return ''.join(parts)

    def register_fn(self):
        names = self.provides + self.calls
        steps = []
        locals_ = '    Int32 result = kXrpcErrOk;\n    UInt32 i = 0U;\n'
        if names:
            locals_ += '    TXrpcProcessTable table;\n'
            steps.append('''    for (i = 0U; i < PROCESS_COUNT; i++)
    {
        s_process_objects[i].static_desc = &s_process_descs[i];
        s_process_objects[i].runtime_data = &s_process_runtimes[i];
        s_process_table[i] = &s_process_objects[i];
    }
    table.process_table = s_process_table;
    table.count = PROCESS_COUNT;
    result = TXrpcNodeSetProcessTable(node, &table);
''')
        if self.bindings:
            steps.append('''    if (result == kXrpcErrOk)
    {
        result = TXrpcNodeSetProcessBindings(node, s_bindings,
                                             sizeof(s_bindings) / sizeof(s_bindings[0]));
    }
''')
        if self.relay is not None:
            locals_ += '    TXrpcRelayConfig relay;\n'
            steps.append('''    if (result == kXrpcErrOk)
    {
        memset(&relay, 0, sizeof(relay));
        relay.routes = s_relay_routes;
        relay.route_count = sizeof(s_relay_routes) / sizeof(s_relay_routes[0]);
        relay.calls = s_relay_calls;
        relay.call_count = sizeof(s_relay_calls) / sizeof(s_relay_calls[0]);
        relay.knock_timeout_ms = %dU;
        relay.call_timeout_ms = %dU;
%s        result = TXrpcNodeSetRelay(node, &relay);
    }
''' % (int(self.relay.get('knock_timeout_ms', 0)), int(self.relay.get('call_timeout_ms', 0)),
       '''        relay.forwards = s_forwards;
        relay.forward_count = sizeof(s_forwards) / sizeof(s_forwards[0]);
        relay.ack_records = s_ack_records;
        relay.ack_record_count = sizeof(s_ack_records) / sizeof(s_ack_records[0]);
        relay.notify_timeout_ms = %dU;
''' % int(self.relay.get('notify_timeout_ms', 0)) if self.relay.get('forwards') else ''))
        if self.publishes:
            steps.append('''    for (i = 0U; i < PUB_COUNT; i++)
    {
        s_pub_objects[i].desc = &s_pub_descs[i];
        s_pub_objects[i].runtime = &s_pub_runtimes[i];
        s_pub_table[i] = &s_pub_objects[i];
    }
    if (result == kXrpcErrOk)
    {
        result = TXrpcNodeSetNotifyPubs(node, s_pub_table, PUB_COUNT, seq_seed);
    }
''')
        if self.subscribes:
            steps.append('''    for (i = 0U; i < SUB_COUNT; i++)
    {
        s_sub_objects[i].desc = &s_sub_descs[i];
        s_sub_objects[i].runtime = &s_sub_runtimes[i];
        s_sub_table[i] = &s_sub_objects[i];
    }
    if (result == kXrpcErrOk)
    {
        result = TXrpcNodeSetNotifySubs(node, s_sub_table, SUB_COUNT);
    }
''')
        return '''Int32 %sXrpcRegister(TXrpcNode *node, UInt32 seq_seed)
{
%s
    (void)node;
    (void)seq_seed;
    (void)i;
%s
    return result;
}
''' % (self.prefix, locals_, ''.join(steps))

    def source(self):
        procs = self.spec['processes']
        notifies = self.spec['notifies']
        parts = ['''/**
 * @file        %s_xrpc.c
 * @brief       由 xrpc_api_gen.py 生成，不要手改，改接口定义后重新生成。
 */

#include "%s_xrpc.h"

#include <string.h>

#if defined(__GNUC__)
#define XRPC_GEN_UNUSED __attribute__((unused))
#else
#define XRPC_GEN_UNUSED
#endif

''' % (self.module, self.module), self.codecs()]
        for name in self.provides:
            parts.append(self.exec_fn(procs[name]))
        for name in self.calls:
            parts.append(self.call_fn(procs[name]))
            parts.append(self.async_fn(procs[name]))
        for entry in self.publishes:
            parts.append(self.publish_fn(notifies[entry['notify']]))
        seen = set()
        for entry in self.subscribes:
            if entry['notify'] not in seen:
                seen.add(entry['notify'])
                parts.append(self.sub_fn(notifies[entry['notify']]))
        parts.append(self.tables())
        parts.append(self.register_fn())
        return ''.join(parts)

    # ---------- 实现框架 ----------

    def impl_items(self):
        """[(函数名, 骨架)]"""
        procs = self.spec['processes']
        notifies = self.spec['notifies']
        items = []
        for name in self.provides:
            proc = procs[name]
            unused = ''
            if proc['input']:
                unused += '    (void)input;\n'
            if proc['output']:
                unused += '    (void)output;\n'
            summary = proc.get('brief')
            note = '/** %s */\n' % summary if summary else ''
            items.append(('%sProvide%s' % (self.prefix, name),
                          '%s%s\n{\n    /* TODO: 实现 %s。返回 >= 0 成功，< 0 错误码 */\n%s\n'
                          '    return kXrpcErrNotSupported;\n}\n'
                          % (note, self.provide_sig(proc), name, unused)))
        for name in sorted({entry['notify'] for entry in self.subscribes}):
            notify = notifies[name]
            unused = '    (void)publisher;\n    (void)seq;\n'
            if notify['params']:
                unused += '    (void)params;\n'
            items.append(('%sOn%s' % (self.prefix, name),
                          '%s\n{\n    /* TODO: 处理事件 %s。耗时操作转到业务任务 */\n%s}\n'
                          % (self.on_sig(notify), name, unused)))
        return items

    def impl_new(self):
        head = '''/**
 * @file        %s_xrpc_impl.c
 * @brief       过程与事件回调的实现。这个文件归你所有，重新生成不会覆盖，
 *              只会在末尾追加新增函数的骨架。
 * @note        这里的函数都在轮询线程执行：不能阻塞，不能发起同步调用。
 */

#include "%s_xrpc.h"

''' % (self.module, self.module)
        return head + '\n'.join(body for _, body in self.impl_items())


def write_impl(gen, path):
    """返回 (新建?, 追加的函数, 定义里已经没有的函数)"""
    items = gen.impl_items()
    if not os.path.exists(path):
        write_text(path, gen.impl_new())
        return True, [name for name, _ in items], []
    with open(path, encoding='utf-8') as stream:
        text = stream.read()
    missing = []
    for name, body in items:
        if not re.search(r'\b%s\s*\(' % re.escape(name), text):
            missing.append(name)
            text = text.rstrip('\n') + '\n\n' + body
    if missing:
        write_text(path, text)
    names = {name for name, _ in items}
    existing = re.findall(r'^(?:Int32|void)\s+(%s(?:Provide|On)[A-Za-z0-9]+)\s*\(' % gen.prefix,
                          text, re.M)
    return False, missing, [name for name in existing if name not in names]


def generate_node(spec, node, out_dir):
    """生成一个节点的代码，返回给人看的结果行。命令行和图形界面共用"""
    if node not in spec['nodes']:
        raise ConfigError('没有节点 %s，可选：%s' % (node, '、'.join(spec['nodes'])))
    gen = Generator(spec, node)
    base = os.path.join(out_dir, spec['module'] + '_xrpc')
    write_text(base + '.h', gen.header())
    write_text(base + '.c', gen.source())
    created, added, stale = write_impl(gen, base + '_impl.c')
    lines = ['生成 %s.h' % base, '生成 %s.c' % base,
             '%s %s_impl.c' % ('新建' if created else '保留', base)]
    lines += ['  追加骨架 %s' % name for name in ([] if created else added)]
    lines += ['  定义里已经没有 %s，可以删掉' % name for name in stale]
    return lines


def summary_lines(spec):
    """列出接口、节点和 key"""
    lines = ['过程：']
    for proc in spec['processes'].values():
        lines.append('  0x%08X %-20s key=0x%08X（%s）'
                     % (proc['id'], proc['name'], proc['key'], proc['key_from']))
    lines.append('通知：')
    for notify in spec['notifies'].values():
        lines.append('  0x%08X %-20s key=0x%08X（%s）'
                     % (notify['id'], notify['name'], notify['key'], notify['key_from']))
    lines.append('节点：%s' % '、'.join(spec['nodes']))
    return lines


def main():
    parser = argparse.ArgumentParser(description='XRPC 远程调用与发布订阅代码生成工具')
    parser.add_argument('--version', action='version', version=component_version())
    parser.add_argument('spec', help='接口定义文件（.yaml 或 .json）')
    parser.add_argument('--node', help='生成哪个节点的代码')
    parser.add_argument('--out', help='输出目录')
    parser.add_argument('--list', action='store_true', help='列出接口、节点和 key')
    args = parser.parse_args()
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8')

    try:
        spec = parse_spec(load_config(args.spec))
        if args.list:
            print('\n'.join(summary_lines(spec)))
            return 0
        if (args.node is None) or (args.out is None):
            raise ConfigError('要指定 --node 和 --out，或者用 --list')
        print('\n'.join('  ' + line for line in generate_node(spec, args.node, args.out)))
    except ConfigError as error:
        print('错误：%s' % error, file=sys.stderr)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())
