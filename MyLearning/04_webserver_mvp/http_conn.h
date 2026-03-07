#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H

#include<unistd.h>
#include<signal.h>
#include<sys/types.h>
#include<sys/epoll.h>
#include <sys/uio.h>  
#include<fcntl.h>
#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<assert.h>
#include<sys/stat.h>
#include<string.h>
#include<pthread.h>
#include<stdio.h>
#include<stdlib.h>
#include<sys/mman.h>
#include<stdarg.h>
#include<errno.h>

static const int FILENAME_LEN = 200; // 文件名最大长度

// 1：主状态机 (当前正在分析哪一部分？)
enum CHECK_STATE{
    CHECK_STATE_REQUESTLINE=0,  // 正在分析请求行 (第一行: GET /index.html ...)     0
    CHECK_STATE_HEADER,         // 正在分析头部字段 (Host: localhost ...)           1
    CHECK_STATE_CONTENT         // 正在分析包体 (Post 请求才有，比如登录密码)           2
};


// 2：从状态机 (刚才切出来的那一行是啥情况？)
enum LINE_STATUS{
    LINE_OK=0,      // 完整读取了一行 (成功！)              0
    LINE_BAD,       // 这一行语法错误 (比如只有 \r 没有 \n)      1
    LINE_OPEN       // 行数据不完整 (没齐，下次继续读)       2
};  


// 3：HTTP 请求处理结果 (最终要给客户回什么？)
enum HTTP_CODE{
    NO_REQUEST,         // 请求不完整，需要继续读取客户端数据
    GET_REQUEST,        // 获得了一个完整的 GET 请求
    BAD_REQUEST,        // 客户发来的数据看不懂 (400)
    NO_RESOURCE,        // 客户要的资源不存在 (404)
    FORBIDDEN_REQUEST,  // 客户没有权限 (403)
    FILE_REQUEST,       // 请求文件成功
    INTERNAL_ERROR,     // 服务器内部错误 (500)
    CLOSED_CONNECTION   // 客户端关闭连接
};

// 4：HTTP 请求方法 (GET, POST...)
enum METHOD {
    GET = 0, 
    POST, 
    HEAD, 
    PUT, 
    DELETE, 
    TRACE, 
    OPTIONS, 
    CONNECT, 
    PATCH
};


class http_conn{
public:
    // 🌍 所有的 socket 上的事件都被注册到同一个 epoll 内核事件表中
    // 所以设置成 static 静态成员，让所有对象共享
    static int m_epollfd;
    static int m_user_count; // 统计现在的用户总数

    // 📏 定义读写缓冲区的大小
    static const int READ_BUFFER_SIZE=2048;  // 读缓冲区大小
    static const int WRITE_BUFFER_SIZE=1024; // 写缓冲区大小

    // 📏 定义文件大小
    static const int FILENAME_LEN=200;

public:
    http_conn(){}
    ~http_conn(){}

    // 🌟 初始化连接 (当 accept 拿到 connfd 后调用这个)
    void init(int sockfd,const sockaddr_in& addr);

    // 🔄 关闭连接
    void close_conn();

    // 📖 处理客户端请求 (这是核心业务入口！)
    void process();

    // 📥 非阻塞读 (一次性把数据读完)
    bool read_once();

    // 📤 非阻塞写 (把响应发给用户)
    bool write();


private:
    // ⚙️ 私有初始化函数 (重置内部变量)
    void init();

    // ===============================================
    // 🧠 核心解析逻辑
    // ===============================================
    // 从 m_read_buf 读取，并处理请求报文
    HTTP_CODE process_read();

    // 向 m_write_buf 写入响应报文
    bool process_write(HTTP_CODE ret);

    // 下面这一组函数被 process_read 调用以分析 HTTP 请求
    HTTP_CODE parse_request_line(char *text);   // 分析第一行
    HTTP_CODE parse_headers(char *text);        // 分析头部
    HTTP_CODE parse_content(char *text);        // 分析内容 eg:post
    HTTP_CODE do_request();                     // 生成响应
    LINE_STATUS parse_line();                   // ✨切菜刀：获取一行

    // 辅助函数：获取当前行在 buffer 中的起始地址
    char* get_line(){return m_read_buf+m_start_line;}

    // 这一组函数被 process_write 调用以填充 HTTP 应答
    bool add_response(const char* format,...);

    bool add_status_line(int status,const char* title);
    bool add_headers(int content_length);
    bool add_content_length(int content_length);
    bool add_content_type();
    bool add_linger();
    bool add_blank_line();

    bool add_content(const char* content);

    void unmap();

private:
    // 📡 网络相关
    int m_sockfd;           // 该 HTTP 连接的 socket
    sockaddr_in m_address;  // 通信的 socket 地址

    // 📦 读缓冲区
    char m_read_buf[READ_BUFFER_SIZE];

    // 📍 这里的三个变量至关重要！(解析时的游标)
    int m_read_idx;     // 标识读缓冲区中 已经读入的客户数据 的 最后一个字节 的下一个位置
    int m_checked_idx;  // 当前正在分析的字符在 读缓冲区 中的位置
    int m_start_line;   // 当前正在解析的行的起始位置

    // 🏷️ 状态机相关
    CHECK_STATE m_check_state;  // 主状态机当前所处的状态

    // 📦 写缓冲区
    char m_write_buf[WRITE_BUFFER_SIZE];
    int m_write_idx;    // 写缓冲区中待发送的字节数

    // 📂 文件相关 (处理请求的文件)
    char* m_url;            // 客户请求的目标文件名
    char* m_version;        // HTTP 协议版本
    char* m_host;           // 主机名
    int m_content_length;   // HTTP 请求的消息体长度
    bool m_linger;          // HTTP 请求是否要求保持连接 (Keep-Alive)

    // 请求方法 (GET, POST 等)
    METHOD m_method;

    char m_real_file[FILENAME_LEN]; // 拼接后的完整路径
    char* m_file_address;   // 客户请求的目标文件被 mmap 到内存中的起始位置
    struct stat m_file_stat;// 目标文件的状态 (判断文件是否存在、是否可读)

    // WriteV 相关 
    struct iovec m_iv[2]; // io vector: 两个盘子（头 + 体）
    int m_iv_count;       // 这一次发送我们要用几个盘子？(1个还是2个)
    int bytes_to_send;    // 还有多少字节没发完？
    int bytes_have_send;  // 已经发了多少字节？

};

#endif