/*
 * Copyright (c) 2020-2025 The Khronos Group Inc.
 * Copyright (c) 2020-2025 Valve Corporation
 * Copyright (c) 2020-2025 LunarG, Inc.
 * Copyright (c) 2020-2025 Google, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

#include <vulkan/vulkan_core.h>
#include "../framework/layer_validation_tests.h"
#include "../framework/pipeline_helper.h"
#include "../framework/shader_object_helper.h"
#include "../framework/descriptor_helper.h"
#include "../framework/gpu_av_helper.h"

class NegativeGpuAV : public GpuAVTest {};

TEST_F(NegativeGpuAV, ValidationAbort) {
    TEST_DESCRIPTION("GPU validation: Verify that aborting GPU-AV is safe.");
    // GPU Shader Instrumentation requires Vulkan 1.1 or later
    SetTargetApiVersion(VK_API_VERSION_1_0);
    VkValidationFeaturesEXT validation_features = GetGpuAvValidationFeatures();
    RETURN_IF_SKIP(InitFramework(&validation_features));
    m_errorMonitor->SetDesiredError("GPU-AV is being disabled");
    RETURN_IF_SKIP(InitState());
    m_errorMonitor->VerifyFound();

    // Still make sure we can use Vulkan as expected without errors
    InitRenderTarget();

    CreateComputePipelineHelper pipe(*this);
    pipe.CreateComputePipeline();

    m_command_buffer.Begin();
    vk::CmdBindPipeline(m_command_buffer.handle(), VK_PIPELINE_BIND_POINT_COMPUTE, pipe.Handle());
    vk::CmdDispatch(m_command_buffer.handle(), 1, 1, 1);
    m_command_buffer.End();

    m_default_queue->Submit(m_command_buffer);
    m_default_queue->Wait();
}

TEST_F(NegativeGpuAV, ValidationFeatures) {
    TEST_DESCRIPTION("Validate Validation Features");
    SetTargetApiVersion(VK_API_VERSION_1_1);
    AddRequiredExtensions(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
    VkValidationFeatureEnableEXT enables[] = {VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_RESERVE_BINDING_SLOT_EXT};
    VkValidationFeaturesEXT features = vku::InitStructHelper();
    features.enabledValidationFeatureCount = 1;
    features.pEnabledValidationFeatures = enables;

    auto ici = GetInstanceCreateInfo();
    features.pNext = ici.pNext;
    ici.pNext = &features;
    VkInstance instance;
    m_errorMonitor->SetDesiredError("VUID-VkValidationFeaturesEXT-pEnabledValidationFeatures-02967");
    vk::CreateInstance(&ici, nullptr, &instance);
    m_errorMonitor->VerifyFound();
}

TEST_F(NegativeGpuAV, SelectInstrumentedShaders) {
    TEST_DESCRIPTION("GPU validation: Validate selection of which shaders get instrumented for GPU-AV");
    SetTargetApiVersion(VK_API_VERSION_1_2);
    AddRequiredFeature(vkt::Feature::vertexPipelineStoresAndAtomics);

    std::vector<VkLayerSettingEXT> layer_settings = {
        {OBJECT_LAYER_NAME, "gpuav_select_instrumented_shaders", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &kVkTrue}};
    RETURN_IF_SKIP(InitGpuAvFramework(layer_settings));
    RETURN_IF_SKIP(InitState());
    InitRenderTarget();

    vkt::Buffer write_buffer(*m_device, 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, kHostVisibleMemProps);
    OneOffDescriptorSet descriptor_set(m_device, {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr}});

    const vkt::PipelineLayout pipeline_layout(*m_device, {&descriptor_set.layout_});
    descriptor_set.WriteDescriptorBufferInfo(0, write_buffer.handle(), 0, 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    descriptor_set.UpdateDescriptorSets();
    static const char vertshader[] = R"glsl(
        #version 450
        layout(set = 0, binding = 0) buffer StorageBuffer { uint data[]; } Data;
        void main() {
                Data.data[4] = 0xdeadca71;
        }
        )glsl";

    VkValidationFeatureEnableEXT enabled[] = {VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT};
    VkValidationFeaturesEXT features = vku::InitStructHelper();
    features.enabledValidationFeatureCount = 1;
    features.pEnabledValidationFeatures = enabled;
    VkShaderObj vs(this, vertshader, VK_SHADER_STAGE_VERTEX_BIT, SPV_ENV_VULKAN_1_0, SPV_SOURCE_GLSL, nullptr, "main", &features);
    CreatePipelineHelper pipe(*this);
    pipe.shader_stages_[0] = vs.GetStageCreateInfo();
    pipe.gp_ci_.layout = pipeline_layout.handle();
    pipe.CreateGraphicsPipeline();

    m_command_buffer.Begin();
    vk::CmdBindPipeline(m_command_buffer.handle(), VK_PIPELINE_BIND_POINT_GRAPHICS, pipe.Handle());
    m_command_buffer.BeginRenderPass(m_renderPassBeginInfo);
    vk::CmdBindDescriptorSets(m_command_buffer.handle(), VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout.handle(), 0, 1,
                              &descriptor_set.set_, 0, nullptr);
    vk::CmdDraw(m_command_buffer.handle(), 3, 1, 0, 0);
    m_command_buffer.EndRenderPass();
    m_command_buffer.End();

    m_errorMonitor->SetDesiredError("VUID-vkCmdDraw-storageBuffers-06936", 3);
    m_default_queue->Submit(m_command_buffer);
    m_default_queue->Wait();
    m_errorMonitor->VerifyFound();
}

TEST_F(NegativeGpuAV, SelectInstrumentedShadersRegex) {
    TEST_DESCRIPTION(
        "Selectively instrument shaders for validation, using regexes: all shaders matching regexes must be instrumented. Here it "
        "means they should emit errors");
    SetTargetApiVersion(VK_API_VERSION_1_2);
    AddRequiredExtensions(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    AddRequiredFeature(vkt::Feature::vertexPipelineStoresAndAtomics);
    std::vector<VkLayerSettingEXT> layer_settings(2);
    layer_settings[0] = {OBJECT_LAYER_NAME, "gpuav_select_instrumented_shaders", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &kVkTrue};
    std::array<const char *, 2> shader_regexes = {{"vertex_foo", "fragment_.*"}};
    layer_settings[1] = {OBJECT_LAYER_NAME, "gpuav_shaders_to_instrument", VK_LAYER_SETTING_TYPE_STRING_EXT, size32(shader_regexes),
                         shader_regexes.data()};

    RETURN_IF_SKIP(InitGpuAvFramework(layer_settings));
    InitState();
    InitRenderTarget();

    vkt::Buffer write_buffer(*m_device, 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, kHostVisibleMemProps);
    OneOffDescriptorSet descriptor_set(m_device, {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr}});

    const vkt::PipelineLayout pipeline_layout(*m_device, {&descriptor_set.layout_});
    descriptor_set.WriteDescriptorBufferInfo(0, write_buffer.handle(), 0, 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    descriptor_set.UpdateDescriptorSets();
    static const char vertshader[] = R"glsl(
        #version 450
        layout(set = 0, binding = 0) buffer StorageBuffer { uint data[]; } Data;
        void main() {
                Data.data[4] = 0xdeadca71;
        }
        )glsl";

    VkShaderObj vs(this, vertshader, VK_SHADER_STAGE_VERTEX_BIT, SPV_ENV_VULKAN_1_0, SPV_SOURCE_GLSL, nullptr, "main");
    VkShaderObj fs(this, kFragmentMinimalGlsl, VK_SHADER_STAGE_FRAGMENT_BIT, SPV_ENV_VULKAN_1_0, SPV_SOURCE_GLSL, nullptr, "main");

    VkDebugUtilsObjectNameInfoEXT name_info = vku::InitStructHelper();
    name_info.objectType = VK_OBJECT_TYPE_SHADER_MODULE;

    name_info.pObjectName = "vertex_foo";
    name_info.objectHandle = uint64_t(vs.handle());
    vk::SetDebugUtilsObjectNameEXT(device(), &name_info);

    name_info.pObjectName = "fragment_bar";
    name_info.objectHandle = uint64_t(fs.handle());
    vk::SetDebugUtilsObjectNameEXT(device(), &name_info);

    CreatePipelineHelper pipe(*this);
    pipe.shader_stages_ = {vs.GetStageCreateInfo(), fs.GetStageCreateInfo()};

    pipe.gp_ci_.layout = pipeline_layout.handle();
    m_errorMonitor->SetDesiredInfo("vertex_foo");
    m_errorMonitor->SetDesiredInfo("fragment_bar");
    pipe.CreateGraphicsPipeline();
    m_errorMonitor->VerifyFound();

    m_command_buffer.Begin();
    vk::CmdBindPipeline(m_command_buffer.handle(), VK_PIPELINE_BIND_POINT_GRAPHICS, pipe.Handle());
    m_command_buffer.BeginRenderPass(m_renderPassBeginInfo);
    vk::CmdBindDescriptorSets(m_command_buffer.handle(), VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout.handle(), 0, 1,
                              &descriptor_set.set_, 0, nullptr);
    vk::CmdDraw(m_command_buffer.handle(), 3, 1, 0, 0);
    m_command_buffer.EndRenderPass();
    m_command_buffer.End();

    m_errorMonitor->SetDesiredError("VUID-vkCmdDraw-storageBuffers-06936", 3);
    m_default_queue->Submit(m_command_buffer);
    m_default_queue->Wait();
    m_errorMonitor->VerifyFound();
}

TEST_F(NegativeGpuAV, SelectInstrumentedShadersRegexDestroyedShaders) {
    TEST_DESCRIPTION(
        "Selectively instrument shaders for validation, using regexes: all shaders matching regexes must be instrumented. Here it "
        "means they should emit errors. Shaders are destroyed after creating pipeline, it should have no impact.");
    SetTargetApiVersion(VK_API_VERSION_1_2);
    AddRequiredExtensions(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    AddRequiredFeature(vkt::Feature::vertexPipelineStoresAndAtomics);
    std::vector<VkLayerSettingEXT> layer_settings(2);
    layer_settings[0] = {OBJECT_LAYER_NAME, "gpuav_select_instrumented_shaders", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &kVkTrue};
    std::array<const char *, 2> shader_regexes = {{"vertex_foo", "fragment_.*"}};
    layer_settings[1] = {OBJECT_LAYER_NAME, "gpuav_shaders_to_instrument", VK_LAYER_SETTING_TYPE_STRING_EXT, size32(shader_regexes),
                         shader_regexes.data()};

    RETURN_IF_SKIP(InitGpuAvFramework(layer_settings));
    InitState();
    InitRenderTarget();

    vkt::Buffer write_buffer(*m_device, 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, kHostVisibleMemProps);
    OneOffDescriptorSet descriptor_set(m_device, {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr}});

    const vkt::PipelineLayout pipeline_layout(*m_device, {&descriptor_set.layout_});
    descriptor_set.WriteDescriptorBufferInfo(0, write_buffer.handle(), 0, 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    descriptor_set.UpdateDescriptorSets();
    static const char vertshader[] = R"glsl(
        #version 450
        layout(set = 0, binding = 0) buffer StorageBuffer { uint data[]; } Data;
        void main() {
            Data.data[4] = 0xdeadca71;
        }
    )glsl";

    VkShaderObj vs(this, vertshader, VK_SHADER_STAGE_VERTEX_BIT, SPV_ENV_VULKAN_1_0, SPV_SOURCE_GLSL, nullptr, "main");
    VkShaderObj fs(this, kFragmentMinimalGlsl, VK_SHADER_STAGE_FRAGMENT_BIT, SPV_ENV_VULKAN_1_0, SPV_SOURCE_GLSL, nullptr, "main");

    VkDebugUtilsObjectNameInfoEXT name_info = vku::InitStructHelper();
    name_info.objectType = VK_OBJECT_TYPE_SHADER_MODULE;

    name_info.pObjectName = "vertex_foo";
    name_info.objectHandle = uint64_t(vs.handle());
    vk::SetDebugUtilsObjectNameEXT(device(), &name_info);

    name_info.pObjectName = "fragment_bar";
    name_info.objectHandle = uint64_t(fs.handle());
    vk::SetDebugUtilsObjectNameEXT(device(), &name_info);

    CreatePipelineHelper pipe(*this);
    pipe.shader_stages_ = {vs.GetStageCreateInfo(), fs.GetStageCreateInfo()};

    pipe.gp_ci_.layout = pipeline_layout.handle();
    m_errorMonitor->SetDesiredInfo("vertex_foo");
    m_errorMonitor->SetDesiredInfo("fragment_bar");
    pipe.CreateGraphicsPipeline();
    m_errorMonitor->VerifyFound();
    vs.destroy();
    fs.destroy();

    m_command_buffer.Begin();
    vk::CmdBindPipeline(m_command_buffer.handle(), VK_PIPELINE_BIND_POINT_GRAPHICS, pipe.Handle());
    m_command_buffer.BeginRenderPass(m_renderPassBeginInfo);
    vk::CmdBindDescriptorSets(m_command_buffer.handle(), VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout.handle(), 0, 1,
                              &descriptor_set.set_, 0, nullptr);
    vk::CmdDraw(m_command_buffer.handle(), 3, 1, 0, 0);
    m_command_buffer.EndRenderPass();
    m_command_buffer.End();

    m_errorMonitor->SetDesiredError("VUID-vkCmdDraw-storageBuffers-06936", 3);
    m_default_queue->Submit(m_command_buffer);
    m_default_queue->Wait();
    m_errorMonitor->VerifyFound();
}

TEST_F(NegativeGpuAV, SelectInstrumentedShadersShaderObject) {
    TEST_DESCRIPTION("GPU validation: Validate selection of which shaders get instrumented for GPU-AV");
    SetTargetApiVersion(VK_API_VERSION_1_2);
    AddRequiredExtensions(VK_EXT_SHADER_OBJECT_EXTENSION_NAME);
    AddRequiredExtensions(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
    AddRequiredFeature(vkt::Feature::dynamicRendering);
    AddRequiredFeature(vkt::Feature::shaderObject);
    AddRequiredFeature(vkt::Feature::vertexPipelineStoresAndAtomics);

    std::vector<VkLayerSettingEXT> layer_settings = {
        {OBJECT_LAYER_NAME, "gpuav_select_instrumented_shaders", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &kVkTrue}};
    RETURN_IF_SKIP(InitGpuAvFramework(layer_settings));
    RETURN_IF_SKIP(InitState());
    InitDynamicRenderTarget();

    OneOffDescriptorSet vert_descriptor_set(m_device,
                                            {
                                                {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
                                            });
    vkt::PipelineLayout pipeline_layout(*m_device, {&vert_descriptor_set.layout_});

    static const char vert_src[] = R"glsl(
        #version 460
        layout(set = 0, binding = 0) buffer StorageBuffer { uint data[]; } Data;
        void main() {
            Data.data[4] = 0xdeadca71;
        }
    )glsl";

    const auto vert_spv = GLSLToSPV(VK_SHADER_STAGE_VERTEX_BIT, vert_src);
    const auto frag_spv = GLSLToSPV(VK_SHADER_STAGE_FRAGMENT_BIT, kFragmentMinimalGlsl);

    VkDescriptorSetLayout descriptor_set_layouts[] = {vert_descriptor_set.layout_.handle()};

    VkShaderCreateInfoEXT vert_create_info = ShaderCreateInfo(vert_spv, VK_SHADER_STAGE_VERTEX_BIT, 1, descriptor_set_layouts);
    VkShaderCreateInfoEXT frag_create_info = ShaderCreateInfo(frag_spv, VK_SHADER_STAGE_FRAGMENT_BIT, 1, descriptor_set_layouts);

    VkValidationFeatureEnableEXT enabled[] = {VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT};
    VkValidationFeaturesEXT features = vku::InitStructHelper();
    features.enabledValidationFeatureCount = 1;
    features.pEnabledValidationFeatures = enabled;
    vert_create_info.pNext = &features;
    frag_create_info.pNext = &features;

    const vkt::Shader vert_shader(*m_device, vert_create_info);
    const vkt::Shader frag_shader(*m_device, frag_create_info);

    vkt::Buffer buffer(*m_device, 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vert_descriptor_set.WriteDescriptorBufferInfo(0, buffer.handle(), 0, 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    vert_descriptor_set.UpdateDescriptorSets();

    m_command_buffer.Begin();
    m_command_buffer.BeginRenderingColor(GetDynamicRenderTarget(), GetRenderTargetArea());
    SetDefaultDynamicStatesExclude();
    m_command_buffer.BindShaders(vert_shader, frag_shader);
    vk::CmdBindDescriptorSets(m_command_buffer.handle(), VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout.handle(), 0u, 1u,
                              &vert_descriptor_set.set_, 0u, nullptr);
    vk::CmdDraw(m_command_buffer.handle(), 3, 1, 0, 0);
    m_command_buffer.EndRendering();
    m_command_buffer.End();

    // Should get a warning since shader was instrumented
    m_errorMonitor->SetDesiredError("VUID-vkCmdDraw-None-08613", 3);
    m_default_queue->Submit(m_command_buffer);
    m_default_queue->Wait();
    m_errorMonitor->VerifyFound();
}

TEST_F(NegativeGpuAV, UseAllDescriptorSlotsPipelineNotReserved) {
    TEST_DESCRIPTION("Don't reserve a descriptor slot and proceed to use them all so GPU-AV can't");
    SetTargetApiVersion(VK_API_VERSION_1_2);
    AddRequiredExtensions(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
    AddRequiredExtensions(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
    AddRequiredFeature(vkt::Feature::bufferDeviceAddress);

    // not using VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_RESERVE_BINDING_SLOT_EXT
    const VkValidationFeatureEnableEXT gpu_av_enables = VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT;
    VkValidationFeaturesEXT validation_features = vku::InitStructHelper();
    validation_features.enabledValidationFeatureCount = 1;
    validation_features.pEnabledValidationFeatures = &gpu_av_enables;
    RETURN_IF_SKIP(InitFramework(&validation_features));
    if (!CanEnableGpuAV(*this)) {
        GTEST_SKIP() << "Requirements for GPU-AV are not met";
    }
    RETURN_IF_SKIP(InitState());
    m_errorMonitor->ExpectSuccess(kErrorBit | kWarningBit);

    vkt::Buffer block_buffer(*m_device, 16, 0, vkt::device_address);
    vkt::Buffer in_buffer(*m_device, 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, kHostVisibleMemProps);
    auto data = static_cast<VkDeviceAddress *>(in_buffer.Memory().Map());
    data[0] = block_buffer.Address();
    in_buffer.Memory().Unmap();

    OneOffDescriptorSet descriptor_set(m_device, {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr}});
    descriptor_set.WriteDescriptorBufferInfo(0, in_buffer.handle(), 0, VK_WHOLE_SIZE, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    descriptor_set.UpdateDescriptorSets();

    const uint32_t set_limit = m_device->Physical().limits_.maxBoundDescriptorSets;

    // First try to use too many sets in the pipeline layout
    {
        m_errorMonitor->SetDesiredWarning(
            "This Pipeline Layout has too many descriptor sets that will not allow GPU shader instrumentation to be setup for "
            "pipelines created with it");
        std::vector<const vkt::DescriptorSetLayout *> empty_layouts(set_limit);
        for (uint32_t i = 0; i < set_limit; i++) {
            empty_layouts[i] = &descriptor_set.layout_;
        }
        vkt::PipelineLayout bad_pipe_layout(*m_device, empty_layouts);
        m_errorMonitor->VerifyFound();
    }

    // Reduce by one (so there is room now) and do something invalid. (To make sure things still work as expected)
    std::vector<const vkt::DescriptorSetLayout *> layouts(set_limit - 1);
    for (uint32_t i = 0; i < set_limit - 1; i++) {
        layouts[i] = &descriptor_set.layout_;
    }
    vkt::PipelineLayout pipe_layout(*m_device, layouts);

    char const *shader_source = R"glsl(
        #version 450
        #extension GL_EXT_buffer_reference : enable
        layout(buffer_reference, std430) readonly buffer IndexBuffer {
            int indices[];
        };
        layout(set = 0, binding = 0) buffer foo {
            IndexBuffer data;
            int x;
        };
        void main()  {
            x = data.indices[16];
        }
    )glsl";

    CreateComputePipelineHelper pipe(*this);
    pipe.cs_ = std::make_unique<VkShaderObj>(this, shader_source, VK_SHADER_STAGE_COMPUTE_BIT, SPV_ENV_VULKAN_1_2);
    pipe.cp_ci_.layout = pipe_layout.handle();
    pipe.CreateComputePipeline();

    m_command_buffer.Begin();
    vk::CmdBindDescriptorSets(m_command_buffer.handle(), VK_PIPELINE_BIND_POINT_COMPUTE, pipe_layout.handle(), 0, 1,
                              &descriptor_set.set_, 0, nullptr);
    vk::CmdBindPipeline(m_command_buffer.handle(), VK_PIPELINE_BIND_POINT_COMPUTE, pipe.Handle());
    vk::CmdDispatch(m_command_buffer.handle(), 1, 1, 1);
    m_command_buffer.End();

    m_errorMonitor->SetDesiredError("UNASSIGNED-Device address out of bounds");
    m_default_queue->Submit(m_command_buffer);
    m_default_queue->Wait();
    m_errorMonitor->VerifyFound();
}

TEST_F(NegativeGpuAV, UseAllDescriptorSlotsPipelineReserved) {
    TEST_DESCRIPTION("Reserve a descriptor slot and proceed to use them all anyway so GPU-AV can't");
    SetTargetApiVersion(VK_API_VERSION_1_2);
    AddRequiredExtensions(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
    AddRequiredFeature(vkt::Feature::bufferDeviceAddress);

    RETURN_IF_SKIP(InitGpuAvFramework());
    RETURN_IF_SKIP(InitState());
    m_errorMonitor->ExpectSuccess(kErrorBit | kWarningBit);

    vkt::Buffer index_buffer(*m_device, 16, 0, vkt::device_address);
    vkt::Buffer storage_buffer(*m_device, 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, kHostVisibleMemProps);
    auto data = static_cast<VkDeviceAddress *>(storage_buffer.Memory().Map());
    data[0] = index_buffer.Address();
    storage_buffer.Memory().Unmap();

    OneOffDescriptorSet descriptor_set(m_device, {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr}});
    descriptor_set.WriteDescriptorBufferInfo(0, storage_buffer.handle(), 0, VK_WHOLE_SIZE, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    descriptor_set.UpdateDescriptorSets();

    // Add one to use the descriptor slot we tried to reserve
    const uint32_t set_limit = m_device->Physical().limits_.maxBoundDescriptorSets + 1;

    // First try to use too many sets in the pipeline layout
    {
        m_errorMonitor->SetDesiredWarning(
            "This Pipeline Layout has too many descriptor sets that will not allow GPU shader instrumentation to be setup for "
            "pipelines created with it");
        std::vector<const vkt::DescriptorSetLayout *> empty_layouts(set_limit);
        for (uint32_t i = 0; i < set_limit; i++) {
            empty_layouts[i] = &descriptor_set.layout_;
        }
        vkt::PipelineLayout bad_pipe_layout(*m_device, empty_layouts);
        m_errorMonitor->VerifyFound();
    }

    // Reduce by one (so there is room now) and do something invalid. (To make sure things still work as expected)
    std::vector<const vkt::DescriptorSetLayout *> layouts(set_limit - 1);
    for (uint32_t i = 0; i < set_limit - 1; i++) {
        layouts[i] = &descriptor_set.layout_;
    }
    vkt::PipelineLayout pipe_layout(*m_device, layouts);

    char const *shader_source = R"glsl(
        #version 450
        #extension GL_EXT_buffer_reference : enable
        layout(buffer_reference, std430) readonly buffer IndexBuffer {
            int indices[];
        };
        layout(set = 0, binding = 0) buffer storage_buffer {
            IndexBuffer data;
            int x;
        };
        void main()  {
            x = data.indices[16];
        }
    )glsl";

    CreateComputePipelineHelper pipe(*this);
    pipe.cs_ = std::make_unique<VkShaderObj>(this, shader_source, VK_SHADER_STAGE_COMPUTE_BIT, SPV_ENV_VULKAN_1_2);
    pipe.cp_ci_.layout = pipe_layout.handle();
    pipe.CreateComputePipeline();

    m_command_buffer.Begin();
    vk::CmdBindDescriptorSets(m_command_buffer.handle(), VK_PIPELINE_BIND_POINT_COMPUTE, pipe_layout.handle(), 0, 1,
                              &descriptor_set.set_, 0, nullptr);
    vk::CmdBindPipeline(m_command_buffer.handle(), VK_PIPELINE_BIND_POINT_COMPUTE, pipe.Handle());
    vk::CmdDispatch(m_command_buffer.handle(), 1, 1, 1);
    m_command_buffer.End();

    m_errorMonitor->SetDesiredError("UNASSIGNED-Device address out of bounds");
    m_default_queue->Submit(m_command_buffer);
    m_default_queue->Wait();
    m_errorMonitor->VerifyFound();
}

TEST_F(NegativeGpuAV, ForceUniformAndStorageBuffer8BitAccess) {
    TEST_DESCRIPTION("Make sure that GPU-AV enabled uniformAndStorageBuffer8BitAccess on behalf of app");

    SetTargetApiVersion(VK_API_VERSION_1_1);
    AddRequiredFeature(vkt::Feature::fragmentStoresAndAtomics);
    AddRequiredFeature(vkt::Feature::vertexPipelineStoresAndAtomics);
    AddRequiredFeature(vkt::Feature::shaderInt64);
    RETURN_IF_SKIP(InitGpuAvFramework());

    if (!DeviceExtensionSupported(VK_KHR_8BIT_STORAGE_EXTENSION_NAME)) {
        GTEST_SKIP() << VK_KHR_8BIT_STORAGE_EXTENSION_NAME << " not supported, skipping test";
    }

    VkPhysicalDevice8BitStorageFeaturesKHR eight_bit_storage_features = vku::InitStructHelper();
    VkPhysicalDeviceFeatures2 features_2 = vku::InitStructHelper(&eight_bit_storage_features);
    vk::GetPhysicalDeviceFeatures2(Gpu(), &features_2);
    if (!eight_bit_storage_features.uniformAndStorageBuffer8BitAccess) {
        GTEST_SKIP() << "Required feature uniformAndStorageBuffer8BitAccess is not supported, skipping test";
    }

    m_errorMonitor->SetDesiredWarning(
        "Adding a VkPhysicalDevice8BitStorageFeatures to pNext with uniformAndStorageBuffer8BitAccess set to VK_TRUE");

    // noise
    m_errorMonitor->SetAllowedFailureMsg("Adding a VkPhysicalDevice8BitStorageFeatures to pNext with shaderInt64 set to VK_TRUE");
    m_errorMonitor->SetAllowedFailureMsg(
        "Adding a VkPhysicalDeviceVulkanMemoryModelFeatures to pNext with vulkanMemoryModel and vulkanMemoryModelDeviceScope set "
        "to VK_TRU");
    m_errorMonitor->SetAllowedFailureMsg(
        "Adding a VkPhysicalDeviceTimelineSemaphoreFeatures to pNext with timelineSemaphore set to VK_TRUE");
    m_errorMonitor->SetAllowedFailureMsg(
        "Adding a VkPhysicalDeviceBufferDeviceAddressFeatures to pNext with bufferDeviceAddress set to VK_TRUE");
    m_errorMonitor->SetAllowedFailureMsg(
        "Buffer device address validation option was enabled, but required buffer device address extension and/or features are not "
        "enabled");
    m_errorMonitor->SetAllowedFailureMsg("Ray Query validation option was enabled");
    m_errorMonitor->SetAllowedFailureMsg(
        "vkGetDeviceProcAddr(): pName is trying to grab vkGetPhysicalDeviceCalibrateableTimeDomainsKHR which is an instance level "
        "function");
    RETURN_IF_SKIP(InitState());
    m_errorMonitor->VerifyFound();
}

TEST_F(NegativeGpuAV, CopyBufferToImageD32) {
    TEST_DESCRIPTION(
        "Copy depth buffer to image with some of its depth value being outside of the [0, 1] legal range. Depth image has format "
        "VK_FORMAT_D32_SFLOAT.");

    AddRequiredExtensions(VK_KHR_8BIT_STORAGE_EXTENSION_NAME);
    AddRequiredFeature(vkt::Feature::uniformAndStorageBuffer8BitAccess);

    RETURN_IF_SKIP(InitGpuAvFramework());
    RETURN_IF_SKIP(InitState());

    vkt::Buffer copy_src_buffer(*m_device, sizeof(float) * 64 * 64,
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, kHostVisibleMemProps);

    float *ptr = static_cast<float *>(copy_src_buffer.Memory().Map());
    for (size_t i = 0; i < 64 * 64; ++i) {
        ptr[i] = 0.1f;
    }
    ptr[4094] = 42.0f;
    copy_src_buffer.Memory().Unmap();

    vkt::Image copy_dst_image(*m_device, 64, 64, VK_FORMAT_D32_SFLOAT,
                              VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    copy_dst_image.SetLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    m_command_buffer.Begin();

    VkBufferImageCopy buffer_image_copy_1;
    buffer_image_copy_1.bufferOffset = 0;
    buffer_image_copy_1.bufferRowLength = 0;
    buffer_image_copy_1.bufferImageHeight = 0;
    buffer_image_copy_1.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    buffer_image_copy_1.imageOffset = {0, 0, 0};
    buffer_image_copy_1.imageExtent = {64, 64, 1};

    vk::CmdCopyBufferToImage(m_command_buffer, copy_src_buffer, copy_dst_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                             &buffer_image_copy_1);

    VkBufferImageCopy buffer_image_copy_2 = buffer_image_copy_1;
    buffer_image_copy_2.imageOffset = {32, 32, 0};
    buffer_image_copy_2.imageExtent = {32, 32, 1};

    vk::CmdCopyBufferToImage(m_command_buffer, copy_src_buffer, copy_dst_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                             &buffer_image_copy_2);

    m_command_buffer.End();
    m_errorMonitor->SetDesiredError("has a float value at offset 16376 that is not in the range [0, 1]");
    m_errorMonitor->SetDesiredError("has a float value at offset 16376 that is not in the range [0, 1]");
    m_default_queue->Submit(m_command_buffer);
    m_default_queue->Wait();
    m_errorMonitor->VerifyFound();
}

TEST_F(NegativeGpuAV, CopyBufferToImageD32Vk13) {
    TEST_DESCRIPTION(
        "Copy depth buffer to image with some of its depth value being outside of the [0, 1] legal range. Depth image has format "
        "VK_FORMAT_D32_SFLOAT.");
    SetTargetApiVersion(VK_API_VERSION_1_3);
    AddRequiredExtensions(VK_KHR_8BIT_STORAGE_EXTENSION_NAME);
    AddRequiredFeature(vkt::Feature::uniformAndStorageBuffer8BitAccess);
    RETURN_IF_SKIP(InitGpuAvFramework());
    RETURN_IF_SKIP(InitState());

    vkt::Buffer copy_src_buffer(*m_device, sizeof(float) * 64 * 64,
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, kHostVisibleMemProps);

    float *ptr = static_cast<float *>(copy_src_buffer.Memory().Map());
    for (size_t i = 0; i < 64 * 64; ++i) {
        ptr[i] = 0.1f;
    }
    ptr[4094] = 42.0f;
    copy_src_buffer.Memory().Unmap();

    vkt::Image copy_dst_image(*m_device, 64, 64, VK_FORMAT_D32_SFLOAT,
                              VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    copy_dst_image.SetLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    m_command_buffer.Begin();

    VkBufferImageCopy2 region_1 = vku::InitStructHelper();
    region_1.bufferOffset = 0;
    region_1.bufferRowLength = 0;
    region_1.bufferImageHeight = 0;
    region_1.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    region_1.imageOffset = {0, 0, 0};
    region_1.imageExtent = {64, 64, 1};

    VkCopyBufferToImageInfo2 buffer_image_copy = vku::InitStructHelper();
    buffer_image_copy.srcBuffer = copy_src_buffer;
    buffer_image_copy.dstImage = copy_dst_image;
    buffer_image_copy.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    buffer_image_copy.regionCount = 1;
    buffer_image_copy.pRegions = &region_1;

    vk::CmdCopyBufferToImage2(m_command_buffer, &buffer_image_copy);

    region_1.imageOffset = {32, 32, 0};
    region_1.imageExtent = {32, 32, 1};

    vk::CmdCopyBufferToImage2(m_command_buffer, &buffer_image_copy);

    m_command_buffer.End();
    m_errorMonitor->SetDesiredError("has a float value at offset 16376 that is not in the range [0, 1]");
    m_errorMonitor->SetDesiredError("has a float value at offset 16376 that is not in the range [0, 1]");
    m_default_queue->Submit(m_command_buffer);
    m_default_queue->Wait();
    m_errorMonitor->VerifyFound();
}

TEST_F(NegativeGpuAV, CopyBufferToImageD32U8) {
    TEST_DESCRIPTION(
        "Copy depth buffer to image with some of its depth value being outside of the [0, 1] legal range. Depth image has format "
        "VK_FORMAT_D32_SFLOAT_S8_UINT.");
    AddRequiredExtensions(VK_KHR_8BIT_STORAGE_EXTENSION_NAME);
    AddRequiredFeature(vkt::Feature::uniformAndStorageBuffer8BitAccess);
    RETURN_IF_SKIP(InitGpuAvFramework());
    RETURN_IF_SKIP(InitState());

    vkt::Buffer copy_src_buffer(*m_device, 5 * 64 * 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                kHostVisibleMemProps);

    auto ptr = static_cast<uint8_t *>(copy_src_buffer.Memory().Map());
    std::memset(ptr, 0, static_cast<size_t>(copy_src_buffer.CreateInfo().size));
    for (size_t i = 0; i < 64 * 64; ++i) {
        auto ptr_float = reinterpret_cast<float *>(ptr + 5 * i);
        if (i == 64 * 64 - 1) {
            *ptr_float = 42.0f;
        } else {
            *ptr_float = 0.1f;
        }
    }

    copy_src_buffer.Memory().Unmap();

    vkt::Image copy_dst_image(*m_device, 64, 64, VK_FORMAT_D32_SFLOAT_S8_UINT,
                              VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    copy_dst_image.SetLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    m_command_buffer.Begin();

    VkBufferImageCopy buffer_image_copy;
    buffer_image_copy.bufferOffset = 0;
    buffer_image_copy.bufferRowLength = 0;
    buffer_image_copy.bufferImageHeight = 0;
    buffer_image_copy.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    buffer_image_copy.imageOffset = {33, 33, 0};
    buffer_image_copy.imageExtent = {31, 31, 1};

    vk::CmdCopyBufferToImage(m_command_buffer, copy_src_buffer, copy_dst_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                             &buffer_image_copy);

    m_command_buffer.End();
    m_errorMonitor->SetDesiredError("has a float value at offset 20475 that is not in the range [0, 1]");
    m_default_queue->Submit(m_command_buffer);
    m_default_queue->Wait();
    m_errorMonitor->VerifyFound();
}

TEST_F(NegativeGpuAV, CopyBufferToImageD32U8Vk13) {
    TEST_DESCRIPTION(
        "Copy depth buffer to image with some of its depth value being outside of the [0, 1] legal range. Depth image has format "
        "VK_FORMAT_D32_SFLOAT_S8_UINT.");
    SetTargetApiVersion(VK_API_VERSION_1_3);
    AddRequiredExtensions(VK_KHR_8BIT_STORAGE_EXTENSION_NAME);
    AddRequiredFeature(vkt::Feature::uniformAndStorageBuffer8BitAccess);
    RETURN_IF_SKIP(InitGpuAvFramework());
    RETURN_IF_SKIP(InitState());

    vkt::Buffer copy_src_buffer(*m_device, 5 * 64 * 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                kHostVisibleMemProps);

    auto ptr = static_cast<uint8_t *>(copy_src_buffer.Memory().Map());
    std::memset(ptr, 0, static_cast<size_t>(copy_src_buffer.CreateInfo().size));
    for (size_t i = 0; i < 64 * 64; ++i) {
        auto ptr_float = reinterpret_cast<float *>(ptr + 5 * i);
        if (i == 64 * 64 - 1) {
            *ptr_float = 42.0f;
        } else {
            *ptr_float = 0.1f;
        }
    }

    copy_src_buffer.Memory().Unmap();

    vkt::Image copy_dst_image(*m_device, 64, 64, VK_FORMAT_D32_SFLOAT_S8_UINT,
                              VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    copy_dst_image.SetLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    m_command_buffer.Begin();

    VkBufferImageCopy2 region_1 = vku::InitStructHelper();
    region_1.bufferOffset = 0;
    region_1.bufferRowLength = 0;
    region_1.bufferImageHeight = 0;
    region_1.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    region_1.imageOffset = {33, 33, 0};
    region_1.imageExtent = {31, 31, 1};

    VkCopyBufferToImageInfo2 buffer_image_copy = vku::InitStructHelper();
    buffer_image_copy.srcBuffer = copy_src_buffer;
    buffer_image_copy.dstImage = copy_dst_image;
    buffer_image_copy.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    buffer_image_copy.regionCount = 1;
    buffer_image_copy.pRegions = &region_1;

    vk::CmdCopyBufferToImage2(m_command_buffer, &buffer_image_copy);

    m_command_buffer.End();
    m_errorMonitor->SetDesiredError("has a float value at offset 20475 that is not in the range [0, 1]");
    m_default_queue->Submit(m_command_buffer);
    m_default_queue->Wait();
    m_errorMonitor->VerifyFound();
}

TEST_F(NegativeGpuAV, UseAllDescriptorSlotsPipelineLayout) {
    SetTargetApiVersion(VK_API_VERSION_1_1);
    // Use robustness to make sure we don't crash as we won't catch the invalid shader
    AddRequiredFeature(vkt::Feature::robustBufferAccess);
    RETURN_IF_SKIP(InitGpuAvFramework());
    RETURN_IF_SKIP(InitState());
    m_errorMonitor->ExpectSuccess(kErrorBit | kWarningBit);

    vkt::Buffer buffer(*m_device, 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, kHostVisibleMemProps);

    OneOffDescriptorSet descriptor_set(m_device, {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr}});
    descriptor_set.WriteDescriptorBufferInfo(0, buffer.handle(), 0, VK_WHOLE_SIZE, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    descriptor_set.UpdateDescriptorSets();

    // Add one to use the descriptor slot we tried to reserve
    const uint32_t set_limit = m_device->Physical().limits_.maxBoundDescriptorSets + 1;

    std::vector<const vkt::DescriptorSetLayout *> empty_layouts(set_limit);
    for (uint32_t i = 0; i < set_limit; i++) {
        empty_layouts[i] = &descriptor_set.layout_;
    }

    m_errorMonitor->SetAllowedFailureMsg("This Pipeline Layout has too many descriptor sets");
    vkt::PipelineLayout bad_pipe_layout(*m_device, empty_layouts);

    char const *shader_source = R"glsl(
        #version 450
        layout(set = 0, binding = 0) buffer foo {
            int x;
            int indices[];
        };
        void main()  {
            x = indices[64]; // OOB
        }
    )glsl";

    CreateComputePipelineHelper pipe(*this);
    pipe.cs_ = std::make_unique<VkShaderObj>(this, shader_source, VK_SHADER_STAGE_COMPUTE_BIT, SPV_ENV_VULKAN_1_1);
    pipe.cp_ci_.layout = bad_pipe_layout.handle();
    pipe.CreateComputePipeline();

    m_command_buffer.Begin();
    vk::CmdBindDescriptorSets(m_command_buffer.handle(), VK_PIPELINE_BIND_POINT_COMPUTE, bad_pipe_layout.handle(), 0, 1,
                              &descriptor_set.set_, 0, nullptr);
    vk::CmdBindPipeline(m_command_buffer.handle(), VK_PIPELINE_BIND_POINT_COMPUTE, pipe.Handle());
    vk::CmdDispatch(m_command_buffer.handle(), 1, 1, 1);
    m_command_buffer.End();

    // normally would produce a VUID-vkCmdDispatch-storageBuffers-06936 warning, but we didn't instrument the shader in the end
    m_default_queue->Submit(m_command_buffer);
    m_default_queue->Wait();
}

TEST_F(NegativeGpuAV, RemoveGpuAvInPresenceOfSyncVal) {
    TEST_DESCRIPTION("Disabling GPU-AV when requirements are not met and sync val is on should not cause a crash");

    SetTargetApiVersion(VK_API_VERSION_1_0);  // GPU-AV needs >1.1

    const std::array validation_enables = {
        VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT,
        VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_RESERVE_BINDING_SLOT_EXT,
        VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT,
    };
    const std::array validation_disables = {
        VK_VALIDATION_FEATURE_DISABLE_THREAD_SAFETY_EXT, VK_VALIDATION_FEATURE_DISABLE_API_PARAMETERS_EXT,
        VK_VALIDATION_FEATURE_DISABLE_OBJECT_LIFETIMES_EXT, VK_VALIDATION_FEATURE_DISABLE_CORE_CHECKS_EXT};

    AddRequiredExtensions(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
    VkValidationFeaturesEXT validation_features = vku::InitStructHelper();
    validation_features.enabledValidationFeatureCount = size32(validation_enables);
    validation_features.pEnabledValidationFeatures = validation_enables.data();
    if (m_gpuav_disable_core) {
        validation_features.disabledValidationFeatureCount = size32(validation_disables);
        validation_features.pDisabledValidationFeatures = validation_disables.data();
    }

    RETURN_IF_SKIP(InitFramework(&validation_features));

    m_errorMonitor->SetDesiredError("UNASSIGNED-GPU-Assisted-Validation");
    RETURN_IF_SKIP(InitState(nullptr));
    m_errorMonitor->VerifyFound();
}
