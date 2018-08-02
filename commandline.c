/* vim: set et sw=4 ts=4 sts=4 : */
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

/** @file commandline.c
    @brief Command line argument handling
    @author Copyright (C) 2004 Philippe April <papril777@yahoo.com>
*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <syslog.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "config.h"
#include "commandline.h"
#include "debug.h"
#include "version.h"
#include "utils.h"

typedef void signal_func(int);

static signal_func *set_signal_handler(int signo, signal_func *func);
static void usage(const char *appname);

static int is_daemon = 1;

static char *confile = NULL;

/*
 * Fork a child process and then kill the parent so make the calling
 * program a daemon process.
 */
//守护进程是非交互的程序,没有控制终端,没有任何输出
/*
进程组:
1 每个进程属于一个进程组
2 每个进程组都有一个进程组号,该进程组的组长pid即为进程组号
3 一个进程只能为它自己活着子进程设置进程组ID号

登录会话是一个或多个进程组的集合
如果调用setsid()的进程不是一个进程组的组长,那么就会新建一个会话
1. 此进程会变成该会话期的首进程
2. 此进程会变成一个新的进程组的组长进程
3. 此进程没有控制终端,如果在调用setsid()之前,该进程有控制终端,那么该终端的联系解除
如果该进程是一个进程组的组长,那么该函数返回错误!!
4. 为保证3不报错,需要先fork(),然后父进程exit(), 子进程来调用setsid()

两次fork()调用的目的:
1. 第一次调用fork()是为了保证调用setsid()不报错,因为执行setsid()的进程如果是进程组的组长会报错,
如果是子进程,那就不会报错了,并且会新产生一个session会话,与原来的父进程会话脱离,也就是与终端脱离,
终端的任何操作和信号,都不会影响这个子进程了
2. 第二次调用fork()是为了终止上一个父进程,因为上一个父进程是这个会话的首进程,是可能与其他终端关联的
关闭了这个会话的首进程,保留了子进程,那么就能保证这个子进程无法与其他终端再次关联,因为不是会话的首进程了
第二次fork()不是必须的,是可选的
*/
static void makedaemon(void)
{
    //首先fork(), !=0的是父进程,直接退出,==0的是子进程
    if (fork() != 0)
        exit(0);

    //脱离父进程会话组,新建一个会话,便于会话同终端的分离,parent退出将不会影响子进程
    setsid();
    //忽略SIGHUP
    set_signal_handler(SIGHUP, SIG_IGN);

    //继续fork, 父进程退出
    if (fork() != 0)
        exit(0);

    //设置默认权限--x,wrx,wrx
    umask(0177);

    //关闭stdin,stdout,stderr
    close(0);
    close(1);
    close(2);
}

/*
 * Pass a signal number and a signal handling function into this function
 * to handle signals sent to the process.
 */

//重新设置信号处理函数
static signal_func *set_signal_handler(int signo, signal_func *func)
{
    struct sigaction act, oact;

    act.sa_handler = func;
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    if (signo == SIGALRM) {
#ifdef SA_INTERRUPT
        act.sa_flags |= SA_INTERRUPT; /* SunOS 4.x */
#endif
    } else {
#ifdef SA_RESTART
        act.sa_flags |= SA_RESTART; /* SVR4, 4.4BSD */
#endif
    }

    if (sigaction(signo, &act, &oact) < 0)
        return SIG_ERR;

    return oact.sa_handler;
}

int get_daemon_status()
{
    return is_daemon;
}

/** @internal
 * @brief Print usage
 *
 * Prints usage, called when wifidog is run with -h or with an unknown option
 */
static void usage(const char *appname)
{
    fprintf(stdout, "Usage: %s [options]\n", appname);
    fprintf(stdout, "\n");
    fprintf(stdout, "options:\n");
    fprintf(stdout, "  -c [filename] Use this config file\n");
    fprintf(stdout, "  -f            Run in foreground\n");
    fprintf(stdout, "  -d <level>    Debug level\n");
    fprintf(stdout, "  -h            Print usage\n");
    fprintf(stdout, "  -v            Print version information\n");
    fprintf(stdout, "  -r            Print run id of client\n");
    fprintf(stdout, "\n");
}

/** Uses getopt() to parse the command line and set configuration values
 * also populates restartargv
 */
void parse_commandline(int argc, char **argv)
{
    int c;
    int flag = 0;

    while (-1 != (c = getopt(argc, argv, "c:hfd:sw:vrx:i:a:"))) {

        switch (c) {

            case 'h':
                usage(argv[0]);
                exit(1);
                break;

            case 'c':
                if (optarg) {
                    confile = strdup(optarg);   // never free it
                    assert(confile);

                    flag = 1;
                }
                break;

            case 'f':
                is_daemon            = 0;
                debugconf.log_stderr = 1;
                break;

            case 'd':
                if (optarg) {
                    debugconf.debuglevel = atoi(optarg);
                }
                break;

            case 'v':
                fprintf(stdout, "version: " VERSION "\n");
                exit(1);
                break;

            case 'r': {
                char ifname[16] = {0};
                //读取ifname接口名字
                if (get_net_ifname(ifname, 16)) {
                    debug(LOG_ERR, "error: get device sign ifname failed!");
                    exit(0);
                }

                char if_mac[64] = {0};
                //根据ifname获取mac地址
                if (get_net_mac(ifname, if_mac, sizeof(if_mac))) {
                    debug(LOG_ERR, "error: Hard ware MAC address of [%s] get failed!", ifname);
                    exit(0);
                }

                //直接拿mac地址作为runid,打印出来
                fprintf(stdout, "run ID:%s\n", if_mac);
                exit(1);
                break;
            }
            default:
                usage(argv[0]);
                exit(1);
                break;
        }
    }

    if (!flag) {
        usage(argv[0]);
        exit(0);
    }

    //加载配置
    load_config(confile);

    if (is_daemon) {
        //精灵进程惯用法
        makedaemon();
    }
}
