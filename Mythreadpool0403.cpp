#include<pthread.h>
#include "Mythreadpool0403.h"

threadpool_t* threadpool_create(int thread_count, int queue_size, int flags){
    threadpool_t* pthpool = new threadpool_t;

    int iRet=0;
    pthpool->threads=new pthread_t[thread_count];
    pthpool->thread_count=thread_count;
    pthpool->queue_size=queue_size;

    pthpool->count=pthpool->HeadIndex=pthpool->TailIndex=0;
    pthpool->shutdown=false;
    pthpool->started=0;

    //初始化任务队列
    pthpool->Taskqueue=new SyPthreadPool_task_t[queue_size];

    //初始化锁，用于锁住本结构体
    iRet=pthread_mutex_init(&pthpool->Pthlock, nullptr);
    if(iRet!=0){
        return nullptr;
    }

    //初始化条件变量
    iRet=pthread_cond_init(&pthpool->Pthcond, nullptr);
    if(iRet!=0){
        return nullptr;
    }

    if(pthpool->threads==nullptr||pthpool->Taskqueue==nullptr){
        //free
    }

    pthread_attr_t Pthattr;
    iRet=pthread_attr_init(&Pthattr);
    if(iRet!=0){
        return nullptr;
    }

    //设置分离
    pthread_attr_setdetachstate(&Pthattr, PTHREAD_CREATE_DETACHED);

    for(__uint32_t i=0;i<pthpool->thread_count;++i){
        int iRet=pthread_create(&pthpool->threads[i], &Pthattr, thread_func, (void*)pthpool);
        if(iRet!=0){
            return nullptr;
        }

        pthpool->started+=1;
    }

    return pthpool;
}

int threadpool_add(threadpool_t* pool, void*(*callback)(void*), void* arg, int flags){
    int _Index=0;
    if(pool==nullptr||callback==nullptr){
        return -1;
    }

    int iRet=0;
    iRet=pthread_mutex_lock(&pool->Pthlock);
    if(iRet!=0){
        return -2;
    }

    _Index=pool->TailIndex+1;
    _Index=(_Index==pool->queue_size)?0:_Index;

    do{
        if(pool->queue_size==pool->count){
            return -3;
        }

        if(pool->shutdown){
			iRet = threadpool_destroy(pool, 0);
			if (iRet == 0)
				return 0;
			else
				return iRet;            
        }

		pool->Taskqueue[pool->TailIndex].startfunc = callback;
		pool->Taskqueue[pool->TailIndex].arg = arg;
		pool->TailIndex = _Index;

		pool->count += 1;

		//发送一个signel信号唤醒线程
		iRet = pthread_cond_signal(&pool->Pthcond);
		if (iRet != 0)
		{
			return -4;
		}

	} while (0);
	pthread_mutex_unlock(&pool->Pthlock);

	return 0;
}