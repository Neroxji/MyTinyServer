#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<stdio.h>
#include<unistd.h>
#include<errno.h>
#include<string.h>
#include<fcntl.h>
#include<stdlib.h>
#include<sys/epoll.h>
#include"http_conn.h"
#include"threadpool.h"

#define MAX_FD 65536            // 最大的文件描述符个数
#define MAX_EVENT_NUMBER 10000  // Epoll 一次最多监听多少个事件

// 这三个函数我们在 http_conn.cpp 里写过，这里声明一下外部调用
extern void addfd(int epollfd,int fd,bool one_shot);
extern void removefd(int epollfd,int fd);
extern void modfd(int epollfd,int fd,int ev);

int main(int argc,char* argv[]){
    // 1. 检查命令行参数 (运行的时候需要传个端口号，比如 ./server 9000)
    if(argc<=1){
        printf("usage: %s port_number\n",basename(argv[0]));
        return 1;
    }
    int port=atoi(argv[1]); // 把字符串端口号转成整数

    // 2. 创建线程池 (员工休息室).   ??
    threadpool<http_conn>* pool=NULL;
    try{    //?????
        pool=new threadpool<http_conn>;
    }catch(...){
        return 1;
    }

    // 3. 预先分配好所有的客户对象 (准备好所有的餐桌)
    http_conn* users=new http_conn[MAX_FD];

    // 4. 网络编程老三样：创建 socket -> bind -> listen
    int listenfd=socket(PF_INET,SOCK_STREAM,0);

    // 设置端口复用 (防止服务器重启时报 Address already in use)
    int reuse=1;
    setsockopt(listenfd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));

    struct sockaddr_in address;
    bzero(&address,sizeof(address));
    address.sin_family=AF_INET;
    address.sin_addr.s_addr=htonl(INADDR_ANY);  // 监听本机所有网卡 IP
    address.sin_port=htons(port);

    bind(listenfd,(struct sockaddr*)&address,sizeof(address));
    listen(listenfd,5); // 5 是全连接队列的最大长度

    // 5. 创建 Epoll 对象 (大堂经理的监控室).   ??
    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd=epoll_create(5); //epoll_create1是啥？？

    // 把监听 socket 加到 epoll 里
    // 🚨 注意：监听 socket 不能开启 EPOLLONESHOT，否则只能接一个客人！
    addfd(epollfd,listenfd,false);

    // 把 epollfd 赋值给 http_conn 的静态变量，让所有服务员共用一个监控室
    http_conn::m_epollfd=epollfd;

    // ==========================================================
    // 🌀 6. 终极死循环：大堂经理开始站岗
    // ==========================================================
    while(true){
        // epoll_wait 阻塞等待，直到有事件发生
        // 返回的 number 就是有几个 socket 响了
        int number=epoll_wait(epollfd,events,MAX_EVENT_NUMBER,-1);

        // 如果出错且不是因为信号中断，就退出
        if(number<0&&errno!=EINTR){
            printf("epoll failure\n");
            break;
        }

        // 遍历所有响了的 socket
        for(int i=0;i<number;i++){
            int sockfd=events[i].data.fd;

            // 🚨 情况 A：监听 socket 响了，说明有新客人来了！
            if(sockfd=listenfd){
                struct sockaddr_in client_address;
                socklen_t client_addrlength=sizeof(client_address);

                // 接客！拿到接客专属的 connfd
                int connfd=accept(listenfd,(struct sockaddr*)&client_address,&client_addrlength);
                if(connfd<0){
                    continue;
                }

                // 如果目前连接数满了，拒客
                if(http_conn::m_user_count>=MAX_FD){
                    close(connfd);
                    continue;
                }

                // 给新客人分配一个服务员
                users[connfd].init(connfd,client_address);
            }
        }
    }

}