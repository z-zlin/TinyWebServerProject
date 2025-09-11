#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <memory>
#include <string>
#include <sys/epoll.h>

#include "./threadpool/threadpool.h"
#include "./http/http_conn.h"

constexpr int MAX_FD = 65536;           //最大文件描述符
constexpr int MAX_EVENT_NUMBER = 10000; //最大事件数
constexpr int TIMESLOT = 5;             //最小超时单位
static_assert(MAX_FD > 0 && MAX_EVENT_NUMBER > 0 && TIMESLOT > 0, "Constants must be positive");

class WebServer
{
public:
    WebServer();//构造函数：初始化 HTTP 连接数组、设置根目录路径、创建定时器数组
    ~WebServer();//析构函数：清理资源，关闭文件描述符，释放内存

    void init(int port , string user, string passWord, string databaseName,
              int log_write , int opt_linger, int trigmode, int sql_num,
              int thread_num, int close_log, int actor_model);//初始化服务器配置参数
    
    //组件初始化函数
    void thread_pool();// 初始化线程池
    void sql_pool();// 初始化数据库连接池
    void log_write();// 初始化日志系统
    void trig_mode();// 设置触发模式
    void eventListen();// 初始化事件监听
    void eventLoop();// 事件循环

    //连接管理函数
    void timer(int connfd, struct sockaddr_in client_address);// 为新连接创建定时器，还包括绑定文件描述符和将fd挂到epoll树上、初始化新连接
    void adjust_timer(util_timer *timer);// 调整定时器时间
    void deal_timer(util_timer *timer, int sockfd);// 处理定时器超时
    bool dealclientdata();// 处理新客户端连接
    bool dealwithsignal(bool& timeout, bool& stop_server);// 处理信号
    void dealwithread(int sockfd);// 处理读事件
    void dealwithwrite(int sockfd);// 处理写事件

public:
    //基础配置
    int m_port;// 服务器端口
    char *m_root;// 网站根目录路径
    int m_log_write;// 日志写入方式（0-同步，1-异步）
    int m_close_log;// 是否关闭日志（0-不关闭，1-关闭）
    int m_actormodel;// 并发模型（0-Proactor，1-Reacto）

    //网络相关
    int m_pipefd[2];// 管道文件描述符，用于统一事件源
    int m_epollfd;// epoll树根实例文件描述符
    http_conn *users;// HTTP 连接数组，每个元素对应一个客户端连接

    //数据库相关
    connection_pool *m_connPool;// 数据库连接池指针
    string m_user;         //登陆数据库用户名
    string m_passWord;     //登陆数据库密码
    string m_databaseName; //使用数据库名
    int m_sql_num;// 数据库连接池数量

    //线程池相关
    threadpool<http_conn> *m_pool;// 线程池指针
    int m_thread_num;// 线程池线程数量

    //epoll_event相关
    epoll_event events[MAX_EVENT_NUMBER];// epoll 事件数组

    int m_listenfd;// 监听socket文件描述符
    int m_OPT_LINGER;// 优雅关闭连接选项
    int m_TRIGMode;// 触发组合模式
    int m_LISTENTrigmode;// listenfd触发模式
    int m_CONNTrigmode;// connfd触发模式

    //定时器相关
    client_data *users_timer;// 客户端数据数组，每个元素对应一个连接的定时器信息
    Utils utils;// 工具类对象，提供定时器管理和基础操作
private:
    // 仅用于内存安全所有权管理，不改变对外接口
    std::unique_ptr<http_conn[]> users_buf_;
    std::unique_ptr<client_data[]> users_timer_buf_;
    std::unique_ptr<threadpool<http_conn>> m_pool_holder_;
    std::string m_root_storage_;
};
#endif
