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
#include <vector>
#include <mutex>
#include <unordered_map>
#include <queue>
#include <condition_variable>
#include <atomic>
#include <thread>
#include <chrono>

#include <time.h>
#include "../log/log.h"

// 时间管理优化：使用高精度单调时钟
using steady_time_point = std::chrono::steady_clock::time_point;
using steady_duration = std::chrono::steady_clock::duration;
using milliseconds = std::chrono::milliseconds;

class util_timer;

// 时间缓存管理器 - 减少系统调用，提高性能
class time_cache_manager {
public:
    static time_cache_manager& get_instance();
    
    // 获取当前时间（缓存版本）
    steady_time_point get_current_time();
    
    // 启动时间缓存更新线程
    void start();
    
    // 停止时间缓存更新线程
    void stop();
    
    // 将time_t转换为steady_time_point（向后兼容）
    steady_time_point time_t_to_steady(time_t t);
    
    // 将steady_time_point转换为time_t（向后兼容）
    time_t steady_to_time_t(steady_time_point tp);
    
    // 添加毫秒到时间点
    steady_time_point add_milliseconds(steady_time_point base, int64_t ms);
    
    // 计算时间差（毫秒）
    int64_t duration_to_milliseconds(steady_duration d);
    
    // 获取当前时间（time_t格式，缓存版本）
    time_t get_current_time_t();
    
    // 便捷方法：获取当前时间并添加指定秒数
    time_t get_current_time_t_plus_seconds(int seconds);
    
    // 便捷方法：检查时间缓存是否正在运行
    bool is_running() const { return running.load(); }

private:
    time_cache_manager();
    ~time_cache_manager();
    
    void update_loop(); // 时间更新循环
    
    steady_time_point cached_time;
    std::mutex cached_time_mutex;
    std::atomic<bool> running;
    std::thread update_thread;
    std::mutex update_mutex;
    std::condition_variable update_cv;
    milliseconds update_interval;
    
    // 系统启动时间，用于time_t转换
    steady_time_point system_start_time;
    time_t system_start_time_t;
};

struct client_data
{
    sockaddr_in address;// 客户端socket地址
    int sockfd;// 客户端socket文件描述符
    util_timer *timer;// 指向对应定时器的指针
};

class util_timer//定时器节点类，包含超时时间、回调函数和链表指针。
{
public:
    util_timer() : prev(nullptr), next(nullptr), deleted(false) {}

public:
    // 高精度时间管理
    steady_time_point expire; // 定时器超时时间（高精度单调时钟）
    
    void (* cb_func)(client_data *);// 回调函数指针，用于处理超时
    client_data *user_data;// 指向客户端数据的指针
    util_timer *prev;// 指向前一个定时器（保留用于兼容性，但不再使用）
    util_timer *next;// 指向后一个定时器（保留用于兼容性，但不再使用）
    bool deleted; // 标记是否已删除，用于延迟删除
    
    // 简化的接口 - 只保留实际使用的接口
    void set_expire_milliseconds(int64_t ms);
};

// 超时事件结构
struct timeout_event {
    util_timer* timer;
    steady_time_point expire_time;
    
    timeout_event(util_timer* t, steady_time_point et) : timer(t), expire_time(et) {}//构造函数,初始化超时事件.
};

// 超时事件处理器类
class timeout_event_processor {
public:
    timeout_event_processor();
    ~timeout_event_processor();
    
    void start(); // 启动处理线程
    void stop();  // 停止处理线程
    void add_timeout_event(util_timer* timer, steady_time_point expire_time); // 添加超时事件
    void set_batch_size(size_t size) { max_batch_size = size; } // 设置批处理大小
    void set_max_queue_size(size_t size) { max_queue_size = size; } // 设置最大队列大小
    
private:
    void process_loop(); // 处理循环
    void process_batch(); // 批量处理超时事件
    
    std::queue<timeout_event> event_queue; // 超时事件队列
    std::mutex queue_mutex; // 队列互斥锁
    std::condition_variable queue_cv; // 队列条件变量
    std::thread processor_thread; // 处理线程
    std::atomic<bool> running; // 运行状态
    std::atomic<size_t> max_batch_size; // 最大批处理大小
    std::atomic<size_t> max_queue_size; // 最大队列大小
    std::atomic<size_t> current_queue_size; // 当前队列大小
};

class sort_timer_lst
{
public:
    sort_timer_lst();//构造函数：初始化堆
    ~sort_timer_lst();//析构函数：清空堆并删除所有定时器节点

    void add_timer(util_timer *timer);// 添加定时器
    void adjust_timer(util_timer *timer);// 调整定时器位置
    void del_timer(util_timer *timer);// 删除定时器
    void tick();// 处理超时定时器（惰性删除模式）
    
    // 新增方法
    void start_timeout_processor(); // 启动超时事件处理器
    void stop_timeout_processor();  // 停止超时事件处理器
    void set_batch_size(size_t size); // 设置批处理大小
    void set_max_queue_size(size_t size); // 设置最大队列大小

private:
    void heapify_up(int index);// 向上调整堆
    void heapify_down(int index);// 向下调整堆
    void swap_nodes(int i, int j);// 交换两个节点并更新索引映射
    int get_parent(int index) const { return (index - 1) / 2; }// 获取父节点索引
    int get_left_child(int index) const { return 2 * index + 1; }// 获取左子节点索引
    int get_right_child(int index) const { return 2 * index + 2; }// 获取右子节点索引
    
    std::vector<util_timer*> timer_heap;// 定时器堆（vector实现）
    std::unordered_map<util_timer*, int> timer_index_map;// 定时器到索引的映射
    std::mutex heap_mutex;// 互斥锁，保护堆操作
    
    // 新增成员
    timeout_event_processor* event_processor; // 超时事件处理器
    std::atomic<bool> processor_started; // 处理器是否已启动
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
