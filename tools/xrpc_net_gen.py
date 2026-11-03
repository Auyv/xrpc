#!/usr/bin/env python3
# Copyright (c) 2026 AuyvJpz
"""从一份网络描述生成全部节点的代码。

    python tools/xrpc_net_gen.py check net.yaml
    python tools/xrpc_net_gen.py gen net.yaml --out gen
    python tools/xrpc_net_gen.py sim net.yaml --out gen

check 只做一致性检查。gen 为每个节点生成 port/ 和 api/，再写一份 report.md。
sim 在本机跑整个网络：每个节点一个进程，链路换成本机 TCP，不用改网络描述。
"""

import argparse
import os
import sys

import xrpc_api_gen as api_gen
import xrpc_port_gen as port_gen
from xrpc_net_model import parse_net
from xrpc_yaml import ConfigError, component_version, load_config, write_text

SIM_BASE_PORT = 47000


def api_spec(raw, net):
    """翻译给 xrpc_api_gen。接口定义原样传过去，节点的角色用推导结果"""
    procs = [{key: value for key, value in proc.items() if key not in ('provider', 'callers')}
             for proc in raw.get('processes') or []]
    notifies = [{key: value for key, value in notify.items()
                 if key not in ('publisher', 'subscribers', 'observers')}
                for notify in raw.get('notifies') or []]
    nodes = []
    for name, item in net['derived'].items():
        node = {'name': name, 'provides': list(item['provides']), 'calls': list(item['calls']),
                'publishes': [dict(entry) for entry in item['publishes']],
                'subscribes': [dict(entry) for entry in item['subscribes']],
                'bindings': [{'process': proc, 'link': binding[0], 'provider': binding[1]}
                             for proc, binding in sorted(item['bindings'].items())]}
        if item['is_relay']:
            capacity = item['capacity']
            node['relay'] = {'routes': max(1, capacity['routes']),
                             'calls': max(1, capacity['calls']),
                             'ack_records': max(1, capacity['ack_records']),
                             'forwards': [{'notify': notify, 'links': sorted(links)}
                                          for notify, links in sorted(item['forwards'].items())]}
        nodes.append(node)
    return api_gen.parse_spec({'module': raw['module'], 'processes': procs,
                               'notifies': notifies, 'nodes': nodes})


def port_config(net, name, sim):
    node = net['nodes'][name]
    links = []
    for link in node['links']:
        entry = {'name': link['name'], 'topology': link['topology'],
                 'type': link.get('type', 'custom')}
        for key in ('host', 'port', 'server', 'pipe', 'device', 'baud', 'max_frame'):
            if key in link:
                entry[key] = link[key]
        if link['topology'] == 'bus':
            entry['remote_capacity'] = len(net['links'][link['net_link']]['nodes'])
        if sim is not None:
            entry.update(sim[link['net_link']][name])
        links.append(entry)
    config = {'node_id': node['id'], 'user_tag': net['user_tag'],
              'platform': node.get('platform', net['platform']),
              'threads': node.get('threads', net['threads']),
              'crc16': node.get('crc16', net['crc16']),
              'call_slots': node.get('call_slots', 4),
              'frame_size': net['derived'][name]['capacity']['frame_size'],
              'links': links}
    if 'recv_size' in node:
        config['recv_size'] = node['recv_size']
    if sim is not None:
        config['platform'] = 'win32' if os.name == 'nt' else 'posix'
        config['threads'] = 'multi'
        if config['crc16'] == 'custom':
            config['crc16'] = 'ccitt_false'
    return port_gen.validate(config)


def sim_links(net, base_port):
    """每条链路一个本机端口，先列出的节点当服务端。总线没法用点对点 TCP 模拟"""
    result = {}
    for index, link in enumerate(net['links'].values()):
        if link['topology'] != 'p2p':
            raise ConfigError('仿真暂不支持总线链路：%s' % link['name'])
        result[link['name']] = {}
        for member_index, member in enumerate(link['nodes']):
            result[link['name']][member] = {'type': 'tcp', 'host': '127.0.0.1',
                                            'port': base_port + index,
                                            'server': member_index == 0}
    return result


def report(raw, net):
    lines = ['# %s 网络报告' % net['module'], '', '由 xrpc_net_gen.py 生成，不要手改。', '',
             '## 拓扑', '', '```mermaid', 'flowchart LR']
    for name, node in net['nodes'].items():
        lines.append('    %s["%s<br/>0x%08X"]' % (name, name, node['id']))
    for link in net['links'].values():
        if link['topology'] == 'bus':
            lines.append('    %s(("%s"))' % (link['name'], link['name']))
            lines += ['    %s --- %s' % (member, link['name']) for member in link['nodes']]
        else:
            lines.append('    %s ---|"%s"| %s' % (link['nodes'][0], link['name'], link['nodes'][1]))
    lines += ['```', '', '## 调用路径', '', '| 过程 | 调用方 | 路径 |', '| --- | --- | --- |']
    for proc in net['processes'].values():
        for caller, provider in proc['callers'].items():
            hops = net['topology'].path(caller, provider)
            lines.append('| %s | %s | %s |' % (proc['name'], caller,
                                               ' → '.join(hop[0] for hop in hops)))
    lines += ['', '## 事件路径', '', '| 事件 | 发布方 | 接收方 | 路径 | 应答 |',
              '| --- | --- | --- | --- | --- |']
    for notify in net['notifies'].values():
        for target in notify['subscribers'] + notify['observers']:
            hops = net['topology'].path(notify['publisher'], target)
            ack = '是' if notify.get('need_ack') and (target in notify['subscribers']) else '否'
            lines.append('| %s | %s | %s | %s | %s |'
                         % (notify['name'], notify['publisher'], target,
                            ' → '.join(hop[0] for hop in hops), ack))
    lines += ['', '## 节点容量', '',
              '| 节点 | ID | 中继 | 帧缓冲 | 路由表 | 转发槽 | Ack 记录 |',
              '| --- | --- | --- | --- | --- | --- | --- |']
    for name, node in net['nodes'].items():
        item = net['derived'][name]
        capacity = item['capacity']
        lines.append('| %s | 0x%08X | %s | %d | %d | %d | %d |'
                     % (name, node['id'], '是' if item['is_relay'] else '否',
                        capacity['frame_size'], capacity['routes'], capacity['calls'],
                        capacity['ack_records']))
    lines.append('')
    return '\n'.join(lines)


SIM_MAIN = '''/**
 * @file        xrpc_sim_main.c
 * @brief       节点 @NODE@ 的仿真入口，由 xrpc_net_gen.py 生成。
 * @note        业务写在 xrpc_sim_app.c 和 api/@MODULE@_xrpc_impl.c。
 */

#include "xrpc_port_node.h"
#include "xrpc/xrpc.h"
#include "@MODULE@_xrpc.h"

/** 启动轮询之前调用，比如设置状态回调 */
void XrpcSimPrepare(void);

/** 业务入口，返回值就是进程退出码 */
int XrpcSimRun(void);

int main(void)
{
    int code = 1;

    if ((XrpcPortInit() == kXrpcErrOk)
        && (@PREFIX@XrpcRegister(XrpcPortNode(), XrpcGetSysTick()) == kXrpcErrOk))
    {
        XrpcSimPrepare();
    }
    if (XrpcPortStartPollThread() == kXrpcErrOk)
    {
        code = XrpcSimRun();
        XrpcPortStop();
    }

    return code;
}
'''

SIM_APP = '''/**
 * @file        xrpc_sim_app.c
 * @brief       节点 @NODE@ 的仿真业务。这个文件归你所有，重新生成不会覆盖。
 */

#include "xrpc_port_node.h"
#include "xrpc/xrpc.h"
#include "@MODULE@_xrpc.h"

void XrpcSimPrepare(void)
{
    /* TODO: 启动轮询之前要做的事，比如 TXrpcNodeSetStatusCb */
}

int XrpcSimRun(void)
{
    /* TODO: 调用过程、发布事件。返回值是进程退出码 */
    for (;;)
    {
        XrpcSleepMs(1000U);
    }

    return 0;
}
'''

SIM_CMAKE = '''# 由 xrpc_net_gen.py 生成：每个节点一个可执行文件
cmake_minimum_required(VERSION 3.16)
project(@MODULE@_sim C)
set(CMAKE_C_STANDARD 99)

file(GLOB XRPC_CORE_SOURCES "@ROOT@/common/*.c")
foreach(node @NODES@)
    include(${CMAKE_CURRENT_LIST_DIR}/${node}/port/xrpc_port.cmake)
    add_executable(${node}
        ${XRPC_CORE_SOURCES}
        ${XRPC_PORT_SOURCES}
        ${CMAKE_CURRENT_LIST_DIR}/${node}/api/@MODULE@_xrpc.c
        ${CMAKE_CURRENT_LIST_DIR}/${node}/api/@MODULE@_xrpc_impl.c
        ${CMAKE_CURRENT_LIST_DIR}/${node}/xrpc_sim_main.c
        ${CMAKE_CURRENT_LIST_DIR}/${node}/xrpc_sim_app.c)
    target_include_directories(${node} PRIVATE
        ${XRPC_PORT_INCLUDE_DIRS}
        ${CMAKE_CURRENT_LIST_DIR}/${node}/api
        @ROOT@/common
        @ROOT@/common/include
        @ROOT@/common/include/xrpc)
    target_link_libraries(${node} PRIVATE ${XRPC_PORT_LIBS})
    if(WIN32)
        target_link_options(${node} PRIVATE -static)
    else()
        target_link_libraries(${node} PRIVATE pthread)
    endif()
endforeach()
'''


def generate(raw, net, out_dir, sim):
    spec = api_spec(raw, net)
    lines = []
    for name in net['nodes']:
        base = os.path.join(out_dir, name)
        port_gen.generate(port_config(net, name, sim), os.path.join(base, 'port'), True)
        lines += api_gen.generate_node(spec, name, os.path.join(base, 'api'))
    write_text(os.path.join(out_dir, 'report.md'), report(raw, net))
    return lines


def generate_sim(raw, net, out_dir, base_port):
    lines = generate(raw, net, out_dir, sim_links(net, base_port))
    prefix = ''.join(part.capitalize() for part in net['module'].split('_'))
    root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
    for name in net['nodes']:
        write_text(os.path.join(out_dir, name, 'xrpc_sim_main.c'),
                   SIM_MAIN.replace('@NODE@', name).replace('@MODULE@', net['module'])
                   .replace('@PREFIX@', prefix))
        app = os.path.join(out_dir, name, 'xrpc_sim_app.c')
        if not os.path.exists(app):
            write_text(app, SIM_APP.replace('@NODE@', name).replace('@MODULE@', net['module']))
    write_text(os.path.join(out_dir, 'CMakeLists.txt'),
               SIM_CMAKE.replace('@MODULE@', net['module'])
               .replace('@ROOT@', root.replace('\\', '/'))
               .replace('@NODES@', ' '.join(net['nodes'])))
    return lines


def main():
    parser = argparse.ArgumentParser(description='XRPC 网络描述工具')
    parser.add_argument('--version', action='version', version=component_version())
    sub = parser.add_subparsers(dest='command', required=True)
    for name, text in (('check', '一致性检查'), ('gen', '生成全部节点'), ('sim', '生成本机仿真')):
        cmd = sub.add_parser(name, help=text)
        cmd.add_argument('net', help='网络描述文件（.yaml 或 .json）')
        if name != 'check':
            cmd.add_argument('--out', required=True, help='输出目录')
        if name == 'sim':
            cmd.add_argument('--base-port', type=int, default=SIM_BASE_PORT,
                             help='第一条链路的端口，后面的依次加 1')
    args = parser.parse_args()
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8')
        sys.stderr.reconfigure(encoding='utf-8')
    try:
        raw = load_config(args.net)
        net, warnings = parse_net(raw)
        for text in warnings:
            print('警告：%s' % text)
        if args.command == 'gen':
            lines = generate(raw, net, args.out, None)
        elif args.command == 'sim':
            lines = generate_sim(raw, net, args.out, args.base_port)
        else:
            lines = ['检查通过：%d 个节点，%d 条链路，%d 个过程，%d 个事件'
                     % (len(net['nodes']), len(net['links']), len(net['processes']),
                        len(net['notifies']))]
        for text in lines:
            print(text)
    except ConfigError as error:
        print('错误：%s' % error, file=sys.stderr)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())
