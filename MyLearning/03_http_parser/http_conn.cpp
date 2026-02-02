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