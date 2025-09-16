# TinyWebServer 时间管理接口使用总结

## 接口简化结果

经过分析和优化，`lst_timer.cpp` 中的接口已经统一简化，移除了冗余和未使用的函数。

## 保留的核心接口

### 1. 时间缓存管理器 (time_cache_manager)

#### 基础接口
- `get_instance()` - 获取单例实例
- `start()` - 启动时间缓存
- `stop()` - 停止时间缓存
- `is_running()` - 检查运行状态

#### 时间获取接口
- `get_current_time()` - 获取高精度当前时间 (steady_time_point)
- `get_current_time_t()` - 获取当前时间戳 (time_t)

#### 时间计算接口
- `time_t_to_steady(time_t)` - 时间戳转高精度时间
- `steady_to_time_t(steady_time_point)` - 高精度时间转时间戳
- `add_milliseconds(base, ms)` - 添加毫秒到时间点
- `duration_to_milliseconds(duration)` - 时间差转毫秒

#### 便捷接口
- `get_current_time_t_plus_seconds(int)` - 获取当前时间+秒数
- `get_current_time_plus<T>(T)` - 模板函数，自动选择秒/毫秒

### 2. 定时器 (util_timer)

#### 简化的接口
- `set_expire_milliseconds(int64_t)` - 设置超时时间（毫秒）

### 3. 定时器链表 (sort_timer_lst)

#### 核心接口
- `add_timer(util_timer*)` - 添加定时器
- `adjust_timer(util_timer*)` - 调整定时器
- `del_timer(util_timer*)` - 删除定时器
- `tick()` - 处理超时定时器

#### 管理接口
- `start_timeout_processor()` - 启动超时事件处理器
- `stop_timeout_processor()` - 停止超时事件处理器
- `set_batch_size(size_t)` - 设置批处理大小
- `set_max_queue_size(size_t)` - 设置最大队列大小

## 项目中的使用情况

### webserver.cpp 中的使用
```cpp
// 创建定时器时设置超时时间
timer->set_expire_milliseconds(3 * TIMESLOT * 1000);

// 调整定时器时重置超时时间
timer->set_expire_milliseconds(3 * TIMESLOT * 1000);

// 添加定时器到链表
utils.m_timer_lst.add_timer(timer);

// 调整定时器位置
utils.m_timer_lst.adjust_timer(timer);

// 删除定时器
utils.m_timer_lst.del_timer(timer);
```

### log/log.cpp 中的使用
```cpp
// 获取当前时间用于日志记录
auto& time_cache = time_cache_manager::get_instance();
time_t t = time_cache.get_current_time_t();
```

### lst_timer.cpp 内部使用
```cpp
// 在tick()方法中获取当前时间
auto& time_cache = time_cache_manager::get_instance();
steady_time_point cur = time_cache.get_current_time();

// 在定时器设置中获取当前时间
steady_time_point now = time_cache.get_current_time();
```

## 移除的冗余接口

### 已移除的定时器接口
- `set_expire_seconds(time_t)` - 未使用，被毫秒接口替代
- `get_expire_seconds()` - 未使用
- `get_expire_milliseconds()` - 未使用

### 已移除的时间缓存接口
- `get_current_time_plus_milliseconds(int64_t)` - 未使用，功能重复

## 优化效果

1. **接口数量减少**：从12个公共接口减少到8个核心接口
2. **功能集中**：相关功能合并到统一的接口中
3. **使用简化**：只保留实际使用的接口，减少学习成本
4. **性能提升**：移除未使用的代码，减少编译体积

## 使用建议

1. **新项目**：直接使用简化后的接口
2. **现有项目**：逐步迁移到新的接口
3. **性能敏感场景**：优先使用 `get_current_time()` 和 `set_expire_milliseconds()`
4. **兼容性需求**：使用 `get_current_time_t()` 和 `get_current_time_t_plus_seconds()`

## 总结

通过这次接口简化，TinyWebServer 的时间管理模块变得更加简洁高效：
- 保留了所有实际使用的功能
- 移除了冗余和未使用的接口
- 提供了更清晰的API设计
- 提高了代码的可维护性

