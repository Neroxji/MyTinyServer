#ifndef THREADPOOL_H
#define THREADPOOL_H

#include<list>
#include<mutex>
#include<condition_variable>
#include<thread>
#include<vector>
#include<exception>

// 这是一个模板类，T 就是我们要处理的任务类型 (在咱们的项目里，T 就是 http_conn)
template<typename T>
class threadpool{
public:
    // thread_number: 线程池里的打工人数量 (默认 8 个)
    // max_requests: 任务队列里最多能堆积多少个未处理的请求 (默认 10000)
    threadpool(int thread_number=8,int max_requests=10000);
    ~threadpool();

    // 往队列里扔客人的点菜单
    bool append(T* request);

private:
    // 工作线程的死循环函数，不断从队列里抢活干
    void run();

private:
    int m_thread_number;                    // 线程数
    int m_max_requests;                       // 队列最大允许的请求数
    std::vector<std::thread> m_threads;     // 存放打工人的数组
    std::list<T*> m_workqueue;              // 任务队列 (存放 http_conn 指针)

    std::mutex m_queuelock;                // 互斥锁 
    std::condition_variable m_queuecond;    // 条件变量
    bool m_stop;                            // 是否关
};

// 具体实现部分
template<typename T>
threadpool<T>::threadpool(int thread_number,int max_requests):
        m_thread_number(thread_number),m_max_requests(max_requests),m_stop(false){

    if(thread_number<=0||max_requests<=0){
        throw std::exception();
    }

    // 批量招募，并让他们马上开始执行 run() 死循环
    for(int i=0;i<thread_number;i++){
        m_threads.emplace_back(std::thread(&threadpool::run,this));

        // detach 意味着让线程在后台自己玩，主线程不需要等它们结束
        m_threads.back().detach();
    }
}

 template<typename T>
 threadpool<T>::~threadpool(){
    {
        std::unique_lock<std::mutex> lock(m_queuelock);
        m_stop=true; // 关闭
    }
    m_queuecond.notify_all();
}

 template<typename T>
 bool threadpool<T>::append(T* request){
    // 必须先加锁
    std::unique_lock<std::mutex> lock(m_queuelock);

    if(m_workqueue.size()>=m_max_requests){
        return false; // // 队列满
    }

    m_workqueue.push_back(request);
    m_queuecond.notify_one();

    return true;  // 加的锁会在离开这个函数作用域时，由 unique_lock 自动解开
}

template<typename T>
void threadpool<T>::run(){
    while(!m_stop){
        T* request=nullptr;

        {
            // 准备去队列里拿任务，先加锁
            std::unique_lock<std::mutex>lock(m_queuelock);

            // wait 会自动把 m_queuelock 解开，醒来时又会自动加上
            m_queuecond.wait(lock,[this]{return m_stop||!m_workqueue.empty();});

            // 如果是被遣散唤醒的，直接退出走人
            if(m_stop&&m_workqueue.empty()){
                break;
            }

            request=m_workqueue.front();
            m_workqueue.pop_front();
        }

        if(request){
            // 拿着 http_conn 指针，去执行它的业务逻辑
            request->process();
        }
    }
}

#endif