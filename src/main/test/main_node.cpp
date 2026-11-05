#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <iostream>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// 引入你现有的框架头文件
#include "MemManager.h"
#include "moudle_print.h"
#include "moudle_push.h"
#include "moudle_vi.h"

// 读取推流配置：优先命令行参数，其次仓库根目录，最后 build 目录（run.sh 会 cd 到 build）
static bool load_push_config(const char *argv_path, std::string &out) {
  std::vector<std::string> candidates;
  if (argv_path && argv_path[0])
    candidates.push_back(argv_path);
  candidates.push_back("config/push.json");
  candidates.push_back("../config/push.json");

  for (size_t i = 0; i < candidates.size(); ++i) {
    std::ifstream ifs(candidates[i].c_str());
    if (!ifs.is_open())
      continue;
    std::ostringstream oss;
    oss << ifs.rdbuf();
    out = oss.str();
    if (out.empty())
      continue;
    std::cout << "[main] 加载推流配置: " << candidates[i] << std::endl;
    return true;
  }
  return false;
}

int main(int argc, char **argv) {

  MemManager::getInstance().MemInit();

  // ----------------------------------------
  // 2. 创建 3 个节点对象：采集 -> 推流 -> 耗时打印
  // ----------------------------------------
  auto node1 = std::make_shared<ViNode>(); // 源头
  node1->create(0, "vi");
  auto node2 = std::make_shared<PushNode>(); // 中间
  node2->create(0, "rtsp");
  auto node3 = std::make_shared<PrintNode>(); // 终点：打印各节点耗时与总耗时
  node3->create(0, "print");

  // ----------------------------------------
  // 3. 灌入推流配置（宽/高/codec/协议/端口…）
  //    必须在 start() 之前，因为 start() 里会按配置起 RTSP 服务
  // ----------------------------------------
  std::string push_cfg;
  if (load_push_config(argc > 1 ? argv[1] : nullptr, push_cfg)) {
    node2->set_data(push_cfg.c_str());
    // 采集侧也吃同一份配置：按推流分辨率缩放，保证上下游尺寸一致
    node1->set_data(push_cfg.c_str());
    // 打印节点只关心其中的 print 段，没有该段时用默认值
    node3->set_data(push_cfg.c_str());
  } else {
    std::cerr << "[main] 未找到 config/push.json，PushNode 使用内置默认值"
              << std::endl;
  }

  // ----------------------------------------
  // 4. 连线：vi -> rtsp -> print
  // ----------------------------------------
  node1->link_to(node2.get());
  node2->link_to(node3.get());

  // node2->bind_pool(pool2.get());
  // node2 和 node3 不需要 bind_pool，因为它们不调 acquire_block

  // ----------------------------------------
  // 5. 启动所有节点（终点先起，避免队列里堆块）
  // ----------------------------------------
  node3->start();
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
  node3->stop();
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