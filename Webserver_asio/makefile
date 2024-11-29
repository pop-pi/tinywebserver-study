CXX ?= g++

DEBUG ?= 1
ifeq ($(DEBUG), 1)
    CXXFLAGS += -g
else
    CXXFLAGS += -O2

endif

server: main.cpp  ./AsioIOServicePool/asio_io_pool.cpp ./http/http_conn.cpp ./Server/CServer.cpp ./CGImysql/sql_connection_pool.cpp ./log/log.cpp  config.cpp
	$(CXX) -o server  $^ $(CXXFLAGS) -lpthread -lmysqlclient

clean:
	rm  -r server
