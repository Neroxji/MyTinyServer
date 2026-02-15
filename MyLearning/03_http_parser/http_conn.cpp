#include "http_conn.h"

// =================================================================
// 1. 静态成员初始化
// =================================================================

// 所有的 socket 上的事件都被注册到同一个 epoll 对象中
// 所以 epoll 文件描述符是静态的，所有对象共享
int http_conn::m_epollfd=-1;
int http_conn::m_user_count=0;

// =================================================================
// 2. Epoll 辅助函数 (这些是给 Epoll 打下手的工具函数)
// =================================================================

// 🔧 设置文件描述符为非阻塞 (Non-blocking)
// 为什么要非阻塞？因为我们要配合 Epoll 的 ET (边缘触发) 模式！
// 如果是阻塞的，recv 没数据时会把线程卡死，服务器就废了。
int setnonblocking(int fd){

    // fcntl 是 Linux 的文件控制函数 (File Control)
    // F_GETFL: 获取 fd 当前的状态标志 (比如它现在是不是阻塞的)
    int old_option=fcntl(fd,F_GETFL);

    // 给旧标志加上 O_NONBLOCK (非阻塞) 属性
    int new_option=old_option|O_NONBLOCK;

    // F_SETFL: 把新标志设置回去
    fcntl(fd,F_SETFL,new_option);

    return old_option;
}

// 🔧 向 Epoll 中添加需要监听的文件描述符
// fd: 要监听的 socket
// one_shot: 是否开启 EPOLLONESHOT (防止多线程同时处理同一个连接)
void addfd(int epollfd,int fd,bool one_shot){
    epoll_event event;
    event.data.fd=fd;

    // EPOLLIN:  别人发数据来了 (可读)
    // EPOLLET:  边缘触发 (Edge Trigger)，高性能模式，只通知一次！
    // EPOLLRDHUP: TCP连接被对方关闭了
    event.events=EPOLLIN|EPOLLET|EPOLLRDHUP;

    // 如果开启 ONE_SHOT (通常对 socket 连接都要开)
    // 保证一个 socket 在任意时刻只能被一个线程处理，防止数据乱套
    if(one_shot){
        event.events |=EPOLLONESHOT;
    }

    // 调用内核 API 添加监控
    epoll_ctl(epollfd,EPOLL_CTL_ADD,fd,&event);

    // ⚠️ 极其重要：ET 模式下，fd 必须设为非阻塞！
    setnonblocking(fd);
}

// 🔧 从 Epoll 中移除文件描述符
void removefd(int epollfd,int fd){
    // 从内核监控表中删除
    epoll_ctl(epollfd,EPOLL_CTL_DEL,fd,0);
    // 关闭文件句柄 (挂断电话)
    close(fd);
}

// 🔧 修改文件描述符，重置 ONESHOT 事件
// 场景：一个线程处理完读写后，这个 socket 就失效了(因为 ONESHOT)。
// 必须调用这个函数，把它重新激活，让 Epoll 继续监控它。
void modfd(int epollfd,int fd,int ev){
    epoll_event event;
    event.data.fd=fd;

    // 重新把 ONESHOT 加上，并加上新的事件 ev (通常是 EPOLLIN 或 EPOLLOUT)
    event.events=ev|EPOLLET|EPOLLONESHOT|EPOLLRDHUP;// 这里要把 EPOLLONESHOT 再传一次

    epoll_ctl(epollfd,EPOLL_CTL_MOD,fd,&event);
}

// =================================================================
// 3. 连接管理 (初始化与关闭)
// =================================================================

// 🏨 公有初始化：当新客户连接进来时调用
void http_conn::init(int sockfd,const sockaddr_in& addr){
    m_sockfd=sockfd;
    m_address=addr;

    // 端口复用 (调试方便，防止服务器重启报 "Address already in use")
    // 这里的 reuse = 1 表示允许复用
    int reuse=1;
    setsockopt(m_sockfd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));

    // 把它加到 Epoll 监控名单里，并开启 ONESHOT
    addfd(m_epollfd,sockfd,true);
    m_user_count++;

    // 调用私有的 init 做内部变量的大扫除
    init();
}

// 🧹 私有初始化：清空内部变量 (大扫除)
void http_conn::init(){
    // 1. 游标/指针归零 (最关键！)
    // 想象你在读一本书：
    m_check_state = CHECK_STATE_REQUESTLINE; // 从第一行标题开始读
    m_checked_idx = 0; // 读到第几个字
    m_start_line = 0;  // 这一行是从哪开始
    m_read_idx = 0;    // 读缓冲区
    m_write_idx = 0;   // 写缓冲区

    // 2. HTTP 请求信息归零 (把上一个客人的菜单撕掉)
    m_method = GET;      // 默认假设是 GET 请求
    m_url = 0;           // 文件名
    m_version = 0;       // 协议版本
    m_content_length = 0;// 包体有多长
    m_linger = false;    // 默认不保持连接 (Connection: close)
    m_host = 0;          

    // 3. 物理清空缓冲区 (把桌子擦干净)
    // 这一步其实不是必须的（因为游标归零了，新数据会覆盖旧数据），
    // 但为了安全和调试方便，全部刷成 0 (\0)
    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

// 👋 关闭连接
void http_conn::close_conn(){
    // m_sockfd != -1 说明连接还开着
    if(m_sockfd!=-1){// 这句if是起到一个保险作用！因为防止一不小心第二次调用的话m_user_count--可能会出错
        // 从 Epoll 移除，关闭句柄
        removefd(m_epollfd,m_sockfd);
        m_sockfd=-1;// 标记为无效

        m_user_count--;
    }
}

// =================================================================
// 4. IO 读写操作 (核心)
// =================================================================

// 📥 循环读取客户数据，直到无数据可读
// 返回 true: 读取成功 (哪怕没读完，只要没出错)
// 返回 false: 读出错了，或者对方关闭连接了 -> 需要 close_conn
bool http_conn::read_once(){
    // 游标检查：如果缓冲区满了，就别读了，防止溢出
    if(m_read_idx>=READ_BUFFER_SIZE){
        return false;
    }

    int bytes_read=0;// 这次 recv 读到了多少字节

    // 🔄 开启循环
    while(true){
        // 1. m_read_buf + m_read_idx: 存到哪？(注意要接着上次写的地方往后写，不能覆盖！)
        // 2. READ_BUFFER_SIZE - m_read_idx: 还能存多少？(防止越界)
        bytes_read=recv(m_sockfd, m_read_buf+m_read_idx, READ_BUFFER_SIZE-m_read_idx,0);

        if(bytes_read==-1){
            // 🛑 情况 A: 读完了 (EAGAIN / EWOULDBLOCK)
            if(errno==EAGAIN||errno==EWOULDBLOCK){
                break;
            }

            // 🛑 情况 B: 真出错了
            return false;
        }
        else if(bytes_read==0){
            // 🛑 情况 C: 对方关闭连接了 (EOF)
            // recv 返回 0 代表对方调用了 close，也得关
            return false;
        }

        // ✅ 读到了数据
        // 更新游标，为了下一次循环读取做准备
        m_read_idx+=bytes_read;
    }

    return true;
}

// 📤 往 socket 里写数据
// 返回 true: 没出错 (至于发没发完，不一定，可能要等下一轮 Epoll 通知)
// 返回 false: 出错了 (比如对方关连接了)
bool http_conn::write(){
    int temp=0;

    // 如果没啥要发的，那就算发完了
    if(bytes_to_send==0){
        // 既然发完了，就重新设置 Epoll 监听“读事件”，准备接收下一次请求
        modfd(m_epollfd,m_sockfd,EPOLLIN);
        init();
        return true;
    }

    while(true){
        // writev (分散写)
        // 把 m_iv 数组里记录的多个内存块，一次性发给 socket
        temp=writev(m_sockfd,m_iv,m_iv_count);

        if(temp<-1){
            // 🛑 情况 A: 写缓冲区满了 (EAGAIN)
            // 也就是 TCP 发送窗口满了，塞不进去了
            if(errno==EAGAIN){
                // 既然现在塞不进去，那就先设为监听“写事件” (EPOLLOUT)
                // 等缓冲区空了，Epoll 会自动叫醒我们，那时候再接着发
                modfd(m_epollfd,m_sockfd,EPOLLOUT);
                return true;
            }
            // 🛑 情况 B: 真出错了 (比如发送过程中对方断开了)
            unmap(); //释放文件内存映射
            return false;
        }

        // ✅ 成功发送了 temp 字节
        bytes_have_send+=temp;
        bytes_to_send-=temp;

        // 更新 iovec 指针
        // 因为 writev 不保证一次全发完，如果发了一半被截断了，
        // 下次必须从“断点”继续发，不能重头再来！

        // 情况 1: 头部 (iv[0]) 已经发完了，现在发的是文件 (iv[1])
        if(bytes_have_send>=m_iv[0].iov_len){
            // 头部发完了，那就把 iv[0] 废掉 (长度设为0)
            m_iv[0].iov_len=0;
            // 计算文件还剩多少没发，起始位置往后移
            // file_address + (已经发的总数 - 头部长度)
            m_iv[1].iov_base=m_file_address+(bytes_have_send-m_write_idx);
            m_iv[1].iov_len=bytes_to_send;
        }else{// 情况 2: 头部都没发完 （bytes_have_send < m_write_idx）
            // 头部起始位置往后移
            m_iv[0].iov_base=m_write_buf+bytes_have_send;
            m_iv[0].iov_len=m_iv[0].iov_len-temp;
        }

        // 🏁 所有的都发完了
        if(bytes_to_send<=0){
            unmap();// 释放文件内存

            // 决定下一步：是保持连接还是断开？
            // m_linger 是之前解析 HTTP 头解析出来的 Connection: keep-alive
            if(m_linger){
                // 如果是长连接：重置为读模式，准备读下一个请求
                init();
                modfd(m_epollfd,m_sockfd,EPOLLIN);
                return true;
            }else{
                // 如果是短连接：直接返回 false，让上层调用 close_conn
                modfd(m_epollfd,m_sockfd,EPOLLIN);
                return false;
            }
        }
    }
}

// =================================================================
// 5. 业务逻辑入口 (由线程池调用)
// =================================================================

// ⚙️ 处理 HTTP 请求的入口函数
void http_conn::process(){

    // 1. 【读解析】分析 HTTP 请求
    // process_read 是接下来要写的核心大函数
    // 它会返回一个“状态码”，告诉我们请求分析得怎么样了
    HTTP_CODE read_ret=process_read();

    // 🛑 情况 A: 请求不完整 (NO_REQUEST)
    // 比如客户只发了 "GET /ind"，还没发完。
    // 这时候不能急着处理，得继续监听“读事件”，等客户把剩下的发过来。
    if(read_ret==NO_REQUEST){
        modfd(m_epollfd,m_sockfd,EPOLLIN);
        return;
    }

    // 2. 【写准备】生成 HTTP 响应
    // 比如根据 read_ret 生成 "200 OK" 或者 "404 Not Found"
    bool write_ret=process_write(read_ret);

    // 🛑 情况 B: 响应生成失败
    if(!write_ret){
        close_conn(); // 既然没法回复，就关掉连接
    }

    // ✅ 情况 C: 响应准备好了
    // 告诉 Epoll：“我这边数据准备好了，一旦网卡空闲，就提醒我发送 (EPOLLOUT)”
    // 只要 Epoll 触发 EPOLLOUT，主线程就会去调用我们之前写的 write() 函数
    modfd(m_epollfd,m_sockfd,EPOLLOUT);
}

// =================================================================
// 6. HTTP 请求解析 (主状态机)  4个函数！！！
// =================================================================

// 三个分析函数⬇️

// (State 1)解析请求行 
// 📝 解析 HTTP 的第一行
// 目标格式: GET /index.html HTTP/1.1
HTTP_CODE http_conn::parse_request_line(char* text){

    // 1. 解析请求方法 (GET/POST)
    // m_url 此时指向字符串开头
    // strpbrk: 在 text 中寻找第一个 ' ' 或 '\t' 的位置
    m_url=strpbrk(text,"\t");

    // 如果没找到空格，说明格式不对 (HTTP 请求行里必须有空格分隔)
    if(!m_url){
        return BAD_REQUEST;
    }

    // 把找到的那个空格变成 \0，这样前面的字符串就“断开”了
    // 此时 text 变成了 "GET\0/index.html HTTP/1.1"
    *m_url++='\0';

    // 取出前面的方法存起来
    char* method=text;
    if(strcasecmp(method,"GET")==0){
        m_method=GET;
    }else if(strcasecmp(method,"POST")==0){
        m_method=POST;
    }else{
        return BAD_REQUEST;// 目前只支持 GET 和 POST
    }

    // 2. 解析版本号 (HTTP/1.1)
    // m_url 现在指向 "/index.html HTTP/1.1" (刚才跳过了第一个空格)
    // strspn: 检索字符串中第一个不在 " \t" 中出现的字符下标 -> 也就是跳过连续的空格
    m_url+=strspn(m_url,"\t");

    // 继续找下一个空格，分隔 URL 和 Version
    m_version=strpbrk(m_url,"\t");
    if(!m_version){
        return BAD_REQUEST;
    }

    // 同样，把空格变 \0，截断 URL
    *m_version='\0';

    // m_version 现在指向 "HTTP/1.1"
    m_version+=strspn(m_version,"\t"); // 跳过空格

    // 检查版本号是不是 HTTP/1.1
    if(strcasecmp(m_version,"HTTP/1.1")!=0){
        return BAD_REQUEST;
    }

    // 3. 解析 URL (/index.html)
    // 有些客户端发的 URL 可能会带上协议头，比如 http://192.168.1.1/index.html
    // 我们需要把前面的 http:// 剔除掉，只保留 /index.html
    if(strncasecmp(m_url,"http://",7)==0){
        m_url+=7; // 跳过 http://
        // 找域名的结束位置 (第一个 /)
        m_url=strchr(m_url,'/');
    }

    // 同样的逻辑处理 https
    if(strncasecmp(m_url,"https://",8)==0){
        m_url+=8;
        m_url=strchr(m_url,'/');
    }

    // 正常情况下，URL 应该是 / 开头的
    if(!m_url||m_url[0]!='/'){
        return BAD_REQUEST;
    }

    // ⚠️ 特殊处理：如果你直接访问 http://localhost/，默认给你看 index.html
    if(strlen(m_url)==1){
        strcat(m_url,"index.html");
    }

    // ✅ 解析完毕！
    // 状态转移：请求行分析完了，下一步该分析“头部字段”了
    m_check_state=CHECK_STATE_HEADER;

    return NO_REQUEST; // 还没结束，去处理 Header
}

// 解析头部字段 (State 2)
// 📨 解析 HTTP 头部的一行
// 例子: "Connection: keep-alive"
HTTP_CODE http_conn::parse_headers(char* text){

    // 🟢 情况 1: 遇到空行 (最关键的逻辑！)
    // 为什么 text[0] 是 '\0'？
    // 因为 parse_line 把 "\r\n" 变成了 "\0\0"。
    // 如果这一行原本只有 "\r\n" (空行)，被切完后就只剩 "\0" 了。
    if(text[0]=='\0'){

        // 判断：如果有消息体 (比如 POST 请求，Content-Length > 0)
        if(m_content_length!=0){
            // 状态转移：头部读完了，还得去读身体 (Body)
            m_check_state=CHECK_STATE_CONTENT;
            return NO_REQUEST; // 还没结束，继续读
        }

        // 否则说明是 GET，且没有 Body，那整个请求彻底结束了！
        return GET_REQUEST;
    }

    // 🟢 情况 2: 处理 Connection 头部
    // strncasecmp: 比较前 11 个字符是不是 "Connection:"
    else if(strncasecmp(text,"Connection:",11)==0){
        text+=11;   // 跳过 "Connection:"
        text+=strspn(text,"\t");// 跳过冒号后面的空格

        // 看看值是不是 keep-alive
        if(strcasecmp(text,"keep-alive")==0){
            m_linger=true; // 记下来：这是一个长连接
        }
    }

    // 🟢 情况 3: 处理 Content-Length 头部
    else if(strncasecmp(text,"Content-Length:",15)==0){
        text+=15;
        text+=strspn(text,"\t");

        // atol: ASCII to Long (把字符串 "1024" 转换成数字 1024)
        m_content_length=atol(text);
    }

    // 🟢 情况 4: 处理 Host 头部
    else if(strncasecmp(text,"Host:",5)==0){
        text+=5;
        text+=strspn(text,"\t");
        m_host=text;
    }

    // 🟢 情况 5: 其他头部 (User-Agent, Accept 等)
    else{
        printf("oop! unknown header: %s\n", text);
    }

    return NO_REQUEST;
}

// 解析请求体 (State 3)
// 📦 只有 POST 请求会走到这里
// 判断依据很简单：缓冲区里剩下的数据 >= m_content_length
HTTP_CODE http_conn::parse_content(char* text){

    // m_read_idx: 读缓冲区现在的总长度 (recv 到的所有数据)
    // m_checked_idx: 当前已经分析完的长度 (也就是 头部总长度) body开始的位置
    // m_content_length: 刚才在 Header 里读出来的，客户承诺要发的数据量

    // 公式：如果 (现在读到的总数) >= (头部长度 + 身体长度)
    if(m_read_idx>=(m_content_length+m_checked_idx)){
        text[m_content_length]='\0'; // 方便去打印日志
        return GET_REQUEST;
    }

    return NO_REQUEST;
}

// 🧠 核心大脑：分析 HTTP 请求
HTTP_CODE http_conn::process_read(){

    // 这两个变量是用来记录“切行”的结果
    LINE_STATUS line_status=LINE_OK;
    HTTP_CODE ret=NO_REQUEST;
    char* text=0;

    // 🔄 主循环
    while((m_check_state==CHECK_STATE_CONTENT&&line_status==LINE_OK)
        ||(line_status=parse_line())==LINE_OK){

        // 获取刚才切出来的那一行数据的字符串
        // get_line() 是一个小函数，其实就是 return m_read_buf + m_start_line;
        text=get_line();

        // 既然切出了一行，为了下一次切行做准备，把 m_start_line 更新一下
        m_start_line=m_checked_idx;

        // 打印日志 (可选)：看看这一行是啥
        printf("got 1 http line: %s\n", text);

        // 🔀 状态机核心：根据当前状态，决定怎么处理这一行
        switch(m_check_state){

            // 🏷️ 状态 1: 正在分析请求行 (例: "GET /index.html HTTP/1.1")
            case CHECK_STATE_REQUESTLINE:{
                ret=parse_request_line(text); // 调用子函数分析
                if(ret==BAD_REQUEST){
                    return BAD_REQUEST; // 格式错了，直接报错
                }
                break; //这一行处理完了，跳出 switch，去切下一行
            }

            // 📨 状态 2: 正在分析头部字段 (例: "Host: localhost")
            case CHECK_STATE_HEADER:{
                ret=parse_headers(text);
                if(ret==BAD_REQUEST){
                    return BAD_REQUEST;
                }
                // 关键点：如果 parse_headers 返回 GET_REQUEST，说明头读完了！
                else if(ret==GET_REQUEST){
                    // 也就是遇到了 ！！！！空行 ！！！！，意味着请求解析完毕，可以去准备响应了
                    return do_request();
                }
                break;
            }

            // 📦 状态 3: 正在分析请求体 (仅 POST 请求会用到)
            case CHECK_STATE_CONTENT:{
                ret=parse_content(text);
                if(ret==GET_REQUEST){
                    return do_request(); // 体也读完了，去准备响应
                }
                // 如果返回 LINE_OPEN，说明体还没传完，得跳出循环继续读 socket
                line_status=LINE_OPEN;
                break;
            }

            // 💀 默认状态：出错了
            default:{
                return INTERNAL_ERROR;
            }
        }
    }

    // 🛑 循环结束了，通常是因为 parse_line 返回了 LINE_OPEN (数据不完整，只有半行)
    // 或者是 buffer 读空了。
    // 告诉上层：还没处理完，继续监听 socket，等剩下的数据发过来。
    return NO_REQUEST;
}

// =================================================================
// 7. 从状态机：切行 (把一行数据从缓冲区切出来)
// =================================================================

// 分析当前读取的一行内容
// 返回值：
// LINE_OK: 切好了一行
// LINE_BAD: 语法错误
// LINE_OPEN: 数据不完整，还要继续读
LINE_STATUS http_conn::parse_line(){
    char temp;

    // m_checked_idx: 当前 正在分析的字符 在缓冲区的位置
    // m_read_idx: 缓冲区里 最后有效数据 的下一个位置
    for(;m_checked_idx<m_read_idx;++m_checked_idx){

        // 拿到当前字符
        temp=m_read_buf[m_checked_idx];

        // 🟢 情况 1: 如果当前字符是 '\r' (回车)
        if(temp=='\r'){

            // 如果它已经是最后一个字符了，说明后面没东西了 -> 数据不完整
            if((m_checked_idx+1)==m_read_idx){
                return LINE_OPEN;
            }

            // 如果它的下一个字符是 '\n'，说明找到了一行结束！(\r\n)
            else if(m_read_buf[m_checked_idx+1]=='\n'){
                // 把 \r 和 \n 都改成 \0，这样前面的字符串就截断了
                m_read_buf[m_checked_idx++]='\0';
                m_read_buf[m_checked_idx++]='\0';
                return LINE_OK; // 成功切出一行！
            }

            // 否则，说明语法错误 (HTTP 规定必须是 \r\n)
            return LINE_BAD;
        }

        // 🟢 情况 2: 如果当前字符是 '\n' (换行)
        // (有些时候 \r 会在上一轮循环被处理，这里处理 \n)
        else if(temp=='\n'){

            // 看看前一个字符是不是 \r
            if((m_checked_idx>1)&&(m_read_buf[m_checked_idx-1]=='\r')){
                // 是的话，把它们都改成 \0
                m_read_buf[m_checked_idx-1]='\0';
                m_read_buf[m_checked_idx++]='\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }

    // 跑完循环都没找到 \r\n，说明这一行还没发完
    return LINE_OPEN;
}

// =================================================================
// 8. 业务逻辑核心：处理请求 (do_request)
// =================================================================

// 📂 网站根目录 (存放 html, 图片等资源的文件夹路径)
const char* doc_root="/Users/neroji/Desktop/MyTinyServer/resource file";

HTTP_CODE http_conn::do_request(){

    // m_real_file: 最终的物理路径 (doc_root + m_url)
    // 先把根目录拷进去
    strcpy(m_real_file,doc_root);
    int len=strlen(doc_root);

    // 再把 URL 拼接到后面
    strncpy(m_real_file+len,m_url,FILENAME_LEN-len-1);

    // 🔎 1. 获取文件状态 (stat 是 Linux 系统调用)
    // m_file_stat 是 http_conn 类里的成员变量 (struct stat)
    // 如果返回 -1，说明文件不存在 -> 404
    if(stat(m_real_file,&m_file_stat)<0){
        return NO_RESOURCE;
    }

    // 🔒 2. 权限检查 (S_IROTH: 其他人有读权限)
    // 如果没有读权限 -> 403
    if(!(m_file_stat.st_mode&S_IROTH)){
        return FORBIDDEN_REQUEST;
    }

    // 📁 3. 检查是不是目录 (S_ISDIR)
    // 如果请求的是个文件夹 (比如 /home/xxx/resources/) -> 400
    if(S_ISDIR(m_file_stat.st_mode)){
        return BAD_REQUEST;
    }

    // ✅ 文件检查通过！
    // 接下来把文件映射到内存

    // 以只读方式打开文件
    int fd=open(m_real_file,O_RDONLY);// O_RDONLY：只读

    // 调用 mmap
    m_file_address=(char*)mmap(0,m_file_stat.st_size,PROT_READ,MAP_PRIVATE,fd,0);

    // 映射完就可以关掉文件句柄了，内存映射依然有效
    close(fd);

    return FILE_REQUEST;
}

// =================================================================
// 9. 响应构造辅助函数 (专门负责往 m_write_buf 里填数据)
// =================================================================

// 🖊️ 基础写函数：往 m_write_buf 里写入格式化字符串
bool http_conn::add_response(const char* format,...){

    // 如果写入位置超过了缓冲区大小，报错
    if(m_write_idx>=WRITE_BUFFER_SIZE){
        return false;
    }

    // 定义可变参数列表
    va_list arg_list;
    va_start(arg_list,format);

    // vsnprintf: 把参数格式化成字符串，写入 m_write_buf
    int len=vsnprintf(m_write_buf+m_write_idx,WRITE_BUFFER_SIZE-1-m_write_idx,format,arg_list);

    // 如果写入失败，或者缓冲区不够大了
    if(len>=(WRITE_BUFFER_SIZE-1-m_write_idx)){
        return false;
    }

    // 更新写指针
    m_write_idx+=len;
    va_end(arg_list);

    return true;
}

// 🏷️ 添加状态行 (例如: HTTP/1.1 200 OK)
bool http_conn::add_status_line(int status,const char* title){
    return add_response("%s %d %s\r\n","HTTP/1.1",status,title);
}

// 🏷️ 添加消息头 (Content-Length, Connection 等)
bool http_conn::add_headers(int content_len){
    add_content_length(content_len);
    add_linger();
    add_blank_line();
    return true;
}

bool http_conn::add_content_length(int content_len){
    return add_response("Content-Length: %d\r\n",content_len);
}

bool http_conn::add_linger(){
    return add_response("Connection: %s\r\n",(m_linger==true)?"keep-alive":"close");
}

bool http_conn::add_blank_line(){
    return add_response("%s","\r\n");
}

// 🏷️ 添加内容 (主要用于报错时写 "404 Not Found" 这种短文本)
bool http_conn::add_content(const char* content){
    return add_response("%s",content);
}

// 🏷️ 添加内容类型 (Content-Type)
bool http_conn::add_content_type(){
    return add_response("Content-Type:%s\r\n","text/html");
}

