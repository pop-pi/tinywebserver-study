#include<iostream>
#include"./Server/CServer.h"
#include"./http/http_conn.h"
#include"./AsioIOServicePool/asio_io_pool.h"
#include"./config.h"


int main(int argc,char* argv[]){
    string user="root";
    string passwd="LJP0916_";
    string databasename="yourdb";

    Config config;
    try{
        auto& pool=AsioIOServicePool::GetInstance();
        boost::asio::io_context io_context;
        boost::asio::signal_set signals(io_context,SIGINT,SIGTERM);
        signals.async_wait([&io_context,&pool](auto,auto){
            io_context.stop();
            pool.Stop();
        });
        CServer s(io_context,config.PORT,user,passwd,databasename,config.LOGWrite,config.sql_num,config.close_log);
        io_context.run();
    }
    catch(std::exception& e){
        std::cerr<<"Exception: "<<e.what()<<std::endl;
    }
}