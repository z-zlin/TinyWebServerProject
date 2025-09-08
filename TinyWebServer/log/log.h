#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <iostream>
#include <string>
#include <stdarg.h>
#include <pthread.h>
#include "block_queue.h"

using namespace std;

class Log
{
public:
    //C++11以后,使用局部变量懒汉不用加锁
    static Log *get_instance()//获取日志类的单例实例
    {
        static Log instance;
        return &instance;
    }

    static void *flush_log_thread(void *args)//异步写日志的线程入口函数
    {
        Log::get_instance()->async_write_log();
    }


    //可选择的参数有日志文件、日志缓冲区大小、最大行数以及最长日志条队列,根据 max_queue_size 决定使用同步还是异步模式
    bool init(const char *file_name, int close_log, int log_buf_size = 8192, int split_lines = 5000000, int max_queue_size = 0);

    void write_log(int level, const char *format, ...);//写入日志

    void flush(void);//强制刷新日志缓冲区

private:
    Log();
    virtual ~Log();


    void *async_write_log()//异步写入日志的工作函数
    {
        string single_log;
        //从阻塞队列中取出一个日志string，写入文件
        while (m_log_queue->pop(single_log))
        {
            m_mutex.lock();
            fputs(single_log.c_str(), m_fp);
            m_mutex.unlock();
        }
    }

private:
    char dir_name[128]; //路径名
    char log_name[128]; //log文件名


    int m_split_lines;  //日志最大行数,单个日志文件的最大行数，超过则创建新文件
    int m_log_buf_size; //日志缓冲区大小
    long long m_count;  //日志行数记录,当前日志文件已写入的行数
    int m_today;        //因为按天分类,记录当前时间是那一天,当前日志文件的日期（用于按天分割）


    FILE *m_fp;         //打开log的文件指针,指向当前打开的日志文件
    char *m_buf;        ////日志缓冲区,用于格式化日志内容的缓冲区


    block_queue<string> *m_log_queue; //阻塞队列,用于在异步模式下存储待写入的日志
    bool m_is_async;                  //是否同步标志位,true表示异步模式，false表示同步模式


    locker m_mutex;//保护文件写入操作的互斥锁
    int m_close_log; //关闭日志
};

//只有在 m_close_log 为 0（不关闭日志）时才记录日志,自动获取日志单例实例,写入日志后自动调用 flush 确保及时写入,使用 ##__VA_ARGS__ 支持可变参数
#define LOG_DEBUG(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(0, format, ##__VA_ARGS__); Log::get_instance()->flush();}//调试级别日志,详细的调试信息
#define LOG_INFO(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(1, format, ##__VA_ARGS__); Log::get_instance()->flush();}//信息级别日志,一般的程序运行信息
#define LOG_WARN(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(2, format, ##__VA_ARGS__); Log::get_instance()->flush();}//警告级别日志,可能有问题但不影响程序运行的情况
#define LOG_ERROR(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(3, format, ##__VA_ARGS__); Log::get_instance()->flush();}//错误级别日志,错误情况，可能影响程序运行

#endif
