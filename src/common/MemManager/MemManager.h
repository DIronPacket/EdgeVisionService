#ifndef MEM_MANAGER_H
#define MEM_MANAGER_H
#include "MemPool.h"
#include "frame_buffer.h"
class MemManager {
public:
  typedef struct __ImgConfig
  {
    int width;
    int height;
    PixelFormat fmt;
    std::string imgName;
  }ImgConfig;
  static MemManager &getInstance() {
    static MemManager memManager;
    return memManager;
  }

  void addImgConfig(const ImgConfig&img);

  void MemInit(int size = 50);
  void FreeMem();

private:
  MemManager();
  ~MemManager(){};

private:
  int memBlockSize=0;
  std::unordered_map<std::string,ImgConfig> imgMap;
};
#endif