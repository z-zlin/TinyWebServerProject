#include "lst_timer.h"
#include "../http/http_conn.h"

sort_timer_lst::sort_timer_lst()//初始化链表，将头尾指针设为 NULL
{
    head = NULL;
    tail = NULL;
}
sort_timer_lst::~sort_timer_lst()//遍历整个链表，删除所有定时器节点，释放内存
{
    util_timer *tmp = head;
    while (tmp)
    {
        head = tmp->next;
        delete tmp;
        tmp = head;
    }
}

void sort_timer_lst::add_timer(util_timer *timer)//向链表中添加定时器，保持链表按超时时间升序排列。
{
    if (!timer)//检查定时器是否有效
    {
        return;
    }
    if (!head)//如果链表为空，直接设置为头尾节点
    {
        head = tail = timer;
        return;
    }
    if (timer->expire < head->expire)//如果新定时器超时时间小于头节点，插入到链表头部
    {
        timer->next = head;
        head->prev = timer;
        head = timer;
        return;
    }
    add_timer(timer, head);//调用私有 add_timer 方法在合适位置插入
}
void sort_timer_lst::adjust_timer(util_timer *timer)//调整定时器位置
{
    if (!timer)//检查定时器是否有效
    {
        return;
    }
    util_timer *tmp = timer->next;
    if (!tmp || (timer->expire < tmp->expire))//如果定时器位置正确（超时时间小于下一个节点），直接返回
    {
        return;
    }
    if (timer == head)//如果是头节点，先移除再重新插入
    {
        head = head->next;
        head->prev = NULL;
        timer->next = NULL;
        add_timer(timer, head);
    }
    else//如果是中间节点，先移除再从下一个节点开始重新插入
    {
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        add_timer(timer, timer->next);
    }
}
void sort_timer_lst::del_timer(util_timer *timer)//从链表中删除定时器
{
    if (!timer)//检查定时器是否有效
    {
        return;
    }
    if ((timer == head) && (timer == tail))//处理特殊情况：链表只有一个节点、删除头节点、删除尾节点
    {
        delete timer;
        head = NULL;
        tail = NULL;
        return;
    }
    if (timer == head)//处理特殊情况：删除头节点
    {
        head = head->next;
        head->prev = NULL;
        delete timer;
        return;
    }
    if (timer == tail)//处理特殊情况：删除尾节点
    {
        tail = tail->prev;
        tail->next = NULL;
        delete timer;
        return;
    }
    timer->prev->next = timer->next;//处理一般情况：删除中间节点
    timer->next->prev = timer->prev;
    delete timer;
}
void sort_timer_lst::tick()//处理超时定时器
{
    if (!head)//检查链表是否为空
    {
        return;
    }
    
    time_t cur = time(NULL);//获取当前时间
    util_timer *tmp = head;
    while (tmp)//遍历链表，执行所有已超时定时器的回调函数
    {
        if (cur < tmp->expire)
        {
            break;
        }
        tmp->cb_func(tmp->user_data);
        head = tmp->next;
        if (head)
        {
            head->prev = NULL;
        }
        delete tmp;//删除已处理的定时器节点
        tmp = head;
    }
}

void sort_timer_lst::add_timer(util_timer *timer, util_timer *lst_head)//内部使用的添加函数，从指定节点开始查找合适位置插入定时器。
{
    util_timer *prev = lst_head;
    util_timer *tmp = prev->next;
    while (tmp)//从指定节点开始遍历链表
    {
        if (timer->expire < tmp->expire)//找到第一个超时时间大于新定时器的节点，插入到其前面
        {
            prev->next = timer;
            timer->next = tmp;
            tmp->prev = timer;
            timer->prev = prev;
            break;
        }
        prev = tmp;
        tmp = tmp->next;
    }
    if (!tmp)//如果遍历到链表尾部仍未找到，则将新定时器插入到尾部
    {
        prev->next = timer;
        timer->prev = prev;
        timer->next = NULL;
        tail = timer;
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

int *Utils::u_pipefd = 0;
int Utils::u_epollfd = 0;

class Utils;
void cb_func(client_data *user_data)//定时器超时时的回调函数，关闭客户端连接并从epoll中移除。
{
    epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);//从 epoll 实例中移除文件描述符
    assert(user_data);
    close(user_data->sockfd);//关闭 socket 连接
    http_conn::m_user_count--;//减少用户计数
}
