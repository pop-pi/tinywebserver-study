#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<stdio.h>
#include<unistd.h>
#include<errno.h>
#include<string.h>
#include<fcntl.h>
#include<stdlib.h>
#include<cassert>
#include<sys/epoll.h>

#include"threadpool.h"
#include<signal.h>
#include"http_conn.h"
#include"lst_timer.h"
#include"sql_connection_pool.h"
#include"log.h"
#include"locker.h"

#define MAX_FD 65536  // 最大的文件描述符个数
#define MAX_EVENT_NUMBER 10000  // 监听的最大事件数量
#define TIMESLOT 5    // 最小超时单位

#define SYNLOG   // 同步写日志
//#define ASYNLOG   // 异步写日志

//#define listenfdET   // 边缘触发非阻塞
#define listenfdLT  // 水平触发阻塞

// 添加文件描述符  这三个函数在http_conn.cpp中定义，改变链接属性
extern void addfd(int epollfd,int fd,bool one_shot);
extern void removefd(int epollfd,int fd);
extern void setnonblocking(int fd);

//设置定时器相关参数
static int pipefd[2];
static sort_timer_lst timer_lst;
static int epollfd=0;

// 信号处理函数
void sig_handler(int sig){
    // 为保证函数的可重入性，保留原来的errno
    int save_errno=errno;
    int msg=sig;
    send(pipefd[1],(char*)&msg,1,0);
    errno=save_errno;
}

// 设置信号函数
void addsig(int sig,void(handler)(int),bool restart=true){
    struct sigaction sa;
    memset(&sa,'\0',sizeof(sa));
    sa.sa_handler=handler;
    if(restart)
        sa.sa_flags|=SA_RESTART;
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig,&sa,NULL)!=-1);
}

//定时处理任务，重新定时以不断触发SIGALRM信号
void timer_handler(){
    timer_lst.tick();
    alarm(TIMESLOT);
}

//定时器回调函数，删除非活动连接在socket上的注册事件，并关闭
void cb_func(client_data* user_data){
    epoll_ctl(epollfd,EPOLL_CTL_DEL,user_data->sockfd,0);
    assert(user_data);
    close(user_data->sockfd);
    http_conn::m_user_count--;
    LOG_INFO("close fd %d",user_data->sockfd);
    Log::get_instance()->flush();
}

void show_error(int connfd,const char* info){
    printf("%s",info);
    send(connfd,info,strlen(info),0);
    close(connfd);
}

int main(int argc,char* argv[]){
#ifdef ASYNLOG
    Log::get_instance()->init("ServerLog",2000,800000,8); // 异步日志模型
#endif

#ifdef SYNLOG
    Log::get_instance()->init("ServerLog",2000,800000,0); // 同步日志模型
#endif


    if(argc<=1){
        printf("Usage: %s ip_address port_number\n",basename(argv[0]));
        return 1;
    }

    int port=atoi(argv[1]);
    addsig(SIGPIPE,SIG_IGN);

    //创建数据库连接池
    connection_pool* connPool=connection_pool::GetInstance();
     connPool->init("localhost", "root", "LJP0916_", "yourdb", 3306, 8);

    threadpool<http_conn> * pool=NULL;
    try{
        pool=new threadpool<http_conn>(connPool);
    }catch(...){
        return 1;
    }

    http_conn* users=new http_conn[MAX_FD];
    assert(users);

    //初始化数据库读取表
    users->initmysql_result(connPool);

    int listenfd=socket(PF_INET,SOCK_STREAM,0);
    assert(listenfd>=0);

    int ret=0;
    struct sockaddr_in address;
    bzero(&address,sizeof(address));
    address.sin_family=AF_INET;
    address.sin_port=htons(port);
    address.sin_addr.s_addr=INADDR_ANY;

    // 端口复用
    int reuse=1;
    setsockopt(listenfd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
    ret=bind(listenfd,(struct sockaddr*)&address,sizeof(address));
    assert(ret>=0);
    ret=listen(listenfd,8);
    assert(ret>=0);

    // 创建epoll对象和事件数组
    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd=epoll_create(1);
    assert(epollfd!=-1);
    // 添加到epoll 对象中
    addfd(epollfd,listenfd,false);
    http_conn::m_epollfd=epollfd;

    //创建管道
    ret=socketpair(PF_UNIX,SOCK_STREAM,0,pipefd);
    assert(ret!=-1);
    setnonblocking(pipefd[1]);
    addfd(epollfd,pipefd[0],false);

    addsig(SIGALRM,sig_handler,false);
    addsig(SIGTERM,sig_handler,false);
    bool stop_server=false;

    client_data* users_timer=new client_data[MAX_FD];
    bool timeout=false;
    alarm(TIMESLOT);

    while (!stop_server)
    {
       int number=epoll_wait(epollfd,events,MAX_EVENT_NUMBER,-1);
       if((number<0)&&(errno!=EINTR)){
            LOG_ERROR("%s","epoll failure");
            break;
        }

        for(int i=0;i<number;++i){
            int sockfd=events[i].data.fd;

            // 处理新到的客户连接
            if(sockfd==listenfd){
                struct sockaddr_in client_addr;
                socklen_t len=sizeof(client_addr);
#ifdef listenfdLT
                int connfd=accept(listenfd,(struct sockaddr*)&client_addr,&len);

                if(connfd<0){
                    LOG_ERROR("%s:errno is:%d","accept error",errno);
                    continue;
                }
                if(http_conn::m_user_count>=MAX_FD){
                    show_error(connfd,"Internal server busy");
                    LOG_ERROR("%s","Internal server busy");
                    continue;
                }

                users[connfd].init(connfd,client_addr);

                //初始化client_data数据
                //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
                users_timer[connfd].address=client_addr;
                users_timer[connfd].sockfd=connfd;
                util_timer* timer=new util_timer;
                timer->user_data=&users_timer[connfd];
                timer->cb_func=cb_func;
                time_t cur=time(NULL);
                timer->expire=cur+3*TIMESLOT;
                users_timer[connfd].timer=timer;
                timer_lst.add_timer(timer);
#endif

#ifdef listenfdET
                while(1){
                    int connfd=accept(listenfd,(struct sockaddr*)&client_addr,&len);

                    if(connfd<0){
                        LOG_ERROR("%s:error is:%d","accept error",errno);
                        break;
                    }

                    // 当前所连接的客户端数量已经到达上限
                    if(http_conn::m_user_count>=MAX_FD){
                        show_error(connfd,"Internal server busy");
                        LOG_ERROR("%s","Internal server busy");
                        break;
                    }

                    users[connfd].init(connfd,client_addr);

                    //初始化client_data数据
                    //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
                    users_timer[connfd].address=client_addr;
                    users_timer[connfd].sockfd=connfd;
                    util_timer* timer=new util_timer;
                    timer->user_data=&users_timer[connfd];
                    timer->cb_func=cb_func;
                    time_t cur=time(NULL);
                    timer->expire=cur+3*TIMESLOT;
                    users_timer[connfd].timer=timer;
                    timer_lst.add_timer(timer);
                }
                continue;
#endif 
            }
            else if(events[i].events&(EPOLLRDHUP|EPOLLHUP|EPOLLERR)){
                util_timer* timer=users_timer[sockfd].timer;
                timer->cb_func(&users_timer[sockfd]);

                if(timer){
                    timer_lst.del_timer(timer);
                }
            }
            // 处理信号
            else if((sockfd==pipefd[0])&&(events[i].events&EPOLLIN)){
                int sig;
                char signals[1024];
                ret=recv(pipefd[0],signals,sizeof(signals),0);

                if(ret==-1)
                    continue;
                else if(ret==0)
                    continue;
                else{
                    for(int i=0;i<ret;++i){
                        switch (signals[i])
                        {
                        case SIGALRM:
                        {
                            /* 用timeout变量标记有定时任务需要处理，
                               但不立即处理定时任务，因为定时任务的优先级不是很高
                               我们优先处理其他更重要的任务
                            */
                            timeout=true;
                            break;
                        }
                        case SIGTERM:
                        {
                            stop_server=true;
                        }
                        }
                    }
                }
            }
            //处理客户连接上接收到的数据
            else if(events[i].events&EPOLLIN){
                util_timer* timer=users_timer[sockfd].timer;
                if(users[sockfd].read_once()){
                    LOG_INFO("deal with the client(%s)",inet_ntoa(users[sockfd].get_address()->sin_addr));
                    Log::get_instance()->flush();
                    //若监测到读事件，将该事件放入请求队列
                    pool->append(users+sockfd);

                    //若有数据传输，则将定时器往后延迟3个单位
                    //并对新的定时器在链表上的位置进行调整
                    if(timer){
                        time_t cur=time(NULL);
                        timer->expire=cur+3*TIMESLOT;
                        LOG_INFO("%s","adjust timer once");
                        Log::get_instance()->flush();
                        timer_lst.adjust_timer(timer);
                    }
                }
                else{
                    timer->cb_func(&users_timer[sockfd]);
                    if(timer){
                        timer_lst.del_timer(timer);
                    }
                }
            }
            // 处理客户连接上需要发送的数据
            else if(events[i].events&EPOLLOUT){
                util_timer* timer=users_timer[sockfd].timer;
                if(users[sockfd].write()){
                    LOG_INFO("send data to the client(%s)",inet_ntoa(users[sockfd].get_address()->sin_addr));
                    Log::get_instance()->flush();

                    //若有数据传输，则将定时器往后延迟3个单位
                    //并对新的定时器在链表上的位置进行调整
                    if(timer){
                        time_t cur=time(NULL);
                        timer->expire=cur+3*TIMESLOT;
                        LOG_INFO("%s","adjust timer once");
                        Log::get_instance()->flush();
                        timer_lst.adjust_timer(timer);
                    }
                }
                else{
                    timer->cb_func(&users_timer[sockfd]);
                    if(timer){
                        timer_lst.del_timer(timer);
                    }
                }
            }
        }

        if(timeout){
            timer_handler();
            timeout=false;
        }     
    }
    
    close(epollfd);
    close(listenfd);
    close(pipefd[1]);
    close(pipefd[0]);
    delete [] users;
    delete [] users_timer;
    delete pool;
    return 0;
}

