//socket编程释例！要手撕main函数！！！

/*我是谁：Socket 的老祖宗。
  干嘛的：提供了 socket()(买手机)、bind()(插卡）、listen()(监听）、accept()(接电话）
  这些最核心的函数。*/
#include<sys/socket.h>

/*我是谁：互联网地址库。
  干嘛的：定义了那个著名的结构体 struct sockaddr_in。
  你在代码里设置 sin_port（端口）、sin_addr（IP地址）时，全靠它。*/
#include<netinet/in.h>

/*我是谁：IP地址翻译官。
  干嘛的：人类看的是字符串 "192.168.1.1"，电脑看的是二进制整数。
  这个头文件里的 inet_ntop 和 inet_pton 就是专门负责翻译这两个的。*/
#include<arpa/inet.h>
//⬆️网络三剑客（必须背下来，或者存成代码片段）

/*我是谁：Unix/Linux 标准库（Windows 里没有这个！）。    Linux 的灵魂
干嘛的：它是 Linux 的“万能工具箱”。
用到哪了：read()（读数据）、write()（写数据）、close()（关闭连接）都在这里。
没有它，你的程序连退出都不行。*/
#include<unistd.h>// (UNIX Standard)

#include<stdio.h>// printf, perror (打印日志）
#include<errno.h>// 这里只有全局变量 errno (错误号)
#include<string.h>// bzero, strlen, memset 都在这里 (处理内存/字符串)  
#include<stdlib.h>// atoi, exit malloc(类型转换、退出程序)

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

            // ==========================================
            // 🔥 TODO: 新增接收逻辑 (recv) 开始 🔥
            // ==========================================
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

