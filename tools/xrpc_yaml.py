# Copyright (c) 2026 AuyvJpz
"""两个生成工具共用的 YAML 子集解析器，只依赖标准库。

支持：缩进映射、列表（含 "- key: value"）、标量（十进制、0x 十六进制、true/false、
null、带引号字符串）、行内 [a, b] 与 {k: v}、# 注释。
不支持：锚点、多文档、多行字符串。配置文件用不到它们。
"""

import json
import os
import re


def component_version():
    """组件版本，读核心库头文件，工具不另写一份"""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'common', 'include',
                        'xrpc', 'xrpc_common.h')
    with open(path, encoding='utf-8') as stream:
        text = stream.read()
    return '.'.join(re.search(r'XRPC_VERSION_%s\s+\((\d+)\)' % part, text).group(1)
                    for part in ('MAJOR', 'MINOR', 'PATCH'))


class ConfigError(Exception):
    """配置有误。信息直接给使用者看，不打印调用栈。"""


def _strip_comment(text):
    quote = None
    for i, ch in enumerate(text):
        if quote is not None:
            if ch == quote:
                quote = None
        elif ch in '"\'':
            quote = ch
        elif ch == '#' and ((i == 0) or text[i - 1].isspace()):
            return text[:i]
    return text


def _split_top(text, sep):
    """按顶层分隔符切分，跳过引号和括号内部"""
    parts = []
    depth = 0
    quote = None
    current = ''
    for ch in text:
        if quote is not None:
            if ch == quote:
                quote = None
        elif ch in '"\'':
            quote = ch
        elif ch in '[{':
            depth += 1
        elif ch in ']}':
            depth -= 1
        elif (ch == sep) and (depth == 0):
            parts.append(current)
            current = ''
            continue
        current += ch
    if current.strip():
        parts.append(current)
    return parts


def _find_colon(text):
    """找 key: value 的冒号，没有返回 -1"""
    quote = None
    for i, ch in enumerate(text):
        if quote is not None:
            if ch == quote:
                quote = None
        elif ch in '"\'':
            quote = ch
        elif (ch == ':') and ((i + 1 == len(text)) or (text[i + 1] == ' ')):
            return i
    return -1


def _scalar(text, line_no):
    text = text.strip()
    result = text
    if text.startswith('[') or text.startswith('{'):
        result = _flow(text, line_no)
    elif (len(text) >= 2) and (text[0] in '"\'') and (text[-1] == text[0]):
        result = text[1:-1]
    elif text in ('', '~', 'null'):
        result = None
    elif text == 'true':
        result = True
    elif text == 'false':
        result = False
    elif re.fullmatch(r'0[xX][0-9a-fA-F]+', text):
        result = int(text, 16)
    elif re.fullmatch(r'-?[0-9]+', text):
        result = int(text)
    return result


def _flow(text, line_no):
    inner = text[1:-1]
    if (text[0] == '[') and (text[-1] == ']'):
        result = [_scalar(item, line_no) for item in _split_top(inner, ',')]
    elif (text[0] == '{') and (text[-1] == '}'):
        result = {}
        for item in _split_top(inner, ','):
            colon = _find_colon(item.strip())
            if colon < 0:
                raise ConfigError('第 %d 行：行内映射缺少冒号：%s' % (line_no, item.strip()))
            result[item.strip()[:colon].strip()] = _scalar(item.strip()[colon + 1:], line_no)
    else:
        raise ConfigError('第 %d 行：括号不配对：%s' % (line_no, text))
    return result


class _Parser:
    def __init__(self, text):
        self.lines = []
        for number, raw in enumerate(text.splitlines(), 1):
            if '\t' in raw[:len(raw) - len(raw.lstrip())]:
                raise ConfigError('第 %d 行：缩进不能用 Tab' % number)
            body = _strip_comment(raw).rstrip()
            if body.strip() and (body.strip() != '---'):
                self.lines.append((len(body) - len(body.lstrip()), body.strip(), number))
        self.pos = 0

    def parse(self):
        if not self.lines:
            return {}
        value = self._block(self.lines[0][0])
        if self.pos < len(self.lines):
            raise ConfigError('第 %d 行：缩进有误' % self.lines[self.pos][2])
        return value

    def _block(self, indent):
        if self.lines[self.pos][1].startswith('- ') or (self.lines[self.pos][1] == '-'):
            return self._sequence(indent)
        return self._mapping(indent)

    def _sequence(self, indent):
        items = []
        while self.pos < len(self.lines):
            cur_indent, text, number = self.lines[self.pos]
            if (cur_indent != indent) or not (text.startswith('- ') or (text == '-')):
                break
            rest = text[1:].strip()
            if rest == '':
                self.pos += 1
                items.append(self._block(self.lines[self.pos][0]))
            elif (_find_colon(rest) > 0) and (rest[0] not in '[{"\''):
                # "- key: value"：把 "- " 当缩进，后面同缩进的键属于同一个映射
                item_indent = indent + (len(text) - len(rest))
                self.lines[self.pos] = (item_indent, rest, number)
                items.append(self._mapping(item_indent))
            else:
                self.pos += 1
                items.append(_scalar(rest, number))
        return items

    def _mapping(self, indent):
        result = {}
        while self.pos < len(self.lines):
            cur_indent, text, number = self.lines[self.pos]
            if cur_indent < indent:
                break
            if cur_indent > indent:
                raise ConfigError('第 %d 行：缩进有误' % number)
            if text.startswith('- '):
                break
            colon = _find_colon(text)
            if colon <= 0:
                raise ConfigError('第 %d 行：应为 key: value：%s' % (number, text))
            key = text[:colon].strip()
            if key in result:
                raise ConfigError('第 %d 行：重复的键 %s' % (number, key))
            rest = text[colon + 1:].strip()
            self.pos += 1
            if rest:
                result[key] = _scalar(rest, number)
            elif (self.pos < len(self.lines)) and (
                    (self.lines[self.pos][0] > indent)
                    or ((self.lines[self.pos][0] == indent)
                        and self.lines[self.pos][1].startswith('- '))):
                result[key] = self._block(self.lines[self.pos][0])
            else:
                result[key] = None
        return result


def parse_yaml(text):
    return _Parser(text).parse()


def load_config(path):
    """读取 .yaml / .yml / .json 配置"""
    try:
        with open(path, encoding='utf-8') as stream:
            text = stream.read()
    except OSError as error:
        raise ConfigError('读不了配置文件 %s：%s' % (path, error))
    try:
        if os.path.splitext(path)[1].lower() == '.json':
            return json.loads(text)
        return parse_yaml(text)
    except json.JSONDecodeError as error:
        raise ConfigError('%s：%s' % (path, error))
    except ConfigError as error:
        raise ConfigError('%s：%s' % (path, error))


def write_text(path, text):
    os.makedirs(os.path.dirname(path) or '.', exist_ok=True)
    with open(path, 'w', encoding='utf-8', newline='\n') as stream:
        stream.write(text)
