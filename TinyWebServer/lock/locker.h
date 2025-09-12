#ifndef LOCKER_H
#define LOCKER_H

#include <exception>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>

// 使用条件变量实现的计数信号量，避免依赖POSIX信号量
class sem
{
public:
    sem() : m_count(0) {}
    explicit sem(int num) : m_count(num) {}//禁止 int 到 sem 的隐式类型转换,防止意外构造
    ~sem() {}
    bool wait()
    {
        std::unique_lock<std::mutex> lock(m_mutex);//获取互斥锁
        m_cv.wait(lock, [this]() { return m_count > 0; });//等待信号量计数大于0,使用lambda表达式判断信号量计数是否大于0
        --m_count;
        return true;
    }
    bool post()//释放信号量
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);//获取互斥锁,lock_guard 是 std::mutex 的 RAII 包装器，会在作用域结束时自动释放锁
            ++m_count; //增加信号量计数
        }
        m_cv.notify_one();//通知一个等待线程
        return true;
    }
    // 允许在构造后按需设置计数，用于保持旧代码赋值语义
    void set(int count)//设置信号量计数
    {
        std::lock_guard<std::mutex> lock(m_mutex);//获取互斥锁,lock_guard 是 std::mutex 的 RAII 包装器，会在作用域结束时自动释放锁
        m_count = count; //设置信号量计数
        if (m_count > 0)
        {
            m_cv.notify_all();//通知所有等待线程,使用notify_all通知所有等待线程 
        }
    }

private:
    std::mutex m_mutex;//保护信号量计数的互斥锁
    std::condition_variable m_cv;//条件变量，用于等待和通知
    int m_count;//信号量的计数值
};
class locker
{
public:
    locker() {}
    ~locker() {}
    bool lock()
    {
        m_mutex.lock();//获取互斥锁
        return true;
    }
    bool unlock()
    {
        m_mutex.unlock();//释放互斥锁
        return true;
    }
    // 为了保持现有接口兼容，提供一个空指针，调用方不会解引用它
    // 现有的cond::wait会忽略该参数，改用内部的std::condition_variable
    void *get()
    {
        return nullptr;
    }
    std::mutex &native() { return m_mutex; }//返回互斥锁，native是本地意思，表示本地互斥锁,表示返回互斥锁的引用

private:
    std::mutex m_mutex;//标准库中的互斥锁
};
class cond
{
public:
    cond() {}
    ~cond() {}
    // 兼容旧接口，忽略传入参数，使用内部mutex与cv
    bool wait(void *)
    {
        std::unique_lock<std::mutex> lock(m_mutex);//获取互斥锁
        m_cv.wait(lock, [this]() { return m_notified; });//等待被通知，使用谓词条件等待，避免虚假唤醒问题
        m_notified = false; //设置为false，表示是虚假唤醒
        return true;
    }
    // 兼容旧接口的超时等待
    bool timewait(void *, struct timespec t)
    {
        auto ns = std::chrono::seconds(t.tv_sec) + std::chrono::nanoseconds(t.tv_nsec);//计算时间间隔
        auto deadline = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(ns);//计算截止时间
        std::unique_lock<std::mutex> lock(m_mutex);
        bool ok = m_cv.wait_until(lock, deadline, [this]() { return m_notified; });//等待被通知，使用谓词条件等待，避免虚假唤醒问题
        if (ok)
        {
            m_notified = false;//重置通知状态
        }
        return ok;
    }
    bool signal()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);//获取互斥锁,lock_guard 是 std::mutex 的 RAII 包装器，会在作用域结束时自动释放锁
            m_notified = true;//设置为true，表示真被通知
        }
        m_cv.notify_one();//通知一个等待线程
        return true;
    }
    bool broadcast()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_notified = true;//设置为true，表示真被通知
        }
        m_cv.notify_all();//通知所有等待线程
        return true;
    }

private:
    std::mutex m_mutex;//标准库中的互斥锁
    std::condition_variable m_cv;//条件变量，用于等待和通知
    bool m_notified{false};//是否通知，防止虚假唤醒
};
#endif
