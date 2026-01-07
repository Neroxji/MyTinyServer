#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<unistd.h>
#include<stdio.h>
#include<errno.h>
#include<string.h>
#include<stdlib.h>
#include <sys/epoll.h>

int main(){
    // 1. 创建套接字 (买个手机)
    // PF_INET: IPv4, SOCK_STREAM: TCP
    int listenfd=socket(PF_INET,SOCK_STREAM,0);
    if(listenfd==-1){
        perror("socket error");
        return -1;
    }

    // 2. 绑定端口 (插上电话卡)
    struct sockaddr_in address;
    bzero(&address,sizeof(address));//memset(&address,0,sizeof(address))
    address.sin_family=AF_INET;//如果要用 IPv6，这里就要填 AF_INET6
    address.sin_addr.s_addr=htonl(INADDR_ANY); // 允许任何IP连接
    address.sin_port=htons(9006);//把“主机字节序”转成“网络字节序”（防止大小端问题导致 IP 读反了）。
    
    // 这一步是为了防止“端口被占用”报错 (可选，但推荐)
    int reuse=1;
    setsockopt(listenfd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));//调用设置函数

    int ret=bind(listenfd,(struct sockaddr*)&address,sizeof(address));
    if(ret==-1){
      perror("bind error");
      return -1;
    }
    // 3. 监听 (等待电话响)
    // 5: 同时等待队列长度 (同时最多有5个人排队打进来)
    ret=listen(listenfd,5);
    if(ret==-1){
      perror("listen error");
      return -1;
    }

    printf("服务器启动成功！正在监听 9006 端口...\n");

    // ==========================================
    // 🪓 Epoll 第一板斧：创建 Epoll 实例
    // ==========================================
    // 参数 5：以前代表哈希表大小，现在内核只把它当做一个提示（只要 > 0 即可），
    // 内核会根据监听数量动态调整。
    // 返回值：epollfd，这是“管家”的 ID，以后有事都找它。
    int epollfd = epoll_create(5);
    if (epollfd == -1) {
        perror("epoll_create error");
        return -1;
    }

    // ==========================================
    // 🪓 Epoll 第二板斧：注册事件 (epoll_ctl)
    // ==========================================
    // 我们要告诉管家：“帮我盯着 listenfd 这个手机，如果有电话进来了(EPOLLIN)，就告诉我。”
    
    // 1. 准备一个结构体，用来描述我们要盯着什么事件
    struct epoll_event event;
    
    // data.fd: 这是一个自定义数据。等会儿事件发生时，epoll_wait 会原封不动地把这个 fd 还给你。
    // 这样你就知道是“谁”出事了。
    event.data.fd = listenfd; 
    
    // events: 你对什么感兴趣？
    // EPOLLIN: 表示“可读”事件（有新连接来了，或者有数据发过来了）。
    // (默认是 LT 水平触发模式，咱们先不加 EPOLLET，稳扎稳打)
    event.events = EPOLLIN; 

    // 2. 调用 epoll_ctl (Control)
    // 参数含义：
    // epollfd: 找哪个管家？
    // EPOLL_CTL_ADD: 动作是什么？(ADD: 添加, MOD: 修改, DEL: 删除)
    // listenfd: 也就是我们要监控的文件描述符
    // &event: 具体监控要求的详细描述
    ret = epoll_ctl(epollfd, EPOLL_CTL_ADD, listenfd, &event);
    if (ret == -1) {
        perror("epoll_ctl error");
        return -1;
    }

    printf("Epoll 实例创建成功，listenfd 已经加入监控队列！\n");
    
    // 准备一个数组，用来接收管家汇报的事件
    // 假设我们一次最多处理 1024 个事件
    struct epoll_event events[1024];

    // ==========================================
    // 🪓 Epoll 第三板斧：等待事件 (epoll_wait)
    // ==========================================
    //。。。。。。。。

    while(true){
      // 4. 接受连接 (接电话)
      struct sockaddr_in client_address;
      socklen_t client_addrlength=sizeof(client_address);

      // accept 是一个阻塞函数，程序会停在这里等，直到有人连上来
      int connfd=accept(listenfd,(struct sockaddr*)&client_address,&client_addrlength);
      if(connfd<0){
        perror("accept error");
      }else{
        // 成功连上！
        char remoteAddr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET,&client_address.sin_addr,remoteAddr,INET_ADDRSTRLEN);
        printf("有人连上来了!IP是: %s\n", remoteAddr);

        // 准备一个空碗 (数组)，清零
        char buf[1024];
        memset(buf,0,sizeof(buf));

        // 开始接收 (recv)
        ssize_t len=recv(connfd,buf,sizeof(buf)-1,0);//最多读 1023 个字节 (留一个位置给结束符)

        if(len>0){
          printf("收到客户端发来的消息 [%ld bytes]:\n%s\n", len, buf);//%ld:对应long(Long Decimal)。
          //如果你定义 ssize_t len -> 打印用 %ld。如果你定义 int len -> 打印用 %d。
        }else if(len==0){
          printf("客户端断开了连接。\n");
        }else{
          perror("recv 失败");
        }

        // 格式：HTTP版本 状态码 \r\n 头部信息 \r\n \r\n 正文        
        char response[]=
          "HTTP/1.1 200 OK\r\n"
          "Content-Type: text/plain\r\n"
          "\r\n"
          "Hello from c++ Server!";
        send(connfd,response,strlen(response),0);

        close(connfd);// 挂断电话
      }
    }

    //close(listenfd);// 关机
    return 0;
}

