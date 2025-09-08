#include <mysql/mysql.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <stdlib.h>
#include <list>
#include <pthread.h>
#include <iostream>
#include "sql_connection_pool.h"

using namespace std;

connection_pool::connection_pool()//构造函数，初始化连接数为0
{
	m_CurConn = 0;
	m_FreeConn = 0;
}

connection_pool *connection_pool::GetInstance()//使用局部静态变量实现单例模式，返回连接池的唯一实例。
{
	static connection_pool connPool;
	return &connPool;
}

//初始化连接池，根据最大连接数创建连接，并加入到连接池列表中。
void connection_pool::init(string url, string User, string PassWord, string DBName, int Port, int MaxConn, int close_log)
{
	//保存连接参数
	m_url = url;
	m_Port = Port;
	m_User = User;
	m_PassWord = PassWord;
	m_DatabaseName = DBName;
	m_close_log = close_log;

	//创建指定数量的数据库连接
	for (int i = 0; i < MaxConn; i++)
	{
		MYSQL *con = NULL;
		con = mysql_init(con);

		if (con == NULL)
		{
			LOG_ERROR("MySQL Error");
			exit(1);
		}
		con = mysql_real_connect(con, url.c_str(), User.c_str(), PassWord.c_str(), DBName.c_str(), Port, NULL, 0);

		if (con == NULL)
		{
			LOG_ERROR("MySQL Error");
			exit(1);
		}
		connList.push_back(con);
		++m_FreeConn;
	}

	reserve = sem(m_FreeConn);//初始化信号量，初始值为空闲连接数

	m_MaxConn = m_FreeConn;//设置最大连接数
}


//当有请求时，从数据库连接池中返回一个可用连接，更新使用和空闲连接数
MYSQL *connection_pool::GetConnection()
{
	MYSQL *con = NULL;

	if (0 == connList.size())
		return NULL;
	
    //使用信号量等待可用连接，然后加锁从连接池列表头部取出一个连接，更新计数。
	reserve.wait();//等待信号量（等待可用连接）
	
	lock.lock();// 加锁保护共享资源

	con = connList.front();// 从连接池头部获取连接
	connList.pop_front();// 移除该连接

	--m_FreeConn;// 空闲连接数减1
	++m_CurConn;// 当前使用连接数加1

	lock.unlock();// 解锁
	return con;
}

//释放当前使用的连接
bool connection_pool::ReleaseConnection(MYSQL *con)//释放连接，将连接放回连接池尾部，并更新计数，同时通过信号量通知可用连接
{
	if (NULL == con)
		return false;

	lock.lock();// 加锁保护共享资源

	connList.push_back(con);// 将连接放回连接池尾部
	++m_FreeConn;// 空闲连接数加1
	--m_CurConn;// 当前使用连接数减1

	lock.unlock();// 解锁

	reserve.post();// 释放信号量（通知有可用连接）
	return true;
}

//销毁数据库连接池
void connection_pool::DestroyPool()//销毁连接池，关闭所有连接并清空列表。
{

	lock.lock();// 加锁保护共享资源
	if (connList.size() > 0)
	{
		list<MYSQL *>::iterator it;
		for (it = connList.begin(); it != connList.end(); ++it)
		{
			MYSQL *con = *it;
			mysql_close(con);// 关闭数据库连接
		}
		m_CurConn = 0;
		m_FreeConn = 0;
		connList.clear();// 清空连接列表
	}

	lock.unlock(); // 解锁
}

//返回当前空闲的连接数
int connection_pool::GetFreeConn()
{
	return this->m_FreeConn;
}

connection_pool::~connection_pool()
{
	DestroyPool();
}

connectionRAII::connectionRAII(MYSQL **SQL, connection_pool *connPool)//构造函数，从连接池获取一个连接，并将获取的连接赋值给SQL。
{

	*SQL = connPool->GetConnection();// 从连接池获取连接
	
	conRAII = *SQL;
	poolRAII = connPool;
}

connectionRAII::~connectionRAII(){//析构函数，将连接释放回连接池。
	poolRAII->ReleaseConnection(conRAII);// 将连接释放回连接池
}