#include "requestData.h"
#include "epoll.h"
#include "threadpool.h"
#include "util.h"

#include <sys/epoll.h>
#include <queue>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <cstdlib>
#include <iostream>
#include <vector>
#include <unistd.h>

using namespace std;

const int THREADPOOL_THREAD_NUM = 4;
const int QUEUE_SIZE = 65535;

const int PORT = 8888;
const int ASK_STATIC_FILE = 1;
const int ASK_IMAGE_STITCH = 2;

const string PATH = "/";

const int TIMER_TIME_OUT = 500;


extern pthread_mutex_t qlock; //defination in requestData.cpp
extern struct epoll_event* events;  //defination in epoll.cpp
void acceptConnection(int listen_fd, int epoll_fd, const string &path);

extern priority_queue<mytimer*, deque<mytimer*>, timerCmp> myTimerQueue;

int socket_bind_listen(int port)
{
    // 检查port值，取正确区间范围
    if (port < 1024 || port > 65535)
        return -1;

    // 创建socket(IPv4 + TCP)，返回监听描述符
    int listen_fd = 0;
    if((listen_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1)
        return -1;


    // 消除bind时"Address already in use"错误
    /*comments from dyj:
     *为什么会存在address already in use?
     *在服务端中止之后，会有一个TIME_WAIT状态，再次打开会出现
     *服务器端可以尽可能的使用REUSEADDR套接字选项，这样就可以使得不必等待TIME_WAIT状态
     *TIME_WAIT状态还是存在的，但是不影响我们重新启动服务器
     * */
    /*comments from dyj:
     *For Boolean options, 0 indicates that the option is disabled
     *and 1 indicates that the option is enabled.
     * */

    /*comments from stack overflow:
     *etsockopt works for setting many different types of values. While many
     *(possibly even most) are of type int, others are structs of varying
     *sizes. Since the only thing that setsockopt gets is a pointer, it has no
     *way of knowing how large the struct is and that's why you need to tell
     *it.
     * */
    int optval = 1;
    if(setsockopt(listen_fd, SOL_SOCKET,  SO_REUSEADDR, &optval, sizeof(optval)) == -1)
        return -1;

    // 设置服务器IP和Port，和监听描述绑定
    struct sockaddr_in server_addr;
    bzero((char*)&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons((unsigned short)port);
    if(bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1)
        return -1;

    // 无效监听描述符
    if(listen_fd == -1)
    {
        // close(listen_fd);
        return -1;
    }

    // 开始监听，最大等待队列长为LISTENQ
    if(listen(listen_fd, LISTENQ) == -1)
        return -1;

    return listen_fd;
}

void myHandler(void *args)
{
    requestData *req_data = (requestData*)args;
    req_data->handleRequest();
}

void acceptConnection(int listen_fd, int epoll_fd, const string &path)
{
    struct sockaddr_in client_addr;
    memset(&client_addr, 0, sizeof(struct sockaddr_in));
    socklen_t client_addr_len = 0;
    int accept_fd = 0;
    while((accept_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_addr_len)) > 0)
    {
        /*
        // TCP的保活机制默认是关闭的
        int optval = 0;
        socklen_t len_optval = 4;
        getsockopt(accept_fd, SOL_SOCKET,  SO_KEEPALIVE, &optval, &len_optval);
        cout << "optval ==" << optval << endl;
        */
        
        // 设为非阻塞模式
        int ret = setSocketNonBlocking(accept_fd);
        if (ret < 0)
        {
            perror("Set non block failed!");
            return;
        }

        requestData *req_info = new requestData(epoll_fd, accept_fd, path);

        // 文件描述符可以读，快照（EPOLLONESHOT)模式和边缘触发(Edge Triggered)模式，保证一个socket连接在任一时刻只被一个线程处理
        __uint32_t _epo_event = EPOLLIN | EPOLLET | EPOLLONESHOT;
        epoll_add(epoll_fd, accept_fd, static_cast<void*>(req_info), _epo_event);
        
        // 新增时间信息
        mytimer *mtimer = new mytimer(req_info, TIMER_TIME_OUT);
        req_info->addTimer(mtimer);
        pthread_mutex_lock(&qlock);
        myTimerQueue.push(mtimer);
        pthread_mutex_unlock(&qlock);
    }
}
// 分发处理函数
void handle_events(int epoll_fd, int listen_fd, struct epoll_event* events, int events_num, const string &path, threadpool_t* tp)
{
    for(int i = 0; i < events_num; i++)
    {
        printf("分配任务了。。。\n");
        // 获取有事件产生的描述符, events[i].data是union
        requestData* request = (requestData*)(events[i].data.ptr);
        int fd = request->getFd();
        cout<<"事件描述符！"<<fd<<endl;
        // 有事件发生的描述符为监听描述符
        if(fd == listen_fd)
        {
            acceptConnection(listen_fd, epoll_fd, path);
        }
        else
        {
            // 排除错误事件
            if ((events[i].events & EPOLLERR) || (events[i].events & EPOLLHUP)
                || (!(events[i].events & EPOLLIN)))
            {
                printf("error event\n");
                delete request;
                continue;
            }

            // 将请求任务加入到线程池中
            // 加入线程池之前将Timer和request分离
            request->seperateTimer();
            int rc = threadpool_add(tp, myHandler, events[i].data.ptr, 0);

            if(rc<0){
                perror("threadpool_add failure!\n");
            }
        }
    }
}

/* 处理逻辑是这样的~
因为(1) 优先队列不支持随机访问
(2) 即使支持，随机删除某节点后破坏了堆的结构，需要重新更新堆结构。
所以对于被置为deleted的时间节点，会延迟到它(1)超时 或 (2)它前面的节点都被删除时，它才会被删除。
一个点被置为deleted,它最迟会在TIMER_TIME_OUT时间后被删除。
这样做有两个好处：
(1) 第一个好处是不需要遍历优先队列，省时。
(2) 第二个好处是给超时时间一个容忍的时间，就是设定的超时时间是删除的下限(并不是一到超时时间就立即删除)，如果监听的请求在超时后的下一次请求中又一次出现了，
就不用再重新申请requestData节点了，这样可以继续重复利用前面的requestData，减少了一次delete和一次new的时间。
*/

void handle_expired_event()
{
    pthread_mutex_lock(&qlock);
    while (!myTimerQueue.empty())
    {
        mytimer *ptimer_now = myTimerQueue.top();
        if (ptimer_now->isDeleted())
        {
            myTimerQueue.pop();
            delete ptimer_now;
        }
        else if (ptimer_now->isvalid() == false)
        {
            myTimerQueue.pop();
            delete ptimer_now;
        }
        else
        {
            break;
        }
    }
    pthread_mutex_unlock(&qlock);
}

int main()
{
    /*comments from stackoverflow
     *The process received a SIGPIPE, the default behavior for this signal is to end the process
     *A SIGPIPE is sent to a process if it tried to write to a socket that had been shutdown for writing or isn't connected
     *To avoid that the program ends in this case, ignore SIGPIPE.
    */
    handle_for_sigpipe();

    /*epoll init*/
    int epoll_fd = epoll_init();
    if (epoll_fd < 0)
    {
        perror("epoll init failed");
        return 1;
    }
    threadpool_t *threadpool = threadpool_create(THREADPOOL_THREAD_NUM, QUEUE_SIZE, 0);
    int listen_fd = socket_bind_listen(PORT);
    if (listen_fd < 0) 
    {
        perror("socket bind failed");
        return 1;
    }
    /*comments from dyj:
    * 设置listen_fd非阻塞
    * 为什么需要这样设置？
    * epoll当有通知来的时候，若是listen_fd的数据，那么显然阻塞与不阻塞都无所谓。
    * 但是，如果出现这样的一种情况，当accept结束之前，客户端结束连接，那么accept就会阻塞，
    * 影响服务器效率
    * */
    if (setSocketNonBlocking(listen_fd) < 0)
    {
        perror("set socket non block failed");
        return 1;
    }

     /*comments from dyj:
     * 设置为读事件和ET工作模式
     * */
    __uint32_t event = EPOLLIN | EPOLLET;
    requestData *req = new requestData();

    /*comments from dyj:
    * 将req和listen_fd联系起来
    */
    req->setFd(listen_fd);
    epoll_add(epoll_fd, listen_fd, static_cast<void*>(req), event);
    while (true)
    {
        int events_num = my_epoll_wait(epoll_fd, events, MAXEVENTS, -1);
      
        if (events_num == 0)
            continue;
        printf("%d\n", events_num);
        handle_events(epoll_fd, listen_fd, events, events_num, PATH, threadpool);

        handle_expired_event();
    }
    return 0;
}