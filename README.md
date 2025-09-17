

TinyWebServerPro
=====================
Linux 下高性能 C++ 轻量级 Web 服务器，基于 [qinguoyi/TinyWebServer](https://github.com/qinguoyi/TinyWebServer) 二次开发，聚焦**安全性加固、性能提升、代码现代化**三大核心目标，支持高并发连接、HTTP 完整解析、用户认证及静态 / 动态资源访问。

## 项目简介
* 核心架构:**线程池 + 非阻塞 Socket + epoll（LT/ET 双模式） + Reactor/Proactor** 事件模型。
* 基础能力：HTTP 1.1 协议解析（GET/POST）、用户注册登录（MySQL 支持）、静态资源（图片 / 视频）传输、同步 / 异步日志、压力测试。
* 优化亮点：**解决原项目 SQL 注入风险、升级 C++11 线程模型、优化定时器效率**、消除内存安全问题,美化前端界面。

## 核心优化亮点
### 1. SQL注入防护（安全加固）
#### 技术方案
* 使用MySQL预处理语句和参数绑定，彻底杜绝SQL注入漏洞。

	* 注册功能：使用MySQL预处理语句 `INSERT INTO user(username, passwd) VALUES(?, ?)`,用户输入被正确转义，无法执行恶意SQL代码。
	* 登录功能：结合内存查找和数据库预处理语句查询。使用 `SELECT passwd FROM user WHERE username = ?` 安全查询数据库。
* 基于 `MYSQL_BIND` 结构体实现参数绑定，明确指定 `MYSQL_TYPE_STRING` 类型与输入长度限制。
#### 优化效果
* 彻底消除 SQL 注入漏洞，通过恶意输入测试。
* 保持登录内存缓存优先查询的性能优势，数据库交互耗时无明显增加。
### 2. C++11 线程与同步机制迁移（代码现代化）
#### 技术方案

|**模块**|**原实现（pthread）**|**优化实现（C++11）**|
|:---:|:---:|:---:|
|锁管理`（locker.h）`|`pthread_mutex_t / pthread_cond_t`|`std::mutex / std::condition_variable`
|线程池`（threadpool.h）`|`pthread_t*` 数组 + 静态 `worker` 函数|`std::vector<std::thread> `+ `lambda` 回调|
|日志异步线程|`pthread_create` + 全局函数|`std::thread` 成员变量 + 局部 `lambda`|
|信号量|sem_t （系统依赖）|`std::mutex `+ `std::condition_variable` 模拟|
#### 优化效果
* **跨平台兼容：** 支持 Linux（GCC）、Windows（MSVC），无需修改代码。
* **异常安全：** 基于 **RAII** 自动释放锁 / 线程资源，消除锁泄漏、线程悬垂问题。
* **代码简化：** 移除 200+ 行手动内存管理 / 类型转换代码（如 `pthread_join/void*` 强制转换）。

### 3. 定时器模块效率优化（性能提升）
#### 技术方案
* **数据结构升级：** 最小堆 + 哈希表（`std::vector` 存堆节点 + `std::unordered_map` 存索引），CRUD 操作时间复杂度降至 `O (logn)`。
* **超时处理机制：**

    * 惰性删除：超时定时器不立即删除，移入待处理队列批量处理。
    * 流控检查：通过 “多级预警 + 主动处理” 的策略大幅降低了队列满的概率，整体可靠性显著提升。
    * 独立线程消费队列，避免阻塞 epoll 主循环。
* **时间管理优化：** 

    * 替换 `time_t` 为 `std::chrono::steady_clock::time_point`，支持毫秒级精度。
    * 100ms 周期时间缓存（`time_cache_manager` 单例），减少 `time()` 系统调用 99.9%（如每秒查询 10000 次 → 仅 10 次系统调用）。
#### 优化效果
* 定时器操作耗时降低 80%+，10500 并发下超时连接清理耗时从 20ms 降至 3ms。
* 毫秒级精度支持，解决大文件传输时的连接误判超时问题。

### 4. 内存安全改进（稳定性提升）
#### 技术方案
* **资源托管：**用 `std::unique_ptr` 托管线程池（`std::unique_ptr<threadpool<http_conn>>`）、用户连接数组，自动释放资源。
* **字符串处理：** `std::string` 替代 `char*`，内置边界检查,消除缓冲区溢出。
#### 优化效果
* 消除内存泄露风险，简化析构函数，防止悬空指针和重复释放。
* 内存安全性大幅提升，异常安全性增强，代码可维护性提高。

## 压力测试结果
注：测试环境为 Ubuntu 20.04，2C2G 云服务器,使用 Webbench 工具（./webbench -c 10500 -t 5 [URL]），关闭日志后测试（10500 并发连接，持续 5s）

|**触发模式**|**事件模型**|**速度（pages/min）**|**吞吐量（bytes/sec）**|**成功请求数**|
|:---:|:---:|:---:|:---:|:---:|
|LT+LT|Proactor|96,912|180,812|8,076|
|LT+ET|Proactor|133,332|248,864|11,111|
|ET+LT|Proactor|251412|469302|20951|
|ET+ET|Proactor|241,128|441,638|20,094|
|ET+ET|Reactor|186,672|348,454|15,556|
||||||

## 致谢
* 基础架构支持：[qingouyi](https://github.com/qinguoyi)
* 参考书籍：《Unix 环境高级编程》《Unix 网络编程》
* 优化依赖：MySQL 预处理语句、C++11 标准库、Webbench 测试工具