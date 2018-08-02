#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <syslog.h>

#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/listener.h>
#include <event2/event.h>

#include "debug.h"
#include "uthash.h"
#include "common.h"
#include "proxy.h"

//TCP client->server callback, 都是读callback
//
// read from client-working host port
void tcp_proxy_c2s_cb(struct bufferevent *bev, void *ctx)
{
    struct proxy *p             = (struct proxy *) ctx;
    struct bufferevent *partner = p ? p->bev : NULL;
    struct evbuffer *src, *dst;
    size_t len;
    src = bufferevent_get_input(bev);
    len = evbuffer_get_length(src);

	//读到>0的数据,直接将数据放到proxy对应的另一端的buffevent中
    if (len > 0) {
        dst = bufferevent_get_output(partner);
        evbuffer_add_buffer(dst, src);
    }
}


//TODO: 似乎有改进的余地?? 利用libevent的input/output交换
//TCP server->client callback
void tcp_proxy_s2c_cb(struct bufferevent *bev, void *ctx)
{
    struct proxy *p             = (struct proxy *) ctx;
    struct bufferevent *partner = p ? p->bev : NULL;
    struct evbuffer *src, *dst;
    src = bufferevent_get_input(bev);
    dst = bufferevent_get_output(partner);

	//直接把读到的src 加到 dst的后面
    evbuffer_add_buffer(dst, src);
}