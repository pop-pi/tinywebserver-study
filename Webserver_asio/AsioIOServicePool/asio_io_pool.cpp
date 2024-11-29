#include"asio_io_pool.h"
#include<iostream>
using std::cout,std::endl;

AsioIOServicePool::AsioIOServicePool(std::size_t size):_ioServices(size),
	_works(size),_nextIOService(0){
	for (std::size_t i = 0; i < size; ++i) {
		_works[i] = std::unique_ptr<Work>(new Work(_ioServices[i]));
	}

	// 遍历多个ioservice,创建多个线程，每个线程内部启动io_service
	for (std::size_t i = 0; i < _ioServices.size(); ++i) {
		_threads.emplace_back([this, i]() {
			_ioServices[i].run();
			});
	}
}

AsioIOServicePool::~AsioIOServicePool() {
	cout << "AsioIOServicePool destruct" << endl;
}

boost::asio::io_context& AsioIOServicePool::GetIOService() {
	auto& service = _ioServices[_nextIOService++];
	if (_nextIOService == _ioServices.size()) {
		_nextIOService = 0;
	}
	return service;
}

void AsioIOServicePool::Stop() {
	for (std::size_t i = 0; i < _works.size(); ++i) {
		_works[i].reset();
	}

	for (std::size_t i = 0; i < _threads.size(); ++i) {
		_threads[i].join();
	}
}

AsioIOServicePool& AsioIOServicePool::GetInstance() {
	static AsioIOServicePool instance(4);
	return instance;
}