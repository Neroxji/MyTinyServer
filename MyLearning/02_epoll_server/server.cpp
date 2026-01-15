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
    // 要把 listenfd 交给管家盯着，看有没有人来连接
    struct epoll_event event;
    event.data.fd=listenfd;// 记录：这是 listenfd 的事
    event.events=EPOLLIN;
    // 也可以写成 event.events = EPOLLIN | EPOLLET; (如果要用 ET 模式//先用默认的 LT)

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

    // ==========================================
    // 🪓 Epoll 第三板斧：坐等通知 (Wait)
    // ==========================================
    while(true){
    // 1. 让管家开始工作，阻塞等待事件发生
    // epollfd: 管家ID
    // events:  管家拿来装“便签”的篮子 (数组)
    // 1024:    篮子最大能装多少
    // -1:      超时时间 (-1 表示死等，直到有事发生；0 表示不等待；>0 表示毫秒)
    // 返回值 number: 也就是“实际上发生了几件事”
    int number=epoll_wait(epollfd,events,1024,-1);

    if(number<0){
        perror("epoll_wait failure");
        break;
    }

    for(int i=0;i<number;i++){

        // 提取出是哪个 fd 发生了事件
        // 这里的 events[i].data.fd 就是当初存进去的那个 listenfd
        int sockfd=events[i].data.fd;

        // 情况一：前台大门响了 (新用户连接)
        if(sockfd==listenfd){
            struct sockaddr_in client_address;
            socklen_t client_addrlength=sizeof(client_address);

            // 此时调用 accept 绝对不会阻塞，因为 Epoll 告诉你肯定有连接
            int connfd=accept(listenfd,(struct sockaddr*)&client_address,&client_addrlength);

            if(connfd<0){
                perror("accept error");
                continue;//因为在for循环里面
            }

            // 打印一下新客人的信息
            char remoteAddr[INET_ADDRSTRLEN];
            inet_ntop(AF_INET,&client_address.sin_addr,remoteAddr,INET_ADDRSTRLEN);
            printf("1.1 新的连接! FD: %d, IP: %s\n",connfd,remoteAddr);

            // ⚠️【关键一步】：把新进来的客人 (connfd) 也交给 Epoll 管家管理！
            // 如果不加这一步，管家就不认识这个客人，以后他说话你也听不到
            struct epoll_event event;
            event.data.fd=connfd;
            event.events=EPOLLIN;// 关心“读”事件 (他发数据)

            epoll_ctl(epollfd,EPOLL_CTL_ADD,connfd,&event);
            printf("1.2 已将 fd %d 加入 Epoll 监控\n", connfd);
        }

        // 情况二：客房电话响了 (老用户发数据)
        else if(events[i].events&EPOLLIN){
            char buf[1024]={0};

            // 读取数据
            ssize_t len=recv(sockfd,buf,sizeof(buf)-1,0);

            if(len>0){
                printf("2.1 收到来自 fd %d 的消息: %s\n", sockfd, buf);

                // 回复一个消息 (原来的逻辑)
                char response[]=
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/plain\r\n"
                    "Connection: close\r\n"
                    "\r\n"
                    "Hello from Epoll Server!";
                send(sockfd,response,strlen(response),0);

                // 发送完直接移除并关闭
                epoll_ctl(epollfd,EPOLL_CTL_DEL,sockfd,NULL);
                close(sockfd); // 挂断电话
                printf("2.2 -> 响应已发送，主动关闭连接 fd %d\n", sockfd);
            }

            else if(len==0){
                // 客户端断开了连接
                printf("客户端 fd %d 断开了连接\n", sockfd);

                // 1. 从 Epoll 名单里删除 (让管家别盯着了)
                epoll_ctl(epollfd,EPOLL_CTL_DEL,sockfd,NULL);
                // 2. 真正的关闭连接
                close(sockfd); 
            }

            else{
                perror("recv error");
                epoll_ctl(epollfd, EPOLL_CTL_DEL, sockfd, NULL);
                close(sockfd);// 出错也关掉
            }
            
        }
      }
    }
    
    //close(listenfd);// 关机
    return 0;
}