#pragma once
#ifndef  THREADPOOL_H
#define THREADPOOL_H
#include<vector>
#include<queue>
#include<memory> 
#include<atomic>
#include<mutex> 
#include<condition_variable> 
#include<functional>
#include<unordered_map>
#include<thread>
#include<future>
#include<iostream>

const int TASK_MAX_THRESHHOLD = INT32_MAX;
const int THREAD_MAX_THRESHHOLD = 1024;
const int THREAD_MAX_IDLE_TIME = 60; 

//线程池模式
enum class PoolMode
{
	MODE_FIXED, //固定数量的线程
	MODE_CACHED, //线程数量可动态增长
};

//线程类型
class Thread
{
public:
	//线程函数对象类型
	using ThreadFunc = std::function<void(int)>;

	//线程构造
	Thread(ThreadFunc func)
	{
		func_ = func;
		threadId_ = generateId_++;
	}
	//线程析构
	~Thread() = default;

	//启动线程
	void start()
	{
		std::thread t(func_, threadId_); 
		t.detach(); 
	}

	int getId() const
	{
		return threadId_;
	}

private:
	ThreadFunc func_;
	static int generateId_;
	int threadId_;
};

int Thread::generateId_ = 0;

//线程池类型
class ThreadPool
{
public:
	ThreadPool()
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

	~ThreadPool()
	{
		isPoolRunning_ = false;

		//等待线程池里面所有的线程返回 有两种状态 1、阻塞 2、正在执行任务中
		std::unique_lock<std::mutex> lock(taskQueMtx_);
		notEmpty_.notify_all();
		exitCond_.wait(lock, [&]()->bool {return threads_.size() == 0; });
	}

	//设置线程池的工作模式
	void setMode(PoolMode mode)
	{
		if (checkRunningState())
		{
			return;
		}
		poolMode_ = mode;
	}

	//设置task任务队列上限阈值
	void setTaskQueMaxThreshHold(int threshhold)
	{
		if (checkRunningState())
		{
			return;
		}
		taskQueMaxThreshHold_ = threshhold;
	}

	//设置线程池cached模式下线程阈值
	void setThreadSizeThreshHold(int threshhold) 
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

	//给线程池提交任务
	//使用可变参模板编程 让submitTask可以接收任意任务函数和任意数量的参数
	//返回值future<>
	template<typename Func, typename... Args>
	auto submitTask(Func&& func, Args&&... args) -> std::future<decltype(func(args...))>
	{
		using RType = decltype(func(args...));
		auto task = std::make_shared<std::packaged_task<RType()>>(std::bind(std::forward<Func>(func), std::forward<Args>(args)...));
		std::future<RType> result = task->get_future();

		std::unique_lock<std::mutex> lock(taskQueMtx_);

		//线程的通信 等待任务队列有空余 
		//用户提交任务，最长不能阻塞超过1s，否则判断任务提交失败，返回
		if (!notFull_.wait_for(lock, std::chrono::seconds(1),
			[&]()->bool {return taskQue_.size() < taskQueMaxThreshHold_; }))
		{
			//表示notFull_等待1s钟，条件依然没有满足
			std::cerr << "task queue is full, submit task fail." << std::endl;
			auto task = std::make_shared<std::packaged_task<RType()>>([]()->RType {return RType(); });
			(*task)();
			return task->get_future();
		}

		taskQue_.emplace([task](){(*task)();});
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
		return result;
	}

	//开启线程池
	void start(int initThreadSize = std::thread::hardware_concurrency())
	{
		//设置线程池的运行状态
		isPoolRunning_ = true;

		//记录初始线程数量
		initThreadSize_ = initThreadSize;
		curThreadSize_ = initThreadSize;

		//创建线程对象
		for (int i = 0; i < initThreadSize_; i++)
		{
			auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
			int threadId = ptr->getId();
			threads_.emplace(threadId, std::move(ptr));
		}

		for (int i = 0; i < initThreadSize_; i++)
		{
			threads_[i]->start(); //需要执行一个线程函数
			idleThreadSize_++; //记录初始空闲线程的数量
		}
	}

	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;

private:
	//定义线程函数
	void threadFunc(int threadid)
	{
		auto lastTime = std::chrono::high_resolution_clock().now();

		//所有任务必须执行完成，线程池才可以回收所有的线程资源
		for (;;)
		{
			Task task;
			{
				//先获取锁
				std::unique_lock<std::mutex> lock(taskQueMtx_);

				std::cout << "tid:" << std::this_thread::get_id()
					<< "尝试获取任务!" << std::endl;
				std::cout << " " << std::endl;

				//cached模式下，可能已经创建了很多的线程，但是空闲时间超过了60s，应该把多余的线程回收掉
				//当前时间 上一次线程执行的时间 》 60s
				// 锁 + 双重判断
				//每一秒钟返回一次 怎么区分超时返回还是有任务待执行返回
				while (taskQue_.size() == 0)
				{
					//线程池要结束 回收线程资源
					if (!isPoolRunning_)
					{
						threads_.erase(threadid);
						std::cout << "threadid" << std::this_thread::get_id() << "exit" << std::endl;
						exitCond_.notify_all();
						return; //线程函数结束 线程结束
					}

					if (poolMode_ == PoolMode::MODE_CACHED)
					{
						//条件变量，超时返回了
						if (std::cv_status::timeout == notEmpty_.wait_for(lock, std::chrono::seconds(1)))
						{
							auto now = std::chrono::high_resolution_clock().now();
							auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
							if (dur.count() >= THREAD_MAX_IDLE_TIME && curThreadSize_ > initThreadSize_)
							{
								//开始回收当前线程
								//记录线程数量的相关变量的值修改
								//把线程对象从线程列表容器中删除 没有办法匹配 threadFunc 对应哪个 thread对象
								//threadid -> thread对象 -》删除
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
						//等待notEmpty条件 阻塞当前线程，直到任务队列中有任务可处理  只有大于0才继续下面的代码
						notEmpty_.wait(lock);
					}
				}

				idleThreadSize_--;

				std::cout << "tid:" << std::this_thread::get_id()
					<< "获取任务成功!" << std::endl;

				//从任务队列中去一个任务出来
				task = taskQue_.front();
				taskQue_.pop();
				taskSize_--;

				//如果依然有剩余任务，继续通知其他的线程执行任务
				if (taskQue_.size() > 0)
				{
					notEmpty_.notify_all();
				}

				//取出一个任务，进行通知 通知可以继续提交生产任务
				notFull_.notify_all();
			}  // 通过作用域控制 std::unique_lock 的生命周期，实现锁的自动释放 不干扰其他线程

			//当前线程负责执行这个任务
			if (task != nullptr)
			{
				//task->run(); //执行任务，把任务的返回值setVal方法给到Result
				//task->exec();
				task(); //执行function<void()>
			}

			idleThreadSize_++;
			lastTime = std::chrono::high_resolution_clock().now(); //更新线程执行完任务的时间
		}
	}

	//检查pool的运行状态
	bool checkRunningState() const
	{
		return isPoolRunning_;
	}

private:
	//std::vector<std::unique_ptr<Thread>> threads_; //线程列表
	std::unordered_map<int, std::unique_ptr<Thread>> threads_; //线程列表

	int initThreadSize_;  //初始的线程数量 
	int threadSizeThreshHold_; //线程数量上限阈值
	std::atomic_int curThreadSize_; //记录当前线程池里面线程的总数量
	std::atomic_int idleThreadSize_; //记录空闲线程的数量

	using Task = std::function<void()>; 
	std::queue<Task> taskQue_; //任务队列
	std::atomic_int taskSize_; //任务的数量
	int taskQueMaxThreshHold_; //任务队列数量上限阈值

	std::mutex taskQueMtx_; //保证任务队列的线程安全
	std::condition_variable notFull_; //表示任务队列不满
	std::condition_variable notEmpty_; //表示任务队列不空
	std::condition_variable exitCond_; //等待线程资源全部回收

	PoolMode poolMode_; //当前线程池工作模式
	std::atomic_bool isPoolRunning_; //表示当前线程池的启动状态
};

#endif // ! THREADPOOL_H


