#include "http_conn.h"

#include <mysql/mysql.h>
#include <fstream>

//定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

locker m_lock;//保护用户数据的互斥锁
map<string, string> users;//存储用户名和密码的映射，用于用户认证

void http_conn::initmysql_result(connection_pool *connPool)
{
    //先从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    //在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;// 存入全局users映射
    }
}

//对文件描述符设置非阻塞
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
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

//从内核时间表删除描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

//修改 epoll 实例中的文件描述符事件,将事件重置为EPOLLONESHOT
void modfd(int epollfd, int fd, int ev, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;//统计当前用户连接数
int http_conn::m_epollfd = -1;//所有 HTTP 连接共享的 epoll 文件描述符

//关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        printf("close %d\n", m_sockfd);
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

//初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode,
                     int close_log, string user, string passwd, string sqlname)
{
    m_sockfd = sockfd;
    m_address = addr;

    addfd(m_epollfd, sockfd, true, m_TRIGMode);
    m_user_count++;

    //当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
    doc_root = root;
    m_TRIGMode = TRIGMode;
    m_close_log = close_log;

    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    init();
}

//初始化新接受的连接
//check_state默认为分析请求行状态
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    m_state = 0;
    timer_flag = 0;
    improv = 0;

    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

//从状态机，用于分析出一行内容,从缓冲区中解析出一行
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r')
        {
            if ((m_checked_idx + 1) == m_read_idx)
                return LINE_OPEN;
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if (temp == '\n')
        {
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

//循环读取客户数据，直到无数据可读或对方关闭连接
//非阻塞ET工作模式下，需要一次性将数据读完
bool http_conn::read_once()
{
    if (m_read_idx >= READ_BUFFER_SIZE)// 检查缓冲区是否已满
    {
        return false;
    }
    int bytes_read = 0;

    //LT读取数据,读取一次
    if (0 == m_TRIGMode)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read;

        if (bytes_read <= 0)
        {
            return false;
        }

        return true;
    }
    //ET读数据,循环读取直到没有数据
    else
    {
        while (true)
        {
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;// 没有数据了，退出循环
                return false;
            }
            else if (bytes_read == 0)
            {
                return false;// 连接关闭
            }
            m_read_idx += bytes_read;
        }
        return true;
    }
}

//解析http请求行，获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    m_url = strpbrk(text, " \t");
    if (!m_url)
    {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';
    char *method = text;//提取请求方法（GET/POST）
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    else
        return BAD_REQUEST;
    m_url += strspn(m_url, " \t");//提取 URL
    m_version = strpbrk(m_url, " \t");//提取 HTTP 版本
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';//规范化 URL
    m_version += strspn(m_version, " \t");
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }

    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }

    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;
    //当url为/时，显示判断界面
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");
    m_check_state = CHECK_STATE_HEADER;//设置下一状态为解析头部
    return NO_REQUEST;
}

//解析http请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    if (text[0] == '\0')//处理空行（头部结束）
    {
        if (m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    else if (strncasecmp(text, "Connection:", 11) == 0)//提取 Connection 字段（是否保持连接）
    {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;
        }
    }
    else if (strncasecmp(text, "Content-length:", 15) == 0)//提取 Content-length 字段（内容长度）
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    else if (strncasecmp(text, "Host:", 5) == 0)//提取 Host 字段（主机名）
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else
    {
        LOG_INFO("oop!unknow header: %s", text);
    }
    return NO_REQUEST;
}

//判断http请求是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        //POST请求中最后为输入的用户名和密码
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::process_read()//解析 HTTP 请求
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))//使用状态机解析请求的每一行
    {
        text = get_line();
        m_start_line = m_checked_idx;
        LOG_INFO("%s", text);
        switch (m_check_state)//根据当前状态调用相应的解析函数
        {
        case CHECK_STATE_REQUESTLINE:
        {
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            break;
        }
        case CHECK_STATE_HEADER:
        {
            ret = parse_headers(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            else if (ret == GET_REQUEST)
            {
                return do_request();//如果解析到完整请求，调用 do_request 处理请求
            }
            break;
        }
        case CHECK_STATE_CONTENT:
        {
            ret = parse_content(text);
            if (ret == GET_REQUEST)
                return do_request();//如果解析到完整请求，调用 do_request 处理请求
            line_status = LINE_OPEN;
            break;
        }
        default:
            return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::do_request()
{
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    //printf("m_url:%s\n", m_url);
    const char *p = strrchr(m_url, '/');

    //处理CGI（登录和注册）
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {

        //根据标志判断是登录检测还是注册检测
        char flag = m_url[1];

        std::string url_real = "/";
        url_real += (m_url + 2);
        strncpy(m_real_file + len, url_real.c_str(), FILENAME_LEN - len - 1);

        //将用户名和密码提取出来
        //user=123&passwd=123
        char name[100], password[100];
        int i;
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

        // 注册处理
        if (*(p + 1) == '3')
        {
            //如果是注册，先检测数据库中是否有重名的
            //没有重名的，进行增加数据
            if (users.find(name) == users.end())//// 内存中不存在重名用户
            {
                m_lock.lock();
                
                // 使用预处理语句防止SQL注入
                const char *sql_insert = "INSERT INTO user(username, passwd) VALUES(?, ?)";
                MYSQL_STMT *stmt = mysql_stmt_init(mysql);
                
                if (stmt == NULL) {
                    LOG_ERROR("mysql_stmt_init failed: %s", mysql_error(mysql));
                    m_lock.unlock();
                    strcpy(m_url, "/registerError.html");
                } else {
                    // 准备预处理语句
                    if (mysql_stmt_prepare(stmt, sql_insert, strlen(sql_insert)) != 0) {
                        LOG_ERROR("mysql_stmt_prepare failed: %s", mysql_stmt_error(stmt));
                        mysql_stmt_close(stmt);
                        m_lock.unlock();
                        strcpy(m_url, "/registerError.html");
                    } else {
                        // 绑定参数
                        MYSQL_BIND bind[2];
                        memset(bind, 0, sizeof(bind));
                        
                        // 绑定用户名参数
                        bind[0].buffer_type = MYSQL_TYPE_STRING;
                        bind[0].buffer = name;
                        bind[0].buffer_length = strlen(name);
                        
                        // 绑定密码参数
                        bind[1].buffer_type = MYSQL_TYPE_STRING;
                        bind[1].buffer = password;
                        bind[1].buffer_length = strlen(password);
                        
                        if (mysql_stmt_bind_param(stmt, bind) != 0) {
                            LOG_ERROR("mysql_stmt_bind_param failed: %s", mysql_stmt_error(stmt));
                            mysql_stmt_close(stmt);
                            m_lock.unlock();
                            strcpy(m_url, "/registerError.html");
                        } else {
                            // 执行预处理语句
                            if (mysql_stmt_execute(stmt) != 0) {
                                LOG_ERROR("mysql_stmt_execute failed: %s", mysql_stmt_error(stmt));
                                mysql_stmt_close(stmt);
                                m_lock.unlock();
                                strcpy(m_url, "/registerError.html");
                            } else {
                                // 插入成功
                                users.insert(pair<string, string>(name, password));// 更新内存
                                mysql_stmt_close(stmt);
                                m_lock.unlock();
                                strcpy(m_url, "/log.html");
                            }
                        }
                    }
                }
            }
            else// 用户已存在
                strcpy(m_url, "/registerError.html");
        }


        //如果是登录，直接判断
        //若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
        else if (*(p + 1) == '2')
        {
            // 首先从内存中查找（快速验证）
            if (users.find(name) != users.end() && users[name] == password) {
                strcpy(m_url, "/welcome.html");
            } else {
                // 如果内存中没有找到，使用预处理语句查询数据库
                m_lock.lock();
                
                const char *sql_select = "SELECT passwd FROM user WHERE username = ?";
                MYSQL_STMT *stmt = mysql_stmt_init(mysql);
                
                if (stmt == NULL) {
                    LOG_ERROR("mysql_stmt_init failed: %s", mysql_error(mysql));
                    m_lock.unlock();
                    strcpy(m_url, "/logError.html");
                } else {
                    // 准备预处理语句
                    if (mysql_stmt_prepare(stmt, sql_select, strlen(sql_select)) != 0) {
                        LOG_ERROR("mysql_stmt_prepare failed: %s", mysql_stmt_error(stmt));
                        mysql_stmt_close(stmt);
                        m_lock.unlock();
                        strcpy(m_url, "/logError.html");
                    } else {
                        // 绑定参数
                        MYSQL_BIND bind[1];
                        memset(bind, 0, sizeof(bind));
                        
                        // 绑定用户名参数
                        bind[0].buffer_type = MYSQL_TYPE_STRING;
                        bind[0].buffer = name;
                        bind[0].buffer_length = strlen(name);
                        
                        if (mysql_stmt_bind_param(stmt, bind) != 0) {
                            LOG_ERROR("mysql_stmt_bind_param failed: %s", mysql_stmt_error(stmt));
                            mysql_stmt_close(stmt);
                            m_lock.unlock();
                            strcpy(m_url, "/logError.html");
                        } else {
                            // 执行预处理语句
                            if (mysql_stmt_execute(stmt) != 0) {
                                LOG_ERROR("mysql_stmt_execute failed: %s", mysql_stmt_error(stmt));
                                mysql_stmt_close(stmt);
                                m_lock.unlock();
                                strcpy(m_url, "/logError.html");
                            } else {
                                // 绑定结果
                                MYSQL_BIND result_bind;
                                memset(&result_bind, 0, sizeof(result_bind));
                                
                                char db_password[100];
                                unsigned long password_length;
                                
                                result_bind.buffer_type = MYSQL_TYPE_STRING;
                                result_bind.buffer = db_password;
                                result_bind.buffer_length = sizeof(db_password);
                                result_bind.length = &password_length;
                                
                                if (mysql_stmt_bind_result(stmt, &result_bind) != 0) {
                                    LOG_ERROR("mysql_stmt_bind_result failed: %s", mysql_stmt_error(stmt));
                                    mysql_stmt_close(stmt);
                                    m_lock.unlock();
                                    strcpy(m_url, "/logError.html");
                                } else {
                                    // 获取结果
                                    if (mysql_stmt_fetch(stmt) == 0) {
                                        // 找到用户，验证密码
                                        db_password[password_length] = '\0';
                                        if (strcmp(db_password, password) == 0) {
                                            // 登录成功，更新内存缓存
                                            users[name] = password;
                                            mysql_stmt_close(stmt);
                                            m_lock.unlock();
                                            strcpy(m_url, "/welcome.html");
                                        } else {
                                            mysql_stmt_close(stmt);
                                            m_lock.unlock();
                                            strcpy(m_url, "/logError.html");
                                        }
                                    } else {
                                        // 用户不存在
                                        mysql_stmt_close(stmt);
                                        m_lock.unlock();
                                        strcpy(m_url, "/logError.html");
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }


    // 处理静态页面请求
    if (*(p + 1) == '0')
    {
        const char *m_url_real = "/register.html";
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
    }
    else if (*(p + 1) == '1')
    {
        const char *m_url_real = "/log.html";
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
    }
    else if (*(p + 1) == '5')
    {
        const char *m_url_real = "/picture.html";
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
    }
    else if (*(p + 1) == '6')
    {
        const char *m_url_real = "/video.html";
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
    }
    else if (*(p + 1) == '7')
    {
        const char *m_url_real = "/fans.html";
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
    }
    else
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);//复制登录和注册的文件到真实文件路径

    if (stat(m_real_file, &m_file_stat) < 0)// 检查文件是否存在和可访问
        return NO_RESOURCE;

    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;

    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;


    // 将文件映射到内存
    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}


void http_conn::unmap()//取消文件内存映射
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);//释放映射的内存区域
        m_file_address = 0;
    }
}


bool http_conn::write()//向客户端发送 HTTP 响应
{
    int temp = 0;

    if (bytes_to_send == 0)// 如果没有数据要发送，重新注册读事件并重置连接
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        init();
        return true;
    }

    while (1)
    {
        temp = writev(m_sockfd, m_iv, m_iv_count);// 使用writev集中写方式发送数据

        if (temp < 0)
        {
            // 如果发送缓冲区已满，等待下次可写事件
            if (errno == EAGAIN)
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
                return true;
            }
            // 其他错误，取消文件映射并返回失败
            unmap();
            return false;
        }

        // 更新已发送和待发送字节数
        bytes_have_send += temp;
        bytes_to_send -= temp;

        // 调整iovec结构
        if (bytes_have_send >= m_iv[0].iov_len)
        {
            // 第一个缓冲区已发送完，调整第二个缓冲区
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else
        {
            // 第一个缓冲区还未发送完
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        // 检查是否所有数据都已发送完毕
        if (bytes_to_send <= 0)
        {
            unmap();// 取消文件映射
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);// 重新注册读事件

            if (m_linger)// 根据是否保持连接决定是否重置连接状态
            {
                init();
                return true;
            }
            else
            {
                return false;
            }
        }
    }
}
bool http_conn::add_response(const char *format, ...)// 使用可变参数格式化字符串到写缓冲区
{
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;
    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);

    LOG_INFO("request:%s", m_write_buf);

    return true;
}
bool http_conn::add_status_line(int status, const char *title)// 添加状态行
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}
bool http_conn::add_headers(int content_len)// 添加头部字段
{
    return add_content_length(content_len) && add_linger() &&
           add_blank_line();
}
bool http_conn::add_content_length(int content_len)// 添加Content-Length字段
{
    return add_response("Content-Length:%d\r\n", content_len);
}
bool http_conn::add_content_type()// 添加Content-Type字段
{
    return add_response("Content-Type:%s\r\n", "text/html");
}
bool http_conn::add_linger()// 添加Connection字段
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}
bool http_conn::add_blank_line() // 添加空行
{
    return add_response("%s", "\r\n");
}
bool http_conn::add_content(const char *content)// 添加内容
{
    return add_response("%s", content);
}

//根据处理结果生成 HTTP 响应
bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
    case INTERNAL_ERROR:
    {
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        if (!add_content(error_500_form))
            return false;
        break;
    }
    case BAD_REQUEST:
    {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
    }
    case FORBIDDEN_REQUEST:
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
    }
    case FILE_REQUEST:
    {
        add_status_line(200, ok_200_title);
        if (m_file_stat.st_size != 0)
        {
            add_headers(m_file_stat.st_size);
            //设置 iovec 结构用于高效发送
            m_iv[0].iov_base = m_write_buf;// HTTP头部缓冲区
            m_iv[0].iov_len = m_write_idx;// HTTP头部长度
            m_iv[1].iov_base = m_file_address;// 文件内容地址
            m_iv[1].iov_len = m_file_stat.st_size;// 文件内容长度
            m_iv_count = 2;// 两个缓冲区
            bytes_to_send = m_write_idx + m_file_stat.st_size;//计算需要发送的字节数
            return true;
        }
        else
        {
            const char *ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if (!add_content(ok_string))
                return false;
        }
    }
    default:
        return false;
    }
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}
void http_conn::process()//处理 HTTP 请求的入口函数
{
    HTTP_CODE read_ret = process_read();//解析 HTTP 请求
    if (read_ret == NO_REQUEST)//如果请求不完整，重新注册读事件
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        return;
    }
    bool write_ret = process_write(read_ret);//生成 HTTP 响应
    if (!write_ret)//如果写入失败，关闭连接
    {
        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);//注册写事件准备发送响应
}
