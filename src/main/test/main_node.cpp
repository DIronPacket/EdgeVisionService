#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <queue>
#include <thread>

// 引入你现有的框架头文件
#include "MemManager.h"
#include "moudle_push.h"
#include "moudle_vi.h"

int main() {

  MemManager::getInstance().MemInit();

  // ----------------------------------------
  // 2. 创建 3 个同类的节点对象
  // ----------------------------------------
  auto node1 = std::make_shared<ViNode>(); // 源头
  node1->create(0, "vi");
  auto node2 = std::make_shared<PushNode>(); // 中间
  node2->create(0, "rtsp");

  // ----------------------------------------
  // 3. 连线
  // ----------------------------------------
  node1->link_to(node2.get());

  // node2->bind_pool(pool2.get());
  // node2 和 node3 不需要 bind_pool，因为它们不调 acquire_block

  // ----------------------------------------
  // 5. 启动所有节点
  // ----------------------------------------
  node2->start();
  node1->start();

  std::cout << "--- Pipeline Running for 2 seconds ---\n";

  // ----------------------------------------
  // 6. 运行 2 秒后优雅关闭
  // ----------------------------------------
  std::this_thread::sleep_for(std::chrono::seconds(2));

  while (1) {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
  }
  node1->stop(); // 先停源头
  node2->stop();
  // 因为没走 MemoryApplication，这里手动清空
  MemManager::getInstance().FreeMem();
  std::cout << "--- Stopping Pipeline ---\n";

  // ----------------------------------------
  // 7. 检查内存池是否全部归还
  // ----------------------------------------
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  std::cout << "--- Node Finished ---\n";
  return 0;
}