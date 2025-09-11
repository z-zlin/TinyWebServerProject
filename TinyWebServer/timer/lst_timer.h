#ifndef LST_TIMER
#define LST_TIMER

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>

#include <time.h>
#include "../log/log.h"

class util_timer;

struct client_data
{
    sockaddr_in address;// 客户端socket地址
    int sockfd;// 客户端socket文件描述符
    util_timer *timer;// 指向对应定时器的指针
};

class util_timer//定时器节点类，包含超时时间、回调函数和链表指针。
{
public:
    util_timer() : prev(nullptr), next(nullptr) {}

public:
    time_t expire; // 定时器超时时间（绝对时间）
    
    void (* cb_func)(client_data *);// 回调函数指针，用于处理超时
    client_data *user_data;// 指向客户端数据的指针
    util_timer *prev;// 指向前一个定时器
    util_timer *next;// 指向后一个定时器
};

class sort_timer_lst
{
public:
    sort_timer_lst();//构造函数：初始化链表头尾指针为NULL
    ~sort_timer_lst();//析构函数：遍历链表并删除所有定时器节点

    void add_timer(util_timer *timer);// 添加定时器
    void adjust_timer(util_timer *timer);// 调整定时器位置
    void del_timer(util_timer *timer);// 删除定时器
    void tick();// 处理超时定时器

private:
    void add_timer(util_timer *timer, util_timer *lst_head);// 内部添加函数

    util_timer *head;// 链表头指针
    util_timer *tail;// 链表尾指针
};

class Utils//工具类，提供信号处理、文件描述符设置和定时器管理等功能
{
public:
    Utils() {}
    ~Utils() {}

    void init(int timeslot);// 初始化时间槽

    //对文件描述符设置非阻塞
    int setnonblocking(int fd);

    //将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
    void addfd(int epollfd, int fd, bool one_shot, int TRIGMode);

    //信号处理函数
    static void sig_handler(int sig);

    //设置信号函数
    void addsig(int sig, void(handler)(int), bool restart = true);

    //定时处理任务，重新定时以不断触发SIGALRM信号
    void timer_handler();

    void show_error(int connfd, const char *info);// 显示错误信息

public:
    static int *u_pipefd;// 管道文件描述符（用于统一事件源）
    sort_timer_lst m_timer_lst;// 定时器链表
    static int u_epollfd;// epoll文件描述符
    int m_TIMESLOT;// 时间槽（定时器超时单位）
};

void cb_func(client_data *user_data);//定时器超时时的回调函数，关闭客户端连接并从epoll中移除。

#endif
