#include "MemManager.h"
#include "MemMeta.h"
#include "frame_mem.h"
#include "node_profiler.h"

MemManager::MemManager()
{
  addImgConfig(ImgConfig{1920, 1080, PixelFormat::FMT_NV12, "nv12"});
}
void MemManager::addImgConfig(const ImgConfig &img)
{
  imgMap.insert(std::pair<std::string, ImgConfig>(img.imgName, img));
}
void MemManager::MemInit(int size)
{
  pool::MemPool &pool = pool::MemPool::getInstance();
  memBlockSize = size;
  for (int i = 0; i < memBlockSize; ++i)
  {
    MemMeta *_memMeta = new MemMeta();
    // 视频帧初始化
    for (auto iter = imgMap.begin(); iter != imgMap.end(); ++iter)
    {
      ImgConfig imgConfig = iter->second;
      FrameBuffer *frame = new FrameBuffer;
      // 只走 Allocate()：宽高/格式由 CalcMemoryLayout() 填，内存在一起记进 owned_mem。
      // 不要再跟一句 BindMemory(frame->data[0], ...)：BindMemory 的语义是「绑定外部内存
      // 的视图」，拿自己的堆块去绑等于把所有权说两遍——将来只要有人让 BindMemory 顺手
      // 清掉 owned_mem，Release() 就变成空操作，内存又回到 P0-9 那条老路。
      if (!frame->Allocate(imgConfig.width, imgConfig.height, imgConfig.fmt))
      {
        // 分配失败必须立刻回收：set_ptr 之后没人知道这块没内存，
        // 下游节点会拿着空 data 指针去写（P0-9）
        std::cerr << "[MemManager] frame 分配失败: " << imgConfig.imgName << " "
                  << imgConfig.width << "x" << imgConfig.height << std::endl;
        delete frame;
        continue;
      }
      // printf("successfully bindmemory: data[0]:%p  data[1]:%p  data[2]:%p\n",
      //   frame->data[0],frame->data[1],frame->data[2]);
      _memMeta->set_ptr(iter->first, frame);
    }
    // 耗时统计结构初始化：每块一个，节点从 block 取出后记录本帧各阶段耗时
    // （槽位名见 profile::kProfilerSlot，节点通过 profile::get_profiler(block) 获取）
    _memMeta->set_ptr(profile::kProfilerSlot, new profile::NodeProfiler());
    // 结构体初始化
    pool.addBuffer(_memMeta);
  }
}
void MemManager::FreeMem()
{
  pool::MemPool &pool = pool::MemPool::getInstance();
  int freed_blocks = 0;
  size_t freed_slots = 0;
  for (int i = 0; i < memBlockSize; ++i)
  {
    pool::MemBlock *block = pool.delGetBlock();
    if (nullptr == block)
      continue; // 池里已经没有块了（例如中途 init 失败），不必空转剩下的次数

    MemMeta *_memData = block->data();
    if (nullptr != _memData)
    {
      // 统一按槽位释放：MemInit 挂的帧 / 统计、节点自建槽位（如 vpss 的 vpss_out）
      // 都在同一个 map 里，清一次就够。不要再按 imgMap 点名遍历 ——
      // 那样只回收 MemInit 自己登记的槽位，节点后来挂上去的全部漏掉（P0-11）
      freed_slots += _memData->clear();
      ++freed_blocks;
    }

    delete _memData; // MemMeta 析构里还有一次 clear() 兜底，此时 map 已空
    delete block;
  }

  if (freed_blocks > 0)
  {
    std::cout << "[MemManager] FreeMem: 释放 " << freed_blocks << " 块 / "
              << freed_slots << " 个槽位" << std::endl;
  }

  // 帧内存后端收尾：MPP 模式下 mpp_buffer_put 只把缓冲挂回组的 unused 链表，
  // DMA 内存仍被组占着，clear+put 才是真正还系统的那一步（堆模式下这个函数是空实现）。
  // 必须放在所有块都还完之后——本函数返回后池里已无块，满足这个前提。
  pool::FrameMemDeinit();
}
