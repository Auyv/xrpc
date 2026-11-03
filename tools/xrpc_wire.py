# Copyright (c) 2026 AuyvJpz
"""协议 v2 的线上编解码，诊断工具共用，只依赖标准库。

规则见 docs/XRPC协议规范.md。这是 C 核心库之外的第二份实现，由
tools/tests/test_wire.py 用黄金字节和规范里的向量核对。
"""

import struct

from xrpc_api_gen import check_param_list, fill_key
from xrpc_yaml import ConfigError, load_config

TAG = b'XRPC'
HEAD_SIZE = 28
CRC_OFFSET = 8
FRAME_LEN_MIN = 20
VERSION = 0x0200
TTL_DEFAULT = 255
BROADCAST = 0xFFFFFFFF
FLAG_NEED_ACK = 0x01

HELLO_REQ = 0x0102
HELLO_RSP = 0xF102
KNOCK_REQ = 0x0101
KNOCK_RSP = 0xF101
RETIRED_ONEWAY = 0x0110
CALL_REQ = 0x0111
CALL_RSP = 0xF111
NOTIFY_EVENT = 0x0120
NOTIFY_ACK = 0xF120
ADDRESSED = 0x0130

TYPE_NAMES = {
    HELLO_REQ: 'HelloReq', HELLO_RSP: 'HelloRsp', KNOCK_REQ: 'KnockReq', KNOCK_RSP: 'KnockRsp',
    CALL_REQ: 'CallReq', CALL_RSP: 'CallRsp', NOTIFY_EVENT: 'NotifyEvent',
    NOTIFY_ACK: 'NotifyAck', ADDRESSED: 'Addressed',
}

ERROR_NAMES = {
    -1: 'InvalidParam', -2: 'BufferOverflow', -3: 'ListFull', -4: 'ItemNotFound', -5: 'Timeout',
    -6: 'LinkSendFailed', -7: 'TxBufferOverflow', -8: 'ProcessNotFound', -9: 'ProcessStatus',
    -10: 'PeerNodeUnknow', -11: 'RxBufferOverflow', -12: 'ProviderUnknow', -13: 'PeerStatusInvalid',
    -14: 'NotSupported', -15: 'FrameTooLarge', -16: 'NoFreeSlot', -17: 'TtlExpired',
}

# 类型码：(名字, struct 格式)
SCALAR_CODES = {
    0x00: ('u8', '>B'), 0x01: ('i8', '>b'), 0x02: ('u16', '>H'), 0x03: ('i16', '>h'),
    0x04: ('u32', '>I'), 0x05: ('i32', '>i'), 0x06: ('u64', '>Q'), 0x07: ('i64', '>q'),
}
BYTES_CODE = 0x10
STRING_CODE = 0x11
NAME_TO_CODE = {name: code for code, (name, _) in SCALAR_CODES.items()}
NAME_TO_CODE.update({'bytes': BYTES_CODE, 'string': STRING_CODE})


class WireError(Exception):
    """载荷和协议不符"""


def crc_ccitt_false(data):
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def crc_fixed(value):
    """本机联调用：所有帧都是同一个 CRC 值"""
    return lambda data: value


def pack_frame(node_id, msg_type, payload=b'', crc=crc_ccitt_false, user_tag=0, seq=1,
               ttl=TTL_DEFAULT, version=VERSION):
    body = struct.pack('>IHHIIBBH', node_id, version, msg_type, user_tag, seq, ttl, 0, 0) + payload
    return TAG + struct.pack('>HH', crc(body), len(body)) + body


class Frame:
    """一帧的帧头字段和载荷"""

    def __init__(self, raw):
        self.raw = bytes(raw)
        self.crc, self.frame_len = struct.unpack('>HH', self.raw[4:8])
        (self.node_id, self.version, self.type, self.user_tag, self.seq, self.ttl, self.flags,
         _) = struct.unpack('>IHHIIBBH', self.raw[8:HEAD_SIZE])
        self.payload = self.raw[HEAD_SIZE:]


class Skipped:
    """流里被跳过的字节，解码工具要显示出来"""

    def __init__(self, data, reason):
        self.data = bytes(data)
        self.reason = reason


class FrameReader:
    """流式拼帧。crc 为 None 时不校验"""

    def __init__(self, crc=crc_ccitt_false):
        self.crc = crc
        self.buffer = bytearray()

    def _resync(self, items, reason, start):
        """从 start 找下一个 tag，之前的字节算跳过"""
        found = self.buffer.find(TAG, start)
        end = found if found >= 0 else max(len(self.buffer) - (len(TAG) - 1), start)
        items.append(Skipped(self.buffer[:end], reason))
        del self.buffer[:end]

    def feed(self, data):
        """送入字节，返回这次解析出的 Frame 和 Skipped"""
        self.buffer.extend(data)
        items = []
        while len(self.buffer) >= len(TAG):
            if not self.buffer.startswith(TAG):
                self._resync(items, '不是帧头', 0)
                continue
            if len(self.buffer) < HEAD_SIZE:
                break
            frame_len = struct.unpack('>H', self.buffer[6:8])[0]
            if frame_len < FRAME_LEN_MIN:
                self._resync(items, 'frame_len 太小', 1)
                continue
            size = CRC_OFFSET + frame_len
            if len(self.buffer) < size:
                break
            raw = self.buffer[:size]
            expect = struct.unpack('>H', raw[4:6])[0]
            if (self.crc is not None) and (self.crc(bytes(raw[CRC_OFFSET:])) != expect):
                self._resync(items, 'CRC 错误', 1)
                continue
            items.append(Frame(raw))
            del self.buffer[:size]
        return items

    def flush(self):
        """流结束时，还没拼成帧的字节算跳过"""
        items = [Skipped(self.buffer, '流结束时帧不完整')] if self.buffer else []
        self.buffer = bytearray()
        return items


def encode_param(param_id, code, value):
    if code in SCALAR_CODES:
        data = struct.pack(SCALAR_CODES[code][1], value)
    elif code == STRING_CODE:
        data = value.encode('utf-8') if isinstance(value, str) else bytes(value)
    elif code == BYTES_CODE:
        data = bytes(value)
    else:
        raise WireError('未知参数类型 0x%02X' % code)
    return struct.pack('>BBH', param_id, code, len(data)) + data


def decode_params(data, count):
    """返回 [(id, 类型码, 值)]"""
    params = []
    offset = 0
    for _ in range(count):
        if len(data) < offset + 4:
            raise WireError('参数个数和内容不符')
        param_id, code, size = struct.unpack_from('>BBH', data, offset)
        offset += 4
        if len(data) < offset + size:
            raise WireError('参数 %d 超出载荷' % param_id)
        value = bytes(data[offset:offset + size])
        offset += size
        if code in SCALAR_CODES:
            fmt = SCALAR_CODES[code][1]
            if struct.calcsize(fmt) != size:
                raise WireError('参数 %d 的长度和类型不符' % param_id)
            value = struct.unpack(fmt, value)[0]
        elif code == STRING_CODE:
            value = value.decode('utf-8', 'replace')
        elif code != BYTES_CODE:
            raise WireError('参数 %d 的类型 0x%02X 未知' % (param_id, code))
        params.append((param_id, code, value))
    return params


def knock_req(process_id, key):
    return struct.pack('>II', process_id, key)


def call_req(process_id, key, params=()):
    return struct.pack('>IIB', process_id, key, len(params)) + b''.join(params)


def notify_ack(notify_id, origin, publisher, seq, result=0):
    return struct.pack('>IIIIi', notify_id, origin, publisher, seq, result)


def _need(payload, size, name):
    if len(payload) < size:
        raise WireError('%s 载荷不足 %d 字节' % (name, size))


def decode_payload(msg_type, payload):
    """按消息类型解开载荷，返回字段字典。未知类型抛 WireError"""
    fields = {}
    if msg_type in (HELLO_REQ, HELLO_RSP):
        if len(payload) >= 4:
            fields['max_recv_frame'] = struct.unpack_from('>I', payload)[0]
    elif msg_type == KNOCK_REQ:
        _need(payload, 8, 'KnockReq')
        fields['process_id'], fields['key'] = struct.unpack_from('>II', payload)
    elif msg_type == KNOCK_RSP:
        _need(payload, 12, 'KnockRsp')
        fields['req_seq'], fields['process_id'], fields['errcode'] = \
            struct.unpack_from('>IIi', payload)
    elif msg_type == CALL_REQ:
        _need(payload, 9, 'CallReq')
        fields['process_id'], fields['key'], count = struct.unpack_from('>IIB', payload)
        fields['params'] = decode_params(payload[9:], count)
    elif msg_type == CALL_RSP:
        _need(payload, 13, 'CallRsp')
        fields['req_seq'], fields['process_id'], fields['errcode'], count = \
            struct.unpack_from('>IIiB', payload)
        fields['params'] = decode_params(payload[13:], count)
    elif msg_type == NOTIFY_EVENT:
        _need(payload, 18, 'NotifyEvent')
        fields['notify_id'], fields['key'], fields['origin'], fields['seq'], flags, count = \
            struct.unpack_from('>IIIIBB', payload)
        fields['need_ack'] = bool(flags & FLAG_NEED_ACK)
        fields['params'] = decode_params(payload[18:], count)
    elif msg_type == NOTIFY_ACK:
        _need(payload, 20, 'NotifyAck')
        fields['notify_id'], fields['origin'], fields['publisher'], fields['seq'], \
            fields['result'] = struct.unpack_from('>IIIIi', payload)
    elif msg_type == ADDRESSED:
        _need(payload, 6, 'Addressed')
        fields['dst_node'], fields['inner_type'] = struct.unpack_from('>IH', payload)
        fields['inner'] = decode_payload(fields['inner_type'], payload[6:])
    else:
        raise WireError('未知消息类型 0x%04X' % msg_type)
    return fields


class Api:
    """接口定义里的名字和 key。接受接口定义 YAML 和网络描述 YAML"""

    def __init__(self, path):
        raw = load_config(path)
        if not isinstance(raw, dict):
            raise ConfigError('定义文件顶层应该是映射')
        self.user_tag = raw.get('user_tag', 0)
        self.processes = {}
        self.notifies = {}
        self.node_names = {}
        for item in raw.get('processes') or []:
            item['input'] = check_param_list(item.get('input'), item.get('name'))
            item['output'] = check_param_list(item.get('output'), item.get('name'))
            fill_key(item, 'process')
            self.processes[item['id']] = item
        for item in raw.get('notifies') or []:
            item['params'] = check_param_list(item.get('params'), item.get('name'))
            fill_key(item, 'notify')
            self.notifies[item['id']] = item
        for node in raw.get('nodes') or []:
            if 'id' in node:
                self.node_names[node['id']] = node['name']

    def process_by_name(self, name):
        for proc in self.processes.values():
            if proc['name'] == name:
                return proc
        raise ConfigError('没有过程 %s' % name)
