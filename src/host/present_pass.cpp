// The presenter's one pass over the swapchain image (host/present_pass.h).
// One pipeline, one descriptor set for the display image, dynamic rendering
// so nothing has to follow the swapchain's images but their views.

#include "host/present_pass.h"

#include "host/present_spv.h"
#include "log.h"

namespace {

VkDevice g_device = VK_NULL_HANDLE;
VkPhysicalDevice g_phys = VK_NULL_HANDLE;
VkFormat g_format = VK_FORMAT_UNDEFINED;  // the swapchain's, the pipeline's attachment
VkDescriptorSetLayout g_dsl = VK_NULL_HANDLE;
VkPipelineLayout g_layout = VK_NULL_HANDLE;
VkPipeline g_pipeline = VK_NULL_HANDLE;
VkSampler g_sampler = VK_NULL_HANDLE;
VkDescriptorPool g_pool = VK_NULL_HANDLE;
VkDescriptorSet g_set = VK_NULL_HANDLE;
// The display image's view for this frame. Made each frame, as FSR's is: the
// game's display images are render targets the renderer re-creates when the
// picture's size changes, and a view kept across frames could outlive its
// image (a handle the driver then hands to the next image made).
VkImageView g_source_view = VK_NULL_HANDLE;
// The last source format asked about, and whether it filters linearly.
VkFormat g_checked = VK_FORMAT_UNDEFINED;
bool g_checked_ok = false;

VkShaderModule make_module(VkDevice device, const std::uint32_t* words, std::size_t count) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = count * 4;
    ci.pCode = words;
    VkShaderModule m = VK_NULL_HANDLE;
    return vkCreateShaderModule(device, &ci, nullptr, &m) == VK_SUCCESS ? m : VK_NULL_HANDLE;
}

bool build(VkDevice device) {
    VkDescriptorSetLayoutBinding b{};
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dslci.bindingCount = 1;
    dslci.pBindings = &b;
    if (vkCreateDescriptorSetLayout(device, &dslci, nullptr, &g_dsl) != VK_SUCCESS) return false;

    VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float) * 4};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &g_dsl;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(device, &plci, nullptr, &g_layout) != VK_SUCCESS) return false;

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;  // vkCmdBlitImage's VK_FILTER_LINEAR
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 0.0f;
    if (vkCreateSampler(device, &sci, nullptr, &g_sampler) != VK_SUCCESS) return false;

    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(device, &dpci, nullptr, &g_pool) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = g_pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &g_dsl;
    if (vkAllocateDescriptorSets(device, &dsai, &g_set) != VK_SUCCESS) return false;

    VkShaderModule vs = make_module(device, present_spv::kVert, sizeof(present_spv::kVert) / 4);
    VkShaderModule fs = make_module(device, present_spv::kFrag, sizeof(present_spv::kFrag) / 4);
    if (!vs || !fs) {
        if (vs) vkDestroyShaderModule(device, vs, nullptr);
        if (fs) vkDestroyShaderModule(device, fs, nullptr);
        return false;
    }
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = 0xf;  // every channel, as the blit wrote them
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo prci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    prci.colorAttachmentCount = 1;
    prci.pColorAttachmentFormats = &g_format;
    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.pNext = &prci;
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &ds;
    gp.layout = g_layout;
    const VkResult r = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp, nullptr, &g_pipeline);
    vkDestroyShaderModule(device, vs, nullptr);
    vkDestroyShaderModule(device, fs, nullptr);
    return r == VK_SUCCESS;
}

bool source_filters(VkFormat format) {
    if (format != g_checked) {
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(g_phys, format, &fp);
        const VkFormatFeatureFlags want = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        g_checked = format;
        g_checked_ok = (fp.optimalTilingFeatures & want) == want;
        if (!g_checked_ok) host_log("present: the display format %d does not filter linearly; it is blitted", static_cast<int>(format));
    }
    return g_checked_ok;
}

}  // namespace

bool present_pass_init(VkDevice device, VkPhysicalDevice phys, VkFormat colour_format) {
    if (g_pipeline != VK_NULL_HANDLE && g_format == colour_format && g_device == device) return true;
    present_pass_shutdown(g_device ? g_device : device);
    g_device = device;
    g_phys = phys;
    g_format = colour_format;
    if (!build(device)) {
        host_log("present: the presenting pass could not be made; the picture is blitted");
        present_pass_shutdown(device);  // tried again with the next swapchain
        return false;
    }
    return true;
}

void present_pass_shutdown(VkDevice device) {
    if (!device) return;
    if (g_source_view) vkDestroyImageView(device, g_source_view, nullptr);
    if (g_pipeline) vkDestroyPipeline(device, g_pipeline, nullptr);
    if (g_layout) vkDestroyPipelineLayout(device, g_layout, nullptr);
    if (g_pool) vkDestroyDescriptorPool(device, g_pool, nullptr);
    if (g_dsl) vkDestroyDescriptorSetLayout(device, g_dsl, nullptr);
    if (g_sampler) vkDestroySampler(device, g_sampler, nullptr);
    g_source_view = VK_NULL_HANDLE;
    g_pipeline = VK_NULL_HANDLE;
    g_layout = VK_NULL_HANDLE;
    g_pool = VK_NULL_HANDLE;
    g_set = VK_NULL_HANDLE;
    g_dsl = VK_NULL_HANDLE;
    g_sampler = VK_NULL_HANDLE;
    g_device = VK_NULL_HANDLE;
    g_format = VK_FORMAT_UNDEFINED;
    g_checked = VK_FORMAT_UNDEFINED;
}

bool present_pass_ready() { return g_pipeline != VK_NULL_HANDLE; }

bool present_pass_begin(VkCommandBuffer cmd, VkImage dst, VkImageView dst_view, VkExtent2D extent, VkRect2D shown,
                        const PresentSource* src, const float clear[4]) {
    if (!g_pipeline || !dst_view) return false;
    if (src && (!src->image || !src->width || !src->height || !src->w || !src->h || !source_filters(src->format))) return false;
    // Last frame's view: its commands are done (the caller waited for the
    // presenter's fence).
    if (g_source_view) {
        vkDestroyImageView(g_device, g_source_view, nullptr);
        g_source_view = VK_NULL_HANDLE;
    }
    if (src) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = src->image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = src->format;  // the blit's conversions: an sRGB target decodes, as it did
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(g_device, &vci, nullptr, &g_source_view) != VK_SUCCESS) {
            g_source_view = VK_NULL_HANDLE;
            return false;
        }
        VkDescriptorImageInfo dii{g_sampler, g_source_view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = g_set;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &dii;
        vkUpdateDescriptorSets(g_device, 1, &w, 0, nullptr);
    }
    // The renderer's writes to the display image are in earlier submissions
    // on this queue; submission order alone does not make them visible to
    // this read. The swapchain image's old contents are not wanted: from
    // UNDEFINED, after the stage the acquire is waited at.
    VkMemoryBarrier written{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    written.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    written.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    VkImageMemoryBarrier to_colour{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_colour.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_colour.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_colour.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_colour.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_colour.image = dst;
    to_colour.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    to_colour.srcAccessMask = 0;
    to_colour.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, src ? 1 : 0, &written, 0,
                         nullptr, 1, &to_colour);
    // Every pixel is drawn when the picture fills the image: nothing to load
    // or clear. Bars, or no picture at all, are the clear.
    const bool covers = src && shown.offset.x <= 0 && shown.offset.y <= 0 &&
                        static_cast<std::int64_t>(shown.offset.x) + shown.extent.width >= extent.width &&
                        static_cast<std::int64_t>(shown.offset.y) + shown.extent.height >= extent.height;
    VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    att.imageView = dst_view;
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = covers ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    for (int i = 0; i < 4; ++i) att.clearValue.color.float32[i] = clear[i];
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, extent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;
    vkCmdBeginRendering(cmd, &ri);
    if (src) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_layout, 0, 1, &g_set, 0, nullptr);
        const VkViewport vp{static_cast<float>(shown.offset.x), static_cast<float>(shown.offset.y), static_cast<float>(shown.extent.width),
                            static_cast<float>(shown.extent.height), 0.0f, 1.0f};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &shown);
        const float iw = 1.0f / static_cast<float>(src->width), ih = 1.0f / static_cast<float>(src->height);
        const float uv[4] = {static_cast<float>(src->x) * iw, static_cast<float>(src->y) * ih, static_cast<float>(src->x + src->w) * iw,
                             static_cast<float>(src->y + src->h) * ih};
        vkCmdPushConstants(cmd, g_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(uv), uv);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }
    return true;
}

void present_pass_end(VkCommandBuffer cmd, VkImage dst) {
    vkCmdEndRendering(cmd);
    VkImageMemoryBarrier to_present{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_present.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    to_present.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_present.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_present.image = dst;
    to_present.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    to_present.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    to_present.dstAccessMask = 0;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr,
                         1, &to_present);
}
