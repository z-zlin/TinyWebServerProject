# 定时器超时处理机制优化

## 优化概述

本次优化实现了定时器的惰性删除和批量处理机制，显著提升了定时器系统的性能和稳定性。

## 主要改进

### 1. 惰性删除机制
- **修改前**: `tick()` 函数立即执行超时回调，可能阻塞主事件循环
- **修改后**: `tick()` 函数将超时定时器移动到待处理队列，不立即执行回调

### 2. 异步批量处理
- **新增**: `timeout_event_processor` 类，专门处理超时事件
- **特性**: 使用独立线程批量处理超时事件，减少上下文切换开销
- **优势**: 主事件循环不会被超时处理阻塞

### 3. 流控机制
- **队列大小限制**: 防止超时事件积压导致内存溢出
- **批处理大小控制**: 可配置每批处理的事件数量
- **优雅降级**: 队列满时丢弃事件并记录警告

### 4. 线程安全
- **原子操作**: 使用 `std::atomic` 确保状态一致性
- **互斥锁**: 保护共享数据结构
- **条件变量**: 实现高效的事件通知机制

## 新增接口

### sort_timer_lst 类新增方法
```cpp
void start_timeout_processor();     // 启动超时事件处理器
void stop_timeout_processor();      // 停止超时事件处理器
void set_batch_size(size_t size);   // 设置批处理大小
void set_max_queue_size(size_t size); // 设置最大队列大小
```

### timeout_event_processor 类
```cpp
class timeout_event_processor {
public:
    timeout_event_processor();
    ~timeout_event_processor();
    
    void start();  // 启动处理线程
    void stop();   // 停止处理线程
    void add_timeout_event(util_timer* timer, time_t expire_time);
    void set_batch_size(size_t size);
    void set_max_queue_size(size_t size);
};
```

## 使用方式

### 基本使用
```cpp
sort_timer_lst timer_list;

// 配置参数
timer_list.set_batch_size(50);        // 每批处理50个事件
timer_list.set_max_queue_size(10000); // 最大队列10000个事件

// 启动异步处理
timer_list.start_timeout_processor();

// 正常使用定时器（接口不变）
util_timer* timer = new util_timer();
timer->expire = time(nullptr) + 10;
timer->cb_func = callback_function;
timer->user_data = user_data;
timer_list.add_timer(timer);

// 主事件循环中调用tick（非阻塞）
timer_list.tick();
```

## 性能优势

### 1. 非阻塞处理
- 主事件循环不会被超时回调阻塞
- 提高服务器响应性能

### 2. 批量处理
- 减少频繁的上下文切换
- 提高CPU缓存利用率
- 降低系统调用开销

### 3. 流控保护
- 防止内存溢出
- 系统稳定性提升
- 可配置的性能参数

### 4. 错误隔离
- 超时回调异常不会影响主循环
- 异常处理和日志记录

## 配置建议

### 批处理大小
- **小规模应用**: 10-50
- **中等规模应用**: 50-200
- **大规模应用**: 200-1000

### 队列大小
- **内存充足**: 10000-50000
- **内存受限**: 1000-5000
- **高并发**: 根据预期并发量设置

## 兼容性

- **向后兼容**: 保持原有定时器接口不变
- **渐进式升级**: 可以逐步启用异步处理
- **回退机制**: 处理器未启动时自动回退到同步处理

## 注意事项

1. 确保在程序退出前调用 `stop_timeout_processor()`
2. 根据实际负载调整批处理和队列大小参数
3. 监控队列使用情况，避免事件积压
4. 超时回调函数应该是线程安全的
