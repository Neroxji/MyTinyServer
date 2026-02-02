｜http_conn 基础设施与底层 API 解析｜

	｜设置非阻塞 (setnonblocking) 的位运算逻辑｜
[代码]: 
  int old_option = fcntl(fd, F_GETFL);
  int new_option = old_option | O_NONBLOCK;
  fcntl(fd, F_SETFL, new_option);
[解析]:
  1. 读取旧状态 (F_GETFL): 
     就像查看开关面板，看哪些灯是亮的 (比如 [0 0 1 0])。
  2. 位运算 "或" (|) 的妙用:
     我们要开启“非阻塞”开关 (O_NONBLOCK，假设是 [0 1 0 0])，但不能关闭之前的开关。
     运算过程：
       0 0 1 0 (旧配置)
     | 0 1 0 0 (非阻塞)
     -----------
       0 1 1 0 (新配置：既保留了旧功能，又加了非阻塞)
  3. 写入新状态 (F_SETFL):
     相当于按下“保存”键。告诉内核：“把这个 socket 的属性更新为 0110”。

最后return old_option的原因可以理解成：假设你有一个函数 send_urgently(int fd)，它负责发送紧急数据。 这个函数希望能临时把 Socket 设为非阻塞，尝试发一下，发完之后，它不想破坏 Socket 原来的状态（因为外面可能还有其他代码指望着它是阻塞的）。



	｜event.events 的配置逻辑 (位运算组合)｜
Epoll 的事件配置像是一个自助餐盘，通过按位或 (|) 将不同的宏组合使用。
A. 基础事件 (主菜 - 发生什么事？)
* EPOLLIN : 表示对应的文件描述符可以读 (包括对端正常关闭 socket)。
* EPOLLOUT: 表示对应的文件描述符可以写 (发送缓冲区未满)。
* EPOLLRDHUP: 表示 TCP 连接的远端关闭或半关闭 (非常适合检测客户端断连)。
* EPOLLPRI: 表示有紧急数据 (Out-of-band data) 到达。
* EPOLLERR: 表示发生错误。

B. 行为模式 (调料 - 怎么通知？)
* EPOLLET (Edge Trigger): 边缘触发模式。状态变化时只通知一次 (高性能关键)。
* EPOLLONESHOT: 只监听一次事件。触发后需手动重置，防止多线程竞争同一 Socket。

C. 常用组合套餐
* 默认模式 (LT): event.events = EPOLLIN
* 高性能模式 (ET): event.events = EPOLLIN | EPOLLET
* 严谨模式 (TinyWebServer使用): event.events = EPOLLIN | EPOLLET | EPOLLRDHUP | EPOLLONESHOT

ps：“EPOLLIN: 别人发数据来了”是指事件本身，而“EPOLLET: 边缘触发”是指工作模式。两者是配合关系，不是替代关系。



	｜modfd 函数的核心作用｜
函数原型: void modfd(int epollfd, int fd, int ev)
该函数主要完成两个关键任务：
1. 任务切换: 通过参数 ev 指定接下来 socket 关注的事件类型 (读还是写)。
2. 状态重置: 重新激活 socket 的监听状态，使其能够再次接收系统通知。

为什么必须重新设置 EPOLLONESHOT？
这是一个常见的误区：既然 addfd 时已经设置了 ONESHOT，为什么 modfd 还要再加一遍？
* 核心机制:
EPOLLONESHOT 是一次性的。
当 Epoll 检测到事件并通知了某个线程后，内核会自动将该 socket 从监听队列中暂时“屏蔽”或“移除”。
这意味着，在线程处理完本次任务后，该 socket 处于“失聪”状态，内核不再监控它。

* modfd 的使命:
调用 epoll_ctl 的 EPOLL_CTL_MOD 操作时，不仅是修改事件类型，更重要的是“重新激活”该 socket。
必须再次显式加上 EPOLLONESHOT 标志，告诉内核：“请重新开始监听这个 socket，并且下次触发时依然只通知一次”。
如果不调用 modfd，这个 socket 将永远无法接收后续数据。

参数 ev 的配置逻辑
在 WebServer 场景下，ev 通常只在以下两个宏之间切换：
* EPOLLIN (读模式):
场景: 发送完响应后 (Keep-Alive)，或者刚建立连接时。
意图: 告诉内核“我要准备接收客户端的下一个请求了”。
* EPOLLOUT (写模式):
场景: 刚读取并解析完请求，准备好了响应数据。
意图: 告诉内核“我要发数据，请监控网卡缓冲区，一旦空闲就通知我”。

虽然 ev 理论上支持 EPOLLPRI 等其他标志，但在 HTTP 请求-响应的乒乓逻辑中，主要就是读与写的反复切换。



	｜Errno 错误处理机制｜
errno 是个啥？
它是一个全局变量（在 <errno.h> 头文件里定义）。 每当 Linux 的系统函数（比如 recv、send、open、write）执行失败（通常是返回 -1）时，操作系统会自动在这个变量里写下一个数字。
这个数字就是错误代码，用来告诉你“刚才到底哪里出错了”。

recv 返回值与 errno 详解
recv 返回 -1 时，并不总是代表“出错了”，需要检查全局变量 errno。
A. 假错 (正常结束)
* errno == EAGAIN (或 EWOULDBLOCK)
* 含义: "Resource temporarily unavailable"。
* 解读: 非阻塞模式下，这意味着 TCP 缓冲区已经空了，没有数据可读了。
* 动作: break 退出循环 (读取任务圆满完成)。

B. 打断 (需要重试)
* errno == EINTR
* 含义: "Interrupted system call"。
* 解读: 读取过程被系统信号打断。
* 动作: 视情况而定，通常选择 continue (再试一次)。

C. 真错 (连接故障)
* errno == ECONNRESET (对方强行复位/崩溃)
* errno == EPIPE (向已关闭的连接写入)
* recv 返回值 == 0 (对方正常 close 关闭连接)
* 动作: return false (通知上层逻辑关闭连接)。



	｜writev高级转发技术｜
iovec 结构体详解 
struct iovec { 
void *iov_base; // 内存块的起始地址 
size_t iov_len; // 内存块的长度
 };
三个参数:
1. fd: 目标文件描述符 (即客户端的 socket)。
2. iov: 一个 iovec 结构体数组的指针 (发货清单)。
3. iovcnt: 数组中有几个元素 (清单上有几项货物)。

struct iovec m_iv[2];
* m_iv[0]: 存放 HTTP 响应头部 (Status Line + Headers)。数据在 m_write_buf 中。
* m_iv[1]: 存放 HTTP 响应体 (Body)。数据在 m_file_address (mmap 映射的文件内存) 中。

writev 方式 (高性能):
零拷贝 (Zero Copy) 思想。
不需要拼接，直接告诉内核两块数据的位置，内核会自动把它们拼成一个 TCP 流发出去。
效率极高，特别适合 Web Server 这种 "小头 + 大体" 的数据结构。


两个基准地址：
在 writev 发送过程中，数据并不是存在一块连续内存里的，而是分成了“两块地”：
* 仓库 A (HTTP 头部):
  - 变量: m_write_buf
  - 来源: 用户空间定义的数组 (Stack/Heap)。
  - 内容: 状态行 + 头部字段 (如 Content-Length)。
  - 长度: m_write_idx

* 仓库 B (HTTP 文件体):
  - 变量: m_file_address
  - 来源: mmap 映射的内核/磁盘缓冲区。
  - 内容: 网页文件 (如 index.html, image.jpg)。
  - 特点: 与仓库 A 在物理内存上不连续，相隔很远。

因此，计算偏移量时，必须根据当前发送的进度，分别以 m_write_buf 或 m_file_address 为起跑线 (Base)。


 场景分析：断点续传的两种情况
变量定义:
* bytes_have_send: 累计已发送的总字节数 (包含 头+体)。
* m_write_idx: 头部的固定长度。

情况 A：头部还没发完
判断条件: bytes_have_send < m_write_idx
逻辑:
1. 既然头部没发完，说明目前的进度全都在“仓库 A”里。
2. 我们不需要关心仓库 B，只需要在仓库 A 里移动指针。
公式:
m_iv[0].iov_base = m_write_buf + bytes_have_send;
m_iv[0].iov_len  = m_write_idx - bytes_have_send;

情况 B：头部已发完，正在发文件
判断条件: bytes_have_send >= m_write_idx
逻辑:
1. 头部已经全部发出去了，m_iv[0] 的任务结束 (长度设为 0)。
2. 现在的进度主要在“仓库 B”里。
3. 要计算在仓库 B 里走了多远，必须从“总进度”里扣除掉“头部的长度”。
公式:
// 算出在文件内的相对偏移量
int file_offset = bytes_have_send - m_write_idx;

// 新起点 = 文件基地址 + 相对偏移量
m_iv[1].iov_base = m_file_address + file_offset;

// 剩余长度 = 还需要发送的总字节数
m_iv[1].iov_len  = bytes_to_send;
