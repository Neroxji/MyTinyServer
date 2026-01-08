#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<unistd.h>
#include<stdio.h>
#include<errno.h>
#include<string.h>
#include<stdlib.h>
#include<sys/epoll.h>

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


// ===============================================================

    // ==========================================
    // 🪓 Epoll 第一板斧：创建管家 (Create)
    // ==========================================
    // 创建一个 epoll 实例 (红黑树的根节点)
    // 参数 5 只是个提示，现在内核会自动调整，填 >0 的数就行
    int epollfd=epoll_create(5);
    if(epollfd==-1){
      perror("epoll_create error");
      return -1;
    }

    // ==========================================
    // 🪓 Epoll 第二板斧：给管家派活 (Ctl - Add)
    // ==========================================
    // 咱们要把 listenfd (门卫) 交给管家盯着，看有没有人来连接
    struct epoll_event event;
    event.data.fd=listenfd;// 记录：这是 listenfd 的事
    event.events=EPOLLIN;
    // 也可以写成 event.events = EPOLLIN | EPOLLET; (如果要用 ET 模式，但咱们先用默认的 LT)

    // 把便签条贴到管家身上 (往内核事件表里添加)
    ret=epoll_ctl(epollfd,EPOLL_CTL_ADD,listenfd,&event);
    if(ret==-1){
      perror("epoll_ctl error");
      return -1;
    }

    printf("Epoll 构建完成,listenfd 已加入监控！\n");

    // 准备一个篮子，用来接管家扔出来的事件
    struct epoll_event events[1024];


// ===============================================================

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
