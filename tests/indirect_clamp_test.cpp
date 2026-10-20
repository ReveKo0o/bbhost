// The indirect-dispatch clamp (src/host/shaders/indirectclamp.comp) run on the
// GPU: counts up to 65,535 a dimension pass as they are, any count past it in
// any dimension makes the dispatch run nothing (0 0 0) and raises the flag,
// and the counts as they came are kept for the log. Exit 77 without a device.
#include "host/shaders/indirectclamp.spv.h"
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
bool ok(VkResult r) { return r == VK_SUCCESS; }
}

int main() {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance instance{};
    if (!ok(vkCreateInstance(&ici, nullptr, &instance))) { std::puts("SKIP: no Vulkan"); return 77; }
    std::uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance, &n, nullptr);
    std::vector<VkPhysicalDevice> phys(n);
    vkEnumeratePhysicalDevices(instance, &n, phys.data());
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    std::uint32_t family = 0;
    for (VkPhysicalDevice p : phys) {
        std::uint32_t q = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(p, &q, nullptr);
        std::vector<VkQueueFamilyProperties> qs(q);
        vkGetPhysicalDeviceQueueFamilyProperties(p, &q, qs.data());
        for (std::uint32_t k = 0; k < q; ++k) {
            if (qs[k].queueFlags & VK_QUEUE_COMPUTE_BIT) { pd = p; family = k; break; }
        }
        if (pd) break;
    }
    if (!pd) { std::puts("SKIP: no compute queue"); vkDestroyInstance(instance, nullptr); return 77; }
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = family; qi.queueCount = 1; qi.pQueuePriorities = &prio;
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    v12.bufferDeviceAddress = VK_TRUE;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &v12; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qi;
    VkDevice dev{};
    if (!ok(vkCreateDevice(pd, &dci, nullptr, &dev))) { std::puts("SKIP: no device with buffer device addresses"); return 77; }
    VkQueue queue{};
    vkGetDeviceQueue(dev, family, 0, &queue);
    // The cases, 32 bytes a slot as indirect_clamp.cpp lays them out.
    struct Case { std::uint32_t x, y, z; bool clamped; };
    const Case cases[] = {{1, 1, 1, false}, {65535, 1, 1, false}, {65535, 65535, 65535, false}, {0, 1, 1, false}, {65536, 1, 1, true},
                          {1, 65536, 1, true}, {1, 1, 65536, true}, {3176062614u, 1, 1, true}, {0xffffffffu, 0xffffffffu, 0xffffffffu, true}};
    const std::uint32_t ncases = sizeof(cases) / sizeof(cases[0]);
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = 32 * ncases;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    VkBuffer buf{};
    vkCreateBuffer(dev, &bci, nullptr, &buf);
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(dev, buf, &req);
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    std::uint32_t type = UINT32_MAX;
    for (std::uint32_t k = 0; k < mp.memoryTypeCount; ++k) {
        const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if ((req.memoryTypeBits >> k & 1) && (mp.memoryTypes[k].propertyFlags & want) == want) { type = k; break; }
    }
    VkMemoryAllocateFlagsInfo fl{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    fl.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.pNext = &fl; mai.allocationSize = req.size; mai.memoryTypeIndex = type;
    VkDeviceMemory mem{};
    vkAllocateMemory(dev, &mai, nullptr, &mem);
    vkBindBufferMemory(dev, buf, mem, 0);
    std::uint32_t* map = nullptr;
    vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&map));
    std::memset(map, 0xcd, 32 * ncases);
    for (std::uint32_t k = 0; k < ncases; ++k) { map[k * 8] = cases[k].x; map[k * 8 + 1] = cases[k].y; map[k * 8 + 2] = cases[k].z; }
    VkBufferDeviceAddressInfo bai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    bai.buffer = buf;
    const VkDeviceAddress base = vkGetBufferDeviceAddress(dev, &bai);
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &range;
    VkPipelineLayout layout{};
    vkCreatePipelineLayout(dev, &pli, nullptr, &layout);
    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smi.codeSize = sizeof(k_indirectclamp_spv); smi.pCode = k_indirectclamp_spv;
    VkShaderModule module{};
    vkCreateShaderModule(dev, &smi, nullptr, &module);
    VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cpi.stage.module = module; cpi.stage.pName = "main"; cpi.layout = layout;
    VkPipeline pipe{};
    if (!ok(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipe))) { std::puts("FAIL: pipeline"); return 1; }
    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.queueFamilyIndex = family;
    VkCommandPool pool{};
    vkCreateCommandPool(dev, &cpci, nullptr, &pool);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool; cai.commandBufferCount = 1;
    VkCommandBuffer cmd{};
    vkAllocateCommandBuffers(dev, &cai, &cmd);
    VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cmd, &cbi);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    for (std::uint32_t k = 0; k < ncases; ++k) {
        const VkDeviceAddress a = base + 32ull * k;
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, &a);
        vkCmdDispatch(cmd, 1, 1, 1);
    }
    // The clamped slots are what an indirect dispatch reads: one dispatches nothing (no shader bound
    // to it would run), checked only for validity of the indirect read.
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    bool pass = true;
    for (std::uint32_t k = 0; k < ncases; ++k) {
        const std::uint32_t* s = map + k * 8;
        const Case& c = cases[k];
        const bool good = c.clamped ? (s[0] == 0 && s[1] == 0 && s[2] == 0 && s[3] == 1) : (s[0] == c.x && s[1] == c.y && s[2] == c.z && s[3] == 0);
        const bool kept = s[4] == c.x && s[5] == c.y && s[6] == c.z;
        std::printf("%s %ux%ux%u -> %ux%ux%u flag %u (kept %ux%ux%u)\n", good && kept ? "PASS" : "FAIL", c.x, c.y, c.z, s[0], s[1], s[2], s[3], s[4],
                    s[5], s[6]);
        pass = pass && good && kept;
    }
    vkDestroyPipeline(dev, pipe, nullptr); vkDestroyShaderModule(dev, module, nullptr); vkDestroyPipelineLayout(dev, layout, nullptr);
    vkDestroyCommandPool(dev, pool, nullptr); vkUnmapMemory(dev, mem); vkDestroyBuffer(dev, buf, nullptr); vkFreeMemory(dev, mem, nullptr);
    vkDestroyDevice(dev, nullptr); vkDestroyInstance(instance, nullptr);
    return pass ? 0 : 1;
}
