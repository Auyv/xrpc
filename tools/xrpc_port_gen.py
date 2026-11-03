#!/usr/bin/env python3
# Copyright (c) 2026 AuyvJpz
"""XRPC 组件适配代码生成工具。

生成部署一个节点需要的全部适配代码，并引导使用者实现组件依赖的虚接口：
链路 IXrpcLink、CRC16 IXrpcCrc16、锁 TXrpcLockOps、睡眠与 tick、日志。

生成物不包含 XRPC 核心库的编译：xrpc_port.cmake 只列适配源码和头文件路径，
核心库由使用者自行编译（见 tools/README.md）。

    python xrpc_port_gen.py wizard --out my_port          问答式生成
    python xrpc_port_gen.py generate port.yaml --out my_port
    python xrpc_port_gen.py check my_port                 检查还有哪些虚接口没实现
"""

import argparse
import os
import re
import sys

from xrpc_yaml import ConfigError, component_version, load_config, write_text

PLATFORMS = ('win32', 'posix', 'custom')
CRC_TYPES = ('custom', 'ccitt_false', 'fixed')
LINK_TYPES = ('custom', 'win_pipe', 'tcp', 'serial')
TOPOLOGIES = ('p2p', 'bus')
BAUD_RATES = (9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600)

DEFAULTS = {
    'node_id': None,
    'protocol_version': 0x0200,
    'user_tag': 0,
    'platform': 'custom',
    'threads': 'multi',
    'crc16': 'custom',
    'call_slots': 4,
    'frame_size': 2600,
    'recv_size': 1024,
}

TODO_RE = re.compile(r'XRPC_PORT_TODO\(([^)]+)\):\s*([^"]*)')


def has_poll_thread(config):
    """win32、posix 多线程时才生成轮询线程的辅助函数"""
    return (config['platform'] != 'custom') and (config['threads'] == 'multi')


def todo(tag, message):
    """未实现的虚接口：默认编译失败，定义 XRPC_PORT_ALLOW_TODO 可以先编译通过"""
    return ('#ifndef XRPC_PORT_ALLOW_TODO\n'
            '#error "XRPC_PORT_TODO(%s): %s"\n'
            '#endif\n' % (tag, message))


def validate(raw):
    config = dict(DEFAULTS)
    config.update(raw)
    unknown = set(raw) - set(DEFAULTS) - {'links'}
    if unknown:
        raise ConfigError('不认识的配置项：%s' % '、'.join(sorted(unknown)))
    if not isinstance(config['node_id'], int) or isinstance(config['node_id'], bool) \
            or not (0 < config['node_id'] < 0xFFFFFFFF):
        raise ConfigError('node_id 必填，不能是 0 或 0xFFFFFFFF（保留值）')
    if config['platform'] not in PLATFORMS:
        raise ConfigError('platform 只能是：%s' % '、'.join(PLATFORMS))
    if config['crc16'] not in CRC_TYPES:
        raise ConfigError('crc16 只能是：%s' % '、'.join(CRC_TYPES))
    if config['threads'] not in ('multi', 'single'):
        raise ConfigError('threads 只能是 multi 或 single')
    if not isinstance(config['frame_size'], int) or (config['frame_size'] < 64):
        raise ConfigError('frame_size 不能小于 64，它决定拼帧缓存和发送缓存的大小')
    if not isinstance(config['call_slots'], int) or (config['call_slots'] < 0):
        raise ConfigError('call_slots 不能是负数，0 表示不启用调用槽')

    links = config.get('links')
    if not isinstance(links, list) or not links:
        raise ConfigError('links 至少配置一条链路')
    names = set()
    for index, link in enumerate(links):
        where = 'links[%d]' % index
        if not isinstance(link, dict):
            raise ConfigError('%s 应该是映射' % where)
        name = link.get('name')
        if not isinstance(name, str) or not re.fullmatch(r'[a-z][a-z0-9_]*', name):
            raise ConfigError('%s：name 必填，只能用小写字母、数字、下划线' % where)
        if name in names:
            raise ConfigError('%s：链路名 %s 重复' % (where, name))
        names.add(name)
        link.setdefault('type', 'custom')
        link.setdefault('topology', 'p2p')
        link.setdefault('peer_node_id', 0)
        link.setdefault('max_frame', 0)
        link.setdefault('advertise_max_recv', False)
        link.setdefault('remote_capacity', 8)
        if link['type'] not in LINK_TYPES:
            raise ConfigError('%s：type 只能是：%s' % (where, '、'.join(LINK_TYPES)))
        if link['topology'] not in TOPOLOGIES:
            raise ConfigError('%s：topology 只能是 p2p 或 bus' % where)
        if link['type'] == 'win_pipe':
            if config['platform'] != 'win32':
                raise ConfigError('%s：win_pipe 只能用在 platform: win32' % where)
            if not isinstance(link.get('pipe'), str) or ('server' not in link):
                raise ConfigError('%s：win_pipe 必须配置 pipe（管道名）和 server（true/false）'
                                  % where)
            if link['topology'] != 'p2p':
                raise ConfigError('%s：win_pipe 模板只支持 p2p' % where)
        if link['type'] == 'tcp':
            if config['platform'] == 'custom':
                raise ConfigError('%s：tcp 只能用在 platform: win32 或 posix' % where)
            if not isinstance(link.get('port'), int) or ('server' not in link):
                raise ConfigError('%s：tcp 必须配置 port 和 server（true/false）' % where)
            if link['topology'] != 'p2p':
                raise ConfigError('%s：tcp 模板只支持 p2p' % where)
            link.setdefault('host', '127.0.0.1')
        if link['type'] == 'serial':
            if config['platform'] == 'custom':
                raise ConfigError('%s：serial 只能用在 platform: win32 或 posix，'
                                  'MCU 上用 custom 链路自己实现' % where)
            if not isinstance(link.get('device'), str)                     or not re.fullmatch(r'[A-Za-z0-9_./-]+', link['device']):
                raise ConfigError('%s：serial 必须配置 device，例如 COM3 或 /dev/ttyUSB0' % where)
            link.setdefault('baud', 115200)
            if link['baud'] not in BAUD_RATES:
                raise ConfigError('%s：baud 只能是：%s'
                                  % (where, '、'.join(str(item) for item in BAUD_RATES)))
            if link['topology'] != 'p2p':
                raise ConfigError('%s：serial 模板只支持 p2p' % where)
    return config


# ---------- 头文件适配 ----------

def gen_port_headers(config):
    return {'port_include/xrpc_port.h': '''/* 核心库的日志接口，实现在 xrpc_port_platform.c。
 * XRPC_LOG_T 高频，默认不输出；要看可以定义 XRPC_PORT_LOG_TRACE */
#ifndef _XRPC_PORT_INCLUDE_H_
#define _XRPC_PORT_INCLUDE_H_

void XrpcPortLog(char level, const char *fmt, ...);

#if defined(XRPC_PORT_LOG_TRACE)
#define XRPC_LOG_T(...) XrpcPortLog('T', __VA_ARGS__)
#else
#define XRPC_LOG_T(...) ((void)0)
#endif
#define XRPC_LOG_W(...) XrpcPortLog('W', __VA_ARGS__)
#define XRPC_LOG_E(...) XrpcPortLog('E', __VA_ARGS__)

#endif  ///< _XRPC_PORT_INCLUDE_H_
'''}


# ---------- CRC16 ----------

def gen_crc(config):
    kind = config['crc16']
    head = '''/**
 * @file        xrpc_port_crc16.c
 * @brief       CRC16 虚接口 IXrpcCrc16 的实现。
 * @note        frame_crc 是线上协议的一部分：两端算法必须字节级一致，否则互相丢帧。
 *              实现必须可重入，轮询线程和调用线程都会用到。
 */

#include "xrpc_port_node.h"

'''
    if kind == 'custom':
        body = todo('crc16', '在 L_Crc16Calc 里实现 CRC16，算法必须与对端完全一致') + '''
static UInt16 L_Crc16Calc(IXrpcCrc16 *sender, const UInt8 *data, UInt32 size)
{
    (void)sender;
    (void)data;
    (void)size;

    /* 与现网设备互通时转调现有实现，例如 OfCalcCrc16(data, size) */
    return 0U;
}
'''
    elif kind == 'ccitt_false':
        body = '''/* CRC-16/CCITT-FALSE：多项式 0x1021，初值 0xFFFF，无反射。
 * 这只是一个常见算法，不代表与现网设备一致。要和已有设备互通，先用对方的测试向量核对。 */
static UInt16 L_Crc16Calc(IXrpcCrc16 *sender, const UInt8 *data, UInt32 size)
{
    UInt16 crc = 0xFFFFU;
    UInt32 i = 0U;
    UInt32 bit = 0U;

    (void)sender;
    for (i = 0U; i < size; i++)
    {
        crc = (UInt16)(crc ^ ((UInt16)data[i] << 8));
        for (bit = 0U; bit < 8U; bit++)
        {
            crc = ((crc & 0x8000U) != 0U) ? (UInt16)((crc << 1) ^ 0x1021U) : (UInt16)(crc << 1);
        }
    }

    return crc;
}
'''
    else:
        body = '''/* 固定值：等于关闭 CRC 校验，只能用于本机联调，上线前必须换成真实算法 */
static UInt16 L_Crc16Calc(IXrpcCrc16 *sender, const UInt8 *data, UInt32 size)
{
    (void)sender;
    (void)data;
    (void)size;

    return 0x5AA5U;
}
'''
    return {'xrpc_port_crc16.c': head + body + '\nIXrpcCrc16 g_xrpc_port_crc16 = {L_Crc16Calc};\n'}


# ---------- 平台 ----------

LOG_STDIO = '''void XrpcPortLog(char level, const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    printf("[xrpc %c] ", level);
    vprintf(fmt, args);
    printf("\\n");
    va_end(args);
}
'''

PLATFORM_WIN32 = '''#include "xrpc_port_node.h"
#include "xrpc/xrpc.h"

#include <stdarg.h>
#include <stdio.h>
#include <windows.h>

void XrpcSleepMs(UInt32 time_ms)
{
    Sleep(time_ms);
}

UInt32 XrpcGetSysTick(void)
{
    return (UInt32)GetTickCount();
}

''' + LOG_STDIO + '''//@MULTI_BEGIN

static Bool L_LockOn(void *user_data)
{
    EnterCriticalSection((CRITICAL_SECTION *)user_data);

    return True;
}

static Bool L_LockOff(void *user_data)
{
    LeaveCriticalSection((CRITICAL_SECTION *)user_data);

    return True;
}

void XrpcPortLockInit(TXrpcPortLock *self)
{
    static CRITICAL_SECTION s_sections[XRPC_PORT_LOCK_MAX];
    static UInt32 s_used = 0U;

    self->os_lock = &s_sections[s_used];
    s_used++;
    InitializeCriticalSection((CRITICAL_SECTION *)self->os_lock);
    self->ops.lock = L_LockOn;
    self->ops.unlock = L_LockOff;
    self->ops.user_data = self->os_lock;
}

static DWORD WINAPI L_PollThread(LPVOID arg)
{
    (void)arg;
    while (XrpcPortPollShouldRun() == True)
    {
        (void)XrpcPortPoll();
        Sleep(1);
    }

    return 0;
}

Int32 XrpcPortStartPollThread(void)
{
    return (CreateThread(NULL, 0, L_PollThread, NULL, 0, NULL) != NULL) ? kXrpcErrOk
                                                                       : kXrpcErrInvalidParam;
}
//@MULTI_END
'''

PLATFORM_POSIX = '''/* 让 -std=c99 下也能用 nanosleep、clock_gettime */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "xrpc_port_node.h"
#include "xrpc/xrpc.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

void XrpcSleepMs(UInt32 time_ms)
{
    struct timespec ts;

    ts.tv_sec = (time_t)(time_ms / 1000U);
    ts.tv_nsec = (long)(time_ms % 1000U) * 1000000L;
    (void)nanosleep(&ts, NULL);
}

UInt32 XrpcGetSysTick(void)
{
    struct timespec ts;

    (void)clock_gettime(CLOCK_MONOTONIC, &ts);

    return (UInt32)((UInt64)ts.tv_sec * 1000U + (UInt64)ts.tv_nsec / 1000000U);
}

''' + LOG_STDIO + '''//@MULTI_BEGIN

static Bool L_LockOn(void *user_data)
{
    return (pthread_mutex_lock((pthread_mutex_t *)user_data) == 0) ? True : False;
}

static Bool L_LockOff(void *user_data)
{
    return (pthread_mutex_unlock((pthread_mutex_t *)user_data) == 0) ? True : False;
}

void XrpcPortLockInit(TXrpcPortLock *self)
{
    static pthread_mutex_t s_mutexes[XRPC_PORT_LOCK_MAX];
    static UInt32 s_used = 0U;

    self->os_lock = &s_mutexes[s_used];
    s_used++;
    (void)pthread_mutex_init((pthread_mutex_t *)self->os_lock, NULL);
    self->ops.lock = L_LockOn;
    self->ops.unlock = L_LockOff;
    self->ops.user_data = self->os_lock;
}

static void *L_PollThread(void *arg)
{
    (void)arg;
    while (XrpcPortPollShouldRun() == True)
    {
        (void)XrpcPortPoll();
        XrpcSleepMs(1U);
    }

    return NULL;
}

Int32 XrpcPortStartPollThread(void)
{
    pthread_t thread;

    return (pthread_create(&thread, NULL, L_PollThread, NULL) == 0) ? kXrpcErrOk
                                                                   : kXrpcErrInvalidParam;
}
//@MULTI_END
'''


def platform_custom(config):
    text = '#include "xrpc_port_node.h"\n#include "xrpc/xrpc.h"\n\n'
    text += todo('platform:sleep', '实现 XrpcSleepMs：休眠指定毫秒，RTOS 上要让出 CPU')
    text += '''
void XrpcSleepMs(UInt32 time_ms)
{
    (void)time_ms;
}

'''
    text += todo('platform:tick', '实现 XrpcGetSysTick：毫秒单调计数，允许 32 位回绕')
    text += '''
UInt32 XrpcGetSysTick(void)
{
    return 0U;
}

/* 日志默认丢弃。要输出就在这里转给项目的日志系统；
 * 它内部的锁不能和 XRPC 的节点锁、发送锁是同一个对象 */
void XrpcPortLog(char level, const char *fmt, ...)
{
    (void)level;
    (void)fmt;
}
'''
    if config['threads'] == 'multi':
        text += '\n' + todo('platform:lock', '实现 L_LockOn、L_LockOff 和 XrpcPortLockInit，'
                            '每把锁必须是不同对象，不能是递归锁')
        text += '''
static Bool L_LockOn(void *user_data)
{
    (void)user_data;

    return True;
}

static Bool L_LockOff(void *user_data)
{
    (void)user_data;

    return True;
}

void XrpcPortLockInit(TXrpcPortLock *self)
{
    self->os_lock = NULL;       /* 指向一个新建的互斥量 */
    self->ops.lock = L_LockOn;
    self->ops.unlock = L_LockOff;
    self->ops.user_data = self->os_lock;
}
'''
    return text


def gen_platform(config):
    head = '''/**
 * @file        xrpc_port_platform.c
 * @brief       平台适配：睡眠、tick、日志、锁。
 */

'''
    if config['platform'] == 'win32':
        body = PLATFORM_WIN32
    elif config['platform'] == 'posix':
        body = PLATFORM_POSIX
    else:
        body = platform_custom(config)
    if config['threads'] == 'multi':
        body = body.replace('//@MULTI_BEGIN\n', '').replace('//@MULTI_END\n', '')
    else:
        body = re.sub(r'//@MULTI_BEGIN\n.*//@MULTI_END\n', '', body, flags=re.S)
    return {'xrpc_port_platform.c': head + body}


# ---------- 链路 ----------

LINK_CUSTOM = '''/**
 * @file        xrpc_port_link_@NAME@.c
 * @brief       链路 @NAME@ 的 IXrpcLink 实现。
 * @note        四个回调都在轮询线程或调用线程执行，不能阻塞。
 */

#include "xrpc_port_node.h"

/* 在这里加上链路需要的句柄 */
typedef struct TLinkTag TLink;
struct TLinkTag
{
    IXrpcLink link;     /* 必须是第一个成员 */
    TXrpcLinkState state;
};

@TODO_SWITCH@
/* 下发连接或断开的命令即可，结果由 L_OnState 体现。失败也返回 True，靠状态重试 */
static Bool L_OnConnectSwitch(IXrpcLink *sender, Bool connect_switch)
{
    TLink *self = (TLink *)sender;

    self->state = (connect_switch == True) ? kXrpcLinkStateConnected : kXrpcLinkStateDisconnect;

    return True;
}

@TODO_STATE@
/* 只有 Connected 才会收发。Busy 表示暂时忙，不算断开 */
static TXrpcLinkState L_OnState(IXrpcLink *sender)
{
    return ((TLink *)sender)->state;
}

@TODO_SEND@
/* 必须整帧写入：返回 0 或小于 size 都算发送失败。出错返回负数 */
static Int32 L_OnSend(IXrpcLink *sender, const char *data, Int32 size)
{
    (void)sender;
    (void)data;

    return size;
}

@TODO_RECV@
/* 不能阻塞。无数据返回 0，出错返回负数（会触发断开重连） */
static Int32 L_OnRecv(IXrpcLink *sender, char *buf, Int32 size)
{
    (void)sender;
    (void)buf;
    (void)size;

    return 0;
}

static TLink s_link =
{
    {L_OnConnectSwitch, L_OnSend, L_OnRecv, L_OnState},
    kXrpcLinkStateDisconnect
};

IXrpcLink *XrpcPortLink_@NAME@(void)
{
    return &s_link.link;
}
'''

LINK_WIN_PIPE = '''/**
 * @file        xrpc_port_link_@NAME@.c
 * @brief       链路 @NAME@：Windows 命名管道，字节流，PIPE_NOWAIT 不阻塞。
 */

#include "xrpc_port_node.h"

#include <windows.h>

#define PIPE_NAME       "\\\\\\\\.\\\\pipe\\\\@PIPE@"
#define PIPE_IS_SERVER  (@SERVER@)
#define PIPE_BUFF_SIZE  (64 * 1024)

typedef struct TLinkTag TLink;
struct TLinkTag
{
    IXrpcLink link;     /* 必须是第一个成员 */
    HANDLE handle;
    Bool connected;
    TXrpcLinkState state;
};

static void L_Open(TLink *self)
{
    DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;

#if PIPE_IS_SERVER
    HANDLE handle = CreateNamedPipeA(PIPE_NAME, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | mode, 1,
                                     PIPE_BUFF_SIZE, PIPE_BUFF_SIZE, 0, NULL);
#else
    HANDLE handle = CreateFileA(PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                                OPEN_EXISTING, 0, NULL);
#endif

    if (handle != INVALID_HANDLE_VALUE)
    {
#if !PIPE_IS_SERVER
        (void)SetNamedPipeHandleState(handle, &mode, NULL, NULL);
        self->connected = True;
#endif
        self->handle = handle;
    }
}

static void L_Close(TLink *self)
{
    if (self->handle != NULL)
    {
        (void)CloseHandle(self->handle);
        self->handle = NULL;
    }
    self->connected = False;
}

static Bool L_OnConnectSwitch(IXrpcLink *sender, Bool connect_switch)
{
    TLink *self = (TLink *)sender;

    if (connect_switch == True)
    {
        if (self->handle == NULL)
        {
            L_Open(self);
        }
        /* 客户端要等服务端创建管道，没打开就下一轮再试 */
        self->state = (self->handle == NULL) ? kXrpcLinkStateTimeOut
                                             : kXrpcLinkStateConnectPending;
    }
    else
    {
        L_Close(self);
        self->state = kXrpcLinkStateDisconnect;
    }

    return True;
}

static TXrpcLinkState L_OnState(IXrpcLink *sender)
{
    TLink *self = (TLink *)sender;

    if ((PIPE_IS_SERVER != 0) && (self->handle != NULL) && (self->connected == False)
        && (ConnectNamedPipe(self->handle, NULL) == FALSE)
        && (GetLastError() == ERROR_PIPE_CONNECTED))
    {
        self->connected = True;
    }

    if ((self->state == kXrpcLinkStateConnectPending) && (self->connected == True))
    {
        self->state = kXrpcLinkStateConnected;
    }

    return self->state;
}

static Int32 L_OnSend(IXrpcLink *sender, const char *data, Int32 size)
{
    TLink *self = (TLink *)sender;
    DWORD written = 0;
    Int32 result = -1;

    if (WriteFile(self->handle, data, (DWORD)size, &written, NULL) == TRUE)
    {
        result = (Int32)written;
    }

    return result;
}

static Int32 L_OnRecv(IXrpcLink *sender, char *buf, Int32 size)
{
    TLink *self = (TLink *)sender;
    DWORD got = 0;
    Int32 result = 0;

    if (ReadFile(self->handle, buf, (DWORD)size, &got, NULL) == TRUE)
    {
        result = (Int32)got;
    }
    else if (GetLastError() != ERROR_NO_DATA)
    {
        result = -1;    /* 对端退出 */
    }

    return result;
}

static TLink s_link =
{
    {L_OnConnectSwitch, L_OnSend, L_OnRecv, L_OnState},
    NULL, False, kXrpcLinkStateDisconnect
};

IXrpcLink *XrpcPortLink_@NAME@(void)
{
    return &s_link.link;
}
'''


LINK_TCP = '''/**
 * @file        xrpc_port_link_@NAME@.c
 * @brief       链路 @NAME@：TCP，全部非阻塞，不会卡住 RunOnce。
 * @note        断开后 RunOnce 会自动重连。服务端一次只接受一个连接。
 */

#include "xrpc_port_node.h"
#include "xrpc/xrpc.h"

#include <string.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET TSocket;
#define INVALID_SOCK        INVALID_SOCKET
#define L_CloseSocket       closesocket
#define L_WouldBlock()      (WSAGetLastError() == WSAEWOULDBLOCK)
#define L_InProgress()      (WSAGetLastError() == WSAEWOULDBLOCK)
#define XRPC_SEND_FLAGS     (0)
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int TSocket;
#define INVALID_SOCK        (-1)
#define L_CloseSocket       close
#define L_WouldBlock()      ((errno == EAGAIN) || (errno == EWOULDBLOCK))
#define L_InProgress()      (errno == EINPROGRESS)
#define XRPC_SEND_FLAGS     (MSG_NOSIGNAL)
#endif

#define TCP_HOST        "@HOST@"
#define TCP_PORT        (@PORT@)
#define TCP_IS_SERVER   (@SERVER@)
#define TCP_SEND_WAIT_MS (200)

typedef struct TLinkTag TLink;
struct TLinkTag
{
    IXrpcLink link;     /* 必须是第一个成员 */
    TSocket listener;
    TSocket sock;
    Bool connecting;
    TXrpcLinkState state;
};

static void L_NetInit(void)
{
#if defined(_WIN32)
    static Bool s_done = False;

    if (s_done == False)
    {
        WSADATA data;

        (void)WSAStartup(MAKEWORD(2, 2), &data);
        s_done = True;
    }
#endif
}

static void L_SetNonBlock(TSocket sock)
{
#if defined(_WIN32)
    u_long mode = 1U;

    (void)ioctlsocket(sock, FIONBIO, &mode);
#else
    (void)fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
#endif
}

static void L_SetNoDelay(TSocket sock)
{
    int flag = 1;

    (void)setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&flag, sizeof(flag));
}

static void L_FillAddr(struct sockaddr_in *addr)
{
    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_port = htons(TCP_PORT);
    addr->sin_addr.s_addr = inet_addr(TCP_HOST);
}

static void L_OpenServer(TLink *self)
{
    if (self->listener == INVALID_SOCK)
    {
        struct sockaddr_in addr;
        int reuse = 1;
        TSocket sock = socket(AF_INET, SOCK_STREAM, 0);

        if (sock != INVALID_SOCK)
        {
            (void)setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
            L_SetNonBlock(sock);
            L_FillAddr(&addr);
            if ((bind(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0)
                && (listen(sock, 1) == 0))
            {
                self->listener = sock;
            }
            else
            {
                (void)L_CloseSocket(sock);
            }
        }
    }
}

static void L_OpenClient(TLink *self)
{
    struct sockaddr_in addr;
    TSocket sock = socket(AF_INET, SOCK_STREAM, 0);

    if (sock != INVALID_SOCK)
    {
        L_SetNonBlock(sock);
        L_FillAddr(&addr);
        if ((connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0) || L_InProgress())
        {
            self->sock = sock;
            self->connecting = True;
        }
        else
        {
            (void)L_CloseSocket(sock);
        }
    }
}

static void L_Close(TLink *self)
{
    if (self->sock != INVALID_SOCK)
    {
        (void)L_CloseSocket(self->sock);
        self->sock = INVALID_SOCK;
    }
    self->connecting = False;
}

static Bool L_OnConnectSwitch(IXrpcLink *sender, Bool connect_switch)
{
    TLink *self = (TLink *)sender;

    L_Close(self);
    self->state = kXrpcLinkStateDisconnect;
    if (connect_switch == True)
    {
        L_NetInit();
#if TCP_IS_SERVER
        L_OpenServer(self);
        self->state = (self->listener != INVALID_SOCK) ? kXrpcLinkStateConnectPending
                                                       : kXrpcLinkStateTimeOut;
#else
        L_OpenClient(self);
        self->state = (self->sock != INVALID_SOCK) ? kXrpcLinkStateConnectPending
                                                   : kXrpcLinkStateTimeOut;
#endif
    }

    return True;
}

/* 客户端的非阻塞连接有没有结果 */
static void L_PollConnect(TLink *self)
{
    fd_set writable;
    fd_set failed;
    struct timeval wait = {0, 0};

    FD_ZERO(&writable);
    FD_ZERO(&failed);
    FD_SET(self->sock, &writable);
    FD_SET(self->sock, &failed);
    if (select((int)self->sock + 1, NULL, &writable, &failed, &wait) > 0)
    {
        int error = 0;
        socklen_t size = sizeof(error);

        (void)getsockopt(self->sock, SOL_SOCKET, SO_ERROR, (char *)&error, &size);
        if ((error == 0) && (FD_ISSET(self->sock, &writable) != 0))
        {
            self->connecting = False;
            L_SetNoDelay(self->sock);
            self->state = kXrpcLinkStateConnected;
        }
        else
        {
            L_Close(self);
            self->state = kXrpcLinkStateTimeOut;
        }
    }
}

static TXrpcLinkState L_OnState(IXrpcLink *sender)
{
    TLink *self = (TLink *)sender;

    if (self->state == kXrpcLinkStateConnectPending)
    {
#if TCP_IS_SERVER
        TSocket sock = accept(self->listener, NULL, NULL);

        if (sock != INVALID_SOCK)
        {
            L_SetNonBlock(sock);
            L_SetNoDelay(sock);
            self->sock = sock;
            self->state = kXrpcLinkStateConnected;
        }
#else
        L_PollConnect(self);
#endif
    }

    return self->state;
}

/* 必须整帧写完。发送缓冲满时等一会，等不到按失败处理，由组件决定是否重发 */
static Int32 L_OnSend(IXrpcLink *sender, const char *data, Int32 size)
{
    TLink *self = (TLink *)sender;
    Int32 sent = 0;
    Int32 waited = 0;

    while ((sent < size) && (waited < TCP_SEND_WAIT_MS))
    {
        int done = send(self->sock, &data[sent], size - sent, XRPC_SEND_FLAGS);

        if (done > 0)
        {
            sent += done;
        }
        else if (L_WouldBlock())
        {
            XrpcSleepMs(1U);
            waited++;
        }
        else
        {
            break;
        }
    }

    return (sent == size) ? size : -1;
}

static Int32 L_OnRecv(IXrpcLink *sender, char *buf, Int32 size)
{
    TLink *self = (TLink *)sender;
    int got = recv(self->sock, buf, size, 0);
    Int32 result = -1;

    if (got > 0)
    {
        result = got;
    }
    else if ((got < 0) && L_WouldBlock())
    {
        result = 0;
    }

    return result;
}

static TLink s_link =
{
    {L_OnConnectSwitch, L_OnSend, L_OnRecv, L_OnState},
    INVALID_SOCK, INVALID_SOCK, False, kXrpcLinkStateDisconnect
};

IXrpcLink *XrpcPortLink_@NAME@(void)
{
    return &s_link.link;
}
'''


LINK_SERIAL = '''/**
 * @file        xrpc_port_link_@NAME@.c
 * @brief       链路 @NAME@：串口，8N1，不做流控。USB 虚拟串口也用它。
 * @note        打不开时下一轮再试，设备拔出后 RunOnce 会自动重连。
 */

#include "xrpc_port_node.h"

#include <string.h>

#if defined(_WIN32)
#include <windows.h>
typedef HANDLE TSerial;
#define INVALID_SERIAL      INVALID_HANDLE_VALUE
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
typedef int TSerial;
#define INVALID_SERIAL      (-1)
#endif

#define SERIAL_DEVICE       "@DEVICE@"
#define SERIAL_BAUD         (@BAUD@)
#define SERIAL_SEND_WAIT_MS (200)

typedef struct TLinkTag TLink;
struct TLinkTag
{
    IXrpcLink link;     /* 必须是第一个成员 */
    TSerial handle;
    TXrpcLinkState state;
};

#if defined(_WIN32)

static TSerial L_Open(void)
{
    /* COM10 以上必须带这个前缀，COM1 带上也可以 */
    TSerial handle = CreateFileA("\\\\\\\\.\\\\" SERIAL_DEVICE, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                                 OPEN_EXISTING, 0, NULL);

    if (handle != INVALID_SERIAL)
    {
        DCB dcb;
        COMMTIMEOUTS timeouts;

        memset(&dcb, 0, sizeof(dcb));
        memset(&timeouts, 0, sizeof(timeouts));
        dcb.DCBlength = sizeof(dcb);
        /* 读立即返回；写最多等 SERIAL_SEND_WAIT_MS */
        timeouts.ReadIntervalTimeout = MAXDWORD;
        timeouts.WriteTotalTimeoutConstant = SERIAL_SEND_WAIT_MS;

        if (GetCommState(handle, &dcb) == TRUE)
        {
            dcb.BaudRate = SERIAL_BAUD;
            dcb.ByteSize = 8;
            dcb.Parity = NOPARITY;
            dcb.StopBits = ONESTOPBIT;
            dcb.fBinary = TRUE;
            dcb.fParity = FALSE;
            dcb.fOutxCtsFlow = FALSE;
            dcb.fOutxDsrFlow = FALSE;
            dcb.fOutX = FALSE;
            dcb.fInX = FALSE;
            /* USB 虚拟串口常要 DTR 才开始收发 */
            dcb.fDtrControl = DTR_CONTROL_ENABLE;
            dcb.fRtsControl = RTS_CONTROL_ENABLE;
        }
        if ((SetCommState(handle, &dcb) == TRUE) && (SetCommTimeouts(handle, &timeouts) == TRUE))
        {
            (void)PurgeComm(handle, PURGE_RXCLEAR | PURGE_TXCLEAR);
        }
        else
        {
            (void)CloseHandle(handle);
            handle = INVALID_SERIAL;
        }
    }

    return handle;
}

static void L_Close(TSerial handle)
{
    (void)CloseHandle(handle);
}

static Int32 L_Write(TSerial handle, const char *data, Int32 size)
{
    DWORD written = 0;

    return (WriteFile(handle, data, (DWORD)size, &written, NULL) == TRUE) ? (Int32)written : -1;
}

static Int32 L_Read(TSerial handle, char *buf, Int32 size)
{
    DWORD got = 0;

    return (ReadFile(handle, buf, (DWORD)size, &got, NULL) == TRUE) ? (Int32)got : -1;
}

#else

static speed_t L_BaudCode(void)
{
    speed_t result = 0;

    switch (SERIAL_BAUD)
    {
        case 9600:
            result = B9600;
            break;
        case 19200:
            result = B19200;
            break;
        case 38400:
            result = B38400;
            break;
        case 57600:
            result = B57600;
            break;
        case 115200:
            result = B115200;
            break;
#if defined(B230400)
        case 230400:
            result = B230400;
            break;
#endif
#if defined(B460800)
        case 460800:
            result = B460800;
            break;
#endif
#if defined(B921600)
        case 921600:
            result = B921600;
            break;
#endif
        default:
            break;
    }

    return result;
}

static TSerial L_Open(void)
{
    TSerial handle = open(SERIAL_DEVICE, O_RDWR | O_NOCTTY | O_NONBLOCK);

    if (handle != INVALID_SERIAL)
    {
        struct termios tty;
        Bool ok = False;

        if ((L_BaudCode() != 0) && (tcgetattr(handle, &tty) == 0))
        {
            tty.c_iflag = 0U;
            tty.c_oflag = 0U;
            tty.c_lflag = 0U;
            tty.c_cflag = (tty.c_cflag & ~(tcflag_t)(CSIZE | PARENB | CSTOPB))
                        | CS8 | CREAD | CLOCAL;
            /* VMIN 为 1：没有数据时 read 报 EAGAIN，读到 0 才是设备挂断 */
            tty.c_cc[VMIN] = 1;
            tty.c_cc[VTIME] = 0;
            ok = ((cfsetispeed(&tty, L_BaudCode()) == 0)
                  && (cfsetospeed(&tty, L_BaudCode()) == 0)
                  && (tcsetattr(handle, TCSANOW, &tty) == 0)) ? True : False;
        }
        if (ok == True)
        {
            (void)tcflush(handle, TCIOFLUSH);
        }
        else
        {
            (void)close(handle);
            handle = INVALID_SERIAL;
        }
    }

    return handle;
}

static void L_Close(TSerial handle)
{
    (void)close(handle);
}

/* 写满时等一会，超时算发送失败，不能卡住 RunOnce */
static Int32 L_Write(TSerial handle, const char *data, Int32 size)
{
    Int32 sent = 0;
    Bool failed = False;

    while ((sent < size) && (failed == False))
    {
        ssize_t once = write(handle, &data[sent], (size_t)(size - sent));

        if (once > 0)
        {
            sent += (Int32)once;
        }
        else if ((once < 0) && ((errno == EAGAIN) || (errno == EWOULDBLOCK)))
        {
            fd_set writes;
            struct timeval wait;

            FD_ZERO(&writes);
            FD_SET(handle, &writes);
            wait.tv_sec = 0;
            wait.tv_usec = SERIAL_SEND_WAIT_MS * 1000;
            failed = (select(handle + 1, NULL, &writes, NULL, &wait) > 0) ? False : True;
        }
        else if (once != 0)
        {
            failed = True;
        }
    }

    return (failed == True) ? -1 : sent;
}

static Int32 L_Read(TSerial handle, char *buf, Int32 size)
{
    ssize_t got = read(handle, buf, (size_t)size);
    Int32 result = -1;

    if (got > 0)
    {
        result = (Int32)got;
    }
    else if ((got < 0) && ((errno == EAGAIN) || (errno == EWOULDBLOCK)))
    {
        result = 0;
    }

    return result;
}

#endif

static Bool L_OnConnectSwitch(IXrpcLink *sender, Bool connect_switch)
{
    TLink *self = (TLink *)sender;

    if (self->handle != INVALID_SERIAL)
    {
        L_Close(self->handle);
        self->handle = INVALID_SERIAL;
    }
    if (connect_switch == True)
    {
        self->handle = L_Open();
    }

    if (connect_switch == False)
    {
        self->state = kXrpcLinkStateDisconnect;
    }
    else
    {
        self->state = (self->handle == INVALID_SERIAL) ? kXrpcLinkStateTimeOut
                                                       : kXrpcLinkStateConnected;
    }

    return True;
}

static TXrpcLinkState L_OnState(IXrpcLink *sender)
{
    return ((TLink *)sender)->state;
}

static Int32 L_OnSend(IXrpcLink *sender, const char *data, Int32 size)
{
    return L_Write(((TLink *)sender)->handle, data, size);
}

static Int32 L_OnRecv(IXrpcLink *sender, char *buf, Int32 size)
{
    return L_Read(((TLink *)sender)->handle, buf, size);
}

static TLink s_link =
{
    {L_OnConnectSwitch, L_OnSend, L_OnRecv, L_OnState},
    INVALID_SERIAL, kXrpcLinkStateDisconnect
};

IXrpcLink *XrpcPortLink_@NAME@(void)
{
    return &s_link.link;
}
'''


def gen_links(config):
    files = {}
    for link in config['links']:
        name = link['name']
        if link['type'] == 'win_pipe':
            text = LINK_WIN_PIPE.replace('@PIPE@', link['pipe']) \
                .replace('@SERVER@', '1' if link['server'] else '0')
        elif link['type'] == 'serial':
            text = LINK_SERIAL.replace('@DEVICE@', link['device']) \
                .replace('@BAUD@', str(link['baud']))
        elif link['type'] == 'tcp':
            text = LINK_TCP.replace('@HOST@', link['host']).replace('@PORT@', str(link['port'])) \
                .replace('@SERVER@', '1' if link['server'] else '0')
        else:
            text = LINK_CUSTOM
            for key, message in (
                    ('SWITCH', 'L_OnConnectSwitch：打开或关闭链路'),
                    ('STATE', 'L_OnState：返回链路真实状态'),
                    ('SEND', 'L_OnSend：把整帧写到链路'),
                    ('RECV', 'L_OnRecv：非阻塞读取')):
                text = text.replace('@TODO_%s@' % key,
                                    todo('link:%s' % name, message).rstrip('\n'))
        files['xrpc_port_link_%s.c' % name] = text.replace('@NAME@', name)
    return files


# ---------- 节点装配 ----------

def gen_port_h(config):
    links = config['links']
    use_lock = config['threads'] == 'multi'
    decl = ''.join('IXrpcLink *XrpcPortLink_%s(void);\n' % link['name'] for link in links)
    thread_api = ''
    if has_poll_thread(config):
        thread_api = '''
/** 启动轮询线程，在 XrpcPortPoll 循环里调用 RunOnce */
Int32 XrpcPortStartPollThread(void);

/** 轮询线程是否继续，XrpcPortStop 之后返回 False */
Bool XrpcPortPollShouldRun(void);

/** 让轮询线程退出 */
void XrpcPortStop(void);
'''
    lock_api = ''
    if use_lock:
        lock_api = '''
/* 节点锁加每条链路一把发送锁 */
#define XRPC_PORT_LOCK_MAX  (%d)

typedef struct TXrpcPortLockTag TXrpcPortLock;
struct TXrpcPortLockTag
{
    TXrpcLockOps ops;
    void *os_lock;
};

/** 每次调用都要得到一把新的锁，平台适配里实现 */
void XrpcPortLockInit(TXrpcPortLock *self);
''' % (len(links) + 1)
    return {'xrpc_port_node.h': '''/**
 * @file        xrpc_port_node.h
 * @brief       本节点的 XRPC 适配入口。由 xrpc_port_gen.py 生成，配置改动后重新生成。
 */

#ifndef _XRPC_PORT_H_
#define _XRPC_PORT_H_

#include "xrpc/xrpc_node.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define XRPC_PORT_NODE_ID           (0x%08XU)
#define XRPC_PORT_PROTOCOL_VERSION  (0x%04XU)
#define XRPC_PORT_LINK_COUNT        (%dU)
#define XRPC_PORT_FRAME_SIZE        (%dU)   ///< 拼帧缓存、发送缓存；决定最大帧长
#define XRPC_PORT_RECV_SIZE         (%dU)   ///< 单次读取链路的临时缓冲
#define XRPC_PORT_CALL_SLOTS        (%dU)   ///< 最多同时在途的调用数；0 表示不启用

extern IXrpcCrc16 g_xrpc_port_crc16;

%s%s
/** 装配节点。之后注册过程表和通知表，再启动轮询 */
Int32 XrpcPortInit(void);

/** 取节点对象，传给业务接口 */
TXrpcNode *XrpcPortNode(void);

/** 轮询一次。必须由单一固定线程周期调用，不能和同步调用在同一线程 */
Int32 XrpcPortPoll(void);
%s
#ifdef __cplusplus
}  ///< extern "C"
#endif

#endif  ///< _XRPC_PORT_H_
''' % (config['node_id'], config['protocol_version'], len(links), config['frame_size'],
       config['recv_size'], config['call_slots'], decl, lock_api, thread_api)}


def gen_port_c(config):
    links = config['links']
    use_lock = config['threads'] == 'multi'
    has_bus = any(link['topology'] == 'bus' for link in links)
    lines = []
    for index, link in enumerate(links):
        name = link['name']
        if use_lock:
            lines.append('    XrpcPortLockInit(&s_send_locks[%d]);' % index)
        lines.append('    if (result == kXrpcErrOk)\n    {\n'
                     '        result = TXrpcPeerCacheAddLink(&s_cache, XrpcPortLink_%s(), 0x%08XU,\n'
                     '                                       s_fdn_buffs[%d], XRPC_PORT_FRAME_SIZE,\n'
                     '                                       s_send_buffs[%d], XRPC_PORT_FRAME_SIZE,\n'
                     '                                       %s);\n    }'
                     % (name, link['peer_node_id'], index, index,
                        '&s_send_locks[%d].ops' % index if use_lock else 'NULL'))
        custom_cfg = (link['topology'] == 'bus') or link['max_frame'] or link['advertise_max_recv']
        if custom_cfg:
            topo = 'kXrpcLinkTopologyBus' if link['topology'] == 'bus' else 'kXrpcLinkTopologyP2P'
            lines.append('    memset(&config, 0, sizeof(config));')
            lines.append('    config.topology = %s;' % topo)
            lines.append('    config.max_frame = %dU;' % link['max_frame'])
            lines.append('    config.advertise_max_recv = %s;'
                         % ('True' if link['advertise_max_recv'] else 'False'))
            if link['topology'] == 'bus':
                lines.append('    config.remotes = s_remotes_%s;' % name)
                lines.append('    config.remote_capacity = %dU;' % link['remote_capacity'])
            lines.append('    if (result == kXrpcErrOk)\n    {\n'
                         '        result = TXrpcPeerCacheSetLinkConfig(&s_cache, %dU, &config);\n    }'
                         % index)
    remotes = ''.join('static TXrpcRemoteNode s_remotes_%s[%d];\n'
                      % (link['name'], link['remote_capacity'])
                      for link in links if link['topology'] == 'bus')
    needs_cfg = any((link['topology'] == 'bus') or link['max_frame'] or link['advertise_max_recv']
                    for link in links)
    locks = ''
    if use_lock:
        locks = ('static TXrpcPortLock s_node_lock;\n'
                 'static TXrpcPortLock s_send_locks[XRPC_PORT_LINK_COUNT];\n')
    slots = ''
    slot_call = ''
    if config['call_slots'] > 0:
        slots = 'static TXrpcCallSlot s_slots[XRPC_PORT_CALL_SLOTS];\n'
        slot_call = ('    if (result == kXrpcErrOk)\n    {\n'
                     '        result = TXrpcNodeSetCallSlots(&s_node, s_slots, XRPC_PORT_CALL_SLOTS);\n'
                     '    }\n')
    lock_call = ''
    if use_lock:
        lock_call = ('    /* 先设锁再加链路。节点锁和发送锁必须是不同对象 */\n'
                     '    XrpcPortLockInit(&s_node_lock);\n'
                     '    if (result == kXrpcErrOk)\n    {\n'
                     '        result = TXrpcNodeSetLockMethod(&s_node, &s_node_lock.ops);\n    }\n')
    stop = ''
    if has_poll_thread(config):
        stop = '''
Bool XrpcPortPollShouldRun(void)
{
    return (s_stop == 0) ? True : False;
}

void XrpcPortStop(void)
{
    s_stop = 1;
}
'''
    stop_var = 'static volatile int s_stop = 0;\n' if has_poll_thread(config) else ''
    return {'xrpc_port_node.c': '''/**
 * @file        xrpc_port_node.c
 * @brief       本节点的装配：所有缓冲静态分配，核心库内不分配内存。
 */

#include "xrpc_port_node.h"

#include <string.h>

static TXrpcNode s_node;
static TXrpcPeerCache s_cache;
static TXrpcPeerNode s_peers[XRPC_PORT_LINK_COUNT];
static UInt8 s_fdn_buffs[XRPC_PORT_LINK_COUNT][XRPC_PORT_FRAME_SIZE];
static UInt8 s_send_buffs[XRPC_PORT_LINK_COUNT][XRPC_PORT_FRAME_SIZE];
static UInt8 s_recv_buff[XRPC_PORT_RECV_SIZE];
%s%s%s%s
Int32 XrpcPortInit(void)
{
    Int32 result = TXrpcNodeCreate(&s_node, XRPC_PORT_NODE_ID, XRPC_PORT_PROTOCOL_VERSION);
%s%s
    if (result == kXrpcErrOk)
    {
        result = TXrpcNodeSetCrc16(&s_node, &g_xrpc_port_crc16);
    }
%s    if (result == kXrpcErrOk)
    {
        result = TXrpcPeerCacheCreate(&s_cache, s_peers, XRPC_PORT_LINK_COUNT);
    }
    if (result == kXrpcErrOk)
    {
        result = TXrpcNodeSetPeerCache(&s_node, &s_cache);
    }
%s
%s
    return result;
}

TXrpcNode *XrpcPortNode(void)
{
    return &s_node;
}

Int32 XrpcPortPoll(void)
{
    return TXrpcNodeRunOnce(&s_node, s_recv_buff, XRPC_PORT_RECV_SIZE);
}
%s''' % (remotes, locks, slots, stop_var,
         '    TXrpcLinkConfig config;\n' if needs_cfg else '',
         ('    if (result == kXrpcErrOk)\n    {\n'
          '        result = TXrpcNodeSetUserTag(&s_node, 0x%08XU);\n    }\n'
          % config['user_tag']) if config['user_tag'] else '',
         lock_call, '\n'.join(lines), slot_call, stop)}


def gen_cmake(config, files):
    sources = sorted(name for name in files if name.endswith('.c'))
    listing = ''.join('    ${CMAKE_CURRENT_LIST_DIR}/%s\n' % name for name in sources)
    include = '    ${CMAKE_CURRENT_LIST_DIR}\n'
    include += '    ${CMAKE_CURRENT_LIST_DIR}/port_include\n'
    note = '# 编译核心库时要让它找到 port_include 里的 xrpc_port.h，见 PORTING.md。'
    return {'xrpc_port.cmake': '''# XRPC 适配代码。这里只有适配层，不编译 XRPC 核心库：
#
#   include(xrpc_port.cmake)
#   target_sources(app PRIVATE ${XRPC_PORT_SOURCES})
#   target_include_directories(app PRIVATE ${XRPC_PORT_INCLUDE_DIRS} <xrpc>/common/include)
#   target_link_libraries(app PRIVATE <你编译好的 xrpc 库>)
#
%s

set(XRPC_PORT_SOURCES
%s)

set(XRPC_PORT_INCLUDE_DIRS
%s)

# 链路模板要用到的系统库
set(XRPC_PORT_LIBS%s)
''' % (note, listing, include,
       ' ws2_32' if any((link['type'] == 'tcp') and (config['platform'] == 'win32')
                        for link in config['links']) else '')}


def gen_porting(config, files):
    todo_rows = []
    for name in sorted(files):
        for tag, message in TODO_RE.findall(files[name]):
            todo_rows.append('| `%s` | `%s` | %s |' % (tag, name, message.strip()))
    table = '\n'.join(todo_rows) if todo_rows else '| 无 | | 全部由模板实现 |'
    return {'PORTING.md': '''# XRPC 适配说明

本目录由 `xrpc_port_gen.py` 生成，节点 ID `0x%08X`。**这里不包含 XRPC 核心库的编译**。

## 步骤

```mermaid
flowchart LR
    A["1 实现下表的虚接口"] --> B["2 运行 check"]
    B --> C["3 编译核心库"]
    C --> D["4 编入适配代码"]
    D --> E["5 启动节点"]
```

## 1. 要实现的虚接口

未实现的地方都有 `#error "XRPC_PORT_TODO(...)"`，编译会直接停在那里。实现之后删掉那几行。
想先把工程编过，可以定义 `XRPC_PORT_ALLOW_TODO`，但这时运行不起来。

| 标记 | 文件 | 要做什么 |
| --- | --- | --- |
%s

虚接口的约定：

| 接口 | 必须遵守 |
| --- | --- |
| `IXrpcLink.OnSend_` | 整帧写入；返回 0 或少于整帧都算发送失败 |
| `IXrpcLink.OnRecv_` | 不能阻塞；无数据返回 0；负数触发断开 |
| `IXrpcLink.OnState_` | 只有 `Connected` 才收发；`Busy` 不算断开 |
| `IXrpcCrc16.OnCalc_` | 可重入；与对端算法字节级一致 |
| 锁 | 节点锁、每条链路的发送锁是不同对象，都不能是递归锁，也不能和日志的锁相同 |

## 2. 检查

```bash
python <xrpc>/tools/xrpc_port_gen.py check %s
```

列出还没实现的接口，全部完成后退出码为 0。

## 3. 编译核心库

核心库需要 `port_include` 里的头文件：

```bash
cmake -S <xrpc> -B build-xrpc -DXRPC_PORT_DIR=<本目录>/port_include
cmake --build build-xrpc
```

用自己的构建系统也一样：把 `common/*.c` 编成库，头文件路径加上 `common/include`、`common/include/xrpc`、`common` 和 `port_include`。

## 4. 编入适配代码

把 `xrpc_port.cmake` 列出的源文件加进工程，再链接上一步的库。核心库会用到这里实现的 `XrpcSleepMs`、`XrpcGetSysTick`，链接顺序上库放在适配代码之后。

## 5. 启动节点

```mermaid
sequenceDiagram
    participant M as main
    participant P as 轮询线程
    M->>M: XrpcPortInit()
    M->>M: 注册过程表和通知表
    M->>P: 启动轮询
    P->>P: 循环 XrpcPortPoll()
    M->>M: 业务调用
```

```c
XrpcPortInit();
DemoXrpcRegister(XrpcPortNode(), seq_seed);     /* xrpc_api_gen.py 生成 */
%s
```

同步调用不能和轮询在同一线程，否则收不到应答。过程回调、事件回调都在轮询线程执行，不能阻塞。
''' % (config['node_id'], table, '<本目录>',
       'XrpcPortStartPollThread();' if has_poll_thread(config)
       else '/* 在固定线程里周期调用 XrpcPortPoll() */')}


def generate(config, out_dir, force):
    """用户可能已经填了 TODO，已存在的文件默认不覆盖"""
    files = {}
    for maker in (gen_port_headers, gen_crc, gen_platform, gen_links, gen_port_h, gen_port_c):
        files.update(maker(config))
    files.update(gen_cmake(config, files))
    files.update(gen_porting(config, files))
    written = []
    kept = []
    for name in sorted(files):
        path = os.path.join(out_dir, name)
        if os.path.exists(path) and not force:
            kept.append(name)
        else:
            write_text(path, files[name])
            written.append(name)
    return written, kept


def find_todos(directory):
    found = []
    for root, _, names in os.walk(directory):
        for name in sorted(names):
            if name.endswith(('.c', '.h')):
                path = os.path.join(root, name)
                with open(path, encoding='utf-8') as stream:
                    for number, line in enumerate(stream, 1):
                        match = TODO_RE.search(line)
                        if match and line.lstrip().startswith('#error'):
                            found.append((os.path.relpath(path, directory), number,
                                          match.group(1), match.group(2).strip()))
    return found


def cmd_check(args):
    if not os.path.isfile(os.path.join(args.directory, 'xrpc_port_node.h')):
        raise ConfigError('%s 不是生成的适配目录（找不到 xrpc_port.h）' % args.directory)
    todos = find_todos(args.directory)
    if not todos:
        print('全部虚接口都已实现。')
        return 0
    print('还有 %d 项没实现：' % len(todos))
    for path, number, tag, message in todos:
        print('  %s:%d  [%s] %s' % (path, number, tag, message))
    print('实现之后删掉对应的 #ifndef XRPC_PORT_ALLOW_TODO 块。')
    return 1


def report(written, kept, out_dir):
    for name in written:
        print('  生成 %s' % name)
    for name in kept:
        print('  保留 %s（已存在，--force 才覆盖）' % name)
    todos = find_todos(out_dir)
    print('还有 %d 项虚接口要实现，详见 %s' % (len(todos), os.path.join(out_dir, 'PORTING.md')))


def cmd_generate(args):
    config = validate(load_config(args.config))
    written, kept = generate(config, args.out, args.force)
    report(written, kept, args.out)
    return 0


def ask(prompt, default, choices=None, convert=str):
    while True:
        hint = ' [%s]' % default if default is not None else ''
        answer = input('%s%s: ' % (prompt, hint)).strip()
        text = answer if answer else default
        if text is None:
            print('  这一项必填')
            continue
        try:
            value = convert(text) if isinstance(text, str) else text
        except ValueError:
            print('  格式不对')
            continue
        if (choices is None) or (value in choices):
            return value
        print('  只能选：%s' % ' / '.join(choices))


def to_int(text):
    return int(text, 0)


def yaml_of(config):
    lines = []
    for key in ('node_id', 'protocol_version'):
        lines.append('%s: 0x%X' % (key, config[key]))
    for key in ('platform', 'threads', 'crc16', 'call_slots', 'frame_size', 'recv_size'):
        lines.append('%s: %s' % (key, config[key]))
    lines.append('links:')
    for link in config['links']:
        first = True
        for key, value in link.items():
            text = ('true' if value else 'false') if isinstance(value, bool) else value
            lines.append('%s%s: %s' % ('  - ' if first else '    ', key, text))
            first = False
    return '\n'.join(lines) + '\n'


def cmd_wizard(args):
    print('XRPC 适配向导。回车使用方括号里的默认值。\n')
    config = {}
    print('节点 ID 在网络里唯一，不能是 0 或 0xFFFFFFFF。')
    config['node_id'] = ask('节点 ID', None, convert=to_int)
    print('\n协议版本必须和对端一致，否则帧被丢弃。')
    config['protocol_version'] = ask('协议版本', '0x0200', convert=to_int)
    print('\nwin32、posix 自动实现睡眠、tick、锁；custom 用于 RTOS 或裸机，由你实现。')
    config['platform'] = ask('平台', 'custom', PLATFORMS)
    print('\n同步调用要有独立的轮询线程，这时节点锁必填。全部跑在一个循环里选 single。')
    config['threads'] = ask('线程模型', 'multi', ('multi', 'single'))
    print('\ncustom 由你实现；ccitt_false 是常见算法，不代表与现网一致；fixed 只能本机联调。')
    config['crc16'] = ask('CRC16', 'custom', CRC_TYPES)
    print('\n决定最大帧长。业务最大 2500 字节时建议 2600。')
    config['frame_size'] = ask('帧缓冲大小', '2600', convert=int)
    config['recv_size'] = ask('单次读取缓冲大小', '1024', convert=int)
    print('\n要用异步调用，或同一过程并发调用，就要有调用槽。0 表示不启用。')
    config['call_slots'] = ask('调用槽个数', '4', convert=int)

    config['links'] = []
    count = ask('\n链路条数', '1', convert=int)
    for index in range(count):
        print('\n链路 %d' % index)
        link = {'name': ask('  名字（小写，如 uart0）', None)}
        if config['platform'] == 'win32':
            choices = LINK_TYPES
        elif config['platform'] == 'posix':
            choices = ('custom', 'tcp', 'serial')
        else:
            choices = ('custom',)
        link['type'] = ask('  类型', 'custom', choices)
        link['topology'] = ask('  拓扑', 'p2p', TOPOLOGIES)
        if link['type'] == 'win_pipe':
            link['pipe'] = ask('  管道名', 'xrpc_%s' % link['name'])
            link['server'] = ask('  本端创建管道', 'true', ('true', 'false')) == 'true'
        if link['type'] == 'tcp':
            link['port'] = ask('  端口', '47000', convert=int)
            link['server'] = ask('  本端监听', 'true', ('true', 'false')) == 'true'
        if link['type'] == 'serial':
            link['device'] = ask('  设备，如 COM3 或 /dev/ttyUSB0', None)
            link['baud'] = ask('  波特率', '115200', convert=int)
        if link['topology'] == 'bus':
            link['remote_capacity'] = ask('  总线上最多几个其他节点', '8', convert=int)
        config['links'].append(link)

    checked = validate(dict(config))
    write_text(os.path.join(args.out, 'xrpc_port.yaml'), yaml_of(config))
    written, kept = generate(checked, args.out, args.force)
    print('')
    report(written, kept, args.out)
    print('配置保存在 %s，改完可以用 generate 重新生成。' % os.path.join(args.out, 'xrpc_port.yaml'))
    return 0


def main():
    parser = argparse.ArgumentParser(description='XRPC 组件适配代码生成工具')
    parser.add_argument('--version', action='version', version=component_version())
    sub = parser.add_subparsers(dest='command', required=True)

    wizard = sub.add_parser('wizard', help='问答式生成')
    wizard.add_argument('--out', required=True, help='输出目录')
    wizard.add_argument('--force', action='store_true', help='覆盖已存在的文件')
    wizard.set_defaults(func=cmd_wizard)

    gen = sub.add_parser('generate', help='按配置文件生成')
    gen.add_argument('config', help='配置文件（.yaml 或 .json）')
    gen.add_argument('--out', required=True, help='输出目录')
    gen.add_argument('--force', action='store_true', help='覆盖已存在的文件，已实现的代码会丢失')
    gen.set_defaults(func=cmd_generate)

    check = sub.add_parser('check', help='检查还有哪些虚接口没实现')
    check.add_argument('directory', help='适配目录')
    check.set_defaults(func=cmd_check)

    args = parser.parse_args()
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8')
    try:
        return args.func(args)
    except ConfigError as error:
        print('错误：%s' % error, file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
