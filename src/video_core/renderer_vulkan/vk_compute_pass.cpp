// Copyright 2019 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cstring>
#include <memory>
#include <optional>
#include <utility>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/common_types.h"
#include "video_core/host_shaders/vulkan_quad_array_comp_spv.h"
#include "video_core/host_shaders/vulkan_quad_indexed_comp_spv.h"
#include "video_core/host_shaders/vulkan_uint8_comp_spv.h"
#include "video_core/renderer_vulkan/vk_compute_pass.h"
#include "video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "video_core/renderer_vulkan/vk_device.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/renderer_vulkan/wrapper.h"

namespace Vulkan {

namespace {

VkDescriptorSetLayoutBinding BuildQuadArrayPassDescriptorSetLayoutBinding() {
    return {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .pImmutableSamplers = nullptr,
    };
}

VkDescriptorUpdateTemplateEntryKHR BuildQuadArrayPassDescriptorUpdateTemplateEntry() {
    return {
        .dstBinding = 0,
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .offset = 0,
        .stride = sizeof(DescriptorUpdateEntry),
    };
}

VkPushConstantRange BuildComputePushConstantRange(std::size_t size) {
    return {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = static_cast<u32>(size),
    };
}

std::array<VkDescriptorSetLayoutBinding, 2> BuildInputOutputDescriptorSetBindings() {
    return {{
        {
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .pImmutableSamplers = nullptr,
        },
        {
            .binding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .pImmutableSamplers = nullptr,
        },
    }};
}

VkDescriptorUpdateTemplateEntryKHR BuildInputOutputDescriptorUpdateTemplate() {
    return {
        .dstBinding = 0,
        .dstArrayElement = 0,
        .descriptorCount = 2,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .offset = 0,
        .stride = sizeof(DescriptorUpdateEntry),
    };
}

} // Anonymous namespace

VKComputePass::VKComputePass(const VKDevice& device, VKDescriptorPool& descriptor_pool,
                             vk::Span<VkDescriptorSetLayoutBinding> bindings,
                             vk::Span<VkDescriptorUpdateTemplateEntryKHR> templates,
                             vk::Span<VkPushConstantRange> push_constants,
                             std::span<const u32> code) {
    descriptor_set_layout = device.GetLogical().CreateDescriptorSetLayout({
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .bindingCount = bindings.size(),
        .pBindings = bindings.data(),
    });
    layout = device.GetLogical().CreatePipelineLayout({
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = 1,
        .pSetLayouts = descriptor_set_layout.address(),
        .pushConstantRangeCount = push_constants.size(),
        .pPushConstantRanges = push_constants.data(),
    });
    if (!templates.empty()) {
        descriptor_template = device.GetLogical().CreateDescriptorUpdateTemplateKHR({
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO_KHR,
            .pNext = nullptr,
            .flags = 0,
            .descriptorUpdateEntryCount = templates.size(),
            .pDescriptorUpdateEntries = templates.data(),
            .templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET_KHR,
            .descriptorSetLayout = *descriptor_set_layout,
            .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .pipelineLayout = *layout,
            .set = 0,
        });

        descriptor_allocator.emplace(descriptor_pool, *descriptor_set_layout);
    }
    module = device.GetLogical().CreateShaderModule({
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .codeSize = static_cast<u32>(code.size_bytes()),
        .pCode = code.data(),
    });
    pipeline = device.GetLogical().CreateComputePipeline({
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .stage =
            {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = *module,
                .pName = "main",
                .pSpecializationInfo = nullptr,
            },
        .layout = *layout,
        .basePipelineHandle = nullptr,
        .basePipelineIndex = 0,
    });
}

VKComputePass::~VKComputePass() = default;

VkDescriptorSet VKComputePass::CommitDescriptorSet(
    VKUpdateDescriptorQueue& update_descriptor_queue) {
    if (!descriptor_template) {
        return nullptr;
    }
    const VkDescriptorSet set = descriptor_allocator->Commit();
    update_descriptor_queue.Send(*descriptor_template, set);
    return set;
}

QuadArrayPass::QuadArrayPass(const VKDevice& device_, VKScheduler& scheduler_,
                             VKDescriptorPool& descriptor_pool_,
                             VKStagingBufferPool& staging_buffer_pool_,
                             VKUpdateDescriptorQueue& update_descriptor_queue_)
    : VKComputePass(device_, descriptor_pool_, BuildQuadArrayPassDescriptorSetLayoutBinding(),
                    BuildQuadArrayPassDescriptorUpdateTemplateEntry(),
                    BuildComputePushConstantRange(sizeof(u32)), VULKAN_QUAD_ARRAY_COMP_SPV),
      scheduler{scheduler_}, staging_buffer_pool{staging_buffer_pool_},
      update_descriptor_queue{update_descriptor_queue_} {}

QuadArrayPass::~QuadArrayPass() = default;

std::pair<VkBuffer, VkDeviceSize> QuadArrayPass::Assemble(u32 num_vertices, u32 first) {
    const u32 num_triangle_vertices = (num_vertices / 4) * 6;
    const std::size_t staging_size = num_triangle_vertices * sizeof(u32);
    auto& buffer = staging_buffer_pool.GetUnusedBuffer(staging_size, false);

    update_descriptor_queue.Acquire();
    update_descriptor_queue.AddBuffer(*buffer.handle, 0, staging_size);
    const VkDescriptorSet set = CommitDescriptorSet(update_descriptor_queue);

    scheduler.RequestOutsideRenderPassOperationContext();

    ASSERT(num_vertices % 4 == 0);
    const u32 num_quads = num_vertices / 4;
    scheduler.Record([layout = *layout, pipeline = *pipeline, buffer = *buffer.handle, num_quads,
                      first, set](vk::CommandBuffer cmdbuf) {
        constexpr u32 dispatch_size = 1024;
        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, set, {});
        cmdbuf.PushConstants(layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(first), &first);
        cmdbuf.Dispatch(Common::AlignUp(num_quads, dispatch_size) / dispatch_size, 1, 1);

        VkBufferMemoryBarrier barrier;
        barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        barrier.pNext = nullptr;
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer = buffer;
        barrier.offset = 0;
        barrier.size = static_cast<VkDeviceSize>(num_quads) * 6 * sizeof(u32);
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, {}, {barrier}, {});
    });
    return {*buffer.handle, 0};
}

Uint8Pass::Uint8Pass(const VKDevice& device, VKScheduler& scheduler_,
                     VKDescriptorPool& descriptor_pool, VKStagingBufferPool& staging_buffer_pool_,
                     VKUpdateDescriptorQueue& update_descriptor_queue_)
    : VKComputePass(device, descriptor_pool, BuildInputOutputDescriptorSetBindings(),
                    BuildInputOutputDescriptorUpdateTemplate(), {}, VULKAN_UINT8_COMP_SPV),
      scheduler{scheduler_}, staging_buffer_pool{staging_buffer_pool_},
      update_descriptor_queue{update_descriptor_queue_} {}

Uint8Pass::~Uint8Pass() = default;

std::pair<VkBuffer, u64> Uint8Pass::Assemble(u32 num_vertices, VkBuffer src_buffer,
                                             u64 src_offset) {
    const u32 staging_size = static_cast<u32>(num_vertices * sizeof(u16));
    auto& buffer = staging_buffer_pool.GetUnusedBuffer(staging_size, false);

    update_descriptor_queue.Acquire();
    update_descriptor_queue.AddBuffer(src_buffer, src_offset, num_vertices);
    update_descriptor_queue.AddBuffer(*buffer.handle, 0, staging_size);
    const VkDescriptorSet set = CommitDescriptorSet(update_descriptor_queue);

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([layout = *layout, pipeline = *pipeline, buffer = *buffer.handle, set,
                      num_vertices](vk::CommandBuffer cmdbuf) {
        constexpr u32 dispatch_size = 1024;
        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, set, {});
        cmdbuf.Dispatch(Common::AlignUp(num_vertices, dispatch_size) / dispatch_size, 1, 1);

        VkBufferMemoryBarrier barrier;
        barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        barrier.pNext = nullptr;
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer = buffer;
        barrier.offset = 0;
        barrier.size = static_cast<VkDeviceSize>(num_vertices * sizeof(u16));
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, {}, barrier, {});
    });
    return {*buffer.handle, 0};
}

QuadIndexedPass::QuadIndexedPass(const VKDevice& device_, VKScheduler& scheduler_,
                                 VKDescriptorPool& descriptor_pool_,
                                 VKStagingBufferPool& staging_buffer_pool_,
                                 VKUpdateDescriptorQueue& update_descriptor_queue_)
    : VKComputePass(device_, descriptor_pool_, BuildInputOutputDescriptorSetBindings(),
                    BuildInputOutputDescriptorUpdateTemplate(),
                    BuildComputePushConstantRange(sizeof(u32) * 2), VULKAN_QUAD_INDEXED_COMP_SPV),
      scheduler{scheduler_}, staging_buffer_pool{staging_buffer_pool_},
      update_descriptor_queue{update_descriptor_queue_} {}

QuadIndexedPass::~QuadIndexedPass() = default;

std::pair<VkBuffer, u64> QuadIndexedPass::Assemble(
    Tegra::Engines::Maxwell3D::Regs::IndexFormat index_format, u32 num_vertices, u32 base_vertex,
    VkBuffer src_buffer, u64 src_offset) {
    const u32 index_shift = [index_format] {
        switch (index_format) {
        case Tegra::Engines::Maxwell3D::Regs::IndexFormat::UnsignedByte:
            return 0;
        case Tegra::Engines::Maxwell3D::Regs::IndexFormat::UnsignedShort:
            return 1;
        case Tegra::Engines::Maxwell3D::Regs::IndexFormat::UnsignedInt:
            return 2;
        }
        UNREACHABLE();
        return 2;
    }();
    const u32 input_size = num_vertices << index_shift;
    const u32 num_tri_vertices = (num_vertices / 4) * 6;

    const std::size_t staging_size = num_tri_vertices * sizeof(u32);
    auto& buffer = staging_buffer_pool.GetUnusedBuffer(staging_size, false);

    update_descriptor_queue.Acquire();
    update_descriptor_queue.AddBuffer(src_buffer, src_offset, input_size);
    update_descriptor_queue.AddBuffer(*buffer.handle, 0, staging_size);
    const VkDescriptorSet set = CommitDescriptorSet(update_descriptor_queue);

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([layout = *layout, pipeline = *pipeline, buffer = *buffer.handle, set,
                      num_tri_vertices, base_vertex, index_shift](vk::CommandBuffer cmdbuf) {
        static constexpr u32 dispatch_size = 1024;
        const std::array push_constants = {base_vertex, index_shift};
        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, set, {});
        cmdbuf.PushConstants(layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push_constants),
                             &push_constants);
        cmdbuf.Dispatch(Common::AlignUp(num_tri_vertices, dispatch_size) / dispatch_size, 1, 1);

        VkBufferMemoryBarrier barrier;
        barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        barrier.pNext = nullptr;
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer = buffer;
        barrier.offset = 0;
        barrier.size = static_cast<VkDeviceSize>(num_tri_vertices * sizeof(u32));
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, {}, barrier, {});
    });
    return {*buffer.handle, 0};
}

} // namespace Vulkan
