#include <iostream>
#include <thread>
#include <vector>

const size_t ITERATIONS = 500000000;

// 1. 大型伪共享结构体 (跨越多个 Cacheline)
struct alignas(64) LargeFalseSharing {
    volatile long t1_data[15]; // 0 ~ 119 字节
    volatile long t2_data[15]; // 120 ~ 239 字节
};

// 2. 真共享结构体
struct alignas(64) TrueSharing {
    volatile long shared_val;
};

// 3. 无共享结构体 (作为对照组)
struct alignas(64) NoSharing {
    alignas(64) volatile long a;
    alignas(64) volatile long b;
};

// 全局实例
LargeFalseSharing lfs_data;
TrueSharing ts_data;
NoSharing ns_data;

// ======== 工作函数 ========
// 伪共享组：访问 112~119 字节 和 120~127 字节 (处于同一个 Cacheline)
void work_lfs_1() { for (size_t i = 0; i < ITERATIONS; ++i) lfs_data.t1_data[14]++; }
void work_lfs_2() { for (size_t i = 0; i < ITERATIONS; ++i) lfs_data.t2_data[0]++; }

// 真共享组：死磕同一个变量
void work_ts() { for (size_t i = 0; i < ITERATIONS; ++i) ts_data.shared_val++; }

// 无共享组：互不干扰
void work_ns_a() { for (size_t i = 0; i < ITERATIONS; ++i) ns_data.a++; }
void work_ns_b() { for (size_t i = 0; i < ITERATIONS; ++i) ns_data.b++; }

int main() {
    std::cout << "🔥 开启全并发压力测试 (6 个线程同时运行)..." << std::endl;
    std::vector<std::thread> threads;

    // 1. 注入伪共享噪音
    threads.emplace_back(work_lfs_1);
    threads.emplace_back(work_lfs_2);

    // 2. 注入真共享噪音
    threads.emplace_back(work_ts);
    threads.emplace_back(work_ts);

    // 3. 注入对照组噪音
    threads.emplace_back(work_ns_a);
    threads.emplace_back(work_ns_b);

    for (auto& t : threads) {
        t.join();
    }
    
    std::cout << "✅ 测试完成." << std::endl;
    return 0;
}
/*
g++ -g -O2 -pthread sharing_bench.cpp -o sharing_bench -static

neu@dn:~/exp$ sudo /home/neu/exp/perfparse/target/debug/perfparse ./sharing_bench
🚀 目标程序: /home/neu/exp/sharing_bench
📂 工作目录: /home/neu/exp

▶️  [1/4] 正在执行 sudo perf c2c record... (请耐心等待程序运行结束)
🔥 开启全并发压力测试 (6 个线程同时运行)...
✅ 测试完成.
[ perf record: Woken up 22 times to write data ]
[ perf record: Captured and wrote 7.097 MB perf.data (84339 samples) ]
▶️  [2/4] 正在执行 sudo perf script 导出数据...
▶️  [3/4] 修改 perf.txt 权限...
▶️  [4/4] 正在调用 nm 分析 ELF 符号表...
✅ 成功加载 7528 个符号，动态边界 [_end]: 0x5e67f8

📥 开始读取并解析日志数据...
🔍 成功加载 26586 条精确访问记录，开始统计分析...

==================================================================
🚨 静态数据区 真/伪共享 最终诊断报告 (Max Addr <= 0x5e67f8)
==================================================================
💥 伪共享 (False Sharing) [争用变量]: lfs_data+0x70 与 lfs_data+0x78 | 间距: 08 字节 | 跨核 Ping-Pong: 9043 次
🔴 真共享 (True Sharing) [争用变量]: ts_data 与 ts_data | 间距: 00 字节 | 跨核 Ping-Pong: 8937 次
==================================================================
neu@dn:~/exp$ sudo /home/neu/exp/perfparse/target/debug/perfparse ./sharing_bench

*/