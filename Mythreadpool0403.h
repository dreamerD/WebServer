#include<pthread.h>

typedef struct{
    void* (*startfunc)(void*);
    void* arg;
} SyPthreadPool_task_t;

struct threadpool_t{
    pthread_mutex_t Pthlock;//内部工作的互斥锁
    pthread_cond_t Pthcond;//线程间通知的条件变量
    pthread_t* threads;//线段数组，这里用指针来表示

    SyPthreadPool_task_t* Taskqueue;//存储任务的数组，即任务队列
    int queue_size;//任务队列最大数量
    int count;//当前任务数量
    int HeadIndex;//任务队列头
    int TailIndex;//任务队列尾

    int thread_count;//线程数量

    bool shutdown;//标识线程池是否关闭
    int started;//开启的线程数
};

//创建一个线程池,设定线程池线程数,任务队列数,以及一个你自定义个flags占位
static threadpool_t* threadpool_create(int thread_count, int queue_size, int flags);

//将一个任务加入到线程池中
//指定回调函数不一定是void*(*callback)(void*)返回值,参数可以为任何
//指定回调参数
static	int threadpool_add(threadpool_t* pool, void* (*callback)(void*), void* arg, int flags);

//指定销毁的线程池
static  int threadpool_destroy(threadpool_t* pool, int flags);

static void* thread_func(void *){
    //...
}