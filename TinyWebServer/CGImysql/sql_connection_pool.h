#ifndef _CONNECTION_POOL_
#define _CONNECTION_POOL_

#include <stdio.h>
#include <list>
#include <mysql/mysql.h>
#include <error.h>
#include <string.h>
#include <iostream>
#include <string>
#include "../lock/locker.h"
#include "../log/log.h"

using namespace std;

class connection_pool//数据库连接池类，用于管理多个数据库连接，避免频繁建立和关闭连接的开销。
{
public:
	MYSQL *GetConnection();				 //获取一个数据库连接
	bool ReleaseConnection(MYSQL *conn); //释放连接，将其放回连接池
	int GetFreeConn();					 //获取当前空闲连接数
	void DestroyPool();					 //销毁所有连接

	//单例模式,获取连接池实例
	static connection_pool *GetInstance();

	void init(string url, string User, string PassWord, string DataBaseName, int Port, int MaxConn, int close_log); //初始化连接池

private:
	connection_pool();
	~connection_pool();

	int m_MaxConn;  //最大连接数
	int m_CurConn;  //当前已使用的连接数
	int m_FreeConn; //当前空闲的连接数
	locker lock;//互斥锁，用于线程安全
	list<MYSQL *> connList; //连接池,存储空闲连接的列表（实际是链表）
	sem reserve;//信号量，用于管理连接资源

public:
	string m_url;			 //主机地址
	string m_Port;		 //数据库端口号
	string m_User;		 //登陆数据库用户名
	string m_PassWord;	 //登陆数据库密码
	string m_DatabaseName; //使用数据库名
	int m_close_log;	//日志开关
};


//使用 RAII（Resource Acquisition Is Initialization）模式管理数据库连接
class connectionRAII{//这是一个资源获取即初始化的类，用于自动管理连接。在构造函数中获取连接，在析构函数中释放连接。

public:
	connectionRAII(MYSQL **con, connection_pool *connPool);//从连接池获取一个连接
	~connectionRAII();//将连接释放回连接池
	
private:
	MYSQL *conRAII;//数据库连接
	connection_pool *poolRAII;//连接池
};

#endif
