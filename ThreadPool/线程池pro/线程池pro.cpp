// 线程池pro.cpp : 此文件包含 "main" 函数。程序执行将在此处开始并结束。
//

#include <iostream>
using namespace std;
#include<thread>
#include<functional>
#include<future>

#include "threadpool.h"

int sum1(int a, int b)
{
    return a + b;
}

int sum2(int a, int b, int c)
{
    return a + b + c; 
}

int main()
{
    ThreadPool pool;
    pool.start(4);

    future<int> r1 = pool.submitTask(sum1, 1, 2);

    cout << r1.get() << endl;
   
}

