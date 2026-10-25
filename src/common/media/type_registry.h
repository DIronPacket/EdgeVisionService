#ifndef TYPE_REGISTRY_H
#define TYPE_REGISTRY_H

#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include <iostream>

struct TypeDescriptor {
    std::string name;  
    size_t size;       
    bool is_image;     
    float bytes_per_pixel; // 【新增】仅针对图像：计算内存大小用 (如 NV12 为 1.5)
};

class TypeRegistry {
public:
    static TypeRegistry& instance() {
        static TypeRegistry reg;
        return reg;
    }
    
    // 注册结构体（直接传入 sizeof）
    void register_struct(const std::string& name, size_t size) {
        if (registry_.find(name) != registry_.end()) {
            std::cerr << "[TypeRegistry] Warning: " << name << " already registered!" << std::endl;
            return;
        }
        registry_[name] = {name, size, false, 0.0f};
    }
    
    // 注册图像格式（传入每像素字节数）
    void register_image(const std::string& name, float bytes_per_pixel) {
        if (registry_.find(name) != registry_.end()) {
            std::cerr << "[TypeRegistry] Warning: " << name << " already registered!" << std::endl;
            return;
        }
        registry_[name] = {name, 0, true, bytes_per_pixel};
    }
    
    const TypeDescriptor* get(const std::string& name) {
        auto it = registry_.find(name);
        return it != registry_.end() ? &it->second : nullptr;
    }

private:
    TypeRegistry() = default;
    std::unordered_map<std::string, TypeDescriptor> registry_;
};

#endif