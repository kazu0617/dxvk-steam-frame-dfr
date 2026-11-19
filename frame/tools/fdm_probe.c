// SPDX-License-Identifier: Zlib
// Headless VK_EXT_fragment_density_map probe for the gaze FDM path.
//
// Renders a full-screen triangle whose fragment shader writes gl_FragCoord into
// the colour target, with a density map attached through dynamic rendering, then
// reads the target back and measures the effective fragment size per screen band.
// It answers three questions the DXVK patch depends on:
//   1. does the driver apply the map at all (fragment size > 1 somewhere)?
//   2. where do the density bands land for an exactly sized map versus an
//      oversized image with only its top-left corner written (what the patch does
//      for gaze maps)?
//   3. does it still work through a secondary command buffer
//      (VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT)?
//
//   fdm_probe [--secondary] [--oversized] [--texel N] [--width W --height H]
//
// Expected bands for the default map: columns 0-1/3 full density, 1/3-2/3 half,
// 2/3-1 quarter (so 1, 2 and 4 pixel wide fragments), rows top half full, bottom
// half half density (1 then 2 pixel high fragments).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vulkan/vulkan.h>

#include "fdm_probe_shaders.h"

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
  fprintf(stderr, "%s:%d %s = %d\n", __FILE__, __LINE__, #x, (int)r_); exit(2); } } while (0)

static uint32_t find_memory(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want) {
  VkPhysicalDeviceMemoryProperties props;
  vkGetPhysicalDeviceMemoryProperties(pd, &props);
  for (uint32_t i = 0; i < props.memoryTypeCount; i++)
    if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want) return i;
  fprintf(stderr, "no memory type\n");
  exit(2);
}

static void make_buffer(VkDevice dev, VkPhysicalDevice pd, VkDeviceSize size, VkBufferUsageFlags usage,
                        VkBuffer *buffer, VkDeviceMemory *memory) {
  VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
  info.size = size; info.usage = usage; info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  CHECK(vkCreateBuffer(dev, &info, NULL, buffer));
  VkMemoryRequirements req; vkGetBufferMemoryRequirements(dev, *buffer, &req);
  VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = find_memory(pd, req.memoryTypeBits,
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  CHECK(vkAllocateMemory(dev, &alloc, NULL, memory));
  CHECK(vkBindBufferMemory(dev, *buffer, *memory, 0));
}

static void make_image(VkDevice dev, VkPhysicalDevice pd, VkFormat format, VkExtent2D extent,
                       VkImageUsageFlags usage, VkImage *image, VkImageView *view, VkDeviceMemory *memory) {
  VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
  info.imageType = VK_IMAGE_TYPE_2D; info.format = format;
  info.extent = (VkExtent3D){ extent.width, extent.height, 1 };
  info.mipLevels = 1; info.arrayLayers = 1; info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL; info.usage = usage; info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  CHECK(vkCreateImage(dev, &info, NULL, image));
  VkMemoryRequirements req; vkGetImageMemoryRequirements(dev, *image, &req);
  VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = find_memory(pd, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  CHECK(vkAllocateMemory(dev, &alloc, NULL, memory));
  CHECK(vkBindImageMemory(dev, *image, *memory, 0));
  VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
  vi.image = *image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = format;
  vi.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
  CHECK(vkCreateImageView(dev, &vi, NULL, view));
}

static void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                    VkPipelineStageFlags src, VkAccessFlags srcAccess,
                    VkPipelineStageFlags dst, VkAccessFlags dstAccess) {
  VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
  b.srcAccessMask = srcAccess; b.dstAccessMask = dstAccess; b.oldLayout = from; b.newLayout = to;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = image;
  b.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
  vkCmdPipelineBarrier(cmd, src, dst, 0, 0, NULL, 0, NULL, 1, &b);
}

static VkShaderModule shader(VkDevice dev, const uint32_t *code, size_t bytes) {
  VkShaderModuleCreateInfo info = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
  info.codeSize = bytes; info.pCode = code;
  VkShaderModule module; CHECK(vkCreateShaderModule(dev, &info, NULL, &module));
  return module;
}

// Median run length of identical RGBA pixels along a row segment (columns) or a column segment (rows).
static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }
static int median_run(const uint32_t *px, uint32_t width, uint32_t x0, uint32_t x1, uint32_t y, int vertical,
                      uint32_t height, uint32_t y0, uint32_t y1, uint32_t x) {
  int runs[8192]; int n = 0, len = 1;
  if (!vertical) {
    for (uint32_t i = x0 + 1; i < x1; i++) {
      if (px[y * width + i] == px[y * width + i - 1]) len++;
      else { if (n < 8192) runs[n++] = len; len = 1; }
    }
  } else {
    for (uint32_t j = y0 + 1; j < y1 && j < height; j++) {
      if (px[j * width + x] == px[(j - 1) * width + x]) len++;
      else { if (n < 8192) runs[n++] = len; len = 1; }
    }
  }
  if (!n) return len;
  qsort(runs, n, sizeof(int), cmp_int);
  return runs[n / 2];
}

int main(int argc, char **argv) {
  int secondary = 0, oversized = 0, nomap = 0, optimal = 0, uniform = 0, presubmit = 0, radial = 0; uint32_t texel = 32, W = 2880, H = 1440;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--secondary")) secondary = 1;
    else if (!strcmp(argv[i], "--oversized")) oversized = 1;
    else if (!strcmp(argv[i], "--optimal")) optimal = 1;  // FRAGMENT_DENSITY_MAP_OPTIMAL_EXT instead of GENERAL
    else if (!strcmp(argv[i], "--radial")) radial = 1;  // R=G density rings around the centre, like a gaze map
    else if (!strcmp(argv[i], "--presubmit")) presubmit = 1;  // upload the map in its own earlier submission
    else if (!strcmp(argv[i], "--uniform")) uniform = 1;  // all-255 map
    else if (!strcmp(argv[i], "--nomap")) nomap = 1;  // debugging: render without the density map attachment
    else if (!strcmp(argv[i], "--texel") && i + 1 < argc) texel = (uint32_t)atoi(argv[++i]);
    else if (!strcmp(argv[i], "--width") && i + 1 < argc) W = (uint32_t)atoi(argv[++i]);
    else if (!strcmp(argv[i], "--height") && i + 1 < argc) H = (uint32_t)atoi(argv[++i]);
    else { fprintf(stderr, "unknown argument %s\n", argv[i]); return 2; }
  }

  VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
  app.pApplicationName = "fdm-probe"; app.apiVersion = VK_API_VERSION_1_3;
  VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO }; ici.pApplicationInfo = &app;
  VkInstance inst; CHECK(vkCreateInstance(&ici, NULL, &inst));
  uint32_t n = 4; VkPhysicalDevice pds[4]; CHECK(vkEnumeratePhysicalDevices(inst, &n, pds));
  VkPhysicalDevice pd = pds[0];
  VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(pd, &props);

  VkPhysicalDeviceFragmentDensityMapPropertiesEXT fdmProps = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_PROPERTIES_EXT };
  VkPhysicalDeviceProperties2 props2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &fdmProps };
  vkGetPhysicalDeviceProperties2(pd, &props2);
  printf("device: %s\n", props.deviceName);
  printf("texel size min %ux%u max %ux%u\n", fdmProps.minFragmentDensityTexelSize.width, fdmProps.minFragmentDensityTexelSize.height,
         fdmProps.maxFragmentDensityTexelSize.width, fdmProps.maxFragmentDensityTexelSize.height);

  VkPhysicalDeviceFragmentDensityMapFeaturesEXT fdmFeat = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT };
  VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &fdmFeat };
  f13.dynamicRendering = VK_TRUE;
  VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f13 };
  // Query then keep only what we need.
  vkGetPhysicalDeviceFeatures2(pd, &f2);
  if (!fdmFeat.fragmentDensityMap || !fdmFeat.fragmentDensityMapNonSubsampledImages) { printf("FDM not supported\n"); return 3; }
  printf("features: fdm=%u dynamic=%u nonSubsampled=%u\n", fdmFeat.fragmentDensityMap,
         fdmFeat.fragmentDensityMapDynamic, fdmFeat.fragmentDensityMapNonSubsampledImages);
  fdmFeat.fragmentDensityMapDynamic = VK_FALSE;
  f13.dynamicRendering = VK_TRUE;
  f13.synchronization2 = VK_FALSE; f13.maintenance4 = VK_FALSE;

  float prio = 1.0f; uint32_t qf = 0; uint32_t qn = 8; VkQueueFamilyProperties qp[8];
  vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qp);
  for (uint32_t i = 0; i < qn; i++) if (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qf = i; break; }
  VkDeviceQueueCreateInfo dq = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
  dq.queueFamilyIndex = qf; dq.queueCount = 1; dq.pQueuePriorities = &prio;
  const char *exts[] = { VK_EXT_FRAGMENT_DENSITY_MAP_EXTENSION_NAME };
  VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2 };
  dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &dq; dci.enabledExtensionCount = 1; dci.ppEnabledExtensionNames = exts;
  VkDevice dev; CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
  VkQueue queue; vkGetDeviceQueue(dev, qf, 0, &queue);

  // Colour target and readback buffer.
  VkImage color; VkImageView colorView; VkDeviceMemory colorMem;
  make_image(dev, pd, VK_FORMAT_R8G8B8A8_UNORM, (VkExtent2D){ W, H },
             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &color, &colorView, &colorMem);
  VkBuffer readback; VkDeviceMemory readbackMem; make_buffer(dev, pd, (VkDeviceSize)W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &readback, &readbackMem);

  // Density map: exact size = ceil(extent / texel); oversized = 512x512 with only that corner written.
  uint32_t mapW = (W + texel - 1) / texel, mapH = (H + texel - 1) / texel;
  VkExtent2D imageExtent = oversized ? (VkExtent2D){ 512, 512 } : (VkExtent2D){ mapW, mapH };
  VkImage map; VkImageView mapView; VkDeviceMemory mapMem;
  make_image(dev, pd, VK_FORMAT_R8G8_UNORM, imageExtent,
             VK_IMAGE_USAGE_FRAGMENT_DENSITY_MAP_BIT_EXT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &map, &mapView, &mapMem);
  VkBuffer staging; VkDeviceMemory stagingMem;
  make_buffer(dev, pd, (VkDeviceSize)mapW * mapH * 2, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging, &stagingMem);
  uint8_t *mapped; CHECK(vkMapMemory(dev, stagingMem, 0, VK_WHOLE_SIZE, 0, (void **)&mapped));
  for (uint32_t j = 0; j < mapH; j++) for (uint32_t i = 0; i < mapW; i++) {
    uint32_t band = uniform ? 0 : i * 3 / mapW;
    mapped[(j * mapW + i) * 2] = band == 0 ? 255 : band == 1 ? 128 : 64;
    mapped[(j * mapW + i) * 2 + 1] = (uniform || j < mapH / 2) ? 255 : 128;
    if (radial) {
      float dx = ((float)i + 0.5f) * texel - W * 0.5f, dy = ((float)j + 0.5f) * texel - H * 0.5f;
      float r = sqrtf(dx * dx + dy * dy) / (float)(H < W ? H : W);
      uint8_t d = r < 0.15f ? 255 : r < 0.30f ? 128 : 64;
      mapped[(j * mapW + i) * 2] = d; mapped[(j * mapW + i) * 2 + 1] = d;
    }
  }
  vkUnmapMemory(dev, stagingMem);

  // Pipeline (flag required when the pass uses a density map).
  VkShaderModule vs = shader(dev, fdm_probe_vert_spv, sizeof fdm_probe_vert_spv);
  VkShaderModule fs = shader(dev, fdm_probe_frag_spv, sizeof fdm_probe_frag_spv);
  VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
  VkPipelineLayout layout; CHECK(vkCreatePipelineLayout(dev, &plci, NULL, &layout));
  VkPipelineShaderStageCreateInfo stages[2] = {
    { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL },
    { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL } };
  VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
  VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
  vp.viewportCount = 1; vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
  rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState cba = { 0 }; cba.colorWriteMask = 0xf;
  VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
  cb.attachmentCount = 1; cb.pAttachments = &cba;
  VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
  VkPipelineDynamicStateCreateInfo dsi = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
  dsi.dynamicStateCount = 2; dsi.pDynamicStates = dyn;
  VkFormat colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
  VkPipelineRenderingCreateInfo prci = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
  prci.colorAttachmentCount = 1; prci.pColorAttachmentFormats = &colorFormat;
  VkGraphicsPipelineCreateInfo gpci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &prci };
  gpci.flags = VK_PIPELINE_CREATE_RENDERING_FRAGMENT_DENSITY_MAP_ATTACHMENT_BIT_EXT;
  gpci.stageCount = 2; gpci.pStages = stages; gpci.pVertexInputState = &vi; gpci.pInputAssemblyState = &ia;
  gpci.pViewportState = &vp; gpci.pRasterizationState = &rs; gpci.pMultisampleState = &ms;
  gpci.pColorBlendState = &cb; gpci.pDynamicState = &dsi; gpci.layout = layout;
  VkPipeline pipeline; CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipeline));

  VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
  cpci.queueFamilyIndex = qf;
  VkCommandPool pool; CHECK(vkCreateCommandPool(dev, &cpci, NULL, &pool));
  VkCommandBuffer cmds[3]; VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
  cbai.commandPool = pool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
  CHECK(vkAllocateCommandBuffers(dev, &cbai, &cmds[0]));
  cbai.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
  CHECK(vkAllocateCommandBuffers(dev, &cbai, &cmds[1]));
  cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  CHECK(vkAllocateCommandBuffers(dev, &cbai, &cmds[2]));

  VkViewport viewport = { 0, 0, (float)W, (float)H, 0, 1 }; VkRect2D scissor = { { 0, 0 }, { W, H } };
  VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
  ca.imageView = colorView; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  ca.clearValue.color.float32[0] = 0.5f; ca.clearValue.color.float32[2] = 0.5f; ca.clearValue.color.float32[3] = 1.0f;
  VkRenderingFragmentDensityMapAttachmentInfoEXT fdmInfo = { VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_DENSITY_MAP_ATTACHMENT_INFO_EXT };
  VkImageLayout mapLayout = optimal ? VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT : VK_IMAGE_LAYOUT_GENERAL;
  VkImageLayout copyLayout = optimal ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
  fdmInfo.imageView = mapView; fdmInfo.imageLayout = mapLayout;
  VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO, nomap ? NULL : &fdmInfo };
  ri.renderArea = scissor; ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
  if (secondary) ri.flags = VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT;

  VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (secondary) {
    VkCommandBufferInheritanceRenderingInfo inh = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO };
    inh.colorAttachmentCount = 1; inh.pColorAttachmentFormats = &colorFormat; inh.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    inh.flags = ri.flags & ~VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT;
    VkCommandBufferInheritanceInfo ii = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO, &inh };
    VkCommandBufferBeginInfo sb = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    sb.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
    sb.pInheritanceInfo = &ii;
    CHECK(vkBeginCommandBuffer(cmds[1], &sb));
    vkCmdBindPipeline(cmds[1], VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdSetViewport(cmds[1], 0, 1, &viewport); vkCmdSetScissor(cmds[1], 0, 1, &scissor);
    vkCmdDraw(cmds[1], 3, 1, 0, 0);
    CHECK(vkEndCommandBuffer(cmds[1]));
  }

  VkCommandBuffer up = presubmit ? cmds[2] : cmds[0];
  CHECK(vkBeginCommandBuffer(up, &begin));
  barrier(up, map, VK_IMAGE_LAYOUT_UNDEFINED, copyLayout, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
  VkBufferImageCopy copy = { 0 };
  copy.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
  copy.imageExtent = (VkExtent3D){ mapW, mapH, 1 };
  vkCmdCopyBufferToImage(up, staging, map, copyLayout, 1, &copy);
  barrier(up, map, copyLayout, mapLayout, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_PIPELINE_STAGE_FRAGMENT_DENSITY_PROCESS_BIT_EXT, VK_ACCESS_FRAGMENT_DENSITY_MAP_READ_BIT_EXT);
  if (presubmit) {
    CHECK(vkEndCommandBuffer(up));
    VkSubmitInfo us = { VK_STRUCTURE_TYPE_SUBMIT_INFO }; us.commandBufferCount = 1; us.pCommandBuffers = &up;
    CHECK(vkQueueSubmit(queue, 1, &us, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(queue));
    CHECK(vkBeginCommandBuffer(cmds[0], &begin));
  }
  barrier(cmds[0], color, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
  vkCmdBeginRendering(cmds[0], &ri);
  if (secondary) {
    vkCmdExecuteCommands(cmds[0], 1, &cmds[1]);
  } else {
    vkCmdBindPipeline(cmds[0], VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdSetViewport(cmds[0], 0, 1, &viewport); vkCmdSetScissor(cmds[0], 0, 1, &scissor);
    vkCmdDraw(cmds[0], 3, 1, 0, 0);
  }
  vkCmdEndRendering(cmds[0]);
  barrier(cmds[0], color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
  VkBufferImageCopy rb = { 0 };
  rb.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
  rb.imageExtent = (VkExtent3D){ W, H, 1 };
  vkCmdCopyImageToBuffer(cmds[0], color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &rb);
  CHECK(vkEndCommandBuffer(cmds[0]));

  VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO }; si.commandBufferCount = 1; si.pCommandBuffers = &cmds[0];
  CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
  CHECK(vkQueueWaitIdle(queue));

  uint32_t *px; CHECK(vkMapMemory(dev, readbackMem, 0, VK_WHOLE_SIZE, 0, (void **)&px));
  printf("samples: %08x %08x %08x %08x (clear colour is 0xff800080)\n", px[0], px[1], px[2], px[W * H / 2 + 777]);
  printf("config: %ux%u map %ux%u texel %u image %ux%u %s %s %s%s\n", W, H, mapW, mapH, texel, imageExtent.width, imageExtent.height,
         oversized ? "oversized" : "exact", secondary ? "secondary-cmd" : "inline", optimal ? "optimal-layout" : "general-layout", presubmit ? " presubmit" : "");

  // Horizontal run length per column third on a full-density row and on a half-density row;
  // vertical run length on the left (full x density) third, top and bottom half.
  uint32_t rows[2] = { H / 4, 3 * H / 4 };
  for (int r = 0; r < 2; r++) {
    printf("row y=%u median fragment width px by third: %d %d %d\n", rows[r],
           median_run(px, W, 0, W / 3, rows[r], 0, H, 0, 0, 0),
           median_run(px, W, W / 3, 2 * W / 3, rows[r], 0, H, 0, 0, 0),
           median_run(px, W, 2 * W / 3, W, rows[r], 0, H, 0, 0, 0));
  }
  printf("vertical median fragment height px: top half %d bottom half %d\n",
         median_run(px, W, 0, 0, 0, 1, H, 0, H / 2, W / 6), median_run(px, W, 0, 0, 0, 1, H, H / 2, H, W / 6));

  // Boundary positions along row y=H/4: first x whose run length exceeds 1 and 2.
  uint32_t y = H / 4, firstGt1 = 0, firstGt2 = 0; int len = 1;
  for (uint32_t i = 1; i < W; i++) {
    if (px[y * W + i] == px[y * W + i - 1]) { len++; if (!firstGt1 && len > 1) firstGt1 = i - 1;
      if (!firstGt2 && len > 2) firstGt2 = i - 2; } else len = 1;
  }
  printf("first coarse pixel x (width>1): %u (expected ~%u); first width>2: %u (expected ~%u)\n",
         firstGt1, W / 3, firstGt2, 2 * W / 3);

  if (radial) {
    // Fragment footprint (run of identical pixels) at sample points; expected 1x1 centre, 2x2 ring, 4x4 outside.
    struct { const char *name; float fx, fy; } pts[] = {
      { "centre", 0.5f, 0.5f }, { "ring (r~0.22)", 0.5f + 0.22f * H / W, 0.5f }, { "ring below", 0.5f, 0.72f },
      { "corner", 0.05f, 0.05f }, { "far right", 0.95f, 0.5f } };
    for (unsigned k = 0; k < sizeof pts / sizeof pts[0]; k++) {
      uint32_t x = (uint32_t)(pts[k].fx * W), y = (uint32_t)(pts[k].fy * H);
      uint32_t a = x, b = x; while (a > 0 && px[y * W + a - 1] == px[y * W + x]) a--; while (b + 1 < W && px[y * W + b + 1] == px[y * W + x]) b++;
      uint32_t c = y, d = y; while (c > 0 && px[(c - 1) * W + x] == px[y * W + x]) c--; while (d + 1 < H && px[(d + 1) * W + x] == px[y * W + x]) d++;
      printf("point %-14s (%u,%u): fragment footprint %ux%u px\n", pts[k].name, x, y, b - a + 1, d - c + 1);
    }
  }

  {
    // Coarse picture of the footprint (width x height of the run of identical pixels) over a 12x6 grid.
    printf("footprint grid (rows top->bottom, 12 columns):\n");
    for (int gy = 0; gy < 6; gy++) {
      for (int gx = 0; gx < 12; gx++) {
        uint32_t x = (uint32_t)((gx + 0.5f) * W / 12), y = (uint32_t)((gy + 0.5f) * H / 6);
        uint32_t a = x, b = x, c = y, d = y;
        while (a > 0 && px[y * W + a - 1] == px[y * W + x]) a--; while (b + 1 < W && px[y * W + b + 1] == px[y * W + x]) b++;
        while (c > 0 && px[(c - 1) * W + x] == px[y * W + x]) c--; while (d + 1 < H && px[(d + 1) * W + x] == px[y * W + x]) d++;
        printf(" %ux%u", b - a + 1, d - c + 1);
      }
      printf("\n");
    }
  }
  // Segments of equal fragment width along two rows (start x, width, number of fragments in the segment).
  for (int r = 0; r < 2; r++) {
    uint32_t yy = rows[r];
    printf("row y=%u segments (x:width x count):", yy);
    uint32_t i = 0; int lastw = -1; uint32_t segStart = 0, segCount = 0;
    while (i < W) {
      uint32_t j = i + 1; while (j < W && px[yy * W + j] == px[yy * W + i]) j++;
      int w = (int)(j - i);
      if (w != lastw) { if (lastw > 0) printf(" %u:%dx%u", segStart, lastw, segCount); lastw = w; segStart = i; segCount = 0; }
      segCount++; i = j;
    }
    printf(" %u:%dx%u\n", segStart, lastw, segCount);
  }

  // Hash of the unique colour count as a coarse sanity check that shading really ran.
  uint32_t distinct = 0, prev = 0xdeadbeef;
  for (uint32_t i = 0; i < W * H; i++) if (px[i] != prev) { distinct++; prev = px[i]; }
  printf("colour transitions in image: %u (full-rate would be ~%u)\n", distinct, W * H);
  return 0;
}
