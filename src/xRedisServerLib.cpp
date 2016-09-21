/*
* ----------------------------------------------------------------------------
* Copyright (c) 2015-2016, xSky <guozhw at gmail dot com>
* All rights reserved.
* Distributed under GPL license.
* ----------------------------------------------------------------------------
*/

#include <inttypes.h>
#include <assert.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "xRedisServerLib.h"

#define TIMEOUT_CLOSE      3600
#define TIMEVAL_TIME       600

xRedisConnectorBase::xRedisConnectorBase()
{
    bev = NULL;
    activetime = time(NULL);
    xredisvr = NULL;
    argc = 0;
    argv = NULL;
    argnum = 0;
    parsed = 0;
}

xRedisConnectorBase::~xRedisConnectorBase()
{
    FreeArg();
}

/*
 * @brief 做一些释放args的操作
 * */
bool xRedisConnectorBase::FreeArg()
{
    for (int i = 0; i < argc; ++i){
        if (NULL == argv[i]) {
            fprintf(stderr,"FreeArg error i: %d", i);
        }
        sdsfree(argv[i]);
    }
    if (argv) free(argv);
    cmdbuffer.erase(0, parsed);
    argv = NULL;
    argc = 0;
    argnum = 0;
    parsed = 0;
    return true;
}

bool xRedisConnectorBase::OnTimer()
{
    if (time(NULL) - activetime > TIMEOUT_CLOSE) {
        return false;
    }
    return true;
}

/*
 * @brief 设置fd的opt
 * */
void xRedisConnectorBase::SetSocketOpt()
{
    int optval = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval))) {
        fprintf(stderr, "setsockopt(TCP_NODELAY) failed: %s", strerror(errno));
    }
}

/*
 * @brief server的构造函数
 * */
xRedisServerBase::xRedisServerBase()
{
    evbase = event_base_new();
    assert(evbase != NULL);
    sessionbase = 1000;
    mCmdCount = 0;
}

/*
 * @brief server的释放函数
 * */
xRedisServerBase::~xRedisServerBase()
{
    event_base_free(evbase);
}

int xRedisServerBase::ParaseLength(const char* ptr, int size, int &head_count)
{
    char *lf = (char *)memchr(ptr, '\n', size);
    if (lf == NULL) {
        return 0;
    }
    char tmp[11] = { 0 };
    int i = 0;

    ptr++;
    while (ptr[i] != '\n') {
        if (ptr[i] == '\r') {
            i++;
            continue;
        }
        tmp[i] = ptr[i];
        i++;
    }
    head_count = (int)strtol(tmp, NULL, 10);
    if (0==head_count) {
        return 0;
    }
    return i + 2;
}

int xRedisServerBase::ParaseData(xRedisConnectorBase *pConnector, const char* ptr, int size)
{
    int parased = 0;
    if (ptr[0] != '$') {
        return 0;
    }

    int len = 0;
    int ret = ParaseLength(ptr, size, len);
    if (0 == ret){
        return 0;
    }
    parased += ret;
    ptr += parased;
    size -= parased;
    if (size < len + 2) {
        return 0;
    }
    pConnector->argv[pConnector->argc++] = sdsnewlen(ptr, len);
    pConnector->argnum--;

    return parased + len + 2;
}

/*
 * @brief 真正的业务处理
 * */
bool xRedisServerBase::ProcessCmd(xRedisConnectorBase *pConnector)
{
    pConnector->activetime = time(NULL);
    int size = pConnector->cmdbuffer.length();
    const char *pData = pConnector->cmdbuffer.c_str();

    if (0 == size) {
        return false;
    }

    pData += pConnector->parsed;
    size -= pConnector->parsed;
    const char *ptr = pData;

    if (pConnector->argnum == 0){
        if (ptr[0] == '*') {
            int num = 0;
            int pos = ParaseLength(ptr, size, num);
            if (0 == pos) {
                return false;
            }
            ptr += pos;
            size -= pos;
            pConnector->parsed += pos;
            pConnector->argnum = num;
            if (pConnector->argv) pConnector->FreeArg();
            pConnector->argv = (sds *)malloc(sizeof(sds*)*(pConnector->argnum));
        } else {
            return false;
        }
    }

    while (pConnector->argnum > 0) {
        int  p = ParaseData(pConnector, ptr, size);
        if (p == 0){
            break;
        }
        ptr += p;
        size -= p;
        pConnector->parsed += p;
    }

    if (pConnector->argnum == 0) {
        DoCmd(pConnector);
        return true;
    }
    return false;
}

/*
 * @brief 设置命令处理cb
 * */
bool xRedisServerBase::SetCmdTable(const char* cmd, CmdCallback fun)
{
    if ((NULL == cmd) || (NULL == fun) || (mCmdCount >= CMD_CALLBACK_MAX)) {
        return false;
    }
    mCmdTables[mCmdCount].cmd = cmd;
    mCmdTables[mCmdCount].cb = fun;
    mCmdCount++;
    return true;
}

/*
 * @brief 获取命令处理cb
 * */
CmdFun * xRedisServerBase::GetCmdProcessFun(const char *cmd)
{
    CmdFun *iter;
    for (iter = &mCmdTables[0]; (NULL != iter) && (iter->cmd); ++iter) {
        if ((NULL != iter->cmd) && (0 == strcasecmp(iter->cmd, cmd)))
            return iter;
    }
    return NULL;
}

void xRedisServerBase::DoCmd(xRedisConnectorBase *pConnector)
{
    CmdFun *cmd = GetCmdProcessFun(pConnector->argv[0]);
    if (cmd) {
        (this->*cmd->cb)(pConnector);
    } else {
        SendErrReply(pConnector, pConnector->argv[0], "not suport");
    }
    pConnector->FreeArg();
}

/*
 * @brief 监听cb
 * */
void xRedisServerBase::AcceptCallback(evutil_socket_t listener, short event, void *arg)
{
    class xRedisServerBase *pRedisvr = (class xRedisServerBase *)arg;
    evutil_socket_t fd;
    struct sockaddr_in sin;
    socklen_t slen;
    fd = accept(listener, (struct sockaddr *)&sin, &slen);
    if (fd < 0) {
        return;
    }
    evutil_make_socket_nonblocking(fd);

    pRedisvr->MallocConnection(fd);
}

void xRedisServerBase::ReadCallback(struct bufferevent *bev, void *arg)
{
    xRedisConnectorBase *pConnector = reinterpret_cast<xRedisConnectorBase*>(arg);
    xRedisServerBase *pRedisvr = pConnector->xredisvr;
    struct evbuffer* input = bufferevent_get_input(bev);

    while (1) {
        size_t total_len = evbuffer_get_length(input);
        if (total_len < 2) {
            break;
        }

        //读取连续的数据
        unsigned char *buffer = evbuffer_pullup(input, total_len);
        if (NULL == buffer) {
            fprintf(stderr, "evbuffer_pullup msg_len failed!\r\n");
            return;
        }

        pConnector->cmdbuffer.append((char*)buffer, total_len);

        //从buffer里移除指定的长度
        if (evbuffer_drain(input, total_len) < 0) {
            fprintf(stderr, "evbuffer_drain failed!\r\n");
            return;
        }
    }

    if (pConnector->cmdbuffer.length()) {
        while (pRedisvr->ProcessCmd(pConnector))
        {

        }
    }

}

void xRedisServerBase::WriteCallback(struct bufferevent *bev, void *arg)
{
    
}

/*
 * @brief 超时回调cb
 * */
void xRedisServerBase::TimeoutCallback(int fd, short event, void *arg)
{
    xRedisConnectorBase *pConnector = reinterpret_cast<xRedisConnectorBase*>(arg);
    xRedisServerBase *pRedisvr = pConnector->xredisvr;
    if (pConnector->OnTimer()) {
        struct timeval tv;
        evutil_timerclear(&tv);
        tv.tv_sec = TIMEVAL_TIME;
        //设置timeout
        event_add(&pConnector->evtimer, &tv);
    } else {
        pRedisvr->FreeConnection(pConnector->sid);
    }
}

/*
 * @brief event error回调
 * */
void xRedisServerBase::ErrorCallback(struct bufferevent *bev, short event, void *arg)
{
    xRedisConnectorBase *pConnector = reinterpret_cast<xRedisConnectorBase*>(arg);
    xRedisServerBase *pRedisvr = pConnector->xredisvr;

    evutil_socket_t fd = pConnector->getfd();
    //fprintf(stderr, "error_cb fd:%u, sid:%u bev:%p event:%d arg:%d\n", fd, pConnector->sid, bev, event, arg);
    if (event & BEV_EVENT_TIMEOUT) {
        fprintf(stderr, "Timed out fd:%d \n", fd);
    } else if (event & BEV_EVENT_EOF) {
        fprintf(stderr, "connection closed fd:%d \n", fd);
    } else if (event & BEV_EVENT_ERROR) {
        fprintf(stderr, "BEV_EVENT_ERROR error fd:%d \n", fd);
    } else if (event & BEV_EVENT_READING) {
        fprintf(stderr, "BEV_EVENT_READING error fd:%d \n", fd);
    } else if (event & BEV_EVENT_WRITING) {
        fprintf(stderr, "BEV_EVENT_WRITING error fd:%d \n", fd);
    } else {
    
    }
    pRedisvr->FreeConnection(pConnector->sid);
}

/*
 * @brief 启动服务器
 * */
bool xRedisServerBase::Start(const char* ip, int port)
{
    if (BindPort(ip, port)) {
        return Run();
    }
    return false;
}

/*
 * @brief 绑定端口,并且设置监听cb
 * */
bool xRedisServerBase::BindPort(const char* ip, int port)
{
    if (NULL==ip) {
        return false;
    }
    evutil_socket_t listener;
    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener<=0){
        return false;
    }

    //设置fd为无阻塞
    evutil_make_socket_nonblocking(listener);
    evutil_make_listen_socket_reuseable(listener);

    struct sockaddr_in sin;
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = inet_addr(ip);
    sin.sin_port = htons(port);

    if (::bind(listener, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        return false;
    }

    if (listen(listener, 128) < 0) {
        return false;
    }

    struct event *listen_event;
    listen_event = event_new(evbase, listener, EV_READ | EV_PERSIST, AcceptCallback, (void*)this);
    event_add(listen_event, NULL);

    return true;
}

bool xRedisServerBase::Run()
{
    pthread_t pid;
    int ret = pthread_create(&pid, NULL, Dispatch, evbase);
    return (0==ret);
}

void *xRedisServerBase::Dispatch(void *arg){
    if (NULL == arg) {
        return NULL;
    }
    event_base_dispatch((struct event_base *) arg);

    fprintf(stderr, "Dispatch thread end\n");
    return NULL;
}

/*
 * @brief 初始化conn对象
 * */
bool xRedisServerBase::MallocConnection(evutil_socket_t skt)
{
    xRedisConnectorBase *pConnector = new xRedisConnectorBase;
    if (NULL == pConnector) {
        return false;
    }

    pConnector->fd = skt;
    pConnector->xredisvr = this;
    pConnector->sid = sessionbase++;

    //创建一个bufferevent
    pConnector->bev = bufferevent_socket_new(evbase, skt, BEV_OPT_CLOSE_ON_FREE);
    bufferevent_setcb(pConnector->bev, ReadCallback, NULL, ErrorCallback, pConnector);
    pConnector->SetSocketOpt();

    struct timeval tv;
    tv.tv_sec = TIMEVAL_TIME;
    tv.tv_usec = 0;
    //设置超时cb
    evtimer_set(&pConnector->evtimer, TimeoutCallback, pConnector);
    event_base_set(evbase, &pConnector->evtimer);
    evtimer_add(&pConnector->evtimer, &tv);

    //开启bufferevent
    bufferevent_enable(pConnector->bev, EV_READ | EV_WRITE | EV_PERSIST);

    //sid用来标示conn,conn就是conn对象
    connectionmap.insert(pair<uint32_t, xRedisConnectorBase*>(pConnector->sid, pConnector));

    return true;
}

/*
 * @brief 根据sid查找conn对象
 * */
xRedisConnectorBase* xRedisServerBase::FindConnection(uint32_t sid)
{
    std::map<uint32_t, xRedisConnectorBase*>::iterator iter = connectionmap.find(sid);
    if (iter == connectionmap.end()) {
        return NULL;
    } else {
        return iter->second;
    }
}

/*
 * @brief 做一些释放conn对象的操作
 * */
bool xRedisServerBase::FreeConnection(uint32_t sid)
{
    std::map<uint32_t, xRedisConnectorBase*>::iterator iter = connectionmap.find(sid);
    if (iter == connectionmap.end()) {
        return false;
    } else {//做一些释放conn对象的操作
        iter->second->FreeArg();
        event_del(&iter->second->evtimer);
        bufferevent_free(iter->second->bev);
        delete iter->second;
        connectionmap.erase(iter);
    }
    return true;
}

/*
 * @brief 给客户端发送数据
 * */
bool xRedisServerBase::SendData(xRedisConnectorBase *pConnector, const char* data, int len)
{
    int ret = bufferevent_write(pConnector->bev, data, len);
    return (0 == ret);
}

/*
 * @brief 给客户端返回应答
 * */
int xRedisServerBase::NetPrintf(xRedisConnectorBase *pConnector, const char* fmt, ...)
{
    char szBuf[256] = { 0 };
    int len = 0;
    va_list va;
    va_start(va, fmt);
    len = vsnprintf(szBuf, sizeof(szBuf), fmt, va);
    va_end(va);
    bool bRet = SendData(pConnector, szBuf, len);
    return (bRet) ? len : 0;
}

int xRedisServerBase::SendStatusReply(xRedisConnectorBase *pConnector, const char* str)
{
    return NetPrintf(pConnector, "+%s\r\n", str);
}

int xRedisServerBase::SendNullReply(xRedisConnectorBase *pConnector)
{
    return NetPrintf(pConnector, "$-1\r\n");
}

/*
 * @brief 发送错误返回
 * */
int xRedisServerBase::SendErrReply(xRedisConnectorBase *pConnector, const char *errtype, const char *errmsg)
{
    return NetPrintf(pConnector, "-%s %s\r\n", errtype, errmsg);
}

int xRedisServerBase::SendIntReply(xRedisConnectorBase *pConnector, int64_t ret)
{
    return NetPrintf(pConnector, ":%" PRId64 "\r\n", ret);
}

int xRedisServerBase::SendBulkReply(xRedisConnectorBase *pConnector, const std::string &strResult)
{
    NetPrintf(pConnector, "$%zu\r\n", strResult.size());
    SendData(pConnector, strResult.c_str(), strResult.size());
    SendData(pConnector, "\r\n", 2);
    return 0;
}

int xRedisServerBase::SendMultiBulkReply(xRedisConnectorBase *pConnector, const std::vector<std::string> &vResult)
{
    NetPrintf(pConnector, "*%zu\r\n", vResult.size());
    for (size_t i = 0; i < vResult.size(); ++i) {
        SendBulkReply(pConnector, vResult[i]);
    }
    return 0;
}

