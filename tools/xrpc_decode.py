#!/usr/bin/env python3
# Copyright (c) 2026 AuyvJpz
"""把 XRPC 线上字节翻译成人能读的内容。

    python tools/xrpc_decode.py capture.bin --api net.yaml
    python tools/xrpc_decode.py capture.txt --hex
    python tools/xrpc_decode.py --tcp-proxy 47100:127.0.0.1:47000 --api net.yaml
    python tools/xrpc_decode.py --serial COM3:115200

给了接口定义就显示过程名和参数名。在线读串口要装 pyserial，其他功能只用标准库。
"""

import argparse
import os
import re
import socket
import sys
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import xrpc_wire as wire                        # noqa: E402
from xrpc_yaml import ConfigError, component_version    # noqa: E402

READ_SIZE = 4096


def parse_crc(text):
    """返回 FrameReader 用的校验函数，none 表示不校验"""
    if text == 'ccitt_false':
        return wire.crc_ccitt_false
    if text == 'none':
        return None
    match = re.fullmatch(r'fixed:(0x[0-9A-Fa-f]+|\d+)', text)
    if match is None:
        raise ConfigError('--crc 只能是 ccitt_false、none 或 fixed:<值>')
    return wire.crc_fixed(int(match.group(1), 0))


def parse_hex(text):
    """接受 "58 52"、"5852"、"0x58, 0x52" 这些写法"""
    text = re.sub(r'0[xX]', '', text)
    text = re.sub(r'[\s,]', '', text)
    try:
        return bytes.fromhex(text)
    except ValueError as error:
        raise ConfigError('十六进制内容有误：%s' % error) from error


def format_value(value):
    return value.hex(' ').upper() if isinstance(value, bytes) else repr(value) \
        if isinstance(value, str) else str(value)


def format_params(params, described):
    """described 是接口定义里的参数表，没有时显示参数 ID"""
    names = {item['id']: item['name'] for item in described}
    return ', '.join('%s=%s' % (names.get(param_id, '#%d' % param_id), format_value(value))
                     for param_id, _, value in params)


def format_error(code):
    return '%d(%s)' % (code, wire.ERROR_NAMES[code]) if code in wire.ERROR_NAMES else str(code)


class Formatter:
    """把帧翻译成一行文字"""

    def __init__(self, api=None):
        self.api = api

    def _process(self, process_id):
        if (self.api is not None) and (process_id in self.api.processes):
            return self.api.processes[process_id]
        return {'name': '0x%08X' % process_id, 'input': [], 'output': []}

    def _notify(self, notify_id):
        if (self.api is not None) and (notify_id in self.api.notifies):
            return self.api.notifies[notify_id]
        return {'name': '0x%08X' % notify_id, 'params': []}

    def node(self, node_id):
        if (self.api is not None) and (node_id in self.api.node_names):
            return '%s(0x%02X)' % (self.api.node_names[node_id], node_id)
        return '0x%02X' % node_id

    def _message(self, msg_type, fields):
        """帧头以外的部分"""
        name = wire.TYPE_NAMES[msg_type]
        if msg_type in (wire.HELLO_REQ, wire.HELLO_RSP):
            text = name
            if 'max_recv_frame' in fields:
                text += ' max_recv=%d' % fields['max_recv_frame']
        elif msg_type == wire.KNOCK_REQ:
            text = '%s %s' % (name, self._process(fields['process_id'])['name'])
        elif msg_type == wire.KNOCK_RSP:
            text = '%s req_seq=%d %s %s' % (name, fields['req_seq'],
                                            self._process(fields['process_id'])['name'],
                                            {1: '可用', 0: 'key 不一致'}.get(
                                                fields['errcode'], format_error(fields['errcode'])))
        elif msg_type == wire.CALL_REQ:
            proc = self._process(fields['process_id'])
            text = '%s %s(%s)' % (name, proc['name'], format_params(fields['params'], proc['input']))
        elif msg_type == wire.CALL_RSP:
            proc = self._process(fields['process_id'])
            result = 'ok' if fields['errcode'] >= 0 else format_error(fields['errcode'])
            text = '%s req_seq=%d %s %s %s' % (name, fields['req_seq'], proc['name'], result,
                                               format_params(fields['params'], proc['output']))
        elif msg_type == wire.NOTIFY_EVENT:
            notify = self._notify(fields['notify_id'])
            text = '%s %s origin=%s seq=%d%s (%s)' % (
                name, notify['name'], self.node(fields['origin']), fields['seq'],
                ' need_ack' if fields['need_ack'] else '',
                format_params(fields['params'], notify['params']))
        elif msg_type == wire.NOTIFY_ACK:
            text = '%s %s origin=%s publisher=%s seq=%d %s' % (
                name, self._notify(fields['notify_id'])['name'], self.node(fields['origin']),
                self.node(fields['publisher']), fields['seq'],
                'ok' if fields['result'] == 0 else format_error(fields['result']))
        else:
            target = '广播' if fields['dst_node'] == wire.BROADCAST else self.node(fields['dst_node'])
            text = 'Addressed -> %s %s' % (target, self._message(fields['inner_type'],
                                                                 fields['inner']))
        return text.rstrip()

    def describe(self, item):
        if isinstance(item, wire.Skipped):
            return '?    -> %s，跳过 %d 字节' % (item.reason, len(item.data))
        head = '%s -> ' % self.node(item.node_id)
        tail = ' seq=%d ttl=%d' % (item.seq, item.ttl)
        try:
            return head + self._message(item.type, wire.decode_payload(item.type, item.payload)) \
                + tail
        except wire.WireError as error:
            return head + '类型 0x%04X：%s，载荷 %s' % (item.type, error, item.payload.hex()) + tail


def print_items(items, formatter, counter, prefix=''):
    for item in items:
        counter[0] += 1
        print('#%d %s%s' % (counter[0], prefix, formatter.describe(item)), flush=True)


def decode_file(args, formatter, crc):
    if args.input == '-':
        data = sys.stdin.buffer.read()
    else:
        with open(args.input, 'rb') as stream:
            data = stream.read()
    if args.hex:
        data = parse_hex(data.decode('utf-8', 'replace'))
    reader = wire.FrameReader(crc)
    counter = [0]
    print_items(reader.feed(data), formatter, counter)
    print_items(reader.flush(), formatter, counter)


def _forward(source, target, label, formatter, crc, counter, lock):
    """只转发不修改，解码出错也不影响转发"""
    reader = wire.FrameReader(crc)
    try:
        while True:
            data = source.recv(READ_SIZE)
            if not data:
                break
            target.sendall(data)
            items = reader.feed(data)
            with lock:
                print_items(items, formatter, counter, label)
    except OSError:
        pass
    for sock in (source, target):
        try:
            sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass


def proxy_once(listener, target_addr, formatter, crc):
    """接受一个连接，串到目标上，直到有一边断开"""
    inbound, _ = listener.accept()
    outbound = socket.create_connection(target_addr)
    for sock in (inbound, outbound):
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    counter = [0]
    lock = threading.Lock()
    threads = [threading.Thread(target=_forward, daemon=True,
                                args=(inbound, outbound, '[>] ', formatter, crc, counter, lock)),
               threading.Thread(target=_forward, daemon=True,
                                args=(outbound, inbound, '[<] ', formatter, crc, counter, lock))]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    inbound.close()
    outbound.close()


def parse_proxy(text):
    match = re.fullmatch(r'(\d+):([^:]+):(\d+)', text)
    if match is None:
        raise ConfigError('--tcp-proxy 的格式是 监听端口:目标主机:目标端口')
    return int(match.group(1)), (match.group(2), int(match.group(3)))


def run_proxy(args, formatter, crc):
    port, target = parse_proxy(args.tcp_proxy)
    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(('127.0.0.1', port))
    listener.listen(1)
    print('在 %d 端口等待连接，转发到 %s:%d' % (port, target[0], target[1]), flush=True)
    while True:
        proxy_once(listener, target, formatter, crc)
        print('连接断开，等待下一个连接', flush=True)


def open_serial(text):
    """pyserial 是可选依赖，只有在线读串口才需要"""
    try:
        import serial
    except ImportError as error:
        raise ConfigError('在线读串口要安装 pyserial：pip install pyserial') from error
    match = re.fullmatch(r'(.+):(\d+)', text)
    if match is None:
        raise ConfigError('串口的格式是 设备:波特率，例如 COM3:115200')
    return serial.Serial(match.group(1), int(match.group(2)), timeout=0.1)


def run_serial(args, formatter, crc):
    port = open_serial(args.serial)
    reader = wire.FrameReader(crc)
    counter = [0]
    while True:
        data = port.read(READ_SIZE)
        if data:
            print_items(reader.feed(data), formatter, counter)


def main():
    parser = argparse.ArgumentParser(description='XRPC 协议解码')
    parser.add_argument('--version', action='version', version=component_version())
    parser.add_argument('input', nargs='?', help='抓到的数据文件，- 表示标准输入')
    parser.add_argument('--hex', action='store_true', help='输入是十六进制文本')
    parser.add_argument('--api', help='接口定义或网络描述，用来显示名字')
    parser.add_argument('--crc', default='ccitt_false',
                        help='ccitt_false（默认）、none 不校验、fixed:<值>')
    parser.add_argument('--tcp-proxy', metavar='端口:主机:端口', help='串在两个 TCP 节点之间解码')
    parser.add_argument('--serial', metavar='设备:波特率', help='在线读串口，要安装 pyserial')
    args = parser.parse_args()
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8')
        sys.stderr.reconfigure(encoding='utf-8')

    try:
        formatter = Formatter(wire.Api(args.api) if args.api else None)
        crc = parse_crc(args.crc)
        if args.tcp_proxy:
            run_proxy(args, formatter, crc)
        elif args.serial:
            run_serial(args, formatter, crc)
        elif args.input:
            decode_file(args, formatter, crc)
        else:
            raise ConfigError('要指定输入文件、--tcp-proxy 或 --serial')
    except (ConfigError, OSError) as error:
        print('错误：%s' % error, file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == '__main__':
    sys.exit(main())
