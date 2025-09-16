#include "lst_timer.h"
#include "../http/http_conn.h"
#include <iostream>
#include <thread>

// 时间缓存管理器实现
time_cache_manager& time_cache_manager::get_instance() {
    static time_cache_manager instance;//单例模式,返回时间缓存管理器实例
    return instance;
}

time_cache_manager::time_cache_manager() 
    : running(false), update_interval(milliseconds(100)) {
    // 记录系统启动时间
    system_start_time = std::chrono::steady_clock::now();
    system_start_time_t = time(nullptr);
    cached_time = system_start_time;
}

time_cache_manager::~time_cache_manager() {
    stop();
}

void time_cache_manager::start() {
    if (running.load()) {
        return;
    }
    
    running.store(true);
    update_thread = std::thread(&time_cache_manager::update_loop, this);//启动时间更新线程,更新时间缓存.此线程会周期性更新时间缓存,并通知其他线程更新时间缓存.
}

void time_cache_manager::stop() {
    if (!running.load()) {
        return;
    }
    
    running.store(false);
    update_cv.notify_all();
    
    if (update_thread.joinable()) {
        update_thread.join();
    }
}


void time_cache_manager::update_loop() {
    while (running.load()) {
        // 更新缓存时间
        {
            std::lock_guard<std::mutex> lock(cached_time_mutex);
            cached_time = std::chrono::steady_clock::now();
        }
        
        // 等待指定间隔
        std::unique_lock<std::mutex> lock(update_mutex);
        update_cv.wait_for(lock, update_interval, [this] {
            return !running.load();
        });
    }
}

steady_time_point time_cache_manager::get_current_time() {
    std::lock_guard<std::mutex> lock(cached_time_mutex);
    return cached_time;
}

steady_time_point time_cache_manager::time_t_to_steady(time_t t) {
    // 计算时间差并转换为steady_time_point
    int64_t diff_seconds = t - system_start_time_t;
    auto diff_duration = std::chrono::seconds(diff_seconds);
    return system_start_time + diff_duration;
}

time_t time_cache_manager::steady_to_time_t(steady_time_point tp) {
    // 计算时间差并转换为time_t
    auto diff_duration = tp - system_start_time;
    auto diff_seconds = std::chrono::duration_cast<std::chrono::seconds>(diff_duration).count();
    return system_start_time_t + diff_seconds;
}

steady_time_point time_cache_manager::add_milliseconds(steady_time_point base, int64_t ms) {
    return base + milliseconds(ms);
}

int64_t time_cache_manager::duration_to_milliseconds(steady_duration d) {//
    return std::chrono::duration_cast<milliseconds>(d).count();
}

time_t time_cache_manager::get_current_time_t() {
    steady_time_point now = get_current_time();
    return steady_to_time_t(now);
}

time_t time_cache_manager::get_current_time_t_plus_seconds(int seconds) {
    steady_time_point now = get_current_time();
    steady_time_point future = add_milliseconds(now, seconds * 1000);
    return steady_to_time_t(future);
}

// util_timer接口实现
void util_timer::set_expire_milliseconds(int64_t ms) {//设置定时器超时时间,将时间转换为高精度时间,并添加到时间缓存中.
    auto& time_cache = time_cache_manager::get_instance();
    steady_time_point now = time_cache.get_current_time();
    expire = time_cache.add_milliseconds(now, ms);
}

// 超时事件处理器实现
timeout_event_processor::timeout_event_processor() 
    : running(false), max_batch_size(50), max_queue_size(10000), current_queue_size(0) {
}

timeout_event_processor::~timeout_event_processor() {
    stop();
}

void timeout_event_processor::start() {//启动超时事件处理器
    if (running.load()) {
        return;
    }
    
    running.store(true);
    processor_thread = std::thread(&timeout_event_processor::process_loop, this);
}

void timeout_event_processor::stop() {
    if (!running.load()) {
        return;
    }
    
    running.store(false);
    queue_cv.notify_all();
    
    if (processor_thread.joinable()) {
        processor_thread.join();
    }
}

void timeout_event_processor::add_timeout_event(util_timer* timer, steady_time_point expire_time) {
    if (!running.load()) {
        // 处理器未运行，直接释放定时器内存
        if (timer) {
            delete timer;
        }
        return;
    }
    
    // 流控检查 - 当队列接近满时，尝试强制处理一些事件
    if (current_queue_size.load() >= max_queue_size.load() * 0.9) {
        std::cerr << "Warning: Timeout event queue is nearly full, forcing batch processing" << std::endl;
        // 强制唤醒处理线程
        queue_cv.notify_one();
        
        // 等待一小段时间让处理线程处理一些事件
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    // 如果队列仍然满，则直接处理当前事件，避免内存泄露
    if (current_queue_size.load() >= max_queue_size.load()) {
        std::cerr << "Warning: Timeout event queue is full, processing event immediately to avoid memory leak" << std::endl;
        if (timer && !timer->deleted) {
            try {
                timer->cb_func(timer->user_data);
            } catch (const std::exception& e) {
                std::cerr << "Error in timeout callback (queue full): " << e.what() << std::endl;
            }
            delete timer;
        }
        return;
    }
    
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        event_queue.emplace(timer, expire_time);//添加事件到队列
        current_queue_size.fetch_add(1);
    }
    
    // 只有在队列达到批处理大小的一半时才唤醒线程，实现真正的批量处理
    if (current_queue_size.load() >= max_batch_size.load() / 2) {
        queue_cv.notify_one();
    }
}

void timeout_event_processor::process_loop() {
    while (running.load()) {
        std::unique_lock<std::mutex> lock(queue_mutex);
        
        // 等待事件或超时，增加等待时间以收集更多事件进行批量处理
        queue_cv.wait_for(lock, std::chrono::milliseconds(200), [this] {
            return !event_queue.empty() || !running.load();
        });//等待事件或超时，如果事件队列为空或处理器停止，则退出循环
        
        if (!running.load()) {
            break;
        }
        
        if (!event_queue.empty()) {
            lock.unlock();
            process_batch();//批量处理事件
            
            // 如果队列中还有事件，继续处理（避免积压）
            if (current_queue_size.load() > 0) {
                continue;
            }
        }
    }
}

void timeout_event_processor::process_batch() {
    std::vector<timeout_event> batch;
    batch.reserve(max_batch_size.load());//预留空间
    
    // 批量取出事件
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        size_t batch_size = std::min(event_queue.size(), max_batch_size.load());
        
        for (size_t i = 0; i < batch_size && !event_queue.empty(); ++i) {
            batch.push_back(event_queue.front());
            event_queue.pop();
            current_queue_size.fetch_sub(1);//减少队列大小
        }
    }
    
    // 批量处理事件
    for (auto& event : batch) {
        if (event.timer && !event.timer->deleted) {
            try {
                event.timer->cb_func(event.timer->user_data);
            } catch (const std::exception& e) {//捕获异常,打印错误信息,删除定时器.
                std::cerr << "Error in timeout callback: " << e.what() << std::endl;
            }
            delete event.timer;
        }
    }
}

sort_timer_lst::sort_timer_lst()//初始化堆
{
    // vector和unordered_map会自动初始化为空
    event_processor = new timeout_event_processor();
    processor_started.store(false);
}

sort_timer_lst::~sort_timer_lst()//清空堆并删除所有定时器节点，释放内存
{
    // 停止超时事件处理器
    if (event_processor) {
        event_processor->stop();
        delete event_processor;
        event_processor = nullptr;
    }
    
    std::lock_guard<std::mutex> lock(heap_mutex);
    for (auto timer : timer_heap)
    {
        delete timer;
    }
    timer_heap.clear();
    timer_index_map.clear();
}

void sort_timer_lst::swap_nodes(int i, int j)//交换两个节点并更新索引映射
{
    if (i == j) return;
    
    // 交换vector中的节点
    std::swap(timer_heap[i], timer_heap[j]);
    
    // 更新索引映射
    timer_index_map[timer_heap[i]] = i;
    timer_index_map[timer_heap[j]] = j;
}

void sort_timer_lst::heapify_up(int index)//向上调整堆
{
    while (index > 0)
    {
        int parent = get_parent(index);
        if (timer_heap[index]->expire >= timer_heap[parent]->expire)
        {
            break; // 堆性质已满足
        }
        swap_nodes(index, parent);
        index = parent;
    }
}

void sort_timer_lst::heapify_down(int index)//向下调整堆
{
    int size = timer_heap.size();
    while (index < size)
    {
        int left_child = get_left_child(index);
        int right_child = get_right_child(index);
        int smallest = index;
        
        // 找到最小的子节点
        if (left_child < size && timer_heap[left_child]->expire < timer_heap[smallest]->expire)
        {
            smallest = left_child;
        }
        if (right_child < size && timer_heap[right_child]->expire < timer_heap[smallest]->expire)
        {
            smallest = right_child;
        }
        
        if (smallest == index)
        {
            break; // 堆性质已满足
        }
        
        swap_nodes(index, smallest);
        index = smallest;
    }
}

void sort_timer_lst::add_timer(util_timer *timer)//向堆中添加定时器，保持按超时时间升序排列。
{
    if (!timer)//检查定时器是否有效
    {
        return;
    }
    
    std::lock_guard<std::mutex> lock(heap_mutex);
    
    // 重置删除标记
    timer->deleted = false;
    
    // 将定时器添加到vector末尾
    int index = timer_heap.size();
    timer_heap.push_back(timer);
    timer_index_map[timer] = index;
    
    // 向上调整堆
    heapify_up(index);
}
void sort_timer_lst::adjust_timer(util_timer *timer)//调整定时器位置
{
    if (!timer)//检查定时器是否有效
    {
        return;
    }
    
    std::lock_guard<std::mutex> lock(heap_mutex);
    
    // 查找定时器在堆中的位置
    auto it = timer_index_map.find(timer);
    if (it == timer_index_map.end())
    {
        return; // 定时器不在堆中
    }
    
    int index = it->second;
    
    // 向上和向下调整堆，恢复堆性质
    heapify_up(index);
    heapify_down(index);
}
void sort_timer_lst::del_timer(util_timer *timer)//从堆中删除定时器
{
    if (!timer)//检查定时器是否有效
    {
        return;
    }
    
    std::lock_guard<std::mutex> lock(heap_mutex);
    
    // 查找定时器在堆中的位置
    auto it = timer_index_map.find(timer);
    if (it == timer_index_map.end())
    {
        return; // 定时器不在堆中
    }
    
    int index = it->second;
    int last_index = timer_heap.size() - 1;
    
    // 将待删除的节点与最后一个节点交换
    swap_nodes(index, last_index);
    
    // 删除最后一个节点（即原来的待删除节点）
    timer_heap.pop_back();
    timer_index_map.erase(timer);
    
    // 如果删除的不是最后一个节点，需要调整堆
    if (index < last_index)
    {
        // 从删除位置向上和向下调整堆
        heapify_up(index);
        heapify_down(index);
    }
    
    // 删除定时器对象
    delete timer;
}
void sort_timer_lst::tick()//处理超时定时器（惰性删除模式）
{
    std::lock_guard<std::mutex> lock(heap_mutex);
    
    if (timer_heap.empty())//检查堆是否为空
    {
        return;
    }
    
    // 使用高精度时间缓存，减少系统调用
    auto& time_cache = time_cache_manager::get_instance();//获取时间缓存管理器实例
    steady_time_point cur = time_cache.get_current_time();//获取当前时间
    
    // 处理所有超时的定时器，但不立即执行回调
    while (!timer_heap.empty())
    {
        util_timer *timer = timer_heap[0]; // 堆顶元素
        
        // 如果堆顶定时器未超时，说明后面的定时器也都未超时
        if (cur < timer->expire)
        {
            break;
        }
        
        // 删除堆顶定时器（与最后一个节点交换后删除）
        int last_index = timer_heap.size() - 1;
        swap_nodes(0, last_index);
        
        util_timer *expired_timer = timer_heap[last_index];
        timer_heap.pop_back();
        timer_index_map.erase(expired_timer);
        
        // 如果堆不为空，调整堆顶
        if (!timer_heap.empty())
        {
            heapify_down(0);
        }
        
        // 将超时事件添加到处理队列，而不是立即执行回调
        if (event_processor && processor_started.load()) {
            event_processor->add_timeout_event(expired_timer, expired_timer->expire);
        } else {
            // 如果处理器未启动，回退到同步处理
            expired_timer->cb_func(expired_timer->user_data);
            delete expired_timer;
        }
    }
}

// 新增方法实现
void sort_timer_lst::start_timeout_processor() {
    if (event_processor && !processor_started.load()) {
        event_processor->start();
        processor_started.store(true);
    }
}

void sort_timer_lst::stop_timeout_processor() {
    if (event_processor && processor_started.load()) {
        event_processor->stop();
        processor_started.store(false);
    }
}

void sort_timer_lst::set_batch_size(size_t size) {
    if (event_processor) {
        event_processor->set_batch_size(size);
    }
}

void sort_timer_lst::set_max_queue_size(size_t size) {
    if (event_processor) {
        event_processor->set_max_queue_size(size);
    }
}


void Utils::init(int timeslot)//初始化时间槽，设置定时器超时单位，启动超时事件处理器，配置批处理参数
{
    m_TIMESLOT = timeslot;
    
    // 启动时间缓存管理器
    time_cache_manager::get_instance().start();
    
    // 启动超时事件处理器
    m_timer_lst.start_timeout_processor();
    
    // 配置批处理参数
    m_timer_lst.set_batch_size(50);        // 每批处理50个超时事件
    m_timer_lst.set_max_queue_size(10000); // 最大队列10000个事件
}

//对文件描述符设置非阻塞
int Utils::setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void Utils::addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//信号处理函数
void Utils::sig_handler(int sig)
{
    //为保证函数的可重入性，保留原来的errno
    int save_errno = errno;
    int msg = sig;
    send(u_pipefd[1], (char *)&msg, 1, 0);
    errno = save_errno;
}

//设置信号函数
void Utils::addsig(int sig, void(handler)(int), bool restart)
{
    struct sigaction sa;//初始化 sigaction 结构体
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;//设置信号处理函数
    if (restart)//如果 restart 为 true，设置 SA_RESTART 标志
        sa.sa_flags |= SA_RESTART;
    sigfillset(&sa.sa_mask);//填充信号掩码，阻塞所有其他信号
    assert(sigaction(sig, &sa, NULL) != -1);//注册信号处理函数
}

//定时处理任务，重新定时以不断触发SIGALRM信号
void Utils::timer_handler()
{
    m_timer_lst.tick();//调用定时器链表的 tick 方法处理超时定时器
    alarm(m_TIMESLOT);//重新设置定时器，持续触发 SIGALRM 信号
}

void Utils::show_error(int connfd, const char *info)
{
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int *Utils::u_pipefd = nullptr;
int Utils::u_epollfd = 0;

class Utils;
void cb_func(client_data *user_data)//定时器超时时的回调函数，关闭客户端连接并从epoll中移除。
{
    epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);//从 epoll 实例中移除文件描述符
    assert(user_data);
    close(user_data->sockfd);//关闭 socket 连接
    http_conn::m_user_count--;//减少用户计数
}

