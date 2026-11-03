# Copyright (c) 2026 AuyvJpz
"""网络描述的解析、一致性检查和推导。

一份 xrpc_net.yaml 描述节点、链路、过程、事件，这里算出每个节点要的配置：
哪些节点开中继、事件转发表、应答列表、绑定表、缓冲和表项大小。
只依赖标准库，不生成代码，生成在 xrpc_net_gen.py。
"""

import re

from xrpc_api_gen import check_param_list, list_bytes
from xrpc_yaml import ConfigError

HEAD_SIZE = 28              # 帧头
ENVELOPE_SIZE = 6           # 总线信封
CALL_REQ_FIXED = 9
CALL_RSP_FIXED = 13
NOTIFY_EVENT_FIXED = 18
NOTIFY_ACK_SIZE = 20
TTL_DEFAULT = 255
MIN_FRAME_SIZE = 128
BROADCAST = 0xFFFFFFFF

NAME_RE = r'[a-z][a-z0-9_]*'
TOP_KEYS = {'module', 'user_tag', 'platform', 'threads', 'crc16', 'nodes', 'links',
            'processes', 'notifies'}
NODE_KEYS = {'name', 'id', 'platform', 'threads', 'crc16', 'call_slots', 'recv_size', 'override'}
OVERRIDE_KEYS = {'frame_size', 'routes', 'calls', 'ack_records'}
LINK_KEYS = {'name', 'topology', 'connect'}
ENDPOINT_KEYS = {'name', 'type', 'host', 'port', 'server', 'pipe', 'device', 'baud', 'max_frame'}
PROCESS_KEYS = {'name', 'id', 'key', 'brief', 'timeout_ms', 'input', 'output', 'provider', 'callers'}
NOTIFY_KEYS = {'name', 'id', 'key', 'brief', 'need_ack', 'timeout_ms', 'retry', 'params',
               'publisher', 'subscribers', 'observers'}


def _check_keys(item, allowed, where):
    unknown = set(item) - allowed
    if unknown:
        raise ConfigError('%s：不认识的配置项 %s' % (where, '、'.join(sorted(unknown))))


def _name_of(item, where):
    name = item.get('name') if isinstance(item, dict) else None
    if not isinstance(name, str) or not name:
        raise ConfigError('%s：name 必填' % where)
    return name


def _parse_nodes(raw):
    nodes = {}
    ids = {}
    for index, node in enumerate(raw.get('nodes') or []):
        where = 'nodes[%d]' % index
        name = _name_of(node, where)
        _check_keys(node, NODE_KEYS, '节点 %s' % name)
        if not re.fullmatch(NAME_RE, name):
            raise ConfigError('节点 %s：名字只能用小写字母、数字、下划线' % name)
        if name in nodes:
            raise ConfigError('节点 %s 重复' % name)
        node_id = node.get('id')
        if not isinstance(node_id, int) or isinstance(node_id, bool) \
                or not (0 < node_id < BROADCAST):
            raise ConfigError('节点 %s：id 必填，不能是 0 或广播值' % name)
        if node_id in ids:
            raise ConfigError('节点 %s 和 %s 的 id 相同：0x%08X' % (ids[node_id], name, node_id))
        ids[node_id] = name
        override = node.get('override') or {}
        _check_keys(override, OVERRIDE_KEYS, '节点 %s 的 override' % name)
        nodes[name] = dict(node, override=override, links=[])
    if not nodes:
        raise ConfigError('nodes 至少要有一个节点')
    return nodes


def _parse_links(raw, nodes):
    links = {}
    for index, link in enumerate(raw.get('links') or []):
        where = 'links[%d]' % index
        name = _name_of(link, where)
        _check_keys(link, LINK_KEYS, '链路 %s' % name)
        if name in links:
            raise ConfigError('链路 %s 重复' % name)
        topology = link.get('topology', 'p2p')
        if topology not in ('p2p', 'bus'):
            raise ConfigError('链路 %s：topology 只能是 p2p 或 bus' % name)
        connect = link.get('connect')
        if not isinstance(connect, dict) or len(connect) < 2:
            raise ConfigError('链路 %s：connect 至少要连两个节点' % name)
        if (topology == 'p2p') and (len(connect) != 2):
            raise ConfigError('链路 %s：点对点只能连两个节点，多个节点要写 topology: bus' % name)
        for node_name, endpoint in connect.items():
            if node_name not in nodes:
                raise ConfigError('链路 %s：没有节点 %s' % (name, node_name))
            endpoint = endpoint or {}
            _check_keys(endpoint, ENDPOINT_KEYS, '链路 %s 的 %s 端' % (name, node_name))
            local = endpoint.get('name', name)
            used = [item['name'] for item in nodes[node_name]['links']]
            if local in used:
                raise ConfigError('节点 %s：本地链路名 %s 重复' % (node_name, local))
            nodes[node_name]['links'].append(dict(endpoint, name=local, net_link=name,
                                                  topology=topology))
        links[name] = {'name': name, 'topology': topology, 'nodes': list(connect)}
    return links


def _check_id(item, used, where):
    item_id = item.get('id')
    if not isinstance(item_id, int) or isinstance(item_id, bool) or (item_id == 0):
        raise ConfigError('%s：id 必填且不能是 0' % where)
    if item_id in used:
        raise ConfigError('%s 和 %s 的 id 相同：0x%08X' % (used[item_id], where, item_id))
    used[item_id] = where


def _node_list(value, nodes, where):
    names = value if isinstance(value, list) else ([value] if value else [])
    for name in names:
        if name not in nodes:
            raise ConfigError('%s：没有节点 %s' % (where, name))
    return names


def _parse_processes(raw, nodes):
    used = {}
    processes = {}
    for index, proc in enumerate(raw.get('processes') or []):
        name = _name_of(proc, 'processes[%d]' % index)
        where = '过程 %s' % name
        _check_keys(proc, PROCESS_KEYS, where)
        if name in processes:
            raise ConfigError('%s 重复' % where)
        _check_id(proc, used, where)
        providers = _node_list(proc.get('provider'), nodes, where)
        if not providers:
            raise ConfigError('%s：没有提供者' % where)
        callers = {}
        for entry in proc.get('callers') or []:
            caller = entry if isinstance(entry, str) else _name_of(entry, where)
            if caller not in nodes:
                raise ConfigError('%s：没有节点 %s' % (where, caller))
            if caller in callers:
                raise ConfigError('%s：调用方 %s 重复' % (where, caller))
            if caller in providers:
                raise ConfigError('%s：%s 是提供者，不用再写成调用方' % (where, caller))
            bind = entry.get('bind') if isinstance(entry, dict) else None
            if (bind is not None) and (bind not in providers):
                raise ConfigError('%s：调用方 %s 绑定的 %s 不是提供者' % (where, caller, bind))
            if (bind is None) and (len(providers) > 1):
                raise ConfigError('%s 有多个提供者，调用方 %s 要用 bind 指定' % (where, caller))
            callers[caller] = bind or providers[0]
        processes[name] = dict(proc, providers=providers, callers=callers,
                               in_list=check_param_list(proc.get('input'), '%s.input' % name),
                               out_list=check_param_list(proc.get('output'), '%s.output' % name))
    return processes


def _parse_notifies(raw, nodes):
    used = {}
    notifies = {}
    for index, notify in enumerate(raw.get('notifies') or []):
        name = _name_of(notify, 'notifies[%d]' % index)
        where = '事件 %s' % name
        _check_keys(notify, NOTIFY_KEYS, where)
        if name in notifies:
            raise ConfigError('%s 重复' % where)
        _check_id(notify, used, where)
        publisher = notify.get('publisher')
        if publisher not in nodes:
            raise ConfigError('%s：publisher 必须是一个节点' % where)
        subscribers = _node_list(notify.get('subscribers'), nodes, where)
        observers = _node_list(notify.get('observers'), nodes, where)
        both = set(subscribers) & set(observers)
        if both:
            raise ConfigError('%s：%s 同时是订阅者和观察者' % (where, '、'.join(sorted(both))))
        if (publisher in subscribers) or (publisher in observers):
            raise ConfigError('%s：发布方不用订阅自己' % where)
        notifies[name] = dict(notify, subscribers=subscribers, observers=observers,
                              param_list=check_param_list(notify.get('params'), '%s.params' % name))
    return notifies


class Topology:
    """节点和链路组成的图。网络模型是树，两点之间路径唯一"""

    def __init__(self, nodes, links):
        self.nodes = nodes
        self.links = links
        self.adjacent = {name: [] for name in nodes}
        for link in links.values():
            for member in link['nodes']:
                self.adjacent[member].append(link['name'])
        self._check_tree()

    def _check_tree(self):
        """节点和链路都当成顶点，有环时并查集会合并到同一个集合"""
        parent = {}

        def find(item):
            parent.setdefault(item, item)
            while parent[item] != item:
                parent[item] = parent[parent[item]]
                item = parent[item]
            return item

        for link in self.links.values():
            for member in link['nodes']:
                a = find(('node', member))
                b = find(('link', link['name']))
                if a == b:
                    raise ConfigError('网络成环：节点 %s 经链路 %s 回到了自己所在的连通块。'
                                      '网络模型是树，两点之间只能有一条路径' % (member, link['name']))
                parent[a] = b

    def path(self, src, dst):
        """返回 [(节点, 出口网络链路), ...]，最后一项是 (dst, None)；不通返回 None"""
        previous = {src: None}
        queue = [src]
        while queue:
            node = queue.pop(0)
            if node == dst:
                break
            for link in self.adjacent[node]:
                for member in self.links[link]['nodes']:
                    if member not in previous:
                        previous[member] = (node, link)
                        queue.append(member)
        if dst not in previous:
            return None
        hops = [(dst, None)]
        node = dst
        while previous[node] is not None:
            node, link = previous[node]
            hops.insert(0, (node, link))
        return hops

    def require_path(self, src, dst, what):
        hops = self.path(src, dst)
        if hops is None:
            raise ConfigError('%s：%s 和 %s 不连通' % (what, src, dst))
        return hops


def link_index(node, net_link):
    for index, link in enumerate(node['links']):
        if link['net_link'] == net_link:
            return index
    raise ConfigError('节点 %s 没有链路 %s' % (node['name'], net_link))


def _new_derived(nodes):
    return {name: {'relay_routes': set(), 'relay_callers': set(), 'bindings': {},
                   'forwards': {}, 'ack_events': set(), 'frame': MIN_FRAME_SIZE,
                   'provides': [], 'calls': [], 'publishes': [], 'subscribes': []}
            for name in nodes}


def _frame_size(payload, link):
    return HEAD_SIZE + (ENVELOPE_SIZE if link['topology'] == 'bus' else 0) + payload


def _note_frame(net, derived, hops, payload, what):
    """这条路径上每个节点都要放得下这一帧，每条链路都要允许这个长度"""
    for index, (node_name, net_link) in enumerate(hops):
        links = []
        if net_link is not None:
            links.append(net_link)
        if index > 0:
            links.append(hops[index - 1][1])
        for name in links:
            link = net['links'][name]
            size = _frame_size(payload, link)
            derived[node_name]['frame'] = max(derived[node_name]['frame'], size)
            for endpoint in net['nodes'][node_name]['links']:
                limit = endpoint.get('max_frame', 0)
                if (endpoint['net_link'] == name) and limit and (size > limit):
                    raise ConfigError('%s：帧长 %d 超过节点 %s 链路 %s 的 max_frame %d'
                                      % (what, size, node_name, endpoint['name'], limit))


def _check_ttl(hops, what):
    relays = len(hops) - 2
    if relays >= TTL_DEFAULT - 1:
        raise ConfigError('%s：要经过 %d 个中继，超过了 ttl 上限' % (what, relays))


def _derive_process(net, derived, proc):
    name = proc['name']
    relayed_by = {}
    for provider in proc['providers']:
        derived[provider]['provides'].append(name)
    for caller, provider in proc['callers'].items():
        what = '过程 %s 的调用方 %s 到提供者 %s' % (name, caller, provider)
        hops = net['topology'].require_path(caller, provider, what)
        _check_ttl(hops, what)
        derived[caller]['calls'].append(name)
        _note_frame(net, derived, hops,
                    CALL_REQ_FIXED + list_bytes(proc['in_list']), what)
        _note_frame(net, derived, hops,
                    CALL_RSP_FIXED + list_bytes(proc['out_list']), what)
        for index, (node_name, net_link) in enumerate(hops[:-1]):
            next_node = hops[index + 1][0]
            is_bus = net['links'][net_link]['topology'] == 'bus'
            binding = (link_index(net['nodes'][node_name], net_link),
                       net['nodes'][next_node]['id'] if is_bus else 0)
            old = derived[node_name]['bindings'].get(name)
            if (old is not None) and (old != binding):
                raise ConfigError('过程 %s：节点 %s 上有两个方向的提供者，一个节点只能走一条路由'
                                  % (name, node_name))
            derived[node_name]['bindings'][name] = binding
            if index > 0:
                derived[node_name]['relay_routes'].add(name)
                derived[node_name]['relay_callers'].add(caller)
                if (name in relayed_by) and (relayed_by[name] != provider):
                    raise ConfigError('过程 %s：中继 %s 的路由只能指向一个提供者'
                                      % (name, node_name))
                relayed_by[name] = provider


def _call_slots(node):
    return node.get('call_slots', 4)


def _derive_notify(net, derived, notify):
    name = notify['name']
    publisher = notify['publisher']
    node = net['nodes'][publisher]
    first_hops = []
    need_ack = notify.get('need_ack', False)
    params = list_bytes(notify['param_list'])

    for target in notify['subscribers'] + notify['observers']:
        what = '事件 %s 的发布方 %s 到订阅者 %s' % (name, publisher, target)
        hops = net['topology'].require_path(publisher, target, what)
        _check_ttl(hops, what)
        _note_frame(net, derived, hops, NOTIFY_EVENT_FIXED + params, what)
        _note_frame(net, derived, hops, NOTIFY_ACK_SIZE, what)
        first = link_index(node, hops[0][1])
        if first not in first_hops:
            first_hops.append(first)
        for node_name, net_link in hops[1:-1]:
            index = link_index(net['nodes'][node_name], net_link)
            links = derived[node_name]['forwards'].setdefault(name, [])
            if index not in links:
                links.append(index)
            if need_ack:
                derived[node_name]['ack_events'].add(name)
        derived[target]['subscribes'].append({
            'notify': name, 'publisher': node['id'],
            'send_ack': target in notify['subscribers']})

    if first_hops:
        acks = [net['nodes'][item]['id'] for item in notify['subscribers']] if need_ack else []
        derived[publisher]['publishes'].append({'notify': name, 'links': sorted(first_hops),
                                                'ack_nodes': acks})


def parse_net(raw):
    """解析并检查网络描述，返回 (模型, 警告列表)。有错误时抛 ConfigError"""
    if not isinstance(raw, dict):
        raise ConfigError('网络描述顶层应该是映射')
    _check_keys(raw, TOP_KEYS, '顶层')
    module = raw.get('module')
    if not isinstance(module, str) or not re.fullmatch(NAME_RE, module):
        raise ConfigError('module 必填，只能用小写字母、数字、下划线')

    nodes = _parse_nodes(raw)
    links = _parse_links(raw, nodes)
    processes = _parse_processes(raw, nodes)
    notifies = _parse_notifies(raw, nodes)
    net = {'module': module, 'user_tag': raw.get('user_tag', 0),
           'platform': raw.get('platform', 'custom'), 'threads': raw.get('threads', 'multi'),
           'crc16': raw.get('crc16', 'custom'), 'nodes': nodes, 'links': links,
           'processes': processes, 'notifies': notifies,
           'topology': Topology(nodes, links)}

    warnings = []
    derived = _new_derived(nodes)
    for proc in processes.values():
        if not proc['callers']:
            warnings.append('过程 %s 没有调用方' % proc['name'])
        _derive_process(net, derived, proc)
    for notify in notifies.values():
        if not (notify['subscribers'] or notify['observers']):
            warnings.append('事件 %s 没有订阅者' % notify['name'])
        _derive_notify(net, derived, notify)

    for name, item in derived.items():
        item['relay_call_slots'] = sum(_call_slots(nodes[caller])
                                       for caller in item['relay_callers'])
        _finish_capacity(net['nodes'][name], item, warnings)
    net['derived'] = derived
    return net, warnings


def _finish_capacity(node, item, warnings):
    """算出容量，再应用手工覆盖。覆盖值不够时只警告，这是使用者的取舍"""
    computed = {
        'frame_size': item['frame'],
        'routes': len(item['relay_routes']),
        # 重试时旧转发槽要等应答才释放，留一倍
        'calls': 2 * item['relay_call_slots'],
        # 事件在途时记录才有用，留一倍
        'ack_records': 2 * len(item['ack_events']),
    }
    capacity = dict(computed)
    for key, value in node['override'].items():
        if value < computed[key]:
            warnings.append('节点 %s：%s 配成 %d，算出来至少要 %d'
                            % (node['name'], key, value, computed[key]))
        capacity[key] = value
    item['capacity'] = capacity
    item['is_relay'] = bool(item['relay_routes'] or item['forwards'])
