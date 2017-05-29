#include <tests/test3/vk/ShadowMappingSceneTest.h>

#include <base/ScopedTimer.h>
#include <base/vkx/Utils.h>

#include <glm/vec4.hpp>
#include <vulkan/vulkan.hpp>

namespace tests {
namespace test_vk {
ShadowMappingSceneTest::ShadowMappingSceneTest()
    : BaseShadowMappingSceneTest()
    , VKTest("ShadowMappingSceneTest")
    , _semaphoreIndex(0u)
{
}

void ShadowMappingSceneTest::setup()
{
    VKTest::setup();

    createCommandBuffers();
    createVbos();
    createSemaphores();
    createFences();

    prepareShadowmapPass();
    prepareRenderPass();
}

void ShadowMappingSceneTest::run()
{
    while (!window().shouldClose()) {
        TIME_RESET("Frame times:");

        auto frameIndex = getNextFrameIndex();

        updateTestState(static_cast<float>(window().frameTime()));
        prepareCommandBuffer(frameIndex);
        submitCommandBuffer(frameIndex);
        presentFrame(frameIndex);

        window().update();
    }
}

void ShadowMappingSceneTest::teardown()
{
    device().waitIdle();

    destroyPass(_shadowmapPass);
    destroyPass(_renderPass);

    destroyFences();
    destroySemaphores();
    destroyVbos();
    destroyCommandBuffers();

    VKTest::teardown();
}

void ShadowMappingSceneTest::prepareShadowmapPass()
{
    _shadowmapPass.depthBuffer = createDepthBuffer(shadowmapSize(), vk::ImageUsageFlagBits::eDepthStencilAttachment |
                                                                        vk::ImageUsageFlagBits::eSampled);
    _shadowmapPass.renderPass = createShadowmapRenderPass();
    _shadowmapPass.framebuffers =
        createFramebuffers(_shadowmapPass.renderPass, std::vector<vk::ImageView>(window().swapchainImages().size()),
                           _shadowmapPass.depthBuffer.view, shadowmapSize());
    _shadowmapPass.program = createProgram("resources/test3/shaders/vk/shadowmap");
    _shadowmapPass.descriptorSetLayout = createShadowmapDescriptorSetLayout();
    _shadowmapPass.pipelineLayout = createPipelineLayout({_shadowmapPass.descriptorSetLayout}, false);
    _shadowmapPass.pipeline = createPipeline(_shadowmapPass.program, shadowmapSize(), _shadowmapPass.pipelineLayout,
                                             _shadowmapPass.renderPass, false);
}

void ShadowMappingSceneTest::prepareRenderPass()
{
    _renderPass.depthBuffer = createDepthBuffer(window().size(), vk::ImageUsageFlagBits::eDepthStencilAttachment);
    _renderPass.renderPass = createRenderRenderPass();
    _renderPass.framebuffers = createFramebuffers(_renderPass.renderPass, window().swapchainImageViews(),
                                                  _renderPass.depthBuffer.view, window().size());
    _renderPass.program = createProgram("resources/test3/shaders/vk/render");
    _renderPass.descriptorSetLayout = createRenderDescriptorSetLayout();
    _renderPass.descriptorPool = createRenderDescriptorPool();
    _renderPass.descriptorSet = createRenderDescriptorSet(_renderPass.descriptorPool, _renderPass.descriptorSetLayout);
    _renderPass.sampler = createRenderShadowmapSampler();
    _renderPass.pipelineLayout = createPipelineLayout({_renderPass.descriptorSetLayout}, true);
    _renderPass.pipeline =
        createPipeline(_renderPass.program, window().size(), _renderPass.pipelineLayout, _renderPass.renderPass, true);

    setRenderDescriptorSet(_renderPass.descriptorSet);
}

void ShadowMappingSceneTest::destroyPass(VkPass& pass)
{
    destroyPipeline(pass.pipeline);
    destroyPipelineLayout(pass.pipelineLayout);
    destroySampler(pass.sampler);
    destroyDescriptorSetLayout(pass.descriptorSetLayout);
    destroyDescriptorPool(pass.descriptorPool);
    destroyProgram(pass.program);
    destroyFramebuffers(pass.framebuffers);
    destroyRenderPass(pass.renderPass);
    destroyDepthBuffer(pass.depthBuffer);

    // pass = VkPass{};
}

void ShadowMappingSceneTest::createCommandBuffers()
{
    vk::CommandPoolCreateFlags cmdPoolFlags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
    _cmdPool = device().createCommandPool({cmdPoolFlags, queues().familyIndex()});
    _cmdBuffers = device().allocateCommandBuffers(
        {_cmdPool, vk::CommandBufferLevel::ePrimary, static_cast<uint32_t>(window().swapchainImages().size())});
}

void ShadowMappingSceneTest::createVbos()
{
    _vkRenderObjects.resize(renderObjects().size());

    for (std::size_t index = 0; index < renderObjects().size(); ++index) {
        const common::RenderObject& renderObject = renderObjects()[index];
        auto& vkRenderObject = _vkRenderObjects[index];

        vkRenderObject.modelMatrix = renderObject.modelMatrix;
        vkRenderObject.drawCount = renderObject.vertices.size();

        std::vector<glm::vec4> vboBuffer = renderObject.generateCombinedData();

        vk::DeviceSize size = vboBuffer.size() * sizeof(vboBuffer.front());
        vk::BufferUsageFlags usage = vk::BufferUsageFlagBits::eVertexBuffer;

        base::vkx::Buffer stagingBuffer = memory().createStagingBuffer(size);
        {
            auto vboMemory = device().mapMemory(stagingBuffer.memory, stagingBuffer.offset, stagingBuffer.size, {});
            std::memcpy(vboMemory, vboBuffer.data(), static_cast<std::size_t>(stagingBuffer.size));
            device().unmapMemory(stagingBuffer.memory);
        }
        vkRenderObject.vbo =
            memory().copyToDeviceLocalMemory(stagingBuffer, usage, _cmdBuffers.front(), queues().queue());

        memory().destroyBuffer(stagingBuffer);
    }
}

void ShadowMappingSceneTest::createSemaphores()
{
    for (std::size_t i = 0; i < window().swapchainImages().size(); ++i) {
        _acquireSemaphores.push_back(device().createSemaphore({}));
        _renderSemaphores.push_back(device().createSemaphore({}));
    }
}

void ShadowMappingSceneTest::createFences()
{
    _fences.resize(_cmdBuffers.size());
    for (vk::Fence& fence : _fences) {
        fence = device().createFence({vk::FenceCreateFlagBits::eSignaled});
    }
}

ShadowMappingSceneTest::VkDepthBuffer ShadowMappingSceneTest::createDepthBuffer(const glm::uvec2& size,
                                                                                vk::ImageUsageFlags usage)
{
    VkDepthBuffer depthBuffer;
    depthBuffer.format = vk::Format::eD24UnormS8Uint;

    vk::ImageCreateInfo depthBufferInfo{{},
                                        vk::ImageType::e2D,
                                        depthBuffer.format,
                                        vk::Extent3D{static_cast<uint32_t>(size.x), static_cast<uint32_t>(size.y), 1},
                                        1,
                                        1,
                                        vk::SampleCountFlagBits::e1,
                                        vk::ImageTiling::eOptimal,
                                        usage,
                                        vk::SharingMode::eExclusive,
                                        0,
                                        nullptr,
                                        vk::ImageLayout::eUndefined};
    depthBuffer.image = device().createImage(depthBufferInfo);
    depthBuffer.memory = memory().allocateDeviceLocalMemory(device().getImageMemoryRequirements(depthBuffer.image));
    device().bindImageMemory(depthBuffer.image, depthBuffer.memory, 0);

    // DepthBuffer view
    vk::ImageViewCreateInfo depthBufferViewInfo{{},
                                                depthBuffer.image,
                                                vk::ImageViewType::e2D,
                                                depthBuffer.format,
                                                {},
                                                vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1}};
    depthBuffer.view = device().createImageView(depthBufferViewInfo);

    return depthBuffer;
}

vk::RenderPass ShadowMappingSceneTest::createShadowmapRenderPass()
{
    std::vector<vk::AttachmentDescription> attachments{
        vk::AttachmentDescription{{},
                                  _shadowmapPass.depthBuffer.format,
                                  vk::SampleCountFlagBits::e1,
                                  vk::AttachmentLoadOp::eClear,
                                  vk::AttachmentStoreOp::eStore,
                                  vk::AttachmentLoadOp::eDontCare,
                                  vk::AttachmentStoreOp::eDontCare,
                                  vk::ImageLayout::eUndefined,
                                  vk::ImageLayout::eShaderReadOnlyOptimal},
    };
    vk::AttachmentReference depthReference{0, vk::ImageLayout::eDepthStencilAttachmentOptimal};

    std::vector<vk::SubpassDescription> subpasses{
        {{}, vk::PipelineBindPoint::eGraphics, 0, nullptr, 0, nullptr, nullptr, &depthReference, 0, nullptr},
    };

    std::vector<vk::SubpassDependency> dependencies{
        vk::SubpassDependency{VK_SUBPASS_EXTERNAL, 0, vk::PipelineStageFlagBits::eBottomOfPipe,
                              vk::PipelineStageFlagBits::eLateFragmentTests, vk::AccessFlagBits::eMemoryRead,
                              vk::AccessFlagBits::eDepthStencilAttachmentRead |
                                  vk::AccessFlagBits::eDepthStencilAttachmentWrite,
                              vk::DependencyFlagBits::eByRegion},
        vk::SubpassDependency{0, VK_SUBPASS_EXTERNAL, vk::PipelineStageFlagBits::eLateFragmentTests,
                              vk::PipelineStageFlagBits::eBottomOfPipe,
                              vk::AccessFlagBits::eDepthStencilAttachmentRead |
                                  vk::AccessFlagBits::eDepthStencilAttachmentWrite,
                              vk::AccessFlagBits::eMemoryRead, vk::DependencyFlagBits::eByRegion},
    };

    vk::RenderPassCreateInfo renderPassInfo{{},
                                            static_cast<uint32_t>(attachments.size()),
                                            attachments.data(),
                                            static_cast<uint32_t>(subpasses.size()),
                                            subpasses.data(),
                                            static_cast<uint32_t>(dependencies.size()),
                                            dependencies.data()};

    return device().createRenderPass(renderPassInfo);
}

vk::RenderPass ShadowMappingSceneTest::createRenderRenderPass()
{
    std::vector<vk::AttachmentDescription> attachments{
        vk::AttachmentDescription{{},
                                  window().swapchainImageFormat(),
                                  vk::SampleCountFlagBits::e1,
                                  vk::AttachmentLoadOp::eClear,
                                  vk::AttachmentStoreOp::eStore,
                                  vk::AttachmentLoadOp::eDontCare,
                                  vk::AttachmentStoreOp::eDontCare,
                                  vk::ImageLayout::eUndefined,
                                  vk::ImageLayout::ePresentSrcKHR},
        vk::AttachmentDescription{{},
                                  _renderPass.depthBuffer.format,
                                  vk::SampleCountFlagBits::e1,
                                  vk::AttachmentLoadOp::eClear,
                                  vk::AttachmentStoreOp::eStore,
                                  vk::AttachmentLoadOp::eDontCare,
                                  vk::AttachmentStoreOp::eDontCare,
                                  vk::ImageLayout::eUndefined,
                                  vk::ImageLayout::eDepthStencilAttachmentOptimal}};

    vk::AttachmentReference colorWriteAttachment{0, vk::ImageLayout::eColorAttachmentOptimal};
    vk::AttachmentReference depthWriteAttachment{1, vk::ImageLayout::eDepthStencilAttachmentOptimal};

    std::vector<vk::SubpassDescription> subpasses{
        vk::SubpassDescription{{},
                               vk::PipelineBindPoint::eGraphics,
                               0,
                               nullptr,
                               1,
                               &colorWriteAttachment,
                               nullptr,
                               &depthWriteAttachment,
                               0,
                               nullptr},
    };

    std::vector<vk::SubpassDependency> dependencies{
        vk::SubpassDependency{VK_SUBPASS_EXTERNAL, 0, vk::PipelineStageFlagBits::eBottomOfPipe,
                              vk::PipelineStageFlagBits::eLateFragmentTests, vk::AccessFlagBits::eMemoryRead,
                              vk::AccessFlagBits::eDepthStencilAttachmentRead |
                                  vk::AccessFlagBits::eDepthStencilAttachmentWrite,
                              vk::DependencyFlagBits::eByRegion},
        vk::SubpassDependency{0, VK_SUBPASS_EXTERNAL, vk::PipelineStageFlagBits::eLateFragmentTests,
                              vk::PipelineStageFlagBits::eBottomOfPipe,
                              vk::AccessFlagBits::eDepthStencilAttachmentRead |
                                  vk::AccessFlagBits::eDepthStencilAttachmentWrite,
                              vk::AccessFlagBits::eMemoryRead, vk::DependencyFlagBits::eByRegion},
    };

    vk::RenderPassCreateInfo renderPassInfo{{},
                                            static_cast<uint32_t>(attachments.size()),
                                            attachments.data(),
                                            static_cast<uint32_t>(subpasses.size()),
                                            subpasses.data(),
                                            static_cast<uint32_t>(dependencies.size()),
                                            dependencies.data()};
    return device().createRenderPass(renderPassInfo);
}

std::vector<vk::Framebuffer> ShadowMappingSceneTest::createFramebuffers(const vk::RenderPass& renderPass,
                                                                        const std::vector<vk::ImageView>& colorImages,
                                                                        const vk::ImageView& depthBuffer,
                                                                        const glm::uvec2& size)
{
    std::vector<vk::Framebuffer> framebuffers;
    framebuffers.reserve(colorImages.size());

    for (const vk::ImageView& colorImage : colorImages) {
        std::vector<vk::ImageView> attachments;

        if (colorImage)
            attachments.push_back(colorImage);
        if (depthBuffer)
            attachments.push_back(depthBuffer);

        vk::FramebufferCreateInfo framebufferInfo{{},
                                                  renderPass,
                                                  static_cast<uint32_t>(attachments.size()),
                                                  attachments.data(),
                                                  static_cast<uint32_t>(size.x),
                                                  static_cast<uint32_t>(size.y),
                                                  1};
        framebuffers.push_back(device().createFramebuffer(framebufferInfo));
    }

    return framebuffers;
}

ShadowMappingSceneTest::VkProgram ShadowMappingSceneTest::createProgram(const std::string& path)
{
    VkProgram program;

    program.vertexModule = base::vkx::ShaderModule{device(), path + ".vert.spv"};
    program.fragmentModule = base::vkx::ShaderModule{device(), path + ".frag.spv"};

    return program;
}

vk::DescriptorSetLayout ShadowMappingSceneTest::createShadowmapDescriptorSetLayout()
{
    std::vector<vk::DescriptorSetLayoutBinding> bindings{};
    vk::DescriptorSetLayoutCreateInfo descriptorSetLayoutInfo{{},
                                                              static_cast<uint32_t>(bindings.size()),
                                                              bindings.data()};
    return device().createDescriptorSetLayout(descriptorSetLayoutInfo);
}

vk::DescriptorSetLayout ShadowMappingSceneTest::createRenderDescriptorSetLayout()
{
    std::vector<vk::DescriptorSetLayoutBinding> bindings{
        vk::DescriptorSetLayoutBinding{0, vk::DescriptorType::eCombinedImageSampler, 1,
                                       vk::ShaderStageFlagBits::eFragment, nullptr},
    };
    vk::DescriptorSetLayoutCreateInfo descriptorSetLayoutInfo{{},
                                                              static_cast<uint32_t>(bindings.size()),
                                                              bindings.data()};
    return device().createDescriptorSetLayout(descriptorSetLayoutInfo);
}

vk::DescriptorPool ShadowMappingSceneTest::createRenderDescriptorPool()
{
    std::vector<vk::DescriptorPoolSize> poolSizes{
        vk::DescriptorPoolSize{vk::DescriptorType::eCombinedImageSampler, 1},
    };
    vk::DescriptorPoolCreateInfo poolInfo{{}, 1, static_cast<uint32_t>(poolSizes.size()), poolSizes.data()};
    return device().createDescriptorPool(poolInfo);
}

vk::DescriptorSet ShadowMappingSceneTest::createRenderDescriptorSet(const vk::DescriptorPool& descriptorPool,
                                                                    const vk::DescriptorSetLayout descriptorSetLayout)
{
    vk::DescriptorSetAllocateInfo descriptorSetInfo{descriptorPool, 1, &descriptorSetLayout};
    return device().allocateDescriptorSets(descriptorSetInfo).front();
}

vk::Sampler ShadowMappingSceneTest::createRenderShadowmapSampler()
{
    vk::SamplerCreateInfo samplerInfo{{},
                                      vk::Filter::eNearest,
                                      vk::Filter::eNearest,
                                      vk::SamplerMipmapMode::eNearest,
                                      vk::SamplerAddressMode::eClampToBorder,
                                      vk::SamplerAddressMode::eClampToBorder,
                                      vk::SamplerAddressMode::eClampToBorder,
                                      0.0f,
                                      VK_FALSE,
                                      1.0f,
                                      VK_FALSE,
                                      {},
                                      0.0f,
                                      0.0f,
                                      vk::BorderColor::eFloatOpaqueWhite,
                                      VK_FALSE};
    return device().createSampler(samplerInfo);
}

vk::PipelineLayout ShadowMappingSceneTest::createPipelineLayout(const std::vector<vk::DescriptorSetLayout>& setLayouts,
                                                                bool depthMatrix)
{
    std::vector<vk::PushConstantRange> pushConstants;
    uint32_t rangeSize = static_cast<uint32_t>(sizeof(glm::mat4) * (depthMatrix ? 2 : 1));
    pushConstants.push_back({vk::ShaderStageFlagBits::eVertex, 0, rangeSize}); // MVP matrix (+ depthMVP matrix)

    vk::PipelineLayoutCreateInfo pipelineLayoutInfo{{},
                                                    static_cast<uint32_t>(setLayouts.size()),
                                                    setLayouts.data(),
                                                    static_cast<uint32_t>(pushConstants.size()),
                                                    pushConstants.data()};

    return device().createPipelineLayout(pipelineLayoutInfo);
}

vk::Pipeline ShadowMappingSceneTest::createPipeline(const VkProgram& program,
                                                    const glm::uvec2& renderSize,
                                                    const vk::PipelineLayout& layout,
                                                    const vk::RenderPass& renderPass,
                                                    bool colorBlendEnabled) const
{
    // Shader stages
    std::vector<vk::PipelineShaderStageCreateInfo> shaderStages = getShaderStages(program);

    // Vertex input state
    std::vector<vk::VertexInputBindingDescription> vertexBindingDescriptions{
        {0, 3 * sizeof(glm::vec4), vk::VertexInputRate::eVertex} // Binding #0 - vertex data (position, color, normal)
    };
    std::vector<vk::VertexInputAttributeDescription> vertexAttributeDescription{
        {0, 0, vk::Format::eR32G32B32A32Sfloat, 0}, // Attribute #0 (from binding #0) - vertex position
        {1, 0, vk::Format::eR32G32B32A32Sfloat, sizeof(glm::vec4)}, // Attribute #1 (from binding #1) - vertex color
        {2, 0, vk::Format::eR32G32B32A32Sfloat, 2 * sizeof(glm::vec4)} // Attribute #2 (from binding #0) - vertex normal
    };
    vk::PipelineVertexInputStateCreateInfo vertexInputState{{},
                                                            static_cast<uint32_t>(vertexBindingDescriptions.size()),
                                                            vertexBindingDescriptions.data(),
                                                            static_cast<uint32_t>(vertexAttributeDescription.size()),
                                                            vertexAttributeDescription.data()};

    // Input assembly state
    vk::PipelineInputAssemblyStateCreateInfo inputAssemblyState{{}, vk::PrimitiveTopology::eTriangleList, VK_FALSE};

    // Viewport state
    vk::Viewport viewport{0.0f, 0.0f, static_cast<float>(renderSize.x), static_cast<float>(renderSize.y), 0.0f, 1.0f};
    vk::Rect2D scissor{{0, 0}, {static_cast<uint32_t>(renderSize.x), static_cast<uint32_t>(renderSize.y)}};
    vk::PipelineViewportStateCreateInfo viewportState{{}, 1, &viewport, 1, &scissor};

    // Rasterization state
    vk::PipelineRasterizationStateCreateInfo rasterizationState{{},
                                                                VK_FALSE,
                                                                VK_FALSE,
                                                                vk::PolygonMode::eFill,
                                                                vk::CullModeFlagBits::eNone,
                                                                vk::FrontFace::eCounterClockwise,
                                                                VK_FALSE,
                                                                0.0f,
                                                                0.0f,
                                                                0.0f,
                                                                1.0f};

    // Multisample state
    vk::PipelineMultisampleStateCreateInfo multisampleState{
        {}, vk::SampleCountFlagBits::e1, VK_FALSE, 0.0f, nullptr, VK_FALSE, VK_FALSE};

    // Depth-stencil state
    vk::PipelineDepthStencilStateCreateInfo depthStencilState{{},       VK_TRUE,  VK_TRUE, vk::CompareOp::eLessOrEqual,
                                                              VK_FALSE, VK_FALSE, {},      {},
                                                              0.0f,     1.0f};

    // ColorBlend state
    vk::PipelineColorBlendAttachmentState colorBlendAttachmentState{VK_FALSE};
    colorBlendAttachmentState.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                                               vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
    vk::PipelineColorBlendStateCreateInfo colorBlendState{
        {}, VK_FALSE, vk::LogicOp::eClear, 1, &colorBlendAttachmentState};
    vk::PipelineColorBlendStateCreateInfo* pColorBlendState = (colorBlendEnabled ? &colorBlendState : nullptr);

    // Pipeline creation
    vk::GraphicsPipelineCreateInfo pipelineInfo{{},
                                                static_cast<uint32_t>(shaderStages.size()),
                                                shaderStages.data(),
                                                &vertexInputState,
                                                &inputAssemblyState,
                                                nullptr,
                                                &viewportState,
                                                &rasterizationState,
                                                &multisampleState,
                                                &depthStencilState,
                                                pColorBlendState,
                                                nullptr,
                                                layout,
                                                renderPass,
                                                0};
    return device().createGraphicsPipeline({}, pipelineInfo);
}

void ShadowMappingSceneTest::destroyPipeline(vk::Pipeline& pipeline)
{
    device().destroyPipeline(pipeline);
    pipeline = vk::Pipeline{};
}

void ShadowMappingSceneTest::destroyPipelineLayout(vk::PipelineLayout& pipelineLayout)
{
    device().destroyPipelineLayout(pipelineLayout);
    pipelineLayout = vk::PipelineLayout{};
}

void ShadowMappingSceneTest::destroySampler(vk::Sampler& sampler)
{
    if (sampler) {
        device().destroySampler(sampler);
        sampler = vk::Sampler{};
    }
}

void ShadowMappingSceneTest::destroyDescriptorPool(vk::DescriptorPool& descriptorPool)
{
    if (descriptorPool) {
        device().destroyDescriptorPool(descriptorPool);
        descriptorPool = vk::DescriptorPool{};
    }
}

void ShadowMappingSceneTest::destroyDescriptorSetLayout(vk::DescriptorSetLayout& descriptorSetLayout)
{
    device().destroyDescriptorSetLayout(descriptorSetLayout);
    descriptorSetLayout = vk::DescriptorSetLayout{};
}

void ShadowMappingSceneTest::destroyProgram(VkProgram& program)
{
    program = VkProgram{};
}

void ShadowMappingSceneTest::destroyFramebuffers(std::vector<vk::Framebuffer>& framebuffers)
{
    for (const vk::Framebuffer& framebuffer : framebuffers) {
        device().destroyFramebuffer(framebuffer);
    }
    framebuffers.clear();
}

void ShadowMappingSceneTest::destroyRenderPass(vk::RenderPass& renderPass)
{
    device().destroyRenderPass(renderPass);
    renderPass = vk::RenderPass{};
}

void ShadowMappingSceneTest::destroyDepthBuffer(VkDepthBuffer& depthBuffer)
{
    device().destroyImageView(depthBuffer.view);
    device().destroyImage(depthBuffer.image);
    device().freeMemory(depthBuffer.memory);
    depthBuffer = VkDepthBuffer{};
}

void ShadowMappingSceneTest::destroyFences()
{
    for (const vk::Fence& fence : _fences) {
        device().destroyFence(fence);
    }
    _fences.clear();
}

void ShadowMappingSceneTest::destroySemaphores()
{
    for (const auto& acquireSemaphore : _acquireSemaphores) {
        device().destroySemaphore(acquireSemaphore);
    }
    for (const auto& renderSemaphore : _renderSemaphores) {
        device().destroySemaphore(renderSemaphore);
    }
    _acquireSemaphores.clear();
    _renderSemaphores.clear();
}

void ShadowMappingSceneTest::destroyVbos()
{
    for (auto& vkRenderObject : _vkRenderObjects) {
        memory().destroyBuffer(vkRenderObject.vbo);
    }
    _vkRenderObjects.clear();
}

void ShadowMappingSceneTest::setRenderDescriptorSet(const vk::DescriptorSet& descriptorSet)
{
    vk::DescriptorImageInfo imageInfo{_renderPass.sampler, _shadowmapPass.depthBuffer.view,
                                      vk::ImageLayout::eShaderReadOnlyOptimal};
    std::vector<vk::WriteDescriptorSet> descriptorWrites{
        {descriptorSet, 0, 0, 1, vk::DescriptorType::eCombinedImageSampler, &imageInfo, nullptr, nullptr}};
    std::vector<vk::CopyDescriptorSet> descriptorCopies{};
    device().updateDescriptorSets(descriptorWrites, descriptorCopies);
}

glm::mat4 ShadowMappingSceneTest::convertProjectionToImage(const glm::mat4& matrix) const
{
    // Since in Vulkan we already have depth in [0, 1] range, we don't have to scale it like we do
    // in OpenGL. Thus we only have to scale and move .x and .y coordinates.
    static const glm::mat4 bias = {0.5, 0.0, 0.0, 0.0, //
                                   0.0, 0.5, 0.0, 0.0, //
                                   0.0, 0.0, 1.0, 0.0, //
                                   0.5, 0.5, 0.0, 1.0};

    return bias * matrix;
}

void ShadowMappingSceneTest::destroyCommandBuffers()
{
    device().destroyCommandPool(_cmdPool);
    _cmdBuffers.clear();
}

std::vector<vk::PipelineShaderStageCreateInfo> ShadowMappingSceneTest::getShaderStages(const VkProgram& program) const
{
    std::vector<vk::PipelineShaderStageCreateInfo> stages{
        {{}, vk::ShaderStageFlagBits::eVertex, program.vertexModule, "main", nullptr},
        {{}, vk::ShaderStageFlagBits::eFragment, program.fragmentModule, "main", nullptr}};

    return stages;
}

uint32_t ShadowMappingSceneTest::getNextFrameIndex() const
{
    TIME_IT("Frame image acquisition");

    _semaphoreIndex = (_semaphoreIndex + 1) % _acquireSemaphores.size();
    auto nextFrameAcquireStatus =
        device().acquireNextImageKHR(window().swapchain(), UINT64_MAX, _acquireSemaphores[_semaphoreIndex], {});

    if (nextFrameAcquireStatus.result != vk::Result::eSuccess) {
        throw std::system_error(nextFrameAcquireStatus.result, "Error during acquiring next frame index");
    }

    return nextFrameAcquireStatus.value;
}

void ShadowMappingSceneTest::prepareCommandBuffer(std::size_t frameIndex) const
{
    {
        TIME_IT("Fence waiting");
        device().waitForFences(1, &_fences[frameIndex], VK_FALSE, UINT64_MAX);
        device().resetFences(1, &_fences[frameIndex]);
    }

    {
        TIME_IT("CmdBuffer building");

        const vk::CommandBuffer& cmdBuffer = _cmdBuffers[frameIndex];
        cmdBuffer.reset({});
        cmdBuffer.begin({vk::CommandBufferUsageFlagBits::eOneTimeSubmit, nullptr});

        {
            // Shadowmap pass
            const VkPass& pass = _shadowmapPass;

            cmdBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pass.pipeline);

            const vk::ClearValue clearValue = vk::ClearDepthStencilValue{1.0f, 0};
            vk::RenderPassBeginInfo renderPassInfo{pass.renderPass,
                                                   pass.framebuffers[frameIndex],
                                                   {{}, {shadowmapSize().x, shadowmapSize().y}},
                                                   1,
                                                   &clearValue};
            cmdBuffer.beginRenderPass(renderPassInfo, vk::SubpassContents::eInline);

            for (const auto& renderObject : _vkRenderObjects) {
                glm::mat4 MVP = base::vkx::fixGLMatrix(shadowMatrix() * renderObject.modelMatrix);
                cmdBuffer.pushConstants(pass.pipelineLayout, vk::ShaderStageFlagBits::eVertex, 0, sizeof(MVP), &MVP);

                cmdBuffer.bindVertexBuffers(0, {{renderObject.vbo.buffer}}, {{0}});
                cmdBuffer.draw(renderObject.drawCount, 1, 0, 0);
            }

            cmdBuffer.endRenderPass();
        }

        {
            // Render pass
            const VkPass& pass = _renderPass;

            cmdBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pass.pipelineLayout, 0, 1,
                                         &pass.descriptorSet, 0, nullptr);
            cmdBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pass.pipeline);

            const std::vector<vk::ClearValue> clearValues{vk::ClearColorValue{
                                                              std::array<float, 4>{{0.1f, 0.1f, 0.1f, 1.0f}}},
                                                          vk::ClearDepthStencilValue{1.0f, 0}};
            vk::RenderPassBeginInfo renderPassInfo{pass.renderPass,
                                                   pass.framebuffers[frameIndex],
                                                   {{}, {window().size().x, window().size().y}},
                                                   static_cast<uint32_t>(clearValues.size()),
                                                   clearValues.data()};
            cmdBuffer.beginRenderPass(renderPassInfo, vk::SubpassContents::eInline);

            for (const auto& renderObject : _vkRenderObjects) {
                glm::mat4 matrices[2] = {
                    base::vkx::fixGLMatrix(renderMatrix() * renderObject.modelMatrix),
                    convertProjectionToImage(base::vkx::fixGLMatrix(shadowMatrix() * renderObject.modelMatrix)),
                };
                cmdBuffer.pushConstants(pass.pipelineLayout, vk::ShaderStageFlagBits::eVertex, 0, 2 * sizeof(glm::mat4),
                                        &matrices);

                cmdBuffer.bindVertexBuffers(0, {{renderObject.vbo.buffer}}, {{0}});
                cmdBuffer.draw(renderObject.drawCount, 1, 0, 0);
            }

            cmdBuffer.endRenderPass();
        }

        cmdBuffer.end();
    }
}

void ShadowMappingSceneTest::submitCommandBuffer(std::size_t frameIndex) const
{
    TIME_IT("CmdBuffer submition");

    vk::PipelineStageFlags waitStage = vk::PipelineStageFlagBits::eColorAttachmentOutput;
    vk::SubmitInfo submits{1, &_acquireSemaphores[_semaphoreIndex], &waitStage, 1, &_cmdBuffers[frameIndex],
                           1, &_renderSemaphores[_semaphoreIndex]};
    queues().queue().submit(submits, _fences[frameIndex]);
}

void ShadowMappingSceneTest::presentFrame(std::size_t frameIndex) const
{
    TIME_IT("Frame presentation");

    uint32_t imageIndex = static_cast<uint32_t>(frameIndex);
    vk::PresentInfoKHR presentInfo{1,      &_renderSemaphores[_semaphoreIndex], 1, &window().swapchain(), &imageIndex,
                                   nullptr};
    queues().queue().presentKHR(presentInfo);
}
}
}
