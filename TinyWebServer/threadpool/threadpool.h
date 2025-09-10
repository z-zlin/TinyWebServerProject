#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <thread>
#include <vector>
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"

template <typename T>
class threadpool
{
public:
    /*thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的、等待处理的请求的数量*/
    threadpool(int actor_model, connection_pool *connPool, int thread_number = 8, int max_request = 10000);
    ~threadpool();
    bool append(T *request, int state);
    bool append_p(T *request);

private:
    /*工作线程运行的函数，它不断从工作队列中取出任务并执行之*/
    void run();

private:
    int m_thread_number;        //线程池中的线程数
    int m_max_requests;         //请求队列中允许的最大请求数
    std::vector<std::thread> m_threads; // 工作线程
    std::list<T *> m_workqueue; //请求队列
    locker m_queuelocker;       //保护请求队列的互斥锁
    sem m_queuestat;            //是否有任务需要处理（信号量）,自定义的信号量包装类，工作线程在队列为空时等待，有任务时被唤醒
    connection_pool *m_connPool;  //数据库连接池，工作线程处理请求时可以从连接池获取数据库连接
    int m_actor_model;          //模型切换，0表示Proactor模式，1表示Reactor模式
};
template <typename T>
threadpool<T>::threadpool( int actor_model, connection_pool *connPool, int thread_number, int max_requests) : m_actor_model(actor_model),m_thread_number(thread_number), m_max_requests(max_requests), m_connPool(connPool)
{
    if (thread_number <= 0 || max_requests <= 0)
        throw std::exception();
    
    // 创建线程
    m_threads.reserve(m_thread_number);
    for (int i = 0; i < thread_number; ++i)
    {
        m_threads.emplace_back([this](){ this->run(); });
    }
}
template <typename T>
threadpool<T>::~threadpool()//清理线程池资源
{
    for (auto &t : m_threads)
    {
        if (t.joinable()) t.detach();
    }
}


template <typename T>
bool threadpool<T>::append(T *request, int state)//向请求队列中添加任务（Reactor模式）
{
    m_queuelocker.lock();//加锁保护队列
    if (m_workqueue.size() >= m_max_requests)//检查队列是否已满
    {
        m_queuelocker.unlock();
        return false;
    }
    request->m_state = state;//设置请求状态(读/写)
    m_workqueue.push_back(request);//将请求加入队列
    m_queuelocker.unlock();//解锁
    m_queuestat.post();//通知工作线程
    return true;
}


template <typename T>
bool threadpool<T>::append_p(T *request)//向请求队列中添加任务（Proactor模式）,与append类似，但不设置请求状态
{
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}

template <typename T>
void threadpool<T>::run()
{
    while (true)
    {
        m_queuestat.wait();// 等待任务
        m_queuelocker.lock();// 加锁
        if (m_workqueue.empty())// 检查队列是否为空
        {
            m_queuelocker.unlock();
            continue;
        }
        T *request = m_workqueue.front();// 获取任务
        m_workqueue.pop_front();// 移除任务
        m_queuelocker.unlock();// 解锁
        if (!request)
            continue;
        if (1 == m_actor_model)// Reactor模式
        {
            if (0 == request->m_state)// 读事件
            {
                if (request->read_once())// 读取数据
                {
                    request->improv = 1;
                    connectionRAII mysqlcon(&request->mysql, m_connPool);
                    request->process();// 处理请求
                }
                else
                {
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }
            else// 写事件
            {
                if (request->write())// 写入数据
                {
                    request->improv = 1;
                }
                else
                {
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }
        }
        else// Proactor模式
        {
            connectionRAII mysqlcon(&request->mysql, m_connPool);
            request->process();// 处理请求
        }
    }
}
#endif
