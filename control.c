/* vim: set et ts=4 sts=4 sw=4 : */
/********************************************************************\
 * This program is free software; you can redistribute it and/or    *
 * modify it under the terms of the GNU General Public License as   *
 * published by the Free Software Foundation; either version 2 of   *
 * the License, or (at your option) any later version.              *
 *                                                                  *
 * This program is distributed in the hope that it will be useful,  *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of   *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the    *
 * GNU General Public License for more details.                     *
 *                                                                  *
 * You should have received a copy of the GNU General Public License*
 * along with this program; if not, contact:                        *
 *                                                                  *
 * Free Software Foundation           Voice:  +1-617-542-5942       *
 * 59 Temple Place - Suite 330        Fax:    +1-617-542-2652       *
 * Boston, MA  02111-1307,  USA       gnu@gnu.org                   *
 *                                                                  *
\********************************************************************/

/** @file control.c
    @brief xfrp control protocol implemented
    @author Copyright (C) 2016 Dengfeng Liu <liudengfeng@kunteng.org>
*/

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <assert.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <json-c/json.h>
#include <syslog.h>

#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/listener.h>
#include <event2/util.h>
#include <event2/event.h>
#include <event2/dns.h>
#include <event2/event_struct.h>

#include "debug.h"
#include "client.h"
#include "uthash.h"
#include "config.h"
#include "const.h"
#include "msg.h"
#include "control.h"
#include "uthash.h"
#include "frame.h"
#include "crypto.h"
#include "utils.h"
#include "session.h"
#include "common.h"
#include "login.h"

//全局主控
static struct control *main_ctl;
static int clients_conn_signel = 0;

static void sync_new_work_connection(struct bufferevent *bev);
static void recv_cb(struct bufferevent *bev, void *ctx);

static int is_client_connected()
{
    return clients_conn_signel;
}

static int client_connected(int is_connected)
{
    if (is_connected)
        clients_conn_signel = 1;
    else
        clients_conn_signel = 0;

    return clients_conn_signel;
}

//更新client工作状态
static int set_client_work_start(struct proxy_client *client, int is_start_work)
{
    if (is_start_work) {
        assert(client->ps);
        client->work_started = 1;
    } else
        client->work_started = 0;

    return client->work_started;
}

//状态检查
static int is_client_work_started(struct proxy_client *client)
{
    return client->work_started && client->ps;
}


//客户端开启事件回调callback
static void client_start_event_cb(struct bufferevent *bev, short what, void *ctx)
{
    struct proxy_client *client = ctx;
    assert(client);
    struct common_conf *c_conf = get_common_config();

	//断开事件或者错误事件
    if (what & (BEV_EVENT_EOF | BEV_EVENT_ERROR)) {
        if (client->ctl_bev != bev) {
            debug(LOG_ERR, "Error: should be equal");
            bufferevent_free(client->ctl_bev);
            client->ctl_bev = NULL;
        }
        debug(LOG_ERR, "Proxy connect server [%s:%d] error", c_conf->server_addr,
              c_conf->server_port);
        bufferevent_free(bev);
        free_proxy_client(client);
    } else if (what & BEV_EVENT_CONNECTED) {
		//状态:连接上了

		//新增recv_cb事件处理, 传入client作为client_start_event_cb/recv_cb的参数
        bufferevent_setcb(bev, recv_cb, NULL, client_start_event_cb, client);

		//开启client bufferevent的读写事件
        bufferevent_enable(bev, EV_READ | EV_WRITE);

		//发送workconn消息和cmdSYN给对端
        sync_new_work_connection(bev);
        debug(LOG_INFO, "proxy service start");
    }
}


//新的client连接
static void new_client_connect()
{
	//新建一个client信息结构
    struct proxy_client *client = new_proxy_client();
    struct common_conf *c_conf  = get_common_config();
    assert(c_conf);
    client->base = main_ctl->connect_base;

	//连接服务器ip:port
    struct bufferevent *bev =
        connect_server(client->base, c_conf->server_addr, c_conf->server_port);
    if (!bev) {
        debug(LOG_DEBUG, "Connect server [%s:%d] failed", c_conf->server_addr, c_conf->server_port);
        return;
    }

    debug(LOG_INFO, "work connection: connect server [%s:%d] ......", c_conf->server_addr,
          c_conf->server_port);

	//设置这个client对应的bufferevent为新建立的连接的bufferevent
    client->ctl_bev = bev;

	//开启可写事件
    bufferevent_enable(bev, EV_WRITE);

	//客户端事件
    bufferevent_setcb(bev, NULL, NULL, client_start_event_cb, client);
}

//开启proxy serivce()
static void start_proxy_services()
{
    struct proxy_service *all_ps = get_all_proxy_services();
    assert(all_ps);

    struct proxy_service *ps = NULL, *tmp = NULL;

    debug(LOG_INFO, "Start xfrp proxy services ...");

	//遍历所有的服务
    HASH_ITER(hh, all_ps, ps, tmp)
    {
        if (ps == NULL) {
            debug(LOG_ERR, "proxy service is invalid!");
            return;
        }

		//发送新的proxy服务
        send_new_proxy(ps);
    }
}

#ifdef USEENCRYPTION
static void init_msg_writer()
{
    if (!is_encoder_inited()) {
        struct frp_coder *e = init_main_encoder();
        if (e)
            sync_iv(e->iv);
    }
}

static void init_msg_reader(unsigned char *iv)
{
    if (!is_decoder_inited()) {
        struct frp_coder *d = init_main_decoder(iv);
        if (!d) {
            debug(LOG_DEBUG, "reader init faild!");
        }
    }
}
#endif   // USEENCRYPTION

//发送请求数据
static size_t request(struct bufferevent *bev, struct frame *f)
{
    size_t write_len         = 0;
    struct bufferevent *bout = NULL;

    if (bev) {
        bout = bev;
    } else {
        bout = main_ctl->connect_bev;
    }

    if (!bout)
        return 0;

    struct common_conf *c = get_common_config();
    if (!c)
        return 0;

    write_len = (size_t) f->len;
    if (0 == write_len)
        return 0;

    //直接调用bufferevent往对应的ev发送数据
    bufferevent_write(bout, f->data, write_len);
    return write_len;
}


//
static void base_control_ping(struct bufferevent *bev)
{
    if (!is_client_connected())
        return;

    struct bufferevent *bout = NULL;
	//如果传入bufferevent，则用传入的bufferevent，否则直接往主控bufferevent发送
    if (bev) {
        bout = bev;
    } else {
        bout = main_ctl->connect_bev;
    }

    if (!bout) {
        debug(LOG_ERR, "bufferevent is not legal!");
        return;
    }

	//构造NOP包来ping-pong
    struct frame *f = new_frame(cmdNOP, 0);   // ping sid is 0
    assert(f);

    request(bout, f);
    free_frame(f);
}

//发送ping
static void ping(struct bufferevent *bev)
{
    struct bufferevent *bout = NULL;
    if (bev) {
        bout = bev;
    } else {
        bout = main_ctl->connect_bev;
    }

    if (!bout) {
        debug(LOG_ERR, "bufferevent is not legal!");
        return;
    }

    /* tcp-mux using frame */
    if (get_common_config()->tcp_mux) {
        struct frame *f = new_frame(cmdNOP, 0);   // ping sid is 0
        assert(f);
        request(bout, f);
        free_frame(f);
    }

    uint32_t sid   = get_main_control()->session_id;
    char *ping_msg = "{}";

	//发送TypePing类型包
    send_msg_frp_server(bev, TypePing, ping_msg, strlen(ping_msg), sid);
}

//回送PONG
static void pong(struct bufferevent *bev, struct frame *f)
{
    struct bufferevent *bout = NULL;
    if (bev) {
        bout = bev;
    } else {
        bout = main_ctl->connect_bev;
    }

    if (!bout) {
        debug(LOG_ERR, "bufferevent is not legal!");
        return;
    }

    uint32_t sid = 0;
    if (!f || f->sid == 0)
        sid = get_main_control()->session_id;
    else
        sid = f->sid;

    char *pong_msg = "{}";

    send_msg_frp_server(bev, TypePong, pong_msg, strlen(pong_msg), sid);
}


//同步新的work connection
static void sync_new_work_connection(struct bufferevent *bev)
{
    struct bufferevent *bout = NULL;
    if (bev) {
        bout = bev;
    } else {
        bout = main_ctl->connect_bev;
    }

    if (!bout) {
        debug(LOG_ERR, "bufferevent is not legal!");
        return;
    }

    /* send new work session regist request to frps*/
    uint32_t sid    = 5;
    struct frame *f = new_frame(cmdSYN, sid);
    assert(f);

    struct work_conn *work_c = new_work_conn();
    assert(work_c);
    work_c->run_id = get_run_id();
    if (!work_c->run_id) {
        debug(LOG_ERR, "cannot found run ID, it should inited when login!");
        SAFE_FREE(work_c);
        return;
    }

	//构成work_connection_request_msg
    char *new_work_conn_request_message = NULL;
    int nret = new_work_conn_marshal(work_c, &new_work_conn_request_message);
    if (0 == nret) {
        debug(LOG_ERR, "new work connection request run_id marshal failed!");
        return;
    }

	//发送NewWorkConn请求给frps服务器
    send_msg_frp_server(bev, TypeNewWorkConn, new_work_conn_request_message, nret, f->sid);

	//发送cmdSYN消息
    request(bout, f);

    free_frame(f);
    SAFE_FREE(work_c);
}

//连接proxy server
struct bufferevent *connect_server(struct event_base *base, const char *name, const int port)
{
    //生成bufferevent io base结构
    struct bufferevent *bev = bufferevent_socket_new(base, -1, BEV_OPT_CLOSE_ON_FREE);
    assert(bev);

    //连接name:port
    if (bufferevent_socket_connect_hostname(bev, main_ctl->dnsbase, AF_INET, name, port) < 0) {

        bufferevent_free(bev);
        //失败
        return NULL;
    }

    //成功
    return bev;
}

static void set_ticker_ping_timer(struct event *timeout)
{
    struct timeval tv;
    struct common_conf *c_conf = get_common_config();
    evutil_timerclear(&tv);
    tv.tv_sec = c_conf->heartbeat_interval;
    event_add(timeout, &tv);
}

static void hb_sender_cb(evutil_socket_t fd, short event, void *arg)
{
	//主控keepalive ping-pong
    base_control_ping(NULL);
	//如果client连接，则ping
    if (is_client_connected())
        ping(NULL);

	//设置下一次timer事件
    set_ticker_ping_timer(main_ctl->ticker_ping);
}

// return: 0: raw succeed 1: raw failed
static int proxy_service_resp_raw(struct new_proxy_response *npr)
{
	//检查error
    if (npr->error && strlen(npr->error) > 2) {
        debug(LOG_ERR, "error: new proxy response error_field:%s", npr->error);
        return 1;
    }

	//检查proxy_name
    if ((!npr->proxy_name) || (strlen(npr->proxy_name) <= 0)) {
        debug(LOG_ERR, "error: new proxy response proxy name unmarshal failed!");
        return 1;
    }

	//查找是否属于合法proxy_service
    struct proxy_service *ps = NULL;
    ps                       = get_proxy_service(npr->proxy_name);
    if (!ps) {
        debug(LOG_ERR, "error: proxy_name responsed by TypeNewProxyResp not found!");
        return 1;
    }

	//检查proxy_type
    if (!ps->proxy_type) {
        debug(LOG_ERR, "error: proxy_type is NULL, it should be never happend!");
        return 1;
    }

	// ftp_cfg_proxy_name ftp类型
    if (ps->ftp_cfg_proxy_name) {
        struct proxy_service *main_ps = get_proxy_service(ps->ftp_cfg_proxy_name);
        if (main_ps) {
            debug(LOG_DEBUG, "find main ftp proxy service name [%s]", main_ps->proxy_name);
        } else {
            debug(LOG_ERR, "error: cannot find main ftp proxy service!");
            return 1;
        }

        if (npr->remote_port <= 0) {
            debug(LOG_ERR, "error: ftp remote_data_port [%d] that request from server is invalid!",
                  npr->remote_port);
            return 1;
        }
        main_ps->remote_data_port = npr->remote_port;
    }

    return 0;
}

// msg为json消息字符串, 这里是原始的消息处理
static void raw_message(struct message *msg, struct bufferevent *bev, struct proxy_client *client)
{
    if (client) {// 来自client的消息
        if (client->work_started) {
            debug(LOG_DEBUG, "raw client [%s] control message.", client->ps->proxy_name);
        }
    }

    struct start_work_conn_resp *sr = NULL;   // used in TypeStartWorkConn
    switch (msg->type) {

        //如果收到的是登录Response
        case TypeLoginResp:
            if (msg->data_p == NULL) {
                debug(LOG_ERR, "recved TypeLoginResp but no data, it should be never happend!");
                break;
            }

            //解析login返回的json串
            struct login_resp *lr = login_resp_unmarshal(msg->data_p);
            if (lr == NULL) {
                debug(LOG_ERR, "login response buffer init faild!");
                return;
            }

            //检查是否登录ok
            int is_logged = login_resp_check(lr);
#ifdef USEENCRYPTION
            if (is_logged) {
                init_msg_writer();
            }
#endif   // USEENCRYPTION

            if (!is_logged) {
                //登录失败,则重新调用login
                debug(LOG_ERR, "xfrp login failed, try again!");
				
                login();
                SAFE_FREE(lr);
                return;
            }

			//登录成功
            SAFE_FREE(lr);
            break;

        // ReqWorkConn类型事件 
        case TypeReqWorkConn: //请求WorkConnection

			//如果没有client连上来，表示本地的proxy服务没有开启，那么需要先开启下
            if (!is_client_connected()) {

                //开启proxy services
                start_proxy_services();

                //设置client已经连上
                client_connected(1);

                //发送PING消息
                ping(bev);
            }

            //新的client连上来
            new_client_connect();
            break;

        // NewProxyResp类型事件, 上一个ReqWorkConn中初始会发送TypeNewProxy消息给frps服务器,这里收到返回
        case TypeNewProxyResp: {
            if (msg->data_p == NULL) {
                debug(LOG_ERR, "recved TypeNewProxyResp but no data, it should be never happend!");
                break;
            }

			//从服务器回复中解出new_proxy_resp消息
            struct new_proxy_response *npr = new_proxy_resp_unmarshal(msg->data_p);
            if (npr == NULL) {
                debug(LOG_ERR, "new proxy response buffer unmarshal faild!");
                return;
            }

			//proxy_service_resp消息检查
            proxy_service_resp_raw(npr);

            SAFE_FREE(npr);
            break;
        }

        // StartWorkConn类型事件
        case TypeStartWorkConn:  //创建一个frp tunnel

			//TODO: 这个消息只能从client bufferevent来
        	if(client == NULL){
				break;
			}	

			//解开StartWorkConn的消息体
            sr = start_work_conn_resp_unmarshal(msg->data_p);
            if (!sr) {
                debug(LOG_ERR, "TypeStartWorkConn unmarshal failed, it should never be happend!");
                break;
            }

			//寻找是否已经有此服务
            struct proxy_service *ps = get_proxy_service(sr->proxy_name);
            if (!ps) {
                debug(LOG_ERR,
                      "TypeStartWorkConn requested proxy service [%s] not found, it should nerver "
                      "be happend!",
                      sr->proxy_name);
                break;
            }

			// 如果有这个服务,则为相应client->ps赋值proxy service
            client->ps = ps;
            debug(LOG_INFO, "proxy service [%s] [%s:%d] start work connection.", sr->proxy_name,
                  ps->local_ip, ps->local_port);

			// 服务器发送过来一个请求,来启动client tunnel, 注意proxy serivce已经确定了
			// 启动连接到frps服务器到本地端口服务连接之间的tunnel
            start_xfrp_tunnel(client);

			//设置此client开始工作
            set_client_work_start(client, 1);
            break;

        //相应PING-PONG
        case TypePong:
            pong(bev, NULL);
            break;

        default:
            break;
    }
    SAFE_FREE(sr);
}

//数据handler, 直接处理数据段
static size_t data_handler(unsigned char *buf, ushort len, struct proxy_client *client)
{
    struct bufferevent *bev = NULL;
    if (client) {
		//如果client非空,则表示由client bufferevent接收处理
        debug(LOG_DEBUG, "client(%s): recved control data",
              is_client_work_started(client) ? "work" : "free");

        // bufferevent 置为client对应的bev
        bev = client->ctl_bev;
    }
    unsigned char *ret_buf = NULL;
    struct frame *f        = NULL;

#ifdef RECV_DEBUG
    /* debug showing */
    unsigned int j = 0;
    debug(LOG_DEBUG, "RECV from frps:");
    printf("[");
    for (j = 0; j < len; j++) {
        printf("%d ", (unsigned char) buf[j]);
    }
    printf("]\n");
    /* debug show over */
#endif   //  RECV_DEBUG

    int min_buf_len = 0;
    if (get_common_config()->tcp_mux) {   // TCP 多路复用

		//先取出session帧
        f           = raw_frame(buf, len);
        min_buf_len = get_header_size();
    } else {

        //如果直接是数据帧,则直接读取frame
        f = raw_frame_only_msg(buf, len);
        set_frame_cmd(f, cmdPSH);
    }

	//检查frame是否空
    if (f == NULL) {
        debug(LOG_ERR, "raw_frame faild!");
        goto DATA_H_END;
    }

#ifdef USEENCRYPTION
    if (!is_decoder_inited() && f->len == get_block_size()) {
        init_msg_reader((unsigned char *) f->data);
        debug(LOG_DEBUG, "first recv stream message, init decoder iv succeed!");
        goto DATA_H_END;
    }
#endif   // USEENCRYPTION

	//检查长度是否满足条件
    if (len <= min_buf_len) {
        if (f->cmd == cmdNOP) {  //TODO: 这里需要改成cmdNOP
			//返回一个空NOP包
            base_control_ping(bev);
        }
        goto DATA_H_END;
    }

#ifdef ENCRYPTO
    size_t ret_len3 = encrypt_data(f->data, (size_t) f->len, get_main_encoder(), &ret_buf);
    if (ret_len3 <= 0) {
        debug(LOG_ERR, "message recved decrypt result is 0 bit");
        goto DATA_H_END;
    }

    debug(LOG_DEBUG, "message after test1:");
    for (i = 0; i < ret_len3; i++) {
        printf("%u ", (unsigned char) ret_buf[i]);
    }

    printf("\n");

    size_t ret_len2 = decrypt_data(f->data, (size_t) f->len, get_main_encoder(), &ret_buf);
    debug(LOG_DEBUG, "message after test2:");
    if (ret_len2 <= 0) {
        debug(LOG_ERR, "message recved decrypt result is 0 bit");
        goto DATA_H_END;
    }

    for (i = 0; i < ret_len2; i++) {
        printf("%u ", (unsigned char) ret_buf[i]);
    }
    printf("\n");

    size_t ret_len1 = encrypt_data(f->data, (size_t) f->len, get_main_decoder(), &ret_buf);
    if (ret_len1 <= 0) {
        debug(LOG_ERR, "message recved decrypt result is 0 bit");
        goto DATA_H_END;
    }

    debug(LOG_DEBUG, "message after test3:");
    for (i = 0; i < f->len; i++) {
        printf("%u ", (unsigned char) ret_buf[i]);
    }
    printf("\n encrypto test end \n");

    struct frp_coder *d = get_main_decoder();
    if (!d) {
        debug(LOG_ERR, "decoder (message reader) is not inited!");
        goto DATA_H_END;
    }
    size_t ret_len = decrypt_data(f->data, (size_t) f->len, d, &ret_buf);
    if (ret_len <= 0) {
        debug(LOG_ERR, "message recved decrypt result is 0 bit");
        goto DATA_H_END;
    }

    debug(LOG_DEBUG, "message after decode:");
    for (i = 0; i < f->len; i++) {
        printf("%u ", (unsigned char) ret_buf[i]);
    }
    printf("\n\n");
#endif   // ENCRYPTO

    if (!ret_buf){
		//表示未加密,则直接取frame->data域
        ret_buf = f->data;   // test: no crypto
    }

    struct message *msg = NULL;
    switch (f->cmd) {
        case cmdNOP:   // 3 no options
            break;
        case cmdSYN:   // 0 create a new session
            break;
        case cmdFIN:   // 1 close session
            break;
        case cmdPSH:   // 2

            //实际数据push过来了,先unpack
            msg = unpack(ret_buf, f->len);
            if (!(msg && msg->data_p)) { // msg有问题,忽略此消息
                debug(LOG_ERR, "message received format invalid");
                goto DATA_H_END;
            }
            debug(LOG_DEBUG, "recv <---- %c: %s", msg->type, msg->data_p);

			//TODO: 这里有内存泄漏,需要safe_free(msg)
            if (msg->data_p == NULL)
                goto DATA_H_END;

            //将raw消息解开,并进行相应处理
            raw_message(msg, bev, client);
            break;
        default:
            break;
    }

    

DATA_H_END:
	//TODO: 移到这里来好了
	SAFE_FREE(msg);
	
    free_frame(f);

    return len;
}


// 同上, ctx==NULL,表示调用从common ctrl来, ctx != NULL表示调用从client peer来
// ctx: if recv_cb was called by common control, ctx is NULL
//		when ctx is not NULL it was called by client struct
static unsigned char *multy_recv_buffer_raw(unsigned char *buf, size_t buf_len, size_t *ret_len,
                                            void *ctx)
{
    unsigned char *unraw_buf_p = NULL;
    unsigned char *raw_buf     = NULL;
    size_t split_lv            = sizeof(msg_size_t);   // latest buffer split level
    size_t split_len           = 0;
    size_t raw_static_size     = 1 + sizeof(msg_size_t);   // type 1 + bigend 8
    int splited                = 0;   // signal argument, ==1 after buffer split

    *ret_len = 0;

    if (ctx) { //ctx非空,客户端callback
        struct proxy_client *client = (struct proxy_client *) ctx;

		//如果客户端已经开始工作,则直接返回
        if (is_client_work_started(client)) {
            debug(LOG_DEBUG, "client [%s] send all work data to proxy tunnel.",
                  client->ps->proxy_name);
            return NULL;
        }
    }

    for (;;) {
        if (buf_len > split_lv) { //解析数据,判断长度是否有效
            if (!is_logged()) {
				//如果未登陆
                if (buf[0] == TypeLoginResp) { //49是登录请求的回应
                    msg_size_t data_len_bigend;
                    data_len_bigend     = *(msg_size_t *) (buf + MSG_LEN_I);
                    msg_size_t data_len = msg_ntoh(data_len_bigend);

                    split_len = raw_static_size + data_len;
                    splited   = 1;
					//分离出LoginResp数据段,直接break出来处理
                    break;
                }
            }

#ifdef USEENCRYPTION
            if (!is_decoder_inited()) {
                raw_static_size = get_block_size();
                if (buf_len < raw_static_size) {
                    break;
                }

                debug(LOG_DEBUG, "mulity raw decoder IV...");
                split_len = raw_static_size;
                splited   = 1;
                break;
            }
#endif   // USEENCRYPTION

            if (!splited) {   // ordinary message split
            	//未分离
                char msg_type  = buf[0];
				//类型检查
                int type_valid = msg_type_valid_check(msg_type);
                if (type_valid) {
                    msg_size_t data_len_bigend;
                    data_len_bigend     = *(msg_size_t *) (buf + MSG_LEN_I);
                    msg_size_t data_len = msg_ntoh(data_len_bigend);

                    split_len = raw_static_size + data_len;
                    splited   = 1;
                } else {
					//类型检查错误
                    debug(LOG_ERR, "buffer type [%c] raw failed!", msg_type);
                }
                break;
            }
        }
        break;
    }

    if (!splited) {
        //数据未分离,直接处理返回
        data_handler(buf, buf_len, ctx);
        *ret_len = 0;
        return NULL;

    } else if (split_len) {
		//数据分离过了,将分离过的数据拷贝到raw_buf
        raw_buf = calloc(1, split_len);
        assert(raw_buf);
        memcpy(raw_buf, buf, split_len);
    }

    if (split_len != 0 && raw_buf != NULL) {

		//处理分离出的数据段
        data_handler(raw_buf, split_len, ctx);
        free(raw_buf);

		//剩余数据段
        *ret_len = buf_len - split_len;
        if (split_len < buf_len) {
            unraw_buf_p = buf + split_len;
        }
    }

	//返回未处理的分段
    return unraw_buf_p;
}

// 非常重要的recv_cb回调事件
// 如果ctx为NULL,表示数据callback从common ctrl来
// 如果ctx非空,表示数据callback从client回调来的
// ctx: if recv_cb was called by common control, ctx == NULL
//		else ctx == client struct
static void recv_cb(struct bufferevent *bev, void *ctx)
{
    // 拿到evbuffer,获取buffer长度
    struct evbuffer *input = bufferevent_get_input(bev);
    int len                = evbuffer_get_length(input);

    //长度< 0 则直接返回不响应
    if (len < 0) {
        return;
    }

    //如果长度len==0呢, 这里感觉有个bug， 直接在上面的判断中加 <= 0

    //构造buffer缓冲
    unsigned char *buf = calloc(1, len);
    assert(buf);

    size_t read_n  = 0;
    size_t ret_len = 0;

    //从evbuffer中读取len长度的数据到buf中
    read_n = evbuffer_remove(input, buf, len);

    //
    struct proxy_client *client = (struct proxy_client *) ctx;

    //如果拿到的size > 0
    if (read_n) {

        //准备解析buf
        unsigned char *raw_buf_p = buf;
        for (; raw_buf_p && read_n;) {
// #define CONN_DEBUG 1
#ifdef CONN_DEBUG
			//打印raw数据
            unsigned int i = 0;
            char *dbg_buf  = calloc(1, read_n * 4 + 1);
            assert(dbg_buf);
            for (i = 0; i < read_n && ((2 * i) < (read_n * 2 + 1)); i++) {
                snprintf(dbg_buf + 4 * i, 5, "%3u ", (unsigned char) raw_buf_p[i]);
            }
            debug(LOG_DEBUG, "[%s]: RECV ctl byte:%s", client ? "client" : "control", dbg_buf);
            SAFE_FREE(dbg_buf);
#endif   // CONN_DEBUG

            //进行数据处理，返回剩余部分数据
            raw_buf_p = multy_recv_buffer_raw(raw_buf_p, read_n, &ret_len, client);

			//剩余未处理部分
            read_n    = ret_len;

            if (ctx && is_client_work_started(client) && raw_buf_p && ret_len) {

                debug(LOG_WARNING, "warning: data recved from frps is not split clear");
                unsigned char *dtail = calloc(1, read_n);
                assert(dtail);

				//拷贝剩余数据
                memcpy(dtail, raw_buf_p, read_n);
                client->data_tail      = dtail;
                client->data_tail_size = ret_len;

				//将剩余数据发送到client
                send_client_data_tail(client);

				//清理
                SAFE_FREE(dtail);
                client->data_tail      = NULL;
                client->data_tail_size = 0;
            }
        }
    } else {
        debug(LOG_DEBUG, "recved message but evbuffer_remove faild!");
    }
    SAFE_FREE(buf);

    return;
}

//开始一个新的session
static void open_connection_session(struct bufferevent *bev)
{
    // SYN frame
    struct frame *f = new_frame(cmdSYN, main_ctl->session_id);
    assert(f);

    //发送SYN frame
    request(bev, f);

    //释放
    free_frame(f);
}

// connect callback回调
static void connect_event_cb(struct bufferevent *bev, short what, void *ctx)
{
    struct common_conf *c_conf = get_common_config();
    static int retry_times     = 0;

    //状态, EOF || ERROR
    if (what & (BEV_EVENT_EOF | BEV_EVENT_ERROR)) {

        //重试10次,失败则退出
        if (retry_times >= 10) {   // only try 10 times consecutively
            debug(LOG_ERR, "have retry connect to xfrp server for %d times, exit!", retry_times);

            exit(0);
        }

        retry_times++;
        debug(LOG_ERR, "error: connect server [%s:%d] failed", c_conf->server_addr,
              c_conf->server_port);

        //释放失败的control
        free_control();

        //继续初始化,开始重试
        init_main_control();
        start_base_connect();
        close_main_control();
    } else if (what & BEV_EVENT_CONNECTED) {

        //状态是CONNECTED,重置retry
        retry_times = 0;

        // 设置新的bev, callback事件
        // void bufferevent_setcb(struct bufferevent *bufev,
        // bufferevent_data_cb readcb, bufferevent_data_cb writecb,
        // bufferevent_event_cb eventcb, void *cbarg)
        //
        // 设置read,write,event事件回调
        //
        // 最主要的事件在recv_cb回调中
        // 增加recv_cb到connect bufferevent中
        // recv login-response message before recving othfer fprs messages,
        bufferevent_setcb(bev, recv_cb, NULL, connect_event_cb, NULL);

        //开启读写,并持久Persist,不清除读写标志
        bufferevent_enable(bev, EV_READ | EV_WRITE | EV_PERSIST);

        //如果tcp_mux,则构造init包,发送
        if (get_common_config()->tcp_mux)
            open_connection_session(bev);

        //登录
        login();
    }
}

static void keep_control_alive()
{
	//新建一个ev timer事件
    main_ctl->ticker_ping = evtimer_new(main_ctl->connect_base, hb_sender_cb, NULL);
    if (!main_ctl->ticker_ping) {
        debug(LOG_ERR, "Ping Ticker init failed!");
        return;
    }
    set_ticker_ping_timer(main_ctl->ticker_ping);
}

// dns callback dns回调,对返回addr进行解析
static void server_dns_cb(int event_code, struct evutil_addrinfo *addr, void *ctx)
{
    if (event_code) {
        set_common_server_ip((const char *) evutil_gai_strerror(event_code));
    } else {
        struct evutil_addrinfo *ai;
        if (addr->ai_canonname)
            debug(LOG_DEBUG, "addr->ai_canonname [%s]", addr->ai_canonname);
        for (ai = addr; ai; ai = ai->ai_next) {
            char buf[128];
            const char *s = NULL;
            if (ai->ai_family == AF_INET) {
                struct sockaddr_in *sin = (struct sockaddr_in *) ai->ai_addr;
                s                       = evutil_inet_ntop(AF_INET, &sin->sin_addr, buf, 128);
            } else if (ai->ai_family == AF_INET6) {
                struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *) ai->ai_addr;
                s                         = evutil_inet_ntop(AF_INET6, &sin6->sin6_addr, buf, 128);
            }

            if (s)
                set_common_server_ip(s);
        }
        if (addr)
            evutil_freeaddrinfo(addr);
    }
}


//连接到server的ip:port
void start_base_connect()
{
    struct common_conf *c_conf = get_common_config();

    //连接server,connect_bev存入主控结构
    main_ctl->connect_bev =
        connect_server(main_ctl->connect_base, c_conf->server_addr, c_conf->server_port);
    //连接失败,则退出
    if (!main_ctl->connect_bev) {
        debug(LOG_ERR, "error: connect server [%s:%d] failed", c_conf->server_addr,
              c_conf->server_port);
        exit(0);
    }

    debug(LOG_INFO, "connect server [%s:%d]...", c_conf->server_addr, c_conf->server_port);

    //连接成功后,开启bufferevent io的读写
    bufferevent_enable(main_ctl->connect_bev, EV_WRITE | EV_READ);
    //设置bufferevent io的callback回调, 实质内容都在connect_event_cb
    //设置read,write事件为NULL,设置event事件为connect_event_cb
    bufferevent_setcb(main_ctl->connect_bev, NULL, NULL, connect_event_cb, NULL);
}


//加密同步iv init-vector
void sync_iv(unsigned char *iv)
{
    struct frame *f = new_frame(cmdPSH, main_ctl->session_id);
    assert(f);

    f->len  = (ushort) get_encrypt_block_size();
    f->data = calloc(f->len, 1);
    memcpy(f->data, iv, f->len);

    struct bufferevent *bout = main_ctl->connect_bev;
    if (!bout) {
        return;
    }

    request(bout, f);
    SAFE_FREE(f->data);
    free_frame(f);
}

//发送登录请求
void login()
{
    char *lg_msg = NULL;

    //构造login请求的json串
    int len = login_request_marshal(&lg_msg);   // marshal login request
    if (!lg_msg || !len) {
        debug(LOG_ERR, "error: login_request_marshal failed, it should never be happenned");
        exit(0);
    }

    // using sid = 3 is only for matching fprs, it will change after using tcp-mux
    if (get_common_config()->tcp_mux)
        sync_session_id(3);

    //发送LoginType消息lg_msg到proxy-server
    send_msg_frp_server(NULL, TypeLogin, lg_msg, len, main_ctl->session_id);
    SAFE_FREE(lg_msg);
}

void sync_session_id(uint32_t sid)
{
    struct frame *f = new_frame(cmdNOP, sid);
    assert(f);

    size_t send_len = request(NULL, f);
    debug(LOG_DEBUG, "sync session id %d, len %ld", sid, send_len);
    SAFE_FREE(f);
}


//向bev发送type类型数据包，消息体是json字符串格式，没有进行消息pack
void send_msg_frp_server(struct bufferevent *bev, const enum msg_type type, const char *msg,
                         const size_t msg_len, uint32_t sid)
{
    struct bufferevent *bout = NULL;
    if (bev) {
        bout = bev;
    } else {
        bout = main_ctl->connect_bev;
    }
    assert(bout);

    debug(LOG_DEBUG, "send ----> [%c: %s]", type, msg);

    // msg结构
    struct message req_msg;
    req_msg.data_p   = NULL;
    req_msg.type     = type;
    req_msg.data_len = msg_len;

    char frame_type = 0;
    struct frame *f = NULL;

    //构造新的frame
    // frame_type not truely matter, it will reset by set_frame_cmd
    f = new_frame(frame_type, sid);
    assert(f);

    // json的string msg填入req_msg.data_p
    if (msg) {
        req_msg.data_p = strdup(msg);
        assert(req_msg.data_p);
    }

    //消息进行pack
    unsigned char *pack_buf = NULL;
    size_t pack_buf_len     = pack(&req_msg, &pack_buf);
    if (!pack_buf_len || !pack_buf) {
        debug(LOG_ERR, "error: send buffer pack failed!");
        goto S_M_F_END;
    }

    //设置frame的长度为pack后数据长度
    set_frame_len(f, (ushort) pack_buf_len);
    //设置frame的data字段为pack数据
    f->data = pack_buf;

    if (get_common_config()->tcp_mux) { //多路复用
        switch (type) {
            case TypeLogin:
            case TypePong:
            case TypePing:
            case TypeNewProxy:
                frame_type = cmdPSH;
                break;

            default:
                break;
        }
    } else {
        frame_type = cmdPSH; //非多路，直接PUSH类型
    }

    set_frame_cmd(f, frame_type);

    //直接发送frame出去
    request(bout, f);

S_M_F_END:
    SAFE_FREE(req_msg.data_p);
    SAFE_FREE(pack_buf);
    f->data = NULL;
    free_frame(f);
}

struct control *get_main_control()
{
    return main_ctl;
}

//登录frp server, 似乎跟start_base_connect重复了???(应该是重复了)
void start_login_frp_server(struct event_base *base)
{
    struct common_conf *c_conf = get_common_config();

	//登录server ip:port
    struct bufferevent *bev    = connect_server(base, c_conf->server_addr, c_conf->server_port);
    if (!bev) {
        debug(LOG_DEBUG, "Connect server [%s:%d] failed", c_conf->server_addr, c_conf->server_port);
        return;
    }

    debug(LOG_INFO, "Xfrpc login: connect server [%s:%d] ...", c_conf->server_addr,
          c_conf->server_port);

    bufferevent_enable(bev, EV_WRITE | EV_READ);
	//设置connect事件回调
    bufferevent_setcb(bev, NULL, NULL, connect_event_cb, NULL);
}

//向frps服务器发送NewProxy消息,请求开一个Proxy服务
void send_new_proxy(struct proxy_service *ps)
{
    if (!ps) {
        debug(LOG_ERR, "proxy service is invalid!");
        return;
    }
    debug(LOG_DEBUG, "control proxy client: [%s]", ps->proxy_name);

    char *new_proxy_msg = NULL;

	//构造新的proxy_service消息json字符串
    int len             = new_proxy_service_marshal(ps, &new_proxy_msg);
    if (!new_proxy_msg) {
        debug(LOG_ERR, "proxy service request marshal failed");
        return;
    }

	//向Server主控发送TypeNewProxy消息结构
    send_msg_frp_server(NULL, TypeNewProxy, new_proxy_msg, len, main_ctl->session_id);
    SAFE_FREE(new_proxy_msg);
}

void init_main_control()
{
    //主控
    if (main_ctl && main_ctl->connect_base) {
        event_base_loopbreak(main_ctl->connect_base);
        free(main_ctl);
    }

    //主控初始化
    main_ctl = calloc(sizeof(struct control), 1);
    assert(main_ctl);

    // conf
    struct common_conf *c_conf = get_common_config();

    // tcp mux
    if (c_conf->tcp_mux) {
        //会话id
        uint32_t *sid = init_sid_index();
        assert(sid);
        main_ctl->session_id = *sid;

        debug(LOG_DEBUG, "Connect Frps with control session ID: %d", main_ctl->session_id);
    }

    struct event_base *base    = NULL;
    struct evdns_base *dnsbase = NULL;

    //初始化event base, libevent
    base = event_base_new();
    if (!base) {
        debug(LOG_ERR, "error: event base init failed!");
        exit(0);
    }
    main_ctl->connect_base = base;

    //初始化evdns base
    dnsbase = evdns_base_new(base, 1);
    if (!dnsbase) {
        debug(LOG_ERR, "error: evdns base init failed!");
        exit(0);
    }
    main_ctl->dnsbase = dnsbase;

    //设置超时
    evdns_base_set_option(dnsbase, "timeout", "1.0");

    //设置dns
    // thanks to the following article
    // http://www.wuqiong.info/archives/13/
    evdns_base_set_option(dnsbase, "randomize-case:", "0");     // TurnOff DNS-0x20 encoding
    evdns_base_nameserver_ip_add(dnsbase, "180.76.76.76");      // BaiduDNS
    evdns_base_nameserver_ip_add(dnsbase, "223.5.5.5");         // AliDNS
    evdns_base_nameserver_ip_add(dnsbase, "223.6.6.6");         // AliDNS
    evdns_base_nameserver_ip_add(dnsbase, "114.114.114.114");   // 114DNS

    //如果给定的是ip地址,直接返回,不需要进行dns解析
    // if server_addr is ip, done control init.
    if (is_valid_ip_address((const char *) c_conf->server_addr))
        return;

    // if server_addr is domain, analyze it to ip for server_ip
    debug(LOG_DEBUG, "Get ip address of [%s] from DNServer", c_conf->server_addr);

    struct evutil_addrinfo hints;
    struct evdns_getaddrinfo_request *dns_req;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_flags    = EVUTIL_AI_CANONNAME;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    // dns查询动作,并设置callback动作->server_dns_cb;
    dns_req = evdns_getaddrinfo(dnsbase, c_conf->server_addr, NULL /* no service name given */,
                                &hints, server_dns_cb, NULL);
    if (!dns_req) {
        debug(LOG_ERR, "error: can not analyse the dns of [%s]", c_conf->server_addr);
        exit(0);
    }
}

void close_main_control()
{
    assert(main_ctl);
    event_base_dispatch(main_ctl->connect_base);
    event_base_free(main_ctl->connect_base);
    evdns_base_free(main_ctl->dnsbase, 0);
}

//主控循环
void run_control()
{
    start_base_connect();
    keep_control_alive();
}

void free_control()
{
    if (!main_ctl)
        return;

    SAFE_FREE(main_ctl);
}