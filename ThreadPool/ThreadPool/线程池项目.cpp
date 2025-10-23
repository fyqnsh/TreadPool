// ThreadPool.cpp : 此文件包含 "main" 函数。程序执行将在此处开始并结束。

#include <iostream>
#include<chrono>
#include<thread>
using namespace std;

#include "threadpool.h"

using uLong = unsigned long long;

class MyTask : public Task
{
public:
    MyTask(int begin, int end)
    {
        begin_ = begin;
        end_ = end;
    }
    Any run()  //run方法最终就在线程池分配的线程中去执行了
    {
        std::cout << "tid:" << std::this_thread::get_id()
            << "begin" << std::endl;

        std::this_thread::sleep_for(std::chrono::seconds(3));
                  
        uLong sum = 0;
        for (uLong i = begin_; i <= end_; i++)
        {
            sum += i;
            /*std::cout << "tid:" << std::this_thread::get_id()*/
               /* << "end" << std::endl;*/

        }
        return sum;
    }

private:
    int begin_;
    int end_;
};

int main()
{
    {
        ThreadPool pool;
        //用户自己设置线程池的模式
        pool.setMode(PoolMode::MODE_CACHED);
        //开始启动线程池
        pool.start(4);

        Result res1 = pool.submitTask(std::make_shared<MyTask>(1, 100000000));
        Result res2 = pool.submitTask(std::make_shared<MyTask>(100000001, 200000000));
        Result res3 = pool.submitTask(std::make_shared<MyTask>(200000001, 300000000));
        pool.submitTask(std::make_shared<MyTask>(100000001, 200000000));

        pool.submitTask(std::make_shared<MyTask>(100000001, 200000000));
        pool.submitTask(std::make_shared<MyTask>(100000001, 200000000));

        uLong sum1 = res1.get().cast_<uLong>();
        uLong sum2 = res2.get().cast_<uLong>();
        uLong sum3 = res3.get().cast_<uLong>();

        cout << (sum1 + sum2 + sum3) << endl;
    }
    
    getchar();
}

