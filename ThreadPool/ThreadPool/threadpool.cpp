#include "threadpool.h"

#include<functional>
#include<thread>
#include<iostream>
#include<string>

const int TASK_MAX_THRESHHOLD = INT32_MAX;
const int THREAD_MAX_THRESHHOLD = 1024;
const int THREAD_MAX_IDLE_TIME = 60; //单位：s

//线程池构造
ThreadPool::ThreadPool()
{
	initThreadSize_ = 0;
	taskSize_ = 0;
	idleThreadSize_ = 0;
	curThreadSize_ = 0;
	taskQueMaxThreshHold_ = TASK_MAX_THRESHHOLD;
	threadSizeThreshHold_ = THREAD_MAX_THRESHHOLD;
	poolMode_ = PoolMode::MODE_FIXED;
	isPoolRunning_ = false;
}

//线程池析构
ThreadPool::~ThreadPool()
{
	isPoolRunning_ = false;	

	std::unique_lock<std::mutex> lock(taskQueMtx_);
	notEmpty_.notify_all();
	exitCond_.wait(lock, [&]()->bool {return threads_.size() == 0; });
}

//设置线程池的工作模式
void ThreadPool::setMode(PoolMode mode) 
{
	if (checkRunningState())
	{
		return;
	}
	poolMode_ = mode;
}

//设置task任务队列上限阈值
void ThreadPool::setTaskQueMaxThreshHold(int threshhold)
{
	if (checkRunningState())
	{
		return;
	}
	taskQueMaxThreshHold_ = threshhold;
}

//设置线程池cached模式下线程阈值
void ThreadPool::setThreadSizeThreshHold(int threshhold)
{
	if (checkRunningState())
	{
		return;
	}
	if (poolMode_ == PoolMode::MODE_CACHED)
	{
		threadSizeThreshHold_ = threshhold;
	}
}

//给线程池提交任务 用户调用该接口 传入任务对象 生产任务
Result ThreadPool::submitTask(std::shared_ptr<Task> sp)
{
	//获取锁
	std::unique_lock<std::mutex> lock(taskQueMtx_);

	//线程的通信 等待任务队列有空余 
	//用户提交任务，最长不能阻塞超过1s，否则判断任务提交失败，返回
	if (!notFull_.wait_for(lock, std::chrono::seconds(1),
		[&]()->bool {return taskQue_.size() < taskQueMaxThreshHold_; }))
	{
		std::cerr << "task queue is full, submit task fail." << std::endl;
		return Result(sp,false); //Task Result
	}

	taskQue_.emplace(sp);
	taskSize_++;

	notEmpty_.notify_all();

	//cached模式 任务处理比较紧急 场景：小而快的任务 需要根据任务数量喝空闲线程的数量，判断是否需要创建新的线程出来
	if (poolMode_ == PoolMode::MODE_CACHED && taskSize_ > idleThreadSize_ && curThreadSize_ < threadSizeThreshHold_)
	{
		std::cout << "新的" << std::endl;
		//创建新线程
		auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));  //第三个参数占位符
		int threadId = ptr->getId();
		threads_.emplace(threadId, std::move(ptr));
		//启动线程
		threads_[threadId]->start();
		//修改线程个数相关的变量
		curThreadSize_++;
		idleThreadSize_++;
	}

	//返回任务的Result对象
	return Result(sp);
}

//开始线程池
void ThreadPool::start(int initThreadSize)
{
	//设置线程池的运行状态
	isPoolRunning_ = true;

	//记录初始线程数量
	initThreadSize_ = initThreadSize;
	curThreadSize_ = initThreadSize;

	//创建线程对象
	for (int i = 0; i < initThreadSize_; i++)
	{
		auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this,std::placeholders::_1));
		int threadId = ptr->getId();
		threads_.emplace(threadId, std::move(ptr));
	}

	//启动所有线程
	for (int i = 0; i < initThreadSize_; i++)
	{
		threads_[i]->start(); 
		idleThreadSize_++; 
	}
}

//定义线程函数 线程池的所有线程从任务队列里面消费任务
void ThreadPool::threadFunc(int threadid)
{	
	auto lastTime = std::chrono::high_resolution_clock().now();

	for (;;)
	{
		std::shared_ptr<Task> task;  
		{
			std::unique_lock<std::mutex> lock(taskQueMtx_);

			std::cout << "tid:" << std::this_thread::get_id()
				<< "尝试获取任务!" << std::endl;
			std::cout << " " << std::endl;

			//cached模式下，可能已经创建了很多的线程，空闲时间超过了60s，应该把多余的线程回收掉（超过initTreadSize_数量的线程要回收）
			//当前时间 上一次线程执行的时间 》 60s
			// 锁 + 双重判断
			while (taskQue_.size() == 0)
			{
				if (!isPoolRunning_)
				{
					threads_.erase(threadid);
					std::cout << "threadid" << std::this_thread::get_id() << "exit" << std::endl;
					exitCond_.notify_all();
					return; 
				}

				if (poolMode_ == PoolMode::MODE_CACHED)
				{
					if (std::cv_status::timeout == notEmpty_.wait_for(lock, std::chrono::seconds(1)))
					{
						auto now = std::chrono::high_resolution_clock().now();
						auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
						if (dur.count() >= THREAD_MAX_IDLE_TIME && curThreadSize_ > initThreadSize_)
						{
							threads_.erase(threadid);
							curThreadSize_--;
							idleThreadSize_--;

							std::cout << "threadid" << std::this_thread::get_id() << "exit" << std::endl;
							return;
						}
					}
				}
				else
				{
					notEmpty_.wait(lock);
				}	
			}			

			idleThreadSize_--;

			std::cout << "tid:" << std::this_thread::get_id()
				<< "获取任务成功!" << std::endl;

			task = taskQue_.front();
			taskQue_.pop();
			taskSize_--;

			if (taskQue_.size() > 0)
			{
				notEmpty_.notify_all();
			}

			notFull_.notify_all();
		}
		
		if (task != nullptr)
		{
			task->exec();
		}		

		idleThreadSize_++;
		lastTime = std::chrono::high_resolution_clock().now(); 
	}
}

bool ThreadPool::checkRunningState() const
{
	return isPoolRunning_;
}

//////////////////线程方法实现
int Thread::generateId_ = 0;

//线程构造
Thread::Thread(ThreadFunc func)
{
	func_ = func;
	threadId_ = generateId_++;
}

//线程析构
Thread::~Thread()
{

}

//启动线程
void Thread::start()
{
	//创建一个线程来执行一个线程函数
	std::thread t(func_,threadId_); 
	t.detach(); //设置分离线程
}

//获取线程id
int Thread::getId() const
{
	return threadId_;
}

///////////////////// Task方法实现
Task::Task()
{
	result_ = nullptr;
}

void Task::exec()
{
	if (result_ != nullptr)
	{
		result_->setVal(run()); 
	}
}

void Task::setResult(Result* res)
{
	result_ = res;
}

///////////////////// Result方法的实现
Result::Result(std::shared_ptr<Task> task, bool isValid)
{
	isValid_ = isValid;
	task_ = task;
	task_->setResult(this);
}

Any Result::get() //用户调用的
{
	if (!isValid_)
	{
		return "";
	}
	sem_.wait(); 
	return std::move(any_);
}

void Result::setVal(Any any) //
{
	this->any_ = std::move(any);
	sem_.post(); 
}