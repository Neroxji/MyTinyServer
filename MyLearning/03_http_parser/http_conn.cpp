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

    // 核心事件配置：
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
    m_check_state = CHECK_STATE_REQUESTLINE; // 把“脑子”重置：准备从第一行标题开始读
    m_checked_idx = 0; // 当前读到第几个字了？归零
    m_start_line = 0;  // 当前这一行是从哪开始的？归零
    m_read_idx = 0;    // 读缓冲区里一共存了多少字？归零
    m_write_idx = 0;   // 写缓冲区里准备了多少字？归零

    // 2. HTTP 请求信息归零 (把上一个客人的菜单撕掉)
    m_method = GET;      // 默认假设是 GET 请求
    m_url = 0;           // 忘了用户要啥文件名
    m_version = 0;       // 忘了协议版本
    m_content_length = 0;// 忘了包体有多长
    m_linger = false;    // 默认不保持连接 (Connection: close)
    m_host = 0;          // 忘了主机名

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
