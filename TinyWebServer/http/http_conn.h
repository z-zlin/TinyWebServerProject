#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H
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
#include <map>

#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"
#include "../timer/lst_timer.h"
#include "../log/log.h"


//该类通过状态机模式高效地解析 HTTP 请求，支持 GET 和 POST 方法，能够处理静态文件请求和动态 CGI 请求（登录/注册功能）。同时，它还负责管理连接状态、处理超时和生成适当的 HTTP 响应。
class http_conn
{
public:
    static const int FILENAME_LEN = 200;
    static const int READ_BUFFER_SIZE = 2048;
    static const int WRITE_BUFFER_SIZE = 1024;
    enum METHOD //表示 HTTP 请求方法，包括常见的 GET、POST 等方法。
    {
        GET = 0,
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATH
    };
    enum CHECK_STATE //表示 HTTP 请求解析的状态，用于状态机解析。
    {
        CHECK_STATE_REQUESTLINE = 0,   // 正在解析请求行
        CHECK_STATE_HEADER,  // 正在解析请求头部
        CHECK_STATE_CONTENT  // 正在解析请求内容
    };
    enum HTTP_CODE  //表示 HTTP 请求处理的结果代码。
    {
        NO_REQUEST,// 请求不完整，需要继续读取
        GET_REQUEST,// 获得了一个完整的请求
        BAD_REQUEST,// 请求语法错误
        NO_RESOURCE,// 没有资源
        FORBIDDEN_REQUEST,// 权限不足
        FILE_REQUEST,// 文件请求
        INTERNAL_ERROR,// 服务器内部错误
        CLOSED_CONNECTION// 客户端已关闭连接
    };
    enum LINE_STATUS //表示从缓冲区中读取一行的状态。
    {
        LINE_OK = 0,// 读取到一个完整的行
        LINE_BAD,// 行出错
        LINE_OPEN// 行数据尚不完整
    };

public:
    http_conn() {}
    ~http_conn() {}

public:
    void init(int sockfd, const sockaddr_in &addr, char *, int, int, string user, string passwd, string sqlname);//初始化 HTTP 连接，设置 socket、地址、根目录、触发模式、日志开关、数据库信息等。
    void close_conn(bool real_close = true);//关闭连接，real_close 表示是否真正关闭连接。
    void process();//处理 HTTP 请求的入口函数。
    bool read_once();//读取客户端数据
    bool write();//向客户端写入数据。
    sockaddr_in *get_address()//获取客户端地址信息。
    {
        return &m_address;
    }
    void initmysql_result(connection_pool *connPool);//初始化数据库查询结果，将用户名和密码加载到内存中。
    int timer_flag;// 定时器标志，表示该定时器是否需要被删除，0表示不需要，1表示需要。
    int improv;// 改进标志


private:
    void init();//初始化 HTTP 连接的各个状态变量。
    HTTP_CODE process_read();//这些函数用于解析 HTTP 请求，采用状态机模式。
    bool process_write(HTTP_CODE ret);//这些函数用于解析 HTTP 请求，采用状态机模式。
    HTTP_CODE parse_request_line(char *text);//这些函数用于解析 HTTP 请求，采用状态机模式。
    HTTP_CODE parse_headers(char *text);//这些函数用于解析 HTTP 请求，采用状态机模式。
    HTTP_CODE parse_content(char *text);//这些函数用于解析 HTTP 请求，采用状态机模式。
    HTTP_CODE do_request();//这些函数用于解析 HTTP 请求，采用状态机模式。
    char *get_line() { return m_read_buf + m_start_line; };//这些函数用于解析 HTTP 请求，采用状态机模式。
    LINE_STATUS parse_line();//这些函数用于解析 HTTP 请求，采用状态机模式。
    void unmap();//这些函数用于生成 HTTP 响应。
    bool add_response(const char *format, ...);//这些函数用于生成 HTTP 响应。
    bool add_content(const char *content);//这些函数用于生成 HTTP 响应。
    bool add_status_line(int status, const char *title);//这些函数用于生成 HTTP 响应。
    bool add_headers(int content_length);//这些函数用于生成 HTTP 响应。
    bool add_content_type();//这些函数用于生成 HTTP 响应。
    bool add_content_length(int content_length);//这些函数用于生成 HTTP 响应。
    bool add_linger();//这些函数用于生成 HTTP 响应。
    bool add_blank_line();//这些函数用于生成 HTTP 响应。

public:
    static int m_epollfd;// 所有连接共享的epoll文件描述符
    static int m_user_count;// 统计用户数量
    MYSQL *mysql;  // 数据库连接
    int m_state;  //读为0, 写为1

private:
    int m_sockfd;// 该HTTP连接的socket
    sockaddr_in m_address;// 通信的socket地址


    //缓冲区相关,这些变量管理读写缓冲区及其状态
    char m_read_buf[READ_BUFFER_SIZE]; // 读缓冲区
    long m_read_idx;// 标识读缓冲区中已经读入的客户端数据的最后一个字节的下一个位置
    long m_checked_idx;// 当前正在分析的字符在读缓冲区中的位置
    int m_start_line; // 当前正在解析的行的起始位置
    char m_write_buf[WRITE_BUFFER_SIZE];// 写缓冲区
    int m_write_idx;// 写缓冲区中待发送的字节数

    //解析状态相关,这些变量用于维护 HTTP 请求解析的状态
    CHECK_STATE m_check_state; // 当前主状态机的状态
    METHOD m_method;// 请求方法
    char m_real_file[FILENAME_LEN];// 客户请求的目标文件的完整路径
    char *m_url;// 请求目标文件的文件名
    char *m_version;// 协议版本
    char *m_host;// 主机名
    long m_content_length;// HTTP请求的消息总长度
    bool m_linger;// HTTP请求是否要保持连接


    //文件相关,这些变量用于管理请求的文件和发送操作
    char *m_file_address;// 客户请求的目标文件被mmap到内存中的起始位置
    struct stat m_file_stat;// 目标文件的状态
    struct iovec m_iv[2]; // 采用writev来执行写操作
    int m_iv_count;// 表示被写内存块的数量

    
    int cgi;        //是否启用的POST
    char *m_string; //存储请求头数据
    int bytes_to_send;// 剩余要发送的字节数
    int bytes_have_send; // 已经发送的字节数
    char *doc_root;// 网站根目录

    map<string, string> m_users;// 存储用户名和密码
    int m_TRIGMode;// 触发模式
    int m_close_log;// 日志开关

    char sql_user[100];// 数据库用户名
    char sql_passwd[100];// 数据库密码
    char sql_name[100];// 数据库名
};

#endif
