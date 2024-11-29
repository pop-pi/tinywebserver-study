#ifndef LST_TIMER
#define LST_TIMER

#include<time.h>
#include"log.h"
#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>


class util_timer;
struct client_data{
    sockaddr_in address;
    int sockfd;
    util_timer* timer;
};

class util_timer{
    
public:
    util_timer():prev(NULL),next(NULL){}
public:
    time_t expire;
    void (*cb_func)(client_data*);
    client_data* user_data;
    util_timer* prev;
    util_timer* next;
};

class sort_timer_lst{
public:
    sort_timer_lst():head(NULL),tail(NULL){}
    ~sort_timer_lst(){
        util_timer* tmp=head;
        while(tmp){
            head=tmp->next;
            delete tmp;
            tmp=head;
        }
    }
    // 将目标定时器timer添加到链表中
    void add_timer(util_timer* timer){
        if(!timer){
            return;
        }
        if(!head){
            head=tail=timer;
            return;
        }
        /*
        如果目标定时器的超时时间小于当前链表中所有定时器的超时时间，
        则把该定时器插入链表头部，作为链表新的头节点。
        否则就需要调用重载函数add_timer(util_timer* timer,util_timer* lst_head),
        把它插入链表中合适的位置，以保证链表的升序特性。
        */
        if(timer->expire<head->expire){
            timer->next=head;
            head->prev=timer;
            head=timer;
            return;
        }
        add_timer(timer, head);
    }

    /*
    当某个定时任务发生变化时，调整对应的定时器在链表中的位置。
    这个函数只考虑被调整的定时器的超时时间延长的情况，即该定时器需要往链表的尾部移动
    */
    void adjust_timer(util_timer* timer){
        if(!timer){
            return;
        }
        util_timer *tmp=timer->next;
        if(!tmp||(timer->expire<tmp->expire))
        {
            return;
        }
        /* 如果目标定时器是链表的头节点，则将该定时器从链表中取出并重新插入链表 */
        if(timer==head){
            head=head->next;
            head->prev=NULL;
            timer->next=NULL;
            add_timer(timer,head);
        }
        /* 如果目标定时器不是链表的头节点，则将该定时器从链表中取出，
           然后插入其原来所在位置之后的部分链表中
        */
        else{
            timer->prev->next=timer->next;
            timer->next->prev=timer->prev;
            add_timer(timer,timer->next);
        }
    }

    void del_timer(util_timer* timer){
        if(!timer){
            return;
        }
        if((timer==head)&&(timer==tail)){
            delete timer;
            head=NULL;
            tail=NULL;
            return ;
        }
        if(timer==head){
            head=head->next;
            head->prev=NULL;
            delete timer;
            return;
        }
        if(timer==tail){
            tail=tail->prev;
            tail->next=NULL;
            delete timer;
            return;
        }
        timer->prev->next=timer->next;
        timer->next->prev=timer->prev;
        delete timer;        
    }
    /* SIGALRM信号每次被触发就在其信号处理函数（如果使用统一事件源，则是主函数）
       中执行一次tick函数，以处理链表上到期的任务
    */
    void tick(){
        if(!head){
            return;
        }
        LOG_INFO("%s","timer tick");
        Log::get_instance()->flush();
        time_t cur=time(NULL);  // 获得系统当前的时间
        util_timer* tmp=head;
        // 从头结点开始依次处理每个定时器，直到遇到一个尚未到期的定时器
        while(tmp){
            if(cur<tmp->expire)
                break;
            tmp->cb_func(tmp->user_data);
            head=tmp->next;
            if(head){
                head->prev=NULL;
            }
            delete tmp;
            tmp=head;
        }
    }

private:
    void add_timer(util_timer* timer,util_timer* lst_head){
        util_timer* prev=lst_head;
        util_timer* tmp=prev->next;
        while(tmp){
            if(timer->expire<tmp->expire){
                prev->next=timer;
                timer->next=tmp;
                tmp->prev=timer;
                timer->prev=prev;
                break;
            }
            prev=tmp;
            tmp=tmp->next;
        }
        if(!tmp){
            prev->next=timer;
            timer->prev=prev;
            timer->next=NULL;
            tail=timer;
        }
    }
private:
    util_timer* head;
    util_timer* tail;
};

#endif