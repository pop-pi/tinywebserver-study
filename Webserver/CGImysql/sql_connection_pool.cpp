#include<mysql/mysql.h>
#include<stdio.h>
#include<string>
#include<string.h>
#include<stdlib.h>
#include<list>
#include<pthread.h>
#include<iostream>
#include"sql_connection_pool.h"

connection_pool::connection_pool(){
    this->m_CurConn=0;
    this->m_FreeConn=0;
}

connection_pool* connection_pool::GetInstance(){
    static connection_pool connPool;
    return &connPool;
}

// 构造初始化
void connection_pool::init(string url,string User,string Password,string DBName,int Port,unsigned int MaxConn,  int close_log)
{
    this->m_url=url;
    this->m_User=User;
    this->m_Port=Port;
    this->m_PassWord=Password;
    this->m_DatabaseName=DBName;
    this->m_close_log=close_log;

    for(int i=0;i<MaxConn;++i){
        MYSQL* con=NULL;
        con=mysql_init(con);

        if(con==NULL){
            LOG_ERROR("MySQL Error");
            exit(1);
        }
        con=mysql_real_connect(con,url.c_str(),User.c_str(),Password.c_str(),DBName.c_str(),Port,NULL,0);

        if(con==NULL){
            LOG_ERROR("MySQL Error");
            exit(1);
        }
        connList.push_back(con);
        ++m_FreeConn;
    }
    reserve=sem(m_FreeConn);
    this->m_MaxConn=m_FreeConn;
}

//当有请求时，从数据库连接池中返回一个可用连接，更新使用和空闲连接数
MYSQL* connection_pool:: GetConnection(){
    MYSQL* con=NULL;

    if(0==connList.size()){
        return NULL;
    }

    reserve.wait();
    lock.lock();

    con=connList.front();
    connList.pop_front();

    --m_FreeConn;
    ++m_CurConn;

    lock.unlock();
    return con;
}

//释放当前使用的连接
bool connection_pool::ReleaseConnection(MYSQL* con){
    if(NULL==con)
        return false;
    
    lock.lock();
    connList.push_back(con);
    ++m_FreeConn;
    --m_CurConn;

    lock.unlock();

    reserve.post();
    return true;
}

//销毁数据库连接池
void connection_pool::DestroyPool(){
    lock.lock();
    if(connList.size()>0){
        list<MYSQL*>::iterator it;
        for(it=connList.begin();it!=connList.end();++it){
            MYSQL* con=*it;
            mysql_close(con);
        }
        m_CurConn=0;
        m_FreeConn=0;
        connList.clear();
    }

    lock.unlock();
}

//当前空闲的连接数
int connection_pool::GetFreeConn(){
    return this->m_FreeConn;
}

connection_pool::~connection_pool(){
    DestroyPool();
}

connectionRAII::connectionRAII(MYSQL **SQL,connection_pool* connPool){
    *SQL=connPool->GetConnection();

    conRAII=*SQL;
    poolRAII=connPool;
}

connectionRAII::~connectionRAII(){
    poolRAII->ReleaseConnection(conRAII);
}