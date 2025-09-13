#include "lst_timer.h"
#include "../http/http_conn.h"

sort_timer_lst::sort_timer_lst()//初始化堆
{
    // vector和unordered_map会自动初始化为空
}

sort_timer_lst::~sort_timer_lst()//清空堆并删除所有定时器节点，释放内存
{
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
void sort_timer_lst::tick()//处理超时定时器
{
    std::lock_guard<std::mutex> lock(heap_mutex);
    
    if (timer_heap.empty())//检查堆是否为空
    {
        return;
    }
    
    time_t cur = time(nullptr);//获取当前时间
    
    // 处理所有超时的定时器
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
        
        // 执行超时定时器的回调函数并删除
        expired_timer->cb_func(expired_timer->user_data);
        delete expired_timer;
    }
}


void Utils::init(int timeslot)//初始化时间槽，设置定时器超时单位
{
    m_TIMESLOT = timeslot;
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

