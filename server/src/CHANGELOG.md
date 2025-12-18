---

##  v1.1.0

* **日志和索引刷盘逻辑优化**
    * 显著降低了 $msync$ 调用次数。
    * 现在刷盘逻辑仅依赖 **Linux 系统自动刷盘**、**定时刷盘**，以及**程序内部定时刷盘**（当前配置为 6 分钟一次）。
* **网络 I/O 优化**
    * 调整了 $epoll-update$ 逻辑。
    * $recvmsg$ : $epollctl$ 调用比率已从 **10:24** 优化至 **11:20**。（计划在后续版本中进一步优化）

---

## v2.0.0

- **版本更新**: 更新项目版本至 v2.0.0
- **API 调整**: `客户端 push逻辑变动，服务器响应补充pushresponce的baseoffset字段


---



## v4.0.0

- **版本更新**: 更新项目版本至 v4.0.0
- **API 调整**: 去掉了所有对外api
- **内部调整**: 
1 组协议变动，组成员的分配逻辑现在完全转交给server，并使用一致性hash 算法 管理分区分配场景
2 server端去掉了冗余的对外接口，因为本质上服务器外部显式调用某些接口
3 去掉了大部分事件，完全依赖心跳进行CS双端间的世代，分区等情况的交互（事件码还没去掉），处理心跳逻辑相应改进
4 配置中若IP和port字段会抛出异常并终止程序
5 分区改用以tp为key的扁平化结构，并优化了锁的颗粒度(__consumeroffset暂不调整)
6 组的成员状态改成用期望状态，悲观锁视图，实际状态三个assign表管理分区消费，保证消费唯一性
7 update_subscription这个对外api需要传入心跳包的genid
8 genid现在起到标识成员所在的epoch（纪元）的作用
1 添加了错误码GENERATION_EXPIRED标识提交偏移量时世代已经过期
2 现在offset交由group来管理
3 为快速查询分区的所属权，建立了反向映射，并且补充了相关逻辑
4 为TopicPartition 增加了'<'比较操作符
5 新增在ConsumerGroupState里中增加了get_partition_owner函数用于调试，还有获取/提交offset的接口，上层接口相应更新
6 现在会在assign发生变换是根据用户的配置填入这个分区最新的offset
7 MQ初始化修改，现在Consumeroffset类功能暂时下线
8 去掉了之前已经废弃的函数parse_assignments_message
1 mmap部分方法改写：a.reset改名close，并且不会再清空filename。b.allocate方法现在会先进行防御性检查，不会操作已失效的对象，当容量满和mmap指针失效时只会抛出out_of_range
c.禁用copy语义，实现移动语义
d.新增swap函数和rename_file辅助移动语义，也可以主动调用，代替take_ownership_of_internal的作用
2 去除了Topic类，现在Topic完全是逻辑上的概念
3 有关get_first_baseoffset的函数统一更名为get_earilestoffset
4 replace_mmapfile_content利用移动语义和mmapfile的方法简化重写
5 Logsegment新增mark_as_clean_in_lock方法，辅助未来定时清理过期段的行为，该方法功能是:讲自己这个段强制刷盘，关闭日志段文件描述符，关闭日志段的mmap，将管理的两个文件尾部rename增加".clean"
6 PartitionStorage新增archive_segment，通过指定日志段的baseoffset来将其无效化（无法再被读取）
7 当客户端配置为earliestoffset时，暂时只返回0，后续会补足该逻辑


---



~                                                                                                                                                                                                                                    
~                                                                                                                                                                                                                                    
~                                                                                                                                                                                                                                    
~                                                                                                                                                                                                                                    
~                                                                                                                                                                                                                                    
~                                                          
