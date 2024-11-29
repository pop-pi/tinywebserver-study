#include "http_conn.h"

#include <mysql/mysql.h>
#include <fstream>

//定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

locker m_lock;
map<string, string> users;

void http_conn::initmysql_result(connection_pool *connPool)
{
    //先从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    //在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        printf("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}


int http_conn::m_user_count = 0;

//关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close)
{
    if (real_close && m_sockfd.is_open() )
    {
        printf("close connection\n");
        m_sockfd.close();
        m_timer.cancel();
        m_user_count--;
    }
}

http_conn::http_conn(boost::asio::io_context& io_context,CServer* server,char* root,
                    int close_log,string user,string passwd,string sqlname):
                    m_sockfd(io_context),m_server(server),doc_root(root),m_close_log(close_log),m_timer(io_context, boost::posix_time::seconds(15))
{
    m_user_count++;
    boost::uuids::uuid a_uuid=boost::uuids::random_generator()();
    m_uuid=boost::uuids::to_string(a_uuid);
    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    init();
}

//初始化连接,外部调用初始化套接字地址
void http_conn::init(boost::asio::io_context& io_context, char *root,
                     int close_log, string user, string passwd, string sqlname)
{
    m_user_count++;
    m_sockfd=boost::asio::ip::tcp::socket(io_context);
    //当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
    doc_root = root;
    m_close_log = close_log;

    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    init();
}

//初始化新接受的连接
//check_state默认为分析请求行状态
void http_conn::init()
{
    printf("init()\n");
    // mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    m_state = 0;
    timer_flag = 0;
    improv = 0;
    

    m_read_buf="";
    bzero(m_write_buf,WRITE_BUFFER_SIZE);
    bzero(m_real_file,FILENAME_LEN);
}

//从状态机，用于分析出一行内容
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line() {
    size_t pos = m_read_buf.find("\r\n", m_checked_idx);
    if (pos != std::string::npos) {
        m_read_buf[pos] = '\0';
        m_read_buf[pos + 1] = '\0';
        m_checked_idx = pos + 2;
        return LINE_OK;
    }

    pos = m_read_buf.find('\n', m_checked_idx);
    if (pos != std::string::npos) {
        if (pos > 0 && m_read_buf[pos - 1] == '\r') {
            m_read_buf[pos - 1] = '\0';
        }
        m_read_buf[pos] = '\0';
        m_checked_idx = pos + 1;
        return LINE_OK;
    }

    return LINE_OPEN;
}



//循环读取客户数据，直到无数据可读或对方关闭连接
//非阻塞ET工作模式下，需要一次性将数据读完
bool http_conn::read_once()
{
    auto self(shared_from_this());
    boost::asio::async_read_until(m_sockfd, 
        boost::asio::dynamic_buffer(m_read_buf), 
        "\r\n\r\n", 
        [this, self](boost::system::error_code ec, std::size_t bytes_transferred) {
            if (!ec) {
                m_read_idx+=bytes_transferred;
                // 成功读取，判断是否读取到完整的请求头部
                if (m_read_buf.find("\r\n\r\n") == std::string::npos) {
                    // 如果未读取到完整的请求头部，继续读取
                    read_once();
                } else {
                    // 处理完整的数据
                    process(ec, bytes_transferred);
                }
            } else {
                // 处理读取错误
                close_conn(true);
            }
        });
    return true;
}
//解析http请求行，获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    printf("start parse request line!\n");
    m_url = strpbrk(text, " \t");
    if (!m_url)
    {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';
    char *method = text;
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    else
        return BAD_REQUEST;
    m_url += strspn(m_url, " \t");
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    if (strcasecmp(m_version, "HTTP/1.1") != 0 && strcasecmp(m_version, "HTTP/1.0")!=0)
        return BAD_REQUEST;
    if(strcmp(m_version,"HTTP/1.0")==0){
        m_linger=true;
    }
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }

    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }

    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;
    //当url为/时，显示判断界面
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

//解析http请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    printf("start parse header!\n");
    if (text[0] == '\0')
    {
        if (m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;
        }
    }
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else
    {
        printf("oop!unknow header: %s", text);
    }
    return NO_REQUEST;
}

//判断http请求是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    if (m_read_idx >=  m_checked_idx)
    {
        printf("m_read_idx >= (m_content_length + m_checked_idx)\n");
        text[m_content_length] = '\0';
        //POST请求中最后为输入的用户名和密码
        m_string = text;
        return GET_REQUEST;
    }
    printf("m_read_idx(%d) < (m_content_length + m_checked_idx(%d))\n",m_read_idx,m_content_length);
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        text = get_line();
        m_start_line = m_checked_idx;
        printf("now m_check_state is:%d\n",m_check_state);
        printf("one line:%s\n", text);
        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE:
        {
            ret = parse_request_line(text);
            printf("CHECK_STATE_REQUESTLINE return :%d\n",ret);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            break;
        }
        case CHECK_STATE_HEADER:
        {
            ret = parse_headers(text);
            printf("CHECK_STATE_REQUESTLINE return :%d\n",ret);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            else if (ret == GET_REQUEST)
            {
                return do_request();
            }
            break;
        }
        case CHECK_STATE_CONTENT:
        {
            ret = parse_content(text);
            printf("CHECK_STATE_CONTENT return :%d\n",ret);
            if (ret == GET_REQUEST)
                return do_request();
            line_status = LINE_OPEN;
            break;
        }
        default:
            return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::do_request()
{
    printf("do request!\n");
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    printf("m_real_file:%s\n",m_real_file);
    printf("m_url:%s\n", m_url);
    const char *p = strrchr(m_url, '/');

    //处理cgi
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {

        //根据标志判断是登录检测还是注册检测
        char flag = m_url[1];

        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);

        //将用户名和密码提取出来
        //user=123&passwd=123
        char name[100], password[100];
        int i;
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

        if (*(p + 1) == '3')
        {
            //如果是注册，先检测数据库中是否有重名的
            //没有重名的，进行增加数据
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            if (users.find(name) == users.end())
            {
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();

                if (!res)
                    strcpy(m_url, "/log.html");
                else
                    strcpy(m_url, "/registerError.html");
            }
            else
                strcpy(m_url, "/registerError.html");
        }
        //如果是登录，直接判断
        //若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
        else if (*(p + 1) == '2')
        {
            if (users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/welcome.html");
            else
                strcpy(m_url, "/logError.html");
        }
    }

    if (*(p + 1) == '0')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '1')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '5')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '6')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '7')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;

    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;

    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;

    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}
void http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

bool http_conn::add_response(const char *format, ...)
{
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;
    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);

    printf("request:%s\n", m_write_buf);

    return true;
}
bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}
bool http_conn::add_headers(int content_len)
{
    return add_content_length(content_len) && add_linger() &&
           add_blank_line();
}
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}
bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}
bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}
bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}
bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
    case INTERNAL_ERROR:
    {
        printf("internal error!\n");
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        if (!add_content(error_500_form))
            return false;
        break;
    }
    case BAD_REQUEST:
    {
        printf("bad request!\n");
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
    }
    case FORBIDDEN_REQUEST:
    {
        printf("forbidden request!\n");
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
    }
    case FILE_REQUEST:
    {
        printf("file request!\n");
        add_status_line(200, ok_200_title);
        printf("m_file_stat.st_size:%ld\n",m_file_stat.st_size);
        if (m_file_stat.st_size != 0)
        {
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            bytes_to_send = m_write_idx + m_file_stat.st_size;
            return true;
        }
        else
        {
            const char *ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if (!add_content(ok_string))
                return false;
        }
    }
    default:
        return false;
    }
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

void http_conn::start_timer(){
    m_timer.expires_from_now(boost::posix_time::seconds(3*TIMESLOT));
    auto self(shared_from_this());
    m_timer.async_wait([this, self](const boost::system::error_code& ec) {
            if (!ec) {
                std::cout << "Timeout, closing socket." << std::endl;
                close_conn(true);
            }
        });
}


void http_conn::async_write_chunk() {
    if (bytes_to_send == 0) {
        
        init();
        start();
        return;
    }

    auto self(shared_from_this());

    // 选择需要写的缓冲区
    boost::asio::const_buffer buffer = (bytes_have_send >= m_iv[0].iov_len) ?
        boost::asio::buffer(m_iv[1].iov_base, m_iv[1].iov_len) :
        boost::asio::buffer(m_iv[0].iov_base, m_iv[0].iov_len);

    // 开始异步写操作
    boost::asio::async_write(m_sockfd, buffer,
        [this](boost::system::error_code ec, std::size_t bytes_transferred) {
            if (!ec) {
                // 更新已发送和待发送字节数
                bytes_have_send += bytes_transferred;
                bytes_to_send -= bytes_transferred;

                // 更新iov指针
                if (bytes_have_send >= m_iv[0].iov_len) {
                    m_iv[0].iov_len = 0;
                    m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
                    m_iv[1].iov_len = bytes_to_send;
                } else {
                    m_iv[0].iov_base = m_write_buf + bytes_have_send;
                    m_iv[0].iov_len -= bytes_transferred;
                }

                // 继续写入剩余的数据
                if (bytes_to_send > 0) {
                    async_write_chunk();
                } else {
                    // 所有数据都已发送完成
                    unmap();
                    if (m_linger) {
                        init();
                        start();
                    } else {
                        close_conn(true);
                    }
                }
            } else {
                // 处理写入错误
                close_conn(true);
            }
        }
    );
}


void http_conn::process(const boost::system::error_code& ec,std::size_t)
{
    if(!ec)
    {
        printf("process\n");
        std::cout<<"read_buff:\n" << m_read_buf<<std::endl;
        HTTP_CODE read_ret = process_read();
        if (read_ret == NO_REQUEST)
        {
            cout<<"read return is NO_REQUEST\n";
            return;
        }
        std::cout<<"read return is "<<read_ret<<std::endl;
        bool write_ret = process_write(read_ret);
        if (!write_ret)
        {
            cout<<"write return is false\n";
            close_conn(true);
        }
        async_write_chunk();
        m_read_buf.clear();
        bzero(m_write_buf,WRITE_BUFFER_SIZE);
        bzero(m_real_file,FILENAME_LEN);
    }
    else{
        std::cerr<<"Error on receive: "<<ec.message()<<std::endl;
        close_conn(true);
    }
}

void http_conn::start(){
    read_once();
    start_timer();
}