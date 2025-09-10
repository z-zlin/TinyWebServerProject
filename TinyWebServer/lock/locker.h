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
    explicit sem(int num) : m_count(num) {}
    ~sem() {}
    bool wait()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this]() { return m_count > 0; });
        --m_count;
        return true;
    }
    bool post()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_count;
        }
        m_cv.notify_one();
        return true;
    }
    // 允许在构造后按需设置计数，用于保持旧代码赋值语义
    void set(int count)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_count = count;
        if (m_count > 0)
        {
            m_cv.notify_all();
        }
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    int m_count;
};
class locker
{
public:
    locker() {}
    ~locker() {}
    bool lock()
    {
        m_mutex.lock();
        return true;
    }
    bool unlock()
    {
        m_mutex.unlock();
        return true;
    }
    // 为了保持现有接口兼容，提供一个空指针，调用方不会解引用它
    // 现有的cond::wait会忽略该参数，改用内部的std::condition_variable
    void *get()
    {
        return nullptr;
    }
    std::mutex &native() { return m_mutex; }

private:
    std::mutex m_mutex;
};
class cond
{
public:
    cond() {}
    ~cond() {}
    // 兼容旧接口，忽略传入参数，使用内部mutex与cv
    bool wait(void *)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this]() { return m_notified; });
        m_notified = false;
        return true;
    }
    // 兼容旧接口的超时等待
    bool timewait(void *, struct timespec t)
    {
        auto ns = std::chrono::seconds(t.tv_sec) + std::chrono::nanoseconds(t.tv_nsec);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(ns);
        std::unique_lock<std::mutex> lock(m_mutex);
        bool ok = m_cv.wait_until(lock, deadline, [this]() { return m_notified; });
        if (ok)
        {
            m_notified = false;
        }
        return ok;
    }
    bool signal()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_notified = true;
        }
        m_cv.notify_one();
        return true;
    }
    bool broadcast()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_notified = true;
        }
        m_cv.notify_all();
        return true;
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_notified{false};
};
#endif
