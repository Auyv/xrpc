#!/usr/bin/env python3
# Copyright (c) 2026 AuyvJpz
"""XRPC 代码生成工具的图形界面，整合适配代码、虚接口检查、接口代码和全网生成。

    python xrpc_tool_gui.py

生成逻辑全部来自 xrpc_port_gen.py、xrpc_api_gen.py、xrpc_net_gen.py，这里只有界面。只依赖标准库。
"""

import os
import sys
import tkinter as tk
from tkinter import filedialog, ttk

import xrpc_api_gen as api_gen
import xrpc_net_gen as net_gen
import xrpc_port_gen as port_gen
from xrpc_net_model import parse_net
from xrpc_yaml import ConfigError, load_config, parse_yaml, write_text

HERE = os.path.dirname(os.path.abspath(__file__))

NEW_SPEC = '''# 接口定义，格式见 tools/README.md
module: demo

processes:
  - name: Add
    id: 0x00001001
    input:
      - {id: 1, name: a, type: i32}
      - {id: 2, name: b, type: i32}
    output:
      - {id: 1, name: sum, type: i32}

notifies:
  - name: Alarm
    id: 0x00002001
    params:
      - {id: 1, name: code, type: u32}

nodes:
  - name: mcu
    provides: [Add]
    publishes:
      - {notify: Alarm, links: [0]}
  - name: gateway
    subscribes:
      - {notify: Alarm, publisher: 0x0B}
'''


NEW_NET = '''# 网络描述，格式见 docs/records/XRPC代码框架设计.md
module: demo
crc16: ccitt_false

nodes:
  - {name: gateway, id: 0x0A, platform: posix}
  - {name: mcu, id: 0x0B}

links:
  - name: uart
    connect:
      gateway: {name: uart0, type: serial, device: /dev/ttyUSB0}
      mcu: {name: uart0}

processes:
  - name: Add
    id: 0x00001001
    provider: mcu
    callers: [gateway]
    input:
      - {id: 1, name: a, type: i32}
      - {id: 2, name: b, type: i32}
    output:
      - {id: 1, name: sum, type: i32}
'''


def parse_int(text, label):
    try:
        return int(text.strip(), 0)
    except ValueError:
        raise ConfigError('%s 要填整数，十六进制写 0x 开头：%s' % (label, text))


class PortTab:
    """适配代码：节点配置、链路表、生成"""

    FIELDS = (
        ('node_id', '节点 ID', 'entry', None, '网络内唯一，不能是 0 或 0xFFFFFFFF'),
        ('protocol_version', '协议版本', 'entry', None, '必须与对端一致'),
        ('platform', '平台', 'combo', port_gen.PLATFORMS, 'win32、posix 自动实现睡眠、tick、锁；'
                                                         'custom 用于 RTOS 或裸机'),
        ('threads', '线程模型', 'combo', ('multi', 'single'), '同步调用要有独立轮询线程，选 multi'),
        ('crc16', 'CRC16', 'combo', port_gen.CRC_TYPES, 'custom 由你实现；ccitt_false 不代表与现网一致；'
                                                        'fixed 只能本机联调'),
        ('call_slots', '调用槽个数', 'entry', None, '0 表示不启用，也就不能用异步调用'),
        ('frame_size', '帧缓冲大小', 'entry', None, '决定最大帧长，业务 2500 字节时建议 2600'),
        ('recv_size', '单次读取缓冲', 'entry', None, '所有链路共用'),
    )
    LINK_COLUMNS = (('name', '名字', 90), ('type', '类型', 80), ('topology', '拓扑', 60),
                    ('peer_node_id', '对端 ID', 80), ('detail', '其他', 240))

    def __init__(self, app, parent):
        self.app = app
        self.frame = ttk.Frame(parent, padding=8)
        self.links = []
        self.vars = {}
        self.link_vars = {}
        self.force = tk.BooleanVar(value=False)
        self.out_dir = tk.StringVar(value=os.path.join(os.getcwd(), 'xrpc_port'))
        self._build_node(self.frame)
        self._build_links(self.frame)
        self._build_actions(self.frame)
        self.frame.columnconfigure(0, weight=1)
        self.frame.rowconfigure(1, weight=1)
        self.load_values({'node_id': 0x0B, 'links': [{'name': 'uart0'}]})

    def _build_node(self, parent):
        box = ttk.LabelFrame(parent, text='节点', padding=8)
        box.grid(row=0, column=0, sticky='ew')
        box.columnconfigure(2, weight=1)
        for row, (key, label, kind, choices, hint) in enumerate(self.FIELDS):
            ttk.Label(box, text=label).grid(row=row, column=0, sticky='w', padx=(0, 8), pady=1)
            var = tk.StringVar()
            self.vars[key] = var
            if kind == 'combo':
                widget = ttk.Combobox(box, textvariable=var, values=choices, state='readonly',
                                      width=16)
            else:
                widget = ttk.Entry(box, textvariable=var, width=18)
            widget.grid(row=row, column=1, sticky='w', pady=1)
            ttk.Label(box, text=hint, foreground='#666666').grid(row=row, column=2, sticky='w',
                                                                 padx=(10, 0))

    def _build_links(self, parent):
        box = ttk.LabelFrame(parent, text='链路', padding=8)
        box.grid(row=1, column=0, sticky='nsew', pady=(8, 0))
        box.columnconfigure(0, weight=1)
        box.rowconfigure(0, weight=1)

        self.tree = ttk.Treeview(box, columns=[item[0] for item in self.LINK_COLUMNS],
                                 show='headings', height=4, selectmode='browse')
        for key, title, width in self.LINK_COLUMNS:
            self.tree.heading(key, text=title)
            self.tree.column(key, width=width, anchor='w')
        self.tree.grid(row=0, column=0, sticky='nsew')
        self.tree.bind('<<TreeviewSelect>>', self._on_select_link)

        edit = ttk.Frame(box)
        edit.grid(row=1, column=0, sticky='ew', pady=(6, 0))
        entries = (('name', '名字', 'entry', None, 10),
                   ('type', '类型', 'combo', port_gen.LINK_TYPES, 9),
                   ('topology', '拓扑', 'combo', port_gen.TOPOLOGIES, 5),
                   ('peer_node_id', '对端 ID', 'entry', None, 10),
                   ('max_frame', '单帧上限', 'entry', None, 7),
                   ('remote_capacity', '总线节点数', 'entry', None, 5),
                   ('pipe', '管道名', 'entry', None, 14),
                   ('host', '主机', 'entry', None, 14),
                   ('port', '端口', 'entry', None, 7),
                   ('device', '串口设备', 'entry', None, 14),
                   ('baud', '波特率', 'combo', port_gen.BAUD_RATES, 8))
        self.link_widgets = {}
        for index, (key, label, kind, choices, width) in enumerate(entries):
            row = index // 4
            column = (index % 4) * 2
            ttk.Label(edit, text=label).grid(row=row, column=column, sticky='w', padx=(8, 4), pady=1)
            var = tk.StringVar()
            self.link_vars[key] = var
            if kind == 'combo':
                widget = ttk.Combobox(edit, textvariable=var, values=choices, state='readonly',
                                      width=width)
                widget.bind('<<ComboboxSelected>>', lambda _: self._sync_link_fields())
            else:
                widget = ttk.Entry(edit, textvariable=var, width=width)
            widget.grid(row=row, column=column + 1, sticky='w', pady=1)
            self.link_widgets[key] = widget
        self.link_server = tk.BooleanVar(value=True)
        self.link_advertise = tk.BooleanVar(value=False)
        self.server_check = ttk.Checkbutton(edit, text='本端作服务端', variable=self.link_server)
        self.server_check.grid(row=3, column=0, columnspan=3, sticky='w', padx=(8, 0))
        ttk.Checkbutton(edit, text='Hello 通告接收上限', variable=self.link_advertise) \
            .grid(row=3, column=3, columnspan=3, sticky='w', padx=(8, 0))

        buttons = ttk.Frame(box)
        buttons.grid(row=2, column=0, sticky='e', pady=(6, 0))
        ttk.Button(buttons, text='添加', command=self.add_link).pack(side='left')
        ttk.Button(buttons, text='更新选中', command=self.update_link).pack(side='left', padx=4)
        ttk.Button(buttons, text='删除选中', command=self.delete_link).pack(side='left')

    def _build_actions(self, parent):
        box = ttk.Frame(parent)
        box.grid(row=2, column=0, sticky='ew', pady=(8, 0))
        box.columnconfigure(1, weight=1)
        ttk.Label(box, text='输出目录').grid(row=0, column=0, padx=(0, 6))
        ttk.Entry(box, textvariable=self.out_dir).grid(row=0, column=1, sticky='ew')
        ttk.Button(box, text='浏览', command=self._browse).grid(row=0, column=2, padx=4)

        row = ttk.Frame(box)
        row.grid(row=1, column=0, columnspan=3, sticky='e', pady=(6, 0))
        ttk.Button(row, text='打开配置', command=self.open_config).pack(side='left')
        ttk.Button(row, text='保存配置', command=self.save_config).pack(side='left', padx=4)
        ttk.Checkbutton(row, text='覆盖已有文件', variable=self.force).pack(side='left', padx=8)
        ttk.Button(row, text='生成适配代码', command=self.generate).pack(side='left')

    def _browse(self):
        path = filedialog.askdirectory(initialdir=self.out_dir.get())
        if path:
            self.out_dir.set(path)

    def _sync_link_fields(self):
        """只有相关的字段才能填，避免填了不生效"""
        kind = self.link_vars['type'].get()
        is_bus = self.link_vars['topology'].get() == 'bus'
        self.link_widgets['pipe'].configure(state='normal' if kind == 'win_pipe' else 'disabled')
        self.server_check.configure(state='normal' if kind in ('win_pipe', 'tcp') else 'disabled')
        for key in ('host', 'port'):
            self.link_widgets[key].configure(state='normal' if kind == 'tcp' else 'disabled')
        self.link_widgets['device'].configure(state='normal' if kind == 'serial' else 'disabled')
        self.link_widgets['baud'].configure(state='readonly' if kind == 'serial' else 'disabled')
        self.link_widgets['remote_capacity'].configure(state='normal' if is_bus else 'disabled')

    def _link_from_form(self):
        link = {
            'name': self.link_vars['name'].get().strip(),
            'type': self.link_vars['type'].get(),
            'topology': self.link_vars['topology'].get(),
            'peer_node_id': parse_int(self.link_vars['peer_node_id'].get() or '0', '对端 ID'),
            'max_frame': parse_int(self.link_vars['max_frame'].get() or '0', '单帧上限'),
            'advertise_max_recv': bool(self.link_advertise.get()),
        }
        if link['topology'] == 'bus':
            link['remote_capacity'] = parse_int(self.link_vars['remote_capacity'].get() or '8',
                                                '总线节点数')
        if link['type'] == 'win_pipe':
            link['pipe'] = self.link_vars['pipe'].get().strip()
            link['server'] = bool(self.link_server.get())
        if link['type'] == 'tcp':
            link['host'] = self.link_vars['host'].get().strip() or '127.0.0.1'
            link['port'] = parse_int(self.link_vars['port'].get() or '0', '端口')
            link['server'] = bool(self.link_server.get())
        if link['type'] == 'serial':
            link['device'] = self.link_vars['device'].get().strip()
            link['baud'] = parse_int(self.link_vars['baud'].get() or '115200', '波特率')
        return link

    def _link_to_form(self, link):
        self.link_vars['name'].set(link.get('name', ''))
        self.link_vars['type'].set(link.get('type', 'custom'))
        self.link_vars['topology'].set(link.get('topology', 'p2p'))
        self.link_vars['peer_node_id'].set('0x%X' % link.get('peer_node_id', 0))
        self.link_vars['max_frame'].set(str(link.get('max_frame', 0)))
        self.link_vars['remote_capacity'].set(str(link.get('remote_capacity', 8)))
        self.link_vars['pipe'].set(link.get('pipe', ''))
        self.link_vars['host'].set(link.get('host', ''))
        self.link_vars['port'].set(str(link.get('port', '')))
        self.link_vars['device'].set(link.get('device', ''))
        self.link_vars['baud'].set(str(link.get('baud', 115200)))
        self.link_server.set(link.get('server', True))
        self.link_advertise.set(link.get('advertise_max_recv', False))
        self._sync_link_fields()

    def _refresh_links(self):
        self.tree.delete(*self.tree.get_children())
        for index, link in enumerate(self.links):
            detail = []
            if link.get('pipe'):
                detail.append('管道 %s（%s）' % (link['pipe'], '服务端' if link.get('server') else '客户端'))
            if link.get('port'):
                detail.append('%s:%d（%s）' % (link.get('host', '127.0.0.1'), link['port'],
                                              '服务端' if link.get('server') else '客户端'))
            if link.get('device'):
                detail.append('%s，%d' % (link['device'], link.get('baud', 115200)))
            if link.get('topology') == 'bus':
                detail.append('最多 %d 个节点' % link.get('remote_capacity', 8))
            if link.get('max_frame'):
                detail.append('单帧 %d' % link['max_frame'])
            if link.get('advertise_max_recv'):
                detail.append('通告上限')
            self.tree.insert('', 'end', iid=str(index), values=(
                link.get('name'), link.get('type', 'custom'), link.get('topology', 'p2p'),
                '0x%X' % link.get('peer_node_id', 0), '，'.join(detail)))

    def _on_select_link(self, _):
        selected = self.tree.selection()
        if selected:
            self._link_to_form(self.links[int(selected[0])])

    def add_link(self):
        self.app.guard(self._add_link)

    def _add_link(self):
        self.links.append(self._link_from_form())
        self._refresh_links()

    def update_link(self):
        self.app.guard(self._update_link)

    def _update_link(self):
        selected = self.tree.selection()
        if not selected:
            raise ConfigError('先在表里选中一条链路')
        self.links[int(selected[0])] = self._link_from_form()
        self._refresh_links()

    def delete_link(self):
        selected = self.tree.selection()
        if selected:
            del self.links[int(selected[0])]
            self._refresh_links()

    def collect(self):
        """把表单收集成配置，数字项校验在这里，其余交给 port_gen.validate"""
        raw = {
            'node_id': parse_int(self.vars['node_id'].get(), '节点 ID'),
            'protocol_version': parse_int(self.vars['protocol_version'].get(), '协议版本'),
            'platform': self.vars['platform'].get(),
            'threads': self.vars['threads'].get(),
            'crc16': self.vars['crc16'].get(),
            'call_slots': parse_int(self.vars['call_slots'].get(), '调用槽个数'),
            'frame_size': parse_int(self.vars['frame_size'].get(), '帧缓冲大小'),
            'recv_size': parse_int(self.vars['recv_size'].get(), '单次读取缓冲'),
            'links': [dict(link) for link in self.links],
        }
        return raw

    def load_values(self, raw):
        values = dict(port_gen.DEFAULTS)
        values.update(raw)
        self.vars['node_id'].set('0x%X' % (values['node_id'] or 0))
        self.vars['protocol_version'].set('0x%04X' % values['protocol_version'])
        for key in ('platform', 'threads', 'crc16', 'call_slots', 'frame_size', 'recv_size'):
            self.vars[key].set(str(values[key]))
        self.links = [dict(link) for link in (values.get('links') or [])]
        self._refresh_links()
        self._link_to_form(self.links[0] if self.links else {})

    def open_config(self):
        path = filedialog.askopenfilename(filetypes=[('配置文件', '*.yaml *.yml *.json')])
        if path:
            self.app.guard(lambda: self._open(path))

    def _open(self, path):
        self.load_values(load_config(path))
        self.app.log('已读取 %s' % path)

    def save_config(self):
        path = filedialog.asksaveasfilename(defaultextension='.yaml', initialfile='xrpc_port.yaml',
                                            filetypes=[('YAML', '*.yaml')])
        if path:
            self.app.guard(lambda: self._save(path))

    def _save(self, path):
        raw = self.collect()
        port_gen.validate(self.collect())
        write_text(path, port_gen.yaml_of(raw))
        self.app.log('已保存 %s' % path)

    def generate(self):
        self.app.guard(self._generate)

    def _generate(self):
        config = port_gen.validate(self.collect())
        out_dir = self.out_dir.get().strip()
        if not out_dir:
            raise ConfigError('输出目录不能为空')
        written, kept = port_gen.generate(config, out_dir, bool(self.force.get()))
        for name in written:
            self.app.log('生成 %s' % name)
        for name in kept:
            self.app.log('保留 %s（已存在，勾选"覆盖已有文件"才覆盖）' % name)
        self.app.check_tab.directory.set(out_dir)
        count = self.app.check_tab.refresh()
        self.app.log('完成：%s，还有 %d 项虚接口要实现' % (out_dir, count), 'ok')


class CheckTab:
    """虚接口检查：列出还没实现的 XRPC_PORT_TODO"""

    def __init__(self, app, parent):
        self.app = app
        self.frame = ttk.Frame(parent, padding=8)
        self.directory = tk.StringVar()
        self.summary = tk.StringVar(value='选择适配目录后点"检查"。')
        self.rows = []

        top = ttk.Frame(self.frame)
        top.pack(fill='x')
        ttk.Label(top, text='适配目录').pack(side='left', padx=(0, 6))
        ttk.Entry(top, textvariable=self.directory).pack(side='left', fill='x', expand=True)
        ttk.Button(top, text='浏览', command=self._browse).pack(side='left', padx=4)
        ttk.Button(top, text='检查', command=self.check).pack(side='left')

        ttk.Label(self.frame, textvariable=self.summary).pack(anchor='w', pady=(8, 4))
        self.tree = ttk.Treeview(self.frame, columns=('tag', 'file', 'message'), show='headings')
        for key, title, width in (('tag', '虚接口', 100), ('file', '位置', 170),
                                  ('message', '要做什么', 560)):
            self.tree.heading(key, text=title)
            self.tree.column(key, width=width, anchor='w')
        self.tree.pack(fill='both', expand=True)
        self.tree.bind('<Double-1>', self._open_row)
        ttk.Label(self.frame, foreground='#666666',
                  text='双击打开文件。实现之后删掉对应的 #ifndef XRPC_PORT_ALLOW_TODO 块，再点"检查"。') \
            .pack(anchor='w', pady=(6, 0))

    def _browse(self):
        path = filedialog.askdirectory(initialdir=self.directory.get() or None)
        if path:
            self.directory.set(path)
            self.check()

    def check(self):
        self.app.guard(self.refresh)

    def refresh(self):
        directory = self.directory.get().strip()
        if not os.path.isfile(os.path.join(directory, 'xrpc_port_node.h')):
            raise ConfigError('%s 不是生成的适配目录（找不到 xrpc_port.h）' % directory)
        self.rows = port_gen.find_todos(directory)
        self.tree.delete(*self.tree.get_children())
        for index, (path, number, tag, message) in enumerate(self.rows):
            self.tree.insert('', 'end', iid=str(index),
                             values=(tag, '%s:%d' % (path, number), message))
        if self.rows:
            self.summary.set('还有 %d 项没实现。' % len(self.rows))
        else:
            self.summary.set('全部虚接口都已实现。')
        return len(self.rows)

    def _open_row(self, _):
        selected = self.tree.selection()
        if selected and hasattr(os, 'startfile'):
            path = os.path.join(self.directory.get(), self.rows[int(selected[0])][0])
            os.startfile(path)


class ApiTab:
    """接口代码：编辑定义、校验、按节点生成"""

    def __init__(self, app, parent):
        self.app = app
        self.frame = ttk.Frame(parent, padding=8)
        self.path = tk.StringVar()
        self.node = tk.StringVar()
        self.out_dir = tk.StringVar(value=os.path.join(os.getcwd(), 'xrpc_api'))

        top = ttk.Frame(self.frame)
        top.pack(fill='x')
        ttk.Label(top, text='定义文件').pack(side='left', padx=(0, 6))
        ttk.Entry(top, textvariable=self.path).pack(side='left', fill='x', expand=True)
        ttk.Button(top, text='打开', command=self.open_spec).pack(side='left', padx=4)
        ttk.Button(top, text='保存', command=self.save_spec).pack(side='left')
        ttk.Button(top, text='新建示例', command=self.new_spec).pack(side='left', padx=(4, 0))

        body = ttk.Frame(self.frame)
        body.pack(fill='both', expand=True, pady=(8, 0))
        self.text = tk.Text(body, wrap='none', undo=True, font=('Consolas', 10), height=16)
        scroll = ttk.Scrollbar(body, command=self.text.yview)
        self.text.configure(yscrollcommand=scroll.set)
        self.text.pack(side='left', fill='both', expand=True)
        scroll.pack(side='left', fill='y')

        row = ttk.Frame(self.frame)
        row.pack(fill='x', pady=(8, 0))
        ttk.Button(row, text='校验', command=self.validate).pack(side='left')
        ttk.Label(row, text='节点').pack(side='left', padx=(12, 6))
        self.node_box = ttk.Combobox(row, textvariable=self.node, state='readonly', width=14)
        self.node_box.pack(side='left')
        ttk.Label(row, text='输出目录').pack(side='left', padx=(12, 6))
        ttk.Entry(row, textvariable=self.out_dir).pack(side='left', fill='x', expand=True)
        ttk.Button(row, text='浏览', command=self._browse).pack(side='left', padx=4)
        ttk.Button(row, text='生成接口代码', command=self.generate).pack(side='left')
        self.new_spec()

    def _browse(self):
        path = filedialog.askdirectory(initialdir=self.out_dir.get())
        if path:
            self.out_dir.set(path)

    def get_text(self):
        return self.text.get('1.0', 'end-1c')

    def set_text(self, text):
        self.text.delete('1.0', 'end')
        self.text.insert('1.0', text)

    def new_spec(self):
        self.path.set('')
        self.set_text(NEW_SPEC)

    def open_spec(self):
        path = filedialog.askopenfilename(filetypes=[('接口定义', '*.yaml *.yml')])
        if path:
            self.app.guard(lambda: self.load(path))

    def load(self, path):
        try:
            with open(path, encoding='utf-8') as stream:
                self.set_text(stream.read())
        except OSError as error:
            raise ConfigError('读不了 %s：%s' % (path, error))
        self.path.set(path)
        self.parse()
        self.app.log('已读取 %s' % path)

    def save_spec(self):
        path = self.path.get().strip() or filedialog.asksaveasfilename(
            defaultextension='.yaml', filetypes=[('YAML', '*.yaml')])
        if path:
            self.app.guard(lambda: self._save(path))

    def _save(self, path):
        write_text(path, self.get_text())
        self.path.set(path)
        self.app.log('已保存 %s' % path)

    def parse(self):
        """校验编辑框里的内容，并刷新节点列表"""
        spec = api_gen.parse_spec(parse_yaml(self.get_text()))
        names = list(spec['nodes'])
        self.node_box.configure(values=names)
        if self.node.get() not in names:
            self.node.set(names[0] if names else '')
        return spec

    def validate(self):
        self.app.guard(self._validate)

    def _validate(self):
        for line in api_gen.summary_lines(self.parse()):
            self.app.log(line)
        self.app.log('定义没有问题', 'ok')

    def generate(self):
        self.app.guard(self._generate)

    def _generate(self):
        spec = self.parse()
        node = self.node.get()
        out_dir = self.out_dir.get().strip()
        if not node:
            raise ConfigError('定义里没有节点')
        if not out_dir:
            raise ConfigError('输出目录不能为空')
        for line in api_gen.generate_node(spec, node, out_dir):
            self.app.log(line)
        self.app.log('完成：节点 %s，输出到 %s' % (node, out_dir), 'ok')


class NetTab:
    """全网生成：编辑网络描述、检查、生成全部节点或 PC 仿真"""

    COLUMNS = (('name', '节点', 90), ('id', 'ID', 100), ('relay', '中继', 50),
               ('links', '链路', 240), ('frame', '帧缓冲', 70))

    def __init__(self, app, parent):
        self.app = app
        self.frame = ttk.Frame(parent, padding=8)
        self.path = tk.StringVar()
        self.out_dir = tk.StringVar(value=os.path.join(os.getcwd(), 'xrpc_net'))
        self.base_port = tk.StringVar(value=str(net_gen.SIM_BASE_PORT))

        top = ttk.Frame(self.frame)
        top.pack(fill='x')
        ttk.Label(top, text='网络描述').pack(side='left', padx=(0, 6))
        ttk.Entry(top, textvariable=self.path).pack(side='left', fill='x', expand=True)
        ttk.Button(top, text='打开', command=self.open_net).pack(side='left', padx=4)
        ttk.Button(top, text='保存', command=self.save_net).pack(side='left')
        ttk.Button(top, text='新建示例', command=self.new_net).pack(side='left', padx=(4, 0))

        body = ttk.Frame(self.frame)
        body.pack(fill='both', expand=True, pady=(8, 0))
        self.text = tk.Text(body, wrap='none', undo=True, font=('Consolas', 10), height=12)
        scroll = ttk.Scrollbar(body, command=self.text.yview)
        self.text.configure(yscrollcommand=scroll.set)
        self.text.pack(side='left', fill='both', expand=True)
        scroll.pack(side='left', fill='y')

        self.tree = ttk.Treeview(self.frame, columns=[item[0] for item in self.COLUMNS],
                                 show='headings', height=5, selectmode='none')
        for key, title, width in self.COLUMNS:
            self.tree.heading(key, text=title)
            self.tree.column(key, width=width, anchor='w')
        self.tree.pack(fill='x', pady=(8, 0))

        row = ttk.Frame(self.frame)
        row.pack(fill='x', pady=(8, 0))
        ttk.Button(row, text='检查', command=self.check).pack(side='left')
        ttk.Label(row, text='输出目录').pack(side='left', padx=(12, 6))
        ttk.Entry(row, textvariable=self.out_dir).pack(side='left', fill='x', expand=True)
        ttk.Button(row, text='浏览', command=self._browse).pack(side='left', padx=4)
        ttk.Button(row, text='生成全部节点', command=self.generate).pack(side='left')
        ttk.Label(row, text='起始端口').pack(side='left', padx=(12, 6))
        ttk.Entry(row, textvariable=self.base_port, width=7).pack(side='left')
        ttk.Button(row, text='生成 PC 仿真', command=self.generate_sim).pack(side='left', padx=(4, 0))
        self.new_net()

    def _browse(self):
        path = filedialog.askdirectory(initialdir=self.out_dir.get())
        if path:
            self.out_dir.set(path)

    def get_text(self):
        return self.text.get('1.0', 'end-1c')

    def set_text(self, text):
        self.text.delete('1.0', 'end')
        self.text.insert('1.0', text)

    def new_net(self):
        self.set_text(NEW_NET)
        self.path.set('')

    def open_net(self):
        path = filedialog.askopenfilename(filetypes=[('网络描述', '*.yaml *.yml *.json')])
        if path:
            self.app.guard(lambda: self.load(path))

    def load(self, path):
        with open(path, encoding='utf-8') as stream:
            self.set_text(stream.read())
        self.path.set(path)
        self.app.log('已打开 %s' % path)

    def save_net(self):
        path = self.path.get().strip() or filedialog.asksaveasfilename(defaultextension='.yaml')
        if path:
            self.app.guard(lambda: self._save(path))

    def _save(self, path):
        write_text(path, self.get_text())
        self.path.set(path)
        self.app.log('已保存 %s' % path)

    def parse(self):
        """检查编辑框里的内容，并刷新节点表"""
        raw = parse_yaml(self.get_text())
        if not isinstance(raw, dict):
            raise ConfigError('网络描述顶层应该是映射')
        net, warnings = parse_net(raw)
        self.tree.delete(*self.tree.get_children())
        for name, node in net['nodes'].items():
            item = net['derived'][name]
            self.tree.insert('', 'end', values=(
                name, '0x%08X' % node['id'], '是' if item['is_relay'] else '否',
                '、'.join(link['name'] for link in node['links']),
                item['capacity']['frame_size']))
        for text in warnings:
            self.app.log('警告：%s' % text)
        return raw, net

    def check(self):
        self.app.guard(self._check)

    def _check(self):
        _, net = self.parse()
        self.app.log('检查通过：%d 个节点，%d 条链路，%d 个过程，%d 个事件'
                     % (len(net['nodes']), len(net['links']), len(net['processes']),
                        len(net['notifies'])), 'ok')

    def _output(self):
        out_dir = self.out_dir.get().strip()
        if not out_dir:
            raise ConfigError('输出目录不能为空')
        return out_dir

    def generate(self):
        self.app.guard(self._generate)

    def _generate(self):
        raw, net = self.parse()
        out_dir = self._output()
        lines = net_gen.generate(raw, net, out_dir, None)
        self.app.log('完成：%d 个节点、%d 个文件，输出到 %s，网络报告在 report.md'
                     % (len(net['nodes']), len(lines), out_dir), 'ok')

    def generate_sim(self):
        self.app.guard(self._generate_sim)

    def _generate_sim(self):
        raw, net = self.parse()
        out_dir = self._output()
        lines = net_gen.generate_sim(raw, net, out_dir, parse_int(self.base_port.get(), '起始端口'))
        self.app.log('完成：%d 个文件，用 cmake 构建 %s' % (len(lines), out_dir), 'ok')


class App:
    def __init__(self, root):
        self.root = root
        root.title('XRPC 代码生成工具')
        root.geometry('860x820')
        root.minsize(760, 640)

        notebook = ttk.Notebook(root)
        notebook.pack(fill='both', expand=True, padx=8, pady=(8, 4))
        self.notebook = notebook

        log_frame = ttk.LabelFrame(root, text='输出', padding=4)
        log_frame.pack(fill='x', padx=8, pady=(0, 8))
        self.log_text = tk.Text(log_frame, height=7, state='disabled', wrap='word',
                                font=('Consolas', 9))
        scroll = ttk.Scrollbar(log_frame, command=self.log_text.yview)
        self.log_text.configure(yscrollcommand=scroll.set)
        self.log_text.pack(side='left', fill='both', expand=True)
        scroll.pack(side='left', fill='y')
        self.log_text.tag_configure('error', foreground='#B00020')
        self.log_text.tag_configure('ok', foreground='#1B7F3B')

        self.port_tab = PortTab(self, notebook)
        self.check_tab = CheckTab(self, notebook)
        self.api_tab = ApiTab(self, notebook)
        notebook.add(self.port_tab.frame, text='1 适配代码')
        notebook.add(self.check_tab.frame, text='2 虚接口检查')
        self.net_tab = NetTab(self, notebook)
        notebook.add(self.api_tab.frame, text='3 接口代码')
        notebook.add(self.net_tab.frame, text='4 全网生成')

    def log(self, text, tag=None):
        self.log_text.configure(state='normal')
        self.log_text.insert('end', text + '\n', tag or ())
        self.log_text.see('end')
        self.log_text.configure(state='disabled')

    def guard(self, action):
        """配置错误显示在输出区，不弹窗，也不让界面崩掉"""
        try:
            action()
        except ConfigError as error:
            self.log('错误：%s' % error, 'error')
        except OSError as error:
            self.log('错误：%s' % error, 'error')


def main():
    if sys.platform == 'win32':
        try:
            import ctypes
            ctypes.windll.shcore.SetProcessDpiAwareness(1)
        except (AttributeError, OSError):
            pass
    root = tk.Tk()
    App(root)
    root.mainloop()
    return 0


if __name__ == '__main__':
    sys.exit(main())
