#ifndef HTTP_CONN_H
#define HTTP_CONN_H

#include<unistd.h>
#include<stdlib.h>
#include<stdio.h>
#include<sys/epoll.h>
#include<sys/types.h>
#include<fcntl.h>
#include<sys/socket.h>
#include<string.h>
#include<arpa/inet.h>
#include<assert.h>
#include<sys/stat.h>
#include<pthread.h>
#include<sys/mman.h>
#include<stdarg.h>
#include"../lock/locker.h"
#include"../log/log.h"
#include<sys/uio.h>
#include<errno.h>
#include"../CGImysql/sql_connection_pool.h"
#include<map>
#include<boost/asio.hpp>
#include<boost/uuid/uuid_io.hpp>
#include<boost/uuid/uuid_generators.hpp>

using namespace boost::asio;

class CServer;


class http_conn:public std::enable_shared_from_this<http_conn>{
    public:
        static const int FILENAME_LEN=200;  // 文件名的最大长度
        static const int READ_BUFFER_SIZE=2048; // 读缓冲区的大小
        static const int WRITE_BUFFER_SIZE=2048; // 写缓冲区的大小

        // HTTP请求方法，这里只支持GET
        enum METHOD {GET = 0, POST, HEAD, PUT, DELETE, TRACE, OPTIONS, CONNECT, PATH};
        
        /*
            解析客户端请求时，主状态机的状态
            CHECK_STATE_REQUESTLINE:当前正在分析请求行
            CHECK_STATE_HEADER:当前正在分析头部字段
            CHECK_STATE_CONTENT:当前正在解析请求体
        */
        enum CHECK_STATE { CHECK_STATE_REQUESTLINE = 0, CHECK_STATE_HEADER, CHECK_STATE_CONTENT };
        
        /*
            服务器处理HTTP请求的可能结果，报文解析的结果
            NO_REQUEST          :   请求不完整，需要继续读取客户数据
            GET_REQUEST         :   表示获得了一个完成的客户请求
            BAD_REQUEST         :   表示客户请求语法错误
            NO_RESOURCE         :   表示服务器没有资源
            FORBIDDEN_REQUEST   :   表示客户对资源没有足够的访问权限
            FILE_REQUEST        :   文件请求,获取文件成功
            INTERNAL_ERROR      :   表示服务器内部错误
            CLOSED_CONNECTION   :   表示客户端已经关闭连接了
        */
        enum HTTP_CODE { NO_REQUEST, GET_REQUEST, BAD_REQUEST, NO_RESOURCE, FORBIDDEN_REQUEST, FILE_REQUEST, INTERNAL_ERROR, CLOSED_CONNECTION };
        
        // 从状态机的三种可能状态，即行的读取状态，分别表示
        // 1.读取到一个完整的行 2.行出错 3.行数据尚且不完整
        enum LINE_STATUS { LINE_OK = 0, LINE_BAD, LINE_OPEN };
    public:
        http_conn(boost::asio::io_context& io_context,CServer* server,char*,int,string user,string passwd,string sqlname);
        ~http_conn(){}
    public:
        void init(boost::asio::io_context& io_context,char*,int,string user,string passwd,string sqlname); // 初始化新接受的连接
        void close_conn(bool real_close=true); // 关闭连接
        void process(const boost::system::error_code& ec,std::size_t); // 处理客户端请求
        void start();
        bool read_once(); // 非阻塞读
        void initmysql_result(connection_pool* connPool);
        boost::asio::ip::tcp::socket& GetSocket(){return m_sockfd;}
        std::string& GetUuid(){return m_uuid;}
        int timer_flag;
        int improv;
    private:
        void init(); //初始化连接

        HTTP_CODE process_read(); // 解析HTTP请求
        bool process_write(HTTP_CODE ret);  // 填充HTTP应答
        void start_timer();

        // 下面这组函数被process_read调用以分析HTTP请求
        HTTP_CODE parse_request_line(char* text);
        HTTP_CODE parse_headers(char* text);
        HTTP_CODE parse_content(char* text);
        HTTP_CODE do_request();
        char* get_line() {return &m_read_buf[m_start_line];}
        LINE_STATUS parse_line();

        // 下面这组函数被process_write调用以填充HTTP应答
        void unmap();
        bool add_response(const char* format,...);
        bool add_content(const char* content);
        bool add_content_type();
        bool add_status_line(int status,const char* title);
        bool add_headers(int content_length);
        bool add_content_length(int content_length);
        bool add_linger();
        bool add_blank_line();

        void async_write_chunk();

    public: 
        static int m_user_count;
        MYSQL *mysql;
        int m_state;

    private:
        boost::asio::ip::tcp::socket m_sockfd;   // 该HTTP连接服务器的socket
        std::string m_uuid;   
        CServer* m_server;
        boost::asio::deadline_timer m_timer;
        

        std::string m_read_buf;  // 读缓冲区
        int m_read_idx; // 标识读缓冲区中已经读入客户端数据的最后一个字节的下一个位置
        int m_checked_idx; // 当前正在分析的字符在读缓冲区中的位置
        int m_start_line;  // 当前正在解析的行的起始位置
        char m_write_buf[WRITE_BUFFER_SIZE];  // 写缓冲区
        int m_write_idx;  // 写缓冲区中待发送的字节数

        CHECK_STATE m_check_state;   // 主状态机当前所处的状态
        METHOD m_method;            // 请求方法

        char m_real_file[FILENAME_LEN]; // 客户请求的目标文件的完整路径，其内容等于doc_root+m_url,doc_root是网站根目录
        char* m_url; // 客户请求的目标文件的文件名
        char* m_version;  // HTTP协议版本号，我们仅支持HTTP1.1
        char* m_host;  // 主机号
        int m_content_length;  // HTTP 请求的消息总长度
        bool m_linger;   // HTTP请求是否要求保持连接
            
        char* m_file_address; //客户请求的目标文件被mmap到内存中的起始位置
        struct stat m_file_stat;  // 目标文件的状态，可以通过它来判断文件是否存在、是否为目录、是否可读，并获取文件大小等信息
        struct iovec m_iv[2];    // 我们将采用writev来执行写操作
        int m_iv_count;   // 表示被写内存块的数量

        int cgi;            //是否启用的POST
        char* m_string;     //存储请求头数据

        int bytes_to_send;  // 将要发送的数据的字节数
        int bytes_have_send;    // 已经发送的字节数

        char* doc_root;
        map<string,string> m_users;
        int m_close_log;

        char sql_user[100];
        char sql_passwd[100];
        char sql_name[100];
        const static int TIMESLOT=5;
};      

#endif