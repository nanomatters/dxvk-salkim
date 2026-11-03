#include <vulkan/vulkan.h>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <vector>

namespace dxvk {

template<typename T> using Rc = std::shared_ptr<T>;

enum class DxvkFormatFlag { ColorSpaceSrgb };

struct FormatInfo {
  struct {
    bool srgb = false;
    bool test(DxvkFormatFlag) const { return srgb; }
  } flags;
};

struct DxvkImageCreateInfo {
  VkImageType type;
  VkFormat format;
  VkSampleCountFlagBits sampleCount;
  VkExtent3D extent;
  uint32_t mipLevels, numLayers;
  VkImageUsageFlags usage;
  VkPipelineStageFlags stages;
  VkAccessFlags access;
  VkImageTiling tiling;
  VkImageLayout layout, initialLayout;
  VkColorSpaceKHR colorSpace;
  const char* debugName;
};

struct DxvkImageViewKey {
  VkImageViewType viewType;
  VkFormat format;
  VkImageAspectFlags aspects;
  uint32_t mipIndex, mipCount, layerIndex, layerCount;
  VkImageUsageFlags usage;
};

struct DxvkImage;
struct DxvkImageView {
  DxvkImageViewKey key;
  DxvkImage* resource;
  FormatInfo format;

  DxvkImage* image() const { return resource; }
  const FormatInfo* formatInfo() const { return &format; }
};

struct DxvkImage {
  DxvkImageCreateInfo properties;
  std::vector<DxvkImageViewKey> views;

  const DxvkImageCreateInfo& info() const { return properties; }
  Rc<DxvkImageView> createView(DxvkImageViewKey key) {
    views.push_back(key);
    auto result = std::make_shared<DxvkImageView>();
    result->key = key;
    result->resource = this;
    result->format.flags.srgb = key.format == VK_FORMAT_R8G8B8A8_SRGB;
    return result;
  }
};

struct Device {
  Rc<DxvkImage> createImage(DxvkImageCreateInfo info, VkMemoryPropertyFlags) {
    auto result = std::make_shared<DxvkImage>();
    result->properties = info;
    return result;
  }
};

struct DxvkSwapchainBlitter {
  Rc<Device> m_device = std::make_shared<Device>();
  Rc<DxvkImage> m_hudImage;
  Rc<DxvkImageView> m_hudRtv, m_hudSrv;

  void createHudImage(VkExtent3D extent);
  void destroyHudImage();
  bool needsComposition(const Rc<DxvkImageView>& dstView);
};

#include "composition.h"

}

static unsigned checks;
static void check(bool value, const char* message) {
  ++checks;
  if (!value) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

int main() {
  dxvk::DxvkSwapchainBlitter blitter;
  for (auto size : {VkExtent3D {1920, 1080, 1}, VkExtent3D {2560, 1440, 1}}) {
    blitter.createHudImage(size);
    check(bool(blitter.m_hudImage), "HUD image exists");
    check(bool(blitter.m_hudRtv), "HUD image can be rendered");
    check(bool(blitter.m_hudSrv), "HUD image can be sampled by the final blit");
    check(blitter.m_hudImage->views.size() == 2u, "render and sampled views both exist");
    check(blitter.m_hudRtv->key.usage == VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
      "render view has attachment usage");
    check(blitter.m_hudSrv->key.usage == VK_IMAGE_USAGE_SAMPLED_BIT,
      "sampled view has sampled usage");
    check(blitter.m_hudSrv->image() == blitter.m_hudRtv->image(),
      "both views refer to the rendered image");
    check(blitter.m_hudImage->info().extent.width == size.width
       && blitter.m_hudImage->info().extent.height == size.height,
      "HUD image follows the target size");
    blitter.destroyHudImage();
    check(!blitter.m_hudImage && !blitter.m_hudRtv && !blitter.m_hudSrv,
      "destroy releases the image and both views");
  }

  dxvk::DxvkImage image;
  auto view = std::make_shared<dxvk::DxvkImageView>();
  view->resource = &image;
  image.properties.colorSpace = VK_COLOR_SPACE_HDR10_ST2084_EXT;
  check(blitter.needsComposition(view), "HDR10 uses HUD composition");
  image.properties.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
  check(blitter.needsComposition(view), "SDR UNORM uses HUD composition");
  view->format.flags.srgb = true;
  check(!blitter.needsComposition(view), "SDR sRGB supports direct HUD drawing");
  image.properties.colorSpace = VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT;
  view->format.flags.srgb = false;
  check(!blitter.needsComposition(view), "scRGB supports direct HUD drawing");
  std::cout << "HUD composition: " << checks << " checks passed\n";
}
