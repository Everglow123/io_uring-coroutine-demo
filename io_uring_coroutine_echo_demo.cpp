#include <fcntl.h>
#include <liburing.h>
#include <liburing/io_uring.h>
#include <netinet/in.h>
#include <sys/cdefs.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <cctype>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>  //.h>
#include <cstring>  //.h>
#include <experimental/coroutine>
#include <iostream>
#include <map>
#include <string_view>
#include <tuple>
static constexpr int READ_SZ = 8192;
static constexpr int QUEUE_DEPTH = 1024;

using std::experimental::coroutine_handle;
struct Connection {
	sockaddr_in clientAddr;
	int fd;
	void *handleAddress;
};
struct Task {
	struct promise_type {
		Task get_return_object() { return coroutine_handle<promise_type>::from_promise(*this); }
		auto initial_suspend() noexcept { return std::experimental::suspend_never {}; }
		auto final_suspend() noexcept { return std::experimental::suspend_never {}; }
		void unhandled_exception() {};
		void return_void() {};
	};
	coroutine_handle<promise_type> handle;
	Task(coroutine_handle<promise_type> h) : handle(h) {};
};
//handle是协程句柄的指针，return code存放accept read write 等系统调用的返回值
struct IoRequest {
	void *handle {nullptr};
	int returnCode {0};
};
struct __attribute__((aligned(16))) AsyncAccept {
	io_uring *ring_;
	int localSockFd_;
	socklen_t socklen_;
	sockaddr_in client_addr_ {0};
	IoRequest req_ {};
	/**
	 * @brief
	 *
	 * @return true 如果返回true,那么await_suspend不会被调用
	 * @return false 
	 */
	bool await_ready() noexcept { return false; };
	void await_suspend(coroutine_handle<Task::promise_type> h) noexcept {
		io_uring_sqe *sqe = io_uring_get_sqe(ring_);
		socklen_t socklen_ = sizeof(client_addr_);
		req_.handle = h.address();
		//往io_uring的提交队列中提交accept的请求
		io_uring_prep_accept(sqe, localSockFd_, (sockaddr *)&client_addr_, &socklen_, 0);
		io_uring_sqe_set_data(sqe, &req_);
		io_uring_submit(ring_);
		//提交完之后协程挂起
	}
	/**
	 * @brief 调用handle.resume()后紧接着执行此函数，此时accept请求已经处理完毕
	 * 
	 * @return Connection 
	 */
	Connection await_resume() noexcept {
		return {.clientAddr = client_addr_, .fd = req_.returnCode, .handleAddress = req_.handle};
	}
};
struct AsyncWrite {
	io_uring *ring_;
	char *data_;
	ssize_t length_;
	int sockFd_;
	IoRequest req_;
	bool await_ready() noexcept { return false; };
	void await_suspend(coroutine_handle<Task::promise_type> h) noexcept {
		req_.handle = h.address();
		io_uring_sqe *sqe = io_uring_get_sqe(ring_);
		io_uring_prep_write(sqe, sockFd_, data_, length_, 0);
		io_uring_sqe_set_data(sqe, &req_);
		io_uring_submit(ring_);
	};
	ssize_t await_resume() { return req_.returnCode; };
};

struct AsyncRead {
	// static constexpr size_t READ_SZ=8192;
	io_uring *ring_;
	int sockFd_;
	char *buff {nullptr};
	IoRequest req_;
	bool await_ready() noexcept { return false; }
	void await_suspend(coroutine_handle<Task::promise_type> h) {
		req_.handle = h.address();
		buff = (char *)malloc(READ_SZ);
		io_uring_sqe *sqe = io_uring_get_sqe(ring_);
		io_uring_prep_read(sqe, sockFd_, buff, READ_SZ, 0);
		io_uring_sqe_set_data(sqe, &req_);
		io_uring_submit(ring_);
	}
	/**
	 * @brief buffer,size,used
	 *
	 * @return std::tuple<char*,ssize_t,ssize_t>
	 */
	std::tuple<char *, ssize_t, ssize_t> await_resume() { return {buff, READ_SZ, req_.returnCode}; }
};
/**
 * @brief 单个协程的运行逻辑
 * 
 * @param localfd 监听的sock的fd
 * @param ring io_uring
 * @return Task 
 */
Task Echo(int localfd, io_uring *ring) {
	/**
	 * @brief 主动发起异步接受连接
	 * 
	 */
	Connection conn = co_await AsyncAccept {.ring_ = ring, .localSockFd_ = localfd};

	printf("新请求:\n");
	while (true) {
		//异步读取客户端发的消息，此处的buff size used元组是 AsyncRead await_resume的返回值
		auto [buff, size, used] = co_await AsyncRead {.ring_ = ring, .sockFd_ = conn.fd};
		//used==0意味着连接被关闭
		if (used == 0) {
			free(buff);
			break;
		}
		std::cout << std::string_view {buff, (size_t)used} << std::endl;
		//异步写入
		auto written =
		    co_await AsyncWrite {.ring_ = ring, .data_ = buff, .length_ = used, .sockFd_ = conn.fd};
		if (written == 0) {
			free(buff);
			break;
		}
		free(buff);
	}
	co_return;
}
void fatal_error(const char *syscall) {
	perror(syscall);
	exit(1);
}

int setup_listening_socket(int port) {
	int sock;
	struct sockaddr_in srv_addr;

	sock = socket(PF_INET, SOCK_STREAM, 0);
	if (sock == -1) fatal_error("socket()");

	int enable = 1;
	if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0)
		fatal_error("setsockopt(SO_REUSEADDR)");

	memset(&srv_addr, 0, sizeof(srv_addr));
	srv_addr.sin_family = AF_INET;
	srv_addr.sin_port = htons(port);
	srv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(sock, (const struct sockaddr *)&srv_addr, sizeof(srv_addr)) < 0) fatal_error("bind()");

	if (listen(sock, 10) < 0) fatal_error("listen()");

	return (sock);
}
class EchoServer {
	int port_;
	int sockfd_;
	int coroutineSize;
	io_uring ring_;
	// std::map<coroutine_handle<Task::promise_type>, Task> coroutineMap_;

   public:
	EchoServer(int port, int coroutine_size) : port_(port), coroutineSize(coroutine_size) {};
	void run() {
		sockfd_ = setup_listening_socket(port_);
		io_uring_queue_init(QUEUE_DEPTH, &ring_, 0);
		for (int i = 0; i < coroutineSize; ++i) {
			auto coroutine = Echo(sockfd_, &ring_);
		}
		struct io_uring_cqe *cqe;
		while (true) {
			int ret = io_uring_wait_cqe(&ring_, &cqe);
			if (ret < 0) fatal_error("io_uring_wait_cqe");
			auto *req = (IoRequest *)cqe->user_data;
			req->returnCode = cqe->res;
			auto handle = coroutine_handle<Task::promise_type>::from_address(req->handle);
			io_uring_cqe_seen(&ring_, cqe);
			handle.resume();
			if (!handle) {
				auto coroutine = Echo(port_, &ring_);
				// coroutineMap_.insert({coroutine.handle, std::move(coroutine)});
			}
		}
	}
};
int main() {
	// io_uring_queue_init(QUEUE_DEPTH, &ring, 0);
	// server_loop(server_socket);
	EchoServer echoServer(8888, 10);
	echoServer.run();
	return 0;
}