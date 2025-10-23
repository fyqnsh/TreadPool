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

//�̳߳�ģʽ
enum class PoolMode
{
	MODE_FIXED, //�̶��������߳�
	MODE_CACHED, //�߳������ɶ�̬����
};

//�߳�����
class Thread
{
public:
	//�̺߳�����������
	using ThreadFunc = std::function<void(int)>;

	//�̹߳���
	Thread(ThreadFunc func)
	{
		func_ = func;
		threadId_ = generateId_++;
	}
	//�߳�����
	~Thread() = default;

	//�����߳�
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

//�̳߳�����
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

		std::unique_lock<std::mutex> lock(taskQueMtx_);
		notEmpty_.notify_all();
		exitCond_.wait(lock, [&]()->bool {return threads_.size() == 0; });
	}

	//�����̳߳صĹ���ģʽ
	void setMode(PoolMode mode)
	{
		if (checkRunningState())
		{
			return;
		}
		poolMode_ = mode;
	}

	//����task�������������ֵ
	void setTaskQueMaxThreshHold(int threshhold)
	{
		if (checkRunningState())
		{
			return;
		}
		taskQueMaxThreshHold_ = threshhold;
	}

	//�����̳߳�cachedģʽ���߳���ֵ
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

	//���̳߳��ύ����
	//ʹ�ÿɱ��ģ���� ��submitTask���Խ������������������������Ĳ���
	//����ֵfuture<>
	template<typename Func, typename... Args>
	auto submitTask(Func&& func, Args&&... args) -> std::future<decltype(func(args...))>
	{
		using RType = decltype(func(args...));
		auto task = std::make_shared<std::packaged_task<RType()>>(std::bind(std::forward<Func>(func), std::forward<Args>(args)...));
		std::future<RType> result = task->get_future();

		std::unique_lock<std::mutex> lock(taskQueMtx_);

		//�̵߳�ͨ�� �ȴ���������п��� 
		//�û��ύ�����������������1s�������ж������ύʧ�ܣ�����
		if (!notFull_.wait_for(lock, std::chrono::seconds(1),
			[&]()->bool {return taskQue_.size() < taskQueMaxThreshHold_; }))
		{
			std::cerr << "task queue is full, submit task fail." << std::endl;
			auto task = std::make_shared<std::packaged_task<RType()>>([]()->RType {return RType(); });
			(*task)();
			return task->get_future();
		}

		taskQue_.emplace([task](){(*task)();});
		taskSize_++;

		notEmpty_.notify_all();

		//cachedģʽ ������ȽϽ��� ������С��������� ��Ҫ�������������ȿ����̵߳��������ж��Ƿ���Ҫ�����µ��̳߳���
		if (poolMode_ == PoolMode::MODE_CACHED && taskSize_ > idleThreadSize_ && curThreadSize_ < threadSizeThreshHold_)
		{
			std::cout << "�µ�" << std::endl;
			//�������߳�
			auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));  //����������ռλ��
			int threadId = ptr->getId();
			threads_.emplace(threadId, std::move(ptr));

			threads_[threadId]->start();

			curThreadSize_++;
			idleThreadSize_++;
		}

		return result;
	}

	//�����̳߳�
	void start(int initThreadSize = std::thread::hardware_concurrency())
	{
		//�����̳߳ص�����״̬
		isPoolRunning_ = true;

		//��¼��ʼ�߳�����
		initThreadSize_ = initThreadSize;
		curThreadSize_ = initThreadSize;

		//�����̶߳���
		for (int i = 0; i < initThreadSize_; i++)
		{
			auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
			int threadId = ptr->getId();
			threads_.emplace(threadId, std::move(ptr));
		}

		for (int i = 0; i < initThreadSize_; i++)
		{
			threads_[i]->start(); 
			idleThreadSize_++;
		}
	}

	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;

private:
	//�����̺߳���
	void threadFunc(int threadid)
	{
		auto lastTime = std::chrono::high_resolution_clock().now();

		for (;;)
		{
			Task task;
			{
				std::unique_lock<std::mutex> lock(taskQueMtx_);

				std::cout << "tid:" << std::this_thread::get_id()
					<< "���Ի�ȡ����!" << std::endl;
				std::cout << " " << std::endl;

				//cachedģʽ�£������Ѿ������˺ܶ���̣߳����ǿ���ʱ�䳬����60s��Ӧ�ðѶ�����̻߳��յ�
				//��ǰʱ�� ��һ���߳�ִ�е�ʱ�� �� 60s
				// �� + ˫���ж�
				//ÿһ���ӷ���һ�� ��ô���ֳ�ʱ���ػ����������ִ�з���
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
					<< "��ȡ����ɹ�!" << std::endl;

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
				task(); 
			}

			idleThreadSize_++;
			lastTime = std::chrono::high_resolution_clock().now();
		}
	}

	//���pool������״̬
	bool checkRunningState() const
	{
		return isPoolRunning_;
	}

private:
	std::unordered_map<int, std::unique_ptr<Thread>> threads_; //�߳��б�

	int initThreadSize_;  //��ʼ���߳����� 
	int threadSizeThreshHold_; //�߳�����������ֵ
	std::atomic_int curThreadSize_; //��¼��ǰ�̳߳������̵߳�������
	std::atomic_int idleThreadSize_; //��¼�����̵߳�����

	using Task = std::function<void()>; 
	std::queue<Task> taskQue_; //�������
	std::atomic_int taskSize_; //���������
	int taskQueMaxThreshHold_; //�����������������ֵ

	std::mutex taskQueMtx_; //��֤������е��̰߳�ȫ
	std::condition_variable notFull_; //��ʾ������в���
	std::condition_variable notEmpty_; //��ʾ������в���
	std::condition_variable exitCond_; //�ȴ��߳���Դȫ������

	PoolMode poolMode_; //��ǰ�̳߳ع���ģʽ
	std::atomic_bool isPoolRunning_; //��ʾ��ǰ�̳߳ص�����״̬
};

#endif 


