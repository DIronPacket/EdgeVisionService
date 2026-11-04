#include "MemManager.h"
#include "MemMeta.h"

MemManager::MemManager()
{
  addImgConfig(ImgConfig{1920,1080,PixelFormat::FMT_NV12,"nv12"});
}
void MemManager::addImgConfig(const ImgConfig&img)
{
  imgMap.insert(std::pair<std::string,ImgConfig>(img.imgName,img));
}
void MemManager::MemInit(int size) {
  pool::MemPool &pool = pool::MemPool::getInstance();
  memBlockSize=size;
  for(int i=0;i<memBlockSize;++i)
  {
    MemMeta *_memMeta = new MemMeta();
    //视频帧初始化
    for(auto iter = imgMap.begin();iter!=imgMap.end();++iter)
    {
      ImgConfig imgConfig = iter->second;
      FrameBuffer* frame = new FrameBuffer;
      frame->width=imgConfig.width;
      frame->height=imgConfig.height;
      frame->fmt=imgConfig.fmt;
      frame->Allocate(frame->width,frame->height,frame->fmt);
      frame->BindMemory(frame->data[0],frame->width,frame->height,frame->fmt);
      // printf("successfully bindmemory: data[0]:%p  data[1]:%p  data[2]:%p\n",
      //   frame->data[0],frame->data[1],frame->data[2]);
      _memMeta->set_ptr(iter->first,frame);
    }
    //结构体初始化
    pool::MemBlock *block = new pool::MemBlock(_memMeta);
    pool.addBuffer(_memMeta);
  }

  

}
void MemManager::FreeMem() {
  pool::MemPool &pool = pool::MemPool::getInstance();
  for(int i=0;i<memBlockSize;++i)
  {
    pool::MemBlock *block = pool.delGetBlock();
    if(nullptr != block)
    {
      MemMeta *_memData = block->data();
      //释放视频帧

      //释放结构体

      delete _memData;
    }
  }
}