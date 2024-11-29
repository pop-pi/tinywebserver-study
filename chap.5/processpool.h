#ifndef PROCESSPOOL_H
#define PROCESSPOOL_H

#include<sys/types.h>
#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<assert.h>
#include<stdio.h>
#include<unistd.h>
#include<errno.h>
#include<string.h>
#include<fcntl.h>
#include<stdlib.h>
#include<sys/epoll.h>
#include<signal.h>
#include<sys/wait.h>
#include<sys/stat.h>

// 描述一个子进程的类
class process{
    
public:
    process():m_pid(-1){}
public:
    pid_t m_pid;  // 目标子进程的PID
    int m_pipefd[2];    // 父进程和子进程通信用的管道
};

//进程池类
template<typename T>
class processpool{
private:
    // 将构造函数定义为私有，只能通过后面的create静态函数来创建processpool示例
    processpool(int listenfd,int process_number=8);
public:
    //单体模式，以保证程序最多创建一个processpool实例，这是程序正确处理信号的必要条件
    static processpool<T>* create(int listenfd,int process_number=8){
        if(!m_instance){
            m_instance=new processpool<T>(listenfd,process_number);
        }
        return m_instance;
    }
    ~processpool(){
        delete[] m_sub_process;
    }
    //  启动进程池
    void run();
private:
    void setup_sig_pipe();
    void run_parent();
    void run_child();
private:
    static const int MAX_PROCESS_NUMBER=16; //进程池允许的最大子进程数量
    static const int USER_PER_PROCESS=65536;    // 每个子进程最多能处理的客户数量
    static const int MAX_EVENT_NUMBER=10000;    // epoll最多能处理的事件数
    int m_process_number;   // 进程池中的进程总数
    int m_idx;              // 子进程在池中的序号，从0开始
    int m_epollfd;          // 每个进程都有一个epoll内核时间表，用m_epollfd标识
    int m_listenfd;         // 监听socket
    int m_stop;             // 子进程通过m_stop来决定是否停止运行
    process* m_sub_process; // 保存所有子进程的描述信息
    static processpool<T>* m_instance; // 进程池静态实例
};

template<typename T>
processpool<T>* processpool<T>::m_instance=NULL;

// 用于处理信号的管道，以实现统一事件源，后面称之为信号管道
static int sig_pipefd[2];

/*
    进程池构造函数，参数listenfd是监听socket，
    它必须在创建进程池之前被创建，否则子进程无法直接引用它，
    参数process_number指定进程池中子进程的数量
*/
template<typename T>
processpool<T>::processpool(int listenfd,int process_number)
    :m_listenfd(listenfd),m_process_number(process_number),m_idx(-1),
     m_stop(false)
{
    assert((process_number>0)&&(process_number<=MAX_PROCESS_NUMBER));

    m_sub_process=new process[process_number];
    assert(m_sub_process);

    // 创建process_number个子进程，并建立他们和父进程之间的管道
    for(int i=0;i<process_number;++i){
        int ret=socketpair(PF_UNIX,SOCK_STREAM,0;m_sub_process[i].m_pipefd);
        assert(ret==0);

        m_sub_process[i].m_pid=fork();
        assert(m_sub_process[i].m_pid>=0);
        if(m_sub_process[i].m_pid>0){
            close(m_sub_process[i].m_pipefd[1]);
            continue;
        }
        else{
            close(m_sub_process[i].m_pipefd[0]);
            m_idx=i;
            break;
        }
    }
}

// 统一事件源
template<typename T>
void processpool<T>::setup_sig_pipe(){
    
}
