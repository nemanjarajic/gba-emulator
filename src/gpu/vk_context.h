#pragma once

// Minimal compute-only Vulkan context for the GPU-resident GBA core.
//
// Deliberately no swapchain and no graphics queue: every milestone up to M7 is
// headless, and even at M7 the display is a blit of one instance's framebuffer.
// Keeping this compute-only avoids a large amount of MoltenVK surface handling.

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

namespace gba {

// Aborts with file/line and the stringified VkResult if `expr` is not VK_SUCCESS.
void vkCheckImpl(VkResult r, const char* expr, const char* file, int line);
#define VK_CHECK(expr) ::gba::vkCheckImpl((expr), #expr, __FILE__, __LINE__)

struct VkContext {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;

    VkPhysicalDeviceProperties props{};
    VkPhysicalDeviceMemoryProperties memProps{};
    uint32_t subgroupSize = 0;
    bool validationEnabled = false;

    void init(bool validation, bool debugPrintf);
    void destroy();
};

// Host-visible, host-coherent storage buffer. On Apple Silicon memory is
// unified, so there is no staging-buffer step: the mapped pointer and the GPU
// view are the same physical memory. This is a real simplification the design
// leans on -- uploading a ROM or reading back 4096 framebuffers is a memcpy.
struct Buffer {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void* mapped = nullptr;
};

Buffer createStorageBuffer(VkContext& ctx, VkDeviceSize size);
void destroyBuffer(VkContext& ctx, Buffer& b);

// A compute pipeline over `numBuffers` std430 storage buffers bound at
// consecutive bindings 0..numBuffers-1, plus an optional push-constant block.
struct ComputePipeline {
    VkShaderModule module = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    VkDescriptorSet descSet = VK_NULL_HANDLE;
    uint32_t pushSize = 0;

    void create(VkContext& ctx, const std::string& spvPath, uint32_t numBuffers,
                uint32_t pushConstantSize);
    // Points the descriptor set at `buffers`, in binding order.
    void bindBuffers(VkContext& ctx, const std::vector<Buffer*>& buffers);
    void destroy(VkContext& ctx);
};

// Records and submits one dispatch, then blocks until the GPU finishes.
//
// Blocking per dispatch is correct for M0-M7 and intentionally simple. M8 will
// need to pipeline several dispatches to keep the GPU fed, since at one
// scanline per dispatch the launch overhead would otherwise dominate.
void dispatchBlocking(VkContext& ctx, ComputePipeline& pipe, uint32_t groupsX,
                      const void* pushData, uint32_t pushSize);

std::vector<uint32_t> readFile(const std::string& path);

}  // namespace gba
