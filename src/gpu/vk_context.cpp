#include "gpu/vk_context.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace gba {

void vkCheckImpl(VkResult r, const char* expr, const char* file, int line) {
    if (r == VK_SUCCESS) return;
    std::fprintf(stderr, "[vk] %s:%d: %s failed with VkResult %d\n", file, line, expr, int(r));
    std::abort();
}

std::vector<uint32_t> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        std::fprintf(stderr, "[vk] cannot open %s\n", path.c_str());
        std::abort();
    }
    const auto bytes = static_cast<size_t>(f.tellg());
    std::vector<uint32_t> out((bytes + 3) / 4, 0u);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(bytes));
    return out;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    // debugPrintfEXT output arrives here as an INFO-severity message, which is
    // why info is not filtered out.
    const char* tag = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)     ? "ERROR"
                      : (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ? "WARN"
                                                                                     : "INFO";
    std::fprintf(stderr, "[vk:%s] %s\n", tag, data->pMessage);
    return VK_FALSE;
}

static bool hasLayer(const char* name) {
    uint32_t n = 0;
    vkEnumerateInstanceLayerProperties(&n, nullptr);
    std::vector<VkLayerProperties> v(n);
    vkEnumerateInstanceLayerProperties(&n, v.data());
    for (auto& l : v)
        if (std::strcmp(l.layerName, name) == 0) return true;
    return false;
}

static bool hasDeviceExt(VkPhysicalDevice p, const char* name) {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(p, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> v(n);
    vkEnumerateDeviceExtensionProperties(p, nullptr, &n, v.data());
    for (auto& e : v)
        if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}

void VkContext::init(bool validation, bool debugPrintf) {
    // ---- instance -------------------------------------------------------
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "gba-gpu";
    app.apiVersion = VK_API_VERSION_1_2;

    std::vector<const char*> exts{
        // MoltenVK is a "portability" driver; without this extension plus the
        // ENUMERATE_PORTABILITY flag below, vkEnumeratePhysicalDevices returns
        // nothing on macOS and instance creation fails with INCOMPATIBLE_DRIVER.
        VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
    };
    std::vector<const char*> layers;

    if (validation && hasLayer("VK_LAYER_KHRONOS_validation")) {
        layers.push_back("VK_LAYER_KHRONOS_validation");
        exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        this->validationEnabled = true;
    } else if (validation) {
        std::fprintf(stderr, "[vk] validation layer requested but not found; "
                             "is VK_LAYER_PATH set? (see env.sh)\n");
    }

    // debugPrintfEXT in a shader is routed through the validation layer, so it
    // is only available when validation is on.
    VkValidationFeatureEnableEXT enables[] = {VK_VALIDATION_FEATURE_ENABLE_DEBUG_PRINTF_EXT};
    VkValidationFeaturesEXT vf{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    vf.enabledValidationFeatureCount = 1;
    vf.pEnabledValidationFeatures = enables;

    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = uint32_t(exts.size());
    ici.ppEnabledExtensionNames = exts.data();
    ici.enabledLayerCount = uint32_t(layers.size());
    ici.ppEnabledLayerNames = layers.data();
    if (debugPrintf && this->validationEnabled) ici.pNext = &vf;

    VK_CHECK(vkCreateInstance(&ici, nullptr, &instance));

    if (this->validationEnabled) {
        auto create = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            instance, "vkCreateDebugUtilsMessengerEXT");
        if (create) {
            VkDebugUtilsMessengerCreateInfoEXT mi{
                VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT;
            mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            mi.pfnUserCallback = debugCallback;
            VK_CHECK(create(instance, &mi, nullptr, &messenger));
        }
    }

    // ---- physical device ------------------------------------------------
    uint32_t n = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &n, nullptr));
    if (n == 0) {
        std::fprintf(stderr, "[vk] no Vulkan devices; is VK_DRIVER_FILES set? (see env.sh)\n");
        std::abort();
    }
    std::vector<VkPhysicalDevice> devs(n);
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &n, devs.data()));
    phys = devs[0];

    vkGetPhysicalDeviceProperties(phys, &props);
    vkGetPhysicalDeviceMemoryProperties(phys, &memProps);

    VkPhysicalDeviceSubgroupProperties sub{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &sub;
    vkGetPhysicalDeviceProperties2(phys, &p2);
    subgroupSize = sub.subgroupSize;

    // ---- queue family ---------------------------------------------------
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qs(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, qs.data());
    bool found = false;
    for (uint32_t i = 0; i < qn; ++i) {
        if (qs[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            queueFamily = i;
            found = true;
            break;
        }
    }
    if (!found) {
        std::fprintf(stderr, "[vk] no compute queue family\n");
        std::abort();
    }

    // ---- logical device -------------------------------------------------
    std::vector<const char*> devExts;
    // Required by spec whenever the driver advertises it; MoltenVK always does.
    if (hasDeviceExt(phys, "VK_KHR_portability_subset"))
        devExts.push_back("VK_KHR_portability_subset");
    // Enables debugPrintfEXT in GLSL. Enabled from M0 because it is the only
    // printf-equivalent available, and there is no Metal frame debugger here
    // (Command Line Tools only, no full Xcode).
    if (hasDeviceExt(phys, VK_KHR_SHADER_NON_SEMANTIC_INFO_EXTENSION_NAME))
        devExts.push_back(VK_KHR_SHADER_NON_SEMANTIC_INFO_EXTENSION_NAME);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = uint32_t(devExts.size());
    dci.ppEnabledExtensionNames = devExts.data();

    VK_CHECK(vkCreateDevice(phys, &dci, nullptr, &device));
    vkGetDeviceQueue(device, queueFamily, 0, &queue);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queueFamily;
    VK_CHECK(vkCreateCommandPool(device, &pci, nullptr, &cmdPool));
}

void VkContext::destroy() {
    if (cmdPool) vkDestroyCommandPool(device, cmdPool, nullptr);
    if (device) vkDestroyDevice(device, nullptr);
    if (messenger) {
        auto destroy = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            instance, "vkDestroyDebugUtilsMessengerEXT");
        if (destroy) destroy(instance, messenger, nullptr);
    }
    if (instance) vkDestroyInstance(instance, nullptr);
    *this = VkContext{};
}

// ---------------------------------------------------------------------------

Buffer createStorageBuffer(VkContext& ctx, VkDeviceSize size) {
    Buffer b{};
    b.size = size;

    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(ctx.device, &bci, nullptr, &b.buf));

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(ctx.device, b.buf, &req);

    // Unified memory: ask for host-visible + coherent, preferring a heap that is
    // also device-local. On Apple Silicon this is the same physical memory.
    const VkMemoryPropertyFlags want =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t index = UINT32_MAX;
    for (uint32_t i = 0; i < ctx.memProps.memoryTypeCount; ++i) {
        if (!(req.memoryTypeBits & (1u << i))) continue;
        const auto flags = ctx.memProps.memoryTypes[i].propertyFlags;
        if ((flags & want) != want) continue;
        index = i;
        if (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) break;  // preferred
    }
    if (index == UINT32_MAX) {
        std::fprintf(stderr, "[vk] no host-visible coherent memory type\n");
        std::abort();
    }

    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = index;
    VK_CHECK(vkAllocateMemory(ctx.device, &mai, nullptr, &b.mem));
    VK_CHECK(vkBindBufferMemory(ctx.device, b.buf, b.mem, 0));
    VK_CHECK(vkMapMemory(ctx.device, b.mem, 0, VK_WHOLE_SIZE, 0, &b.mapped));
    return b;
}

void destroyBuffer(VkContext& ctx, Buffer& b) {
    if (b.mapped) vkUnmapMemory(ctx.device, b.mem);
    if (b.buf) vkDestroyBuffer(ctx.device, b.buf, nullptr);
    if (b.mem) vkFreeMemory(ctx.device, b.mem, nullptr);
    b = Buffer{};
}

// ---------------------------------------------------------------------------

void ComputePipeline::create(VkContext& ctx, const std::string& spvPath, uint32_t numBuffers,
                             uint32_t pushConstantSize) {
    pushSize = pushConstantSize;

    const auto code = readFile(spvPath);
    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = code.size() * sizeof(uint32_t);
    smci.pCode = code.data();
    VK_CHECK(vkCreateShaderModule(ctx.device, &smci, nullptr, &module));

    std::vector<VkDescriptorSetLayoutBinding> binds(numBuffers);
    for (uint32_t i = 0; i < numBuffers; ++i) {
        binds[i].binding = i;
        binds[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binds[i].descriptorCount = 1;
        binds[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dsl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dsl.bindingCount = numBuffers;
    dsl.pBindings = binds.data();
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device, &dsl, nullptr, &setLayout));

    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, pushConstantSize};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &setLayout;
    pli.pushConstantRangeCount = pushConstantSize ? 1 : 0;
    pli.pPushConstantRanges = pushConstantSize ? &pcr : nullptr;
    VK_CHECK(vkCreatePipelineLayout(ctx.device, &pli, nullptr, &layout));

    VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpi.stage = VkPipelineShaderStageCreateInfo{
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
        VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
    cpi.layout = layout;
    VK_CHECK(vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipeline));

    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, numBuffers};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 1;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    VK_CHECK(vkCreateDescriptorPool(ctx.device, &dpi, nullptr, &descPool));

    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = descPool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &setLayout;
    VK_CHECK(vkAllocateDescriptorSets(ctx.device, &dai, &descSet));
}

void ComputePipeline::bindBuffers(VkContext& ctx, const std::vector<Buffer*>& buffers) {
    std::vector<VkDescriptorBufferInfo> infos(buffers.size());
    std::vector<VkWriteDescriptorSet> writes(buffers.size());
    for (size_t i = 0; i < buffers.size(); ++i) {
        infos[i] = {buffers[i]->buf, 0, VK_WHOLE_SIZE};
        writes[i] = VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet = descSet;
        writes[i].dstBinding = uint32_t(i);
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(ctx.device, uint32_t(writes.size()), writes.data(), 0, nullptr);
}

void ComputePipeline::destroy(VkContext& ctx) {
    if (descPool) vkDestroyDescriptorPool(ctx.device, descPool, nullptr);
    if (pipeline) vkDestroyPipeline(ctx.device, pipeline, nullptr);
    if (layout) vkDestroyPipelineLayout(ctx.device, layout, nullptr);
    if (setLayout) vkDestroyDescriptorSetLayout(ctx.device, setLayout, nullptr);
    if (module) vkDestroyShaderModule(ctx.device, module, nullptr);
    *this = ComputePipeline{};
}

void dispatchBlocking(VkContext& ctx, ComputePipeline& pipe, uint32_t groupsX,
                      const void* pushData, uint32_t pushSize) {
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = ctx.cmdPool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    VK_CHECK(vkAllocateCommandBuffers(ctx.device, &cai, &cmd));

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe.layout, 0, 1, &pipe.descSet,
                            0, nullptr);
    if (pushSize) vkCmdPushConstants(cmd, pipe.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, pushSize,
                                     pushData);
    vkCmdDispatch(cmd, groupsX, 1, 1);
    VK_CHECK(vkEndCommandBuffer(cmd));

    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    VK_CHECK(vkCreateFence(ctx.device, &fci, nullptr, &fence));

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    VK_CHECK(vkQueueSubmit(ctx.queue, 1, &si, fence));
    VK_CHECK(vkWaitForFences(ctx.device, 1, &fence, VK_TRUE, UINT64_MAX));

    vkDestroyFence(ctx.device, fence, nullptr);
    vkFreeCommandBuffers(ctx.device, ctx.cmdPool, 1, &cmd);
}

}  // namespace gba
