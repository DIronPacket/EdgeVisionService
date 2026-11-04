#ifndef TYPE_REGISTRY_H
#define TYPE_REGISTRY_H

#include "MemMeta.h"
#include "frame_buffer.h"
#include <iostream>
#include <mutex>
#include <string>
#include <unordered_map>

void RegisterAll(MemMeta *_memMeta) {
  FrameBuffer *frame = new FrameBuffer;
  _memMeta->set_ptr("nv12", frame);
}
#endif