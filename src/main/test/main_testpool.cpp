#include <iostream>
#include <thread>
#include <chrono>
#include <cstring>
#include <queue>
#include <condition_variable>

// 引入你现有的框架头文件
#include "global_pool_manager.h"
#include "pipeline_node.h"

// ==========================================
// 1. 定义唯一的一个测试节点
// ==========================================
class TestNode : public PipelineNode {
public:
    TestNode(int channel_id, const std::string& name, bool is_source = false) 
        : PipelineNode(channel_id, name), is_source_(is_source), running_(false) {}

    ~TestNode() override { stop(); }

    void start() override {
        if (running_.exchange(true)) return;
        // 启动线程，运行 loop
        thread_ = std::thread([this]() { thread_loop(); });
        std::cout << "[" << get_full_name() << "] Started." << std::endl;
    }

    void stop() override {
        if (!running_.exchange(false)) return;
        cv_.notify_all(); // 唤醒队列
        if (thread_.joinable()) thread_.join();
        std::cout << "[" << get_full_name() << "] Stopped." << std::endl;
    }

    void set_data(const json& config) override {
        std::lock_guard<std::mutex> lock(mtx_);
        cfg_ = config;
    }

    // 接收上游数据
    void push_input(MediaBlock* block) override {
        std::lock_guard<std::mutex> lock(mtx_);
        q_.push(block);
        cv_.notify_one();
    }

private:
    void thread_loop() {
        while (running_.load()) {
            // 如果是源头节点，没有队列，主动产生数据
            if (is_source_) {
                MediaBlock* block = acquire_block();
                if (!block) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }

                // 模拟填充摄像头数据
                MemorySlot* slot = block->get_slot("nv12");
                if (slot) {
                    std::string data = "TestFrame_" + std::to_string(frame_id_++);
                    memcpy(slot->data, data.c_str(), data.size());
                    slot->actual_size = data.size();
                }
                block->pts = frame_id_ * 33;

                // 送给下游
                dispatch_to_children(block);
                
                // 模拟 30fps 的采集频率
                std::this_thread::sleep_for(std::chrono::milliseconds(33));
            } 
            // 如果是中间或末尾节点，阻塞等待上游发数据
            else {
                MediaBlock* block = nullptr;
                {
                    std::unique_lock<std::mutex> lock(mtx_);
                    cv_.wait_for(lock, std::chrono::milliseconds(100), 
                                 [this]{ return !q_.empty() || !running_.load(); });
                    if (!running_.load() && q_.empty()) break;
                    if (!q_.empty()) {
                        block = q_.front();
                        q_.pop();
                    }
                }

                if (!block) continue;

                // 模拟处理耗时
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                
                MemorySlot* slot = block->get_slot("nv12");
                std::string content = slot ? std::string((char*)slot->data, slot->actual_size) : "Empty";

                std::cout << "[" << get_full_name() << "] Received data: " << content << std::endl;

                // 如果是最后一个节点（没有下游），需要释放；否则继续往下传
                if (is_terminal_) {
                    release_block(block);
                } else {
                    dispatch_to_children(block);
                }
            }
        }
    }

public:
    void set_terminal(bool val) { is_terminal_ = val; }

private:
    bool is_source_;
    bool is_terminal_ = false;
    std::atomic<bool> running_;
    std::thread thread_;
    std::queue<MediaBlock*> q_;
    std::mutex mtx_;
    std::condition_variable cv_;
    json cfg_;
    int frame_id_ = 0;
};

// ==========================================
// 2. 主函数测试
// ==========================================
int main() {
    // 【新增】必须先注册图像格式和结构体，才能初始化池子！
    TypeRegistry::instance().register_image("nv12", 1.5f); 
    // ----------------------------------------
    // 1. 初始化常驻内存池 (测试用)
    // ----------------------------------------
    NodePoolConfig cfg;
    cfg.frame_count = 10;                                 // 池子里只有 10 个块
    cfg.images = {{"nv12", 1920, 1080}};
    GlobalPoolManager::get_instance().register_node_pool("Ch1_Source", cfg);

    auto pool = GlobalPoolManager::get_instance().get_pool("Ch1_Source");

    // ----------------------------------------
    // 2. 创建 3 个同类的节点对象
    // ----------------------------------------
    auto node1 = std::make_shared<TestNode>(1, "Source", true);  // 源头
    auto node2 = std::make_shared<TestNode>(1, "Process", false); // 中间
    auto node3 = std::make_shared<TestNode>(1, "Sink", false);    // 终点
    
    node3->set_terminal(true); // 标记终点

    // ----------------------------------------
    // 3. 连线
    // ----------------------------------------
    node1->link_to(node2);
    node2->link_to(node3);

    // ----------------------------------------
    // 4. 注入池子（只有源头需要拿块，中间节点只收指针）
    // ----------------------------------------
    node1->bind_pool(pool.get());
    // node2 和 node3 不需要 bind_pool，因为它们不调 acquire_block

    // ----------------------------------------
    // 5. 启动所有节点
    // ----------------------------------------
    node3->start(); // 建议从下游往上游启动
    node2->start();
    node1->start();

    std::cout << "--- Pipeline Running for 2 seconds ---\n";

    // ----------------------------------------
    // 6. 运行 2 秒后优雅关闭
    // ----------------------------------------
    std::this_thread::sleep_for(std::chrono::seconds(2));

    std::cout << "--- Stopping Pipeline ---\n";
    node1->stop();  // 先停源头
    node2->stop();
    node3->stop();

    // ----------------------------------------
    // 7. 检查内存池是否全部归还
    // ----------------------------------------
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    // 因为没走 MemoryApplication，这里手动清空
    GlobalPoolManager::get_instance().clear();
    std::cout << "--- Test Finished ---\n";
    
    return 0;
}