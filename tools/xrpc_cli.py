#!/usr/bin/env python3
# Copyright (c) 2026 AuyvJpz
"""PC 上的调试节点：不用写 C 就能调用设备的过程、看事件。

    python tools/xrpc_cli.py --api net.yaml --tcp 127.0.0.1:47000 call Add a=40 b=2
    python tools/xrpc_cli.py --api net.yaml --listen 47000 discover Add
    python tools/xrpc_cli.py --api net.yaml --serial COM3:115200 --node-id gateway listen

这是调试工具，不是产品接口。只连一条点对点链路，不做中继；隔着中继的节点由相邻节点转发。
在线读串口要装 pyserial，其他功能只用标准库。
"""

import argparse
import os
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import xrpc_decode as decode                    # noqa: E402
import xrpc_wire as wire                        # noqa: E402
from xrpc_yaml import ConfigError, component_version    # noqa: E402

DEFAULT_NODE_ID = 0x7F000001
ERR_PROCESS_NOT_FOUND = -8
HELLO_PERIOD_S = 0.5
KNOCK_PERIOD_S = 0.5
POLL_S = 0.05
READ_SIZE = 4096


class CliError(Exception):
    """对端或链路没有按预期工作，信息给使用者看"""


class TcpTransport:
    def __init__(self, sock):
        self.sock = sock
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.sock.settimeout(POLL_S)

    @classmethod
    def connect(cls, text):
        host, _, port = text.rpartition(':')
        if (not host) or (not port.isdigit()):
            raise ConfigError('--tcp 的格式是 主机:端口')
        return cls(socket.create_connection((host, int(port)), timeout=5))

    @classmethod
    def listen(cls, port, timeout):
        listener = socket.socket()
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(('127.0.0.1', port))
        listener.listen(1)
        listener.settimeout(timeout)
        try:
            sock, _ = listener.accept()
        except socket.timeout as error:
            raise CliError('等了 %g 秒没有节点连上来' % timeout) from error
        finally:
            listener.close()
        return cls(sock)

    def send(self, data):
        self.sock.sendall(data)

    def recv(self):
        """没有数据返回空，对端断开时报错"""
        try:
            data = self.sock.recv(READ_SIZE)
        except socket.timeout:
            return b''
        if not data:
            raise CliError('对端断开了连接')
        return data


class SerialTransport:
    def __init__(self, text):
        self.port = decode.open_serial(text)
        self.port.timeout = POLL_S

    def send(self, data):
        self.port.write(data)

    def recv(self):
        return self.port.read(READ_SIZE)


class Client:
    """协议的最小实现：握手、发现、调用，收事件并回 Ack，别人的请求一律回没有这个过程"""

    def __init__(self, transport, api, node_id, crc, formatter):
        self.transport = transport
        self.api = api
        self.node_id = node_id
        self.crc = crc
        self.reader = wire.FrameReader(crc)
        self.formatter = formatter
        self.seq = 0
        self.peer_id = None
        self.on_event = None

    def send(self, msg_type, payload=b'', ttl=wire.TTL_DEFAULT):
        self.seq += 1
        self.transport.send(wire.pack_frame(self.node_id, msg_type, payload, crc=self.crc,
                                            user_tag=self.api.user_tag, seq=self.seq, ttl=ttl))
        return self.seq

    def _answer(self, frame, fields):
        """节点必须应答的消息"""
        if frame.type == wire.HELLO_REQ:
            self.send(wire.HELLO_RSP)
        elif frame.type == wire.KNOCK_REQ:
            self.send(wire.KNOCK_RSP, wire.struct.pack('>IIi', frame.seq, fields['process_id'],
                                                       ERR_PROCESS_NOT_FOUND))
        elif frame.type == wire.CALL_REQ:
            self.send(wire.CALL_RSP, wire.struct.pack('>IIiB', frame.seq, fields['process_id'],
                                                      ERR_PROCESS_NOT_FOUND, 0))
        elif frame.type == wire.NOTIFY_EVENT:
            if fields['need_ack']:
                self.send(wire.NOTIFY_ACK, wire.notify_ack(fields['notify_id'], self.node_id,
                                                           fields['origin'], fields['seq']))
            if self.on_event is not None:
                self.on_event(frame)

    def _frames(self):
        """读一次链路，返回这次收到的帧。版本或 user_tag 不同的帧丢弃"""
        result = []
        for item in self.reader.feed(self.transport.recv()):
            if isinstance(item, wire.Skipped):
                continue
            if (item.version != wire.VERSION) or (item.user_tag != self.api.user_tag):
                continue
            try:
                fields = wire.decode_payload(item.type, item.payload)
            except wire.WireError:
                continue
            if item.type in (wire.HELLO_REQ, wire.HELLO_RSP):
                self.peer_id = item.node_id
            self._answer(item, fields)
            result.append((item, fields))
        return result

    def wait(self, match, timeout):
        """等到 match(frame, fields) 为真的帧，超时返回 None"""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for frame, fields in self._frames():
                if match(frame, fields):
                    return frame, fields
        return None

    def hello(self, timeout):
        deadline = time.monotonic() + timeout
        while self.peer_id is None:
            if time.monotonic() >= deadline:
                raise CliError('握手超时：对端没有应答，检查连接、CRC 和 user_tag')
            self.send(wire.HELLO_REQ)
            self.wait(lambda frame, fields: self.peer_id is not None, HELLO_PERIOD_S)

    def discover(self, proc, timeout):
        """中继刚启动时可能回否定，到超时前一直问"""
        deadline = time.monotonic() + timeout
        last = None
        while time.monotonic() < deadline:
            seq = self.send(wire.KNOCK_REQ, wire.knock_req(proc['id'], proc['key']))
            got = self.wait(lambda frame, fields: (frame.type == wire.KNOCK_RSP)
                            and (fields['req_seq'] == seq), KNOCK_PERIOD_S)
            if got is not None:
                last = got[1]['errcode']
                if last == 1:
                    return
                if last == 0:
                    raise CliError('过程 %s 的 key 和对端不一致' % proc['name'])
        if last is None:
            raise CliError('询问过程 %s 没有应答' % proc['name'])
        raise CliError('没有找到过程 %s：%s' % (proc['name'], decode.format_error(last)))

    def call(self, proc, params, timeout):
        seq = self.send(wire.CALL_REQ, wire.call_req(proc['id'], proc['key'], params))
        got = self.wait(lambda frame, fields: (frame.type == wire.CALL_RSP)
                        and (fields['req_seq'] == seq), timeout)
        if got is None:
            raise CliError('调用 %s 超时' % proc['name'])
        return got[1]


def encode_arguments(proc, items):
    """把 a=1 b=2 按接口定义编成参数"""
    given = {}
    for item in items:
        name, separator, value = item.partition('=')
        if not separator:
            raise ConfigError('参数要写成 名字=值：%s' % item)
        given[name] = value
    params = []
    for param in proc['input']:
        name = param['name']
        if name not in given:
            raise ConfigError('缺少参数 %s' % name)
        text = given.pop(name)
        kind = param['type']
        if kind == 'bytes':
            value = decode.parse_hex(text)
        elif kind == 'string':
            value = text
        else:
            try:
                value = int(text, 0)
            except ValueError as error:
                raise ConfigError('参数 %s 要写整数：%s' % (name, text)) from error
        try:
            params.append(wire.encode_param(param['id'], wire.NAME_TO_CODE[kind], value))
        except wire.struct.error as error:
            raise ConfigError('参数 %s 超出 %s 的范围：%s' % (name, kind, text)) from error
    if given:
        raise ConfigError('过程 %s 没有参数：%s' % (proc['name'], '、'.join(given)))
    return params


def parse_node_id(text, api):
    """数字，或网络描述里的节点名"""
    if text is None:
        return DEFAULT_NODE_ID
    for node_id, name in api.node_names.items():
        if name == text:
            return node_id
    try:
        return int(text, 0)
    except ValueError as error:
        raise ConfigError('没有节点 %s' % text) from error


def open_transport(args):
    if args.tcp:
        return TcpTransport.connect(args.tcp)
    if args.listen is not None:
        return TcpTransport.listen(args.listen, args.timeout)
    if args.serial:
        return SerialTransport(args.serial)
    raise ConfigError('要指定 --tcp、--listen 或 --serial')


def run_command(args, client, api, formatter):
    client.hello(args.timeout)
    print('已连接 %s' % formatter.node(client.peer_id), flush=True)
    if args.command == 'listen':
        client.on_event = lambda frame: print(formatter.describe(frame), flush=True)
        deadline = None if args.seconds is None else time.monotonic() + args.seconds
        while (deadline is None) or (time.monotonic() < deadline):
            client.wait(lambda frame, fields: False, POLL_S)
        return 0

    proc = api.process_by_name(args.name)
    if args.command == 'discover':
        client.discover(proc, args.timeout)
        print('%s 可用' % proc['name'])
        return 0

    params = encode_arguments(proc, args.arguments)
    client.discover(proc, args.timeout)
    fields = client.call(proc, params, args.timeout)
    if fields['errcode'] < 0:
        print('失败：%s' % decode.format_error(fields['errcode']))
        return 1
    print('成功 %s' % decode.format_params(fields['params'], proc['output']))
    return 0


def main():
    parser = argparse.ArgumentParser(description='XRPC 调试节点')
    parser.add_argument('--version', action='version', version=component_version())
    parser.add_argument('--api', required=True, help='接口定义或网络描述')
    parser.add_argument('--tcp', metavar='主机:端口', help='连到节点')
    parser.add_argument('--listen', type=int, metavar='端口', help='等节点连过来')
    parser.add_argument('--serial', metavar='设备:波特率', help='串口，要安装 pyserial')
    parser.add_argument('--node-id', help='本节点的 ID 或网络描述里的名字')
    parser.add_argument('--crc', default='ccitt_false', help='ccitt_false（默认）或 fixed:<值>')
    parser.add_argument('--timeout', type=float, default=5.0, help='每一步的超时，秒')
    sub = parser.add_subparsers(dest='command', required=True)
    call = sub.add_parser('call', help='调用过程')
    call.add_argument('name', help='过程名')
    call.add_argument('arguments', nargs='*', help='参数，写成 名字=值')
    discover = sub.add_parser('discover', help='询问过程是否存在')
    discover.add_argument('name', help='过程名')
    listen = sub.add_parser('listen', help='接收事件并应答')
    listen.add_argument('--seconds', type=float, help='运行多久，默认一直运行')
    args = parser.parse_args()
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8')
        sys.stderr.reconfigure(encoding='utf-8')

    transport = None
    try:
        api = wire.Api(args.api)
        crc = decode.parse_crc(args.crc)
        if crc is None:
            raise ConfigError('调试节点要发帧，--crc 不能是 none')
        formatter = decode.Formatter(api)
        transport = open_transport(args)
        client = Client(transport, api, parse_node_id(args.node_id, api), crc, formatter)
        return run_command(args, client, api, formatter)
    except (ConfigError, CliError, OSError) as error:
        print('错误：%s' % error, file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        return 0


if __name__ == '__main__':
    sys.exit(main())
