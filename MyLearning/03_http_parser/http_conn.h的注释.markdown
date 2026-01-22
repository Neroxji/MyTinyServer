｜ HTTP 连接处理模块 (http_conn) 注释 ｜

enum LINE_STATUS⬇️
 从状态机 (Slave) —— 那个“认死理”的秘书 👓
	•	它的任务：只负责断句。
	•	它的逻辑：它根本不关心信里写的是情书还是欠条，它只关心**“这一行写完了没？”**
	•	它是怎么工作的：
	◦	它拿着放大镜，一个字一个字地看。
	◦	看到 \r\n（回车换行），它就喊：“老板，这里有一行完整的句子！” (LINE_OK)
	◦	如果读到最后没看到 \r\n，它就喊：“老板，这句子写一半没了，等下一封信送来再拼吧。” (LINE_OPEN)
	◦	如果格式乱七八糟，它就喊：“这写的啥玩意儿？” (LINE_BAD)
总结：它是“眼睛”。只管把一大坨数据切成整整齐齐的一行一行。

enum CHECK_STATE⬇️
主状态机 (Master) —— 那个“懂业务”的老板 🧠
	•	它的任务：负责理解。
	•	它的逻辑：它手里拿着一本《HTTP 办事指南》，根据当前读到的行，决定这一行数据代表什么意思。
	•	它是怎么工作的：
	◦	它会问秘书：“下一行给我。”
	◦	第一阶段 (CHECK_STATE_REQUESTLINE)：
	▪	它心里想：“刚开始读，这一行肯定是标题（GET / HTTP/1.1）。”
	▪	读完标题后，它会在小本本上记下：“好，标题读完了，把状态改成读正文。”
	◦	第二阶段 (CHECK_STATE_HEADER)：
	▪	它心里想：“现在的每一行肯定都是附加条款（Host: xxx）。”
	▪	它会一直读，直到秘书递给它一个空行。
	▪	它就明白了：“哦，条款读完了，后面如果还有东西，那就是包裹内容了。”
	◦	第三阶段 (CHECK_STATE_CONTENT)：
	▪	它心里想：“现在读进来的都是包裹内容（比如密码、文件）了。”
总结：它是“大脑”。它根据当前的进度（状态），赋予那一行文字具体的含义。



class http_conn
这个类是给 main.cpp（主线程）和 线程池（Worker Threads）用的。
你可以把 WebServer 想象成一个 “繁忙的办事大厅”：
	•	Main 线程（大堂经理）：负责站在门口（Epoll），看谁来了。
	•	http_conn 对象（办事档案）：每一个连进来的客户（Socket），我们都给他建一个档案。这个档案里记录了他要办什么业务（读缓冲区）、办到哪一步了（状态机）。
	•	线程池（柜台办事员）：经理把档案扔过来，办事员拿着这个档案（调用里面的函数）去处理业务。


WebServer运转流程
[Main 线程] accept() -> 拿到 fd -> 找个 http_conn 对象 -> 🌟init(fd)
      ⬇️
[Epoll] 收到数据 (EPOLLIN)
      ⬇️
[Main 线程] 📥read_once() -> 把数据全吸入 m_read_buf
      ⬇️
[任务队列] 把这个对象扔进去
      ⬇️
[Worker 线程] 抢到任务 -> 📖process()
       ├── process_read() (状态机分析: 要啥?)
       └── process_write() (打包响应: 给啥?) -> 注册 EPOLLOUT
      ⬇️
[Epoll] 网卡空闲 (EPOLLOUT)
      ⬇️
[Main 线程] 📤write() -> 把数据和网页发回给浏览器
      ⬇️
[Main 线程] 发送完毕 -> 🔄close_conn()


——————————————————————
Public: （下面分析的都是 公有 化里面的） ps：为了下一个cpp文件做铺垫 更详细的注释

1️⃣ void init (int sockfd, const sockaddr_in& addr)
[角色]: 建档 / 办理入住
[调用时机]: 
  当 Main 线程 accept 成功，拿到一个新的文件描述符 connfd 的那一瞬间。
[核心动作]:
  1. 记录身份：把 sockfd 和对方的 IP 地址存到 http_conn 对象的成员变量里。
  2. 设置属性：把这个 sockfd 设为【非阻塞】(Non-blocking)。
     * 注意：这对于 Epoll 的 ET (边缘触发) 模式至关重要，防止读写卡死线程。
  3. 上户口：调用 epoll_ctl 把这个新连接注册到内核事件表中。
     * 监听 EPOLLIN 读事件。
     * 设置 EPOLLET (边缘触发)。
     * 设置 EPOLLONESHOT (防止多线程同时处理同一个 socket)。
  4. 清空脑子：调用私有的 init()，初始化所有的状态机变量、清空读写缓冲区，保证它是张白纸。

2️⃣ void close_conn()
[角色]: 销毁 / 退房 / 赶人
[调用时机]: 
  1. 数据发完了（且对方没有设置 Keep-Alive 长连接）。
  2. 或者代码运行出错（比如 read 失败、内存不足）。
[核心动作]:
  1. 摘除监听：从 Epoll 红黑树上删除这个 sockfd（不再监听它了）。
  2. 挂断电话：关闭文件描述符 close(sockfd)。
  3. 更新统计：用户总数 -1 (m_user_count--)。

3️⃣ bool read_once()
[角色]: 速记员 / 一口气听完
[调用时机]: 
  当 Epoll 通知你 EPOLLIN（有消息来了）时。由 Main 线程调用。
[核心动作]:
  1. 循环读取：调用 recv 从 TCP 缓冲区里读数据，存到 m_read_buf 里。
  2. ET模式的关键：
     * 因为 Epoll ET 模式只会通知一次“有数据”。
     * 所以必须写一个 while(true) 循环，一直读、一直读。
     * 直到 recv 返回错误且 errno == EAGAIN，说明“真的啥都没有了”，才能停下来。
  3. 风险提示：如果只读一次没读完就 break，剩下的数据就丢失了（Epoll 不会再次通知）。

4️⃣ void process()
[角色]: 大脑 / 核心业务逻辑
[调用时机]: 
  当 read_once 读完数据后，Main 线程把任务扔进线程池队列，子线程抢到任务后调用。
[核心动作]:
  它是连接“读”和“写”的桥梁：
  1. 理解需求 (process_read): 
     * 启动 HTTP 状态机，分析 m_read_buf 里的数据。
     *如果是 GET_REQUEST，去硬盘里把网页文件找出来（使用 mmap）。
  2. 准备回应 (process_write): 
     * 根据刚才的结果，把 HTTP 响应头（如 "HTTP/1.1 200 OK"）写进 m_write_buf。
  3. 准备发送: 
     * 向 Epoll 注册 EPOLLOUT 事件。
     * 告诉内核：“我数据准备好了，等网卡空闲了通知我发货”。

5️⃣ bool write()
[角色]: 快递员 / 发货
[调用时机]: 
  当 Epoll 通知你 EPOLLOUT（网卡空闲，可以写了）时。由 Main 线程调用。
[核心动作]:
  1. 发送两部分数据 (使用 writev):
     * 头部：m_write_buf 里的响应头。
     * 内容：m_file_address 里的文件内容（内存映射）。
  2. 循环发送: 
     * 同样是非阻塞的，必须在循环里尽可能多地发。
  3. 善后处理:
     * 如果一次没发完：记录发到哪了，等待下次 EPOLLOUT 通知接着发。
     * 如果发完了：检查是否是 Keep-Alive。
       - 是：重置状态，等待下一次请求。
       - 否：调用 close_conn() 断开连接。


——————————————————————
Private: （下面分析的都是 私有 化里面的）  ps：为了下一个cpp文件做铺垫 更详细的注释

// ⚙️ 私有初始化函数 (重置内部变量)
    void init();
这个函数就是用来**“洗白”**自己的。保证每一次处理新请求时，自己都是崭新的，没有任何“前任”留下的痕迹！✨


// http_conn 内部解析逻辑简述 (Process & Parse)

1. 核心总控：HTTP_CODE process_read()
[地位]: HTTP 解析的主入口，驱动主状态机运行。
[逻辑]: 
  1. 开启循环，调用 parse_line() 尝试从缓冲区切出一行。
  2. 根据当前主状态 (m_check_state) 分发任务：
     * CHECK_STATE_REQUESTLINE -> 调用 parse_request_line()
     * CHECK_STATE_HEADER          -> 调用 parse_headers()
     * CHECK_STATE_CONTENT        -> 调用 parse_content()
  3. 如果某个状态解析未完成 (LINE_OPEN)，退出循环继续等待数据。


2. 基础工具：LINE_STATUS parse_line()
[地位]: 从状态机，行级解析器。
[逻辑]: 
  1. 遍历 m_read_buf，查找行结束符 "\r\n"。
  2. 若找到：将 "\r\n" 替换为字符串结束符 "\0\0"，返回 LINE_OK。
  3. 若没找到：返回 LINE_OPEN，表示数据不完整，需继续 recv。


3. 辅助指针：char* get_line()
[逻辑]: return m_read_buf + m_start_line;
[作用]: 获取当前 正在解析 的那一行字符串在内存中的起始地址。


4. 状态机子函数 (The Specialists)
(A) HTTP_CODE parse_request_line(char *text)
  [目标]: 解析请求行 (如 "GET /index.html HTTP/1.1")
  [逻辑]: 
    1. 提取请求方法 (GET/POST)，存入 m_method。
    2. 提取目标 URL，存入 m_url。
    3. 提取协议版本，存入 m_version。
    4. 成功后，状态流转至 CHECK_STATE_HEADER。

(B) HTTP_CODE parse_headers(char *text)
  [目标]: 解析头部字段 (如 "Host: localhost")
  [逻辑]: 
    1. 判断是否为空行：若是，说明头部结束。
       - 若 Content-Length > 0，状态流转至 CHECK_STATE_CONTENT。
       - 否则，说明请求全部结束，返回 GET_REQUEST。
    2. 解析关键字段：记录 Content-Length，判断 Connection 是否为 keep-alive。

(C) HTTP_CODE parse_content(char *text)
  [目标]: 解析请求体  (仅在 POST 请求且 Content-Length > 0 时触发)
  [逻辑]: 
    1. 验证数据是否读够了 Content-Length 指定的长度。
    2. 读取并保存 POST 数据（如用户名密码）。
    3. 解析结束，返回 GET_REQUEST。


5. 响应生成：bool process_write(HTTP_CODE ret)
[地位]: 根据解析结果，生成 HTTP 响应报文。
[逻辑]: 
  根据 process_read 返回的 HTTP_CODE (ret) 执行不同操作：
  * FILE_REQUEST (文件存在):
    - 写入状态行 (200 OK)。
    - 写入响应头 (Content-Length 等)。
    - 准备发送 mmap 映射的文件内容。
  * NO_RESOURCE (404):
    - 写入 404 Not Found 页面内容到 m_write_buf。
  * INTERNAL_ERROR (500):
    - 写入 500 Error 页面。



// HTTP 响应构建函数组 (Response Builders)
作用：辅助 process_write() 将数据格式化写入写缓冲区 (m_write_buf)

1. 基础工具
* add_response(format, ...)
  [逻辑]: 类似 printf，将格式化的字符串写入 m_write_buf，并更新写指针 m_write_idx。
  [注意]: 所有后续的 add_xxx 函数底层都调用它。


2. 头部构建 (Headers)
* add_status_line(status, title)
  [作用]: 写入响应行 (如 "HTTP/1.1 200 OK\r\n")。

* add_headers(content_length)
  [作用]: 统筹调用以下子函数，一次性写入所有头部字段：
    - add_content_length: 写入 "Content-Length: 1024\r\n"。
    - add_content_type: 写入   "Content-Type: text/html\r\n"。
    - add_linger: 写入             "Connection: keep-alive\r\n"。
    - add_blank_line: 写入空行 "\r\n" (头部结束标志)。


3. 内容构建 (Body)
* add_content(content)
  [作用]: 将字符串内容写入缓冲区 (通常用于构建 404/500 等简短的错误提示页面)。
  [注意]: 正经的大文件传输通常使用 m_file_address (mmap)，不走这个函数。


4. 资源清理
* unmap()
  [作用]: 释放 mmap 映射的内存区域。
  [时机]: 当一次文件响应发送完毕后调用，防止内存泄漏。




