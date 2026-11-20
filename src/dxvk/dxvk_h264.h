#pragma once

#include <array>
#include <memory>
#include <vector>

#include "dxvk_fence.h"
#include "dxvk_image.h"

namespace dxvk {

  class DxvkH264Memory;

  struct DxvkH264Reference {
    uint8_t pictureId = 0;
    StdVideoDecodeH264ReferenceInfo info = { };
  };


  struct DxvkH264PictureParameters {
    StdVideoH264SequenceParameterSet sps = { };
    StdVideoH264PictureParameterSet pps = { };
    StdVideoH264ScalingLists scalingLists = { };
    StdVideoDecodeH264PictureInfo picture = { };
    StdVideoDecodeH264ReferenceInfo currentReference = { };
    std::array<DxvkH264Reference, 16> references = { };
    uint32_t referenceCount = 0;
    uint8_t currentPictureId = 0;
    std::vector<uint32_t> sliceOffsets;
  };


  class DxvkH264Image : public RcObject {

  public:

    explicit DxvkH264Image(const Rc<vk::DeviceFn>& vkd);
    ~DxvkH264Image();

  private:

    friend class DxvkH264Decoder;

    Rc<vk::DeviceFn> m_vkd;
    VkImage m_image = VK_NULL_HANDLE;
    std::unique_ptr<DxvkH264Memory> m_memory;
    VkImageView m_view = VK_NULL_HANDLE;

  };


  struct DxvkH264DecodedFrame {
    Rc<DxvkH264Image> storage;
    Rc<DxvkImage> image;
    Rc<DxvkFence> fence;
    uint64_t fenceValue = 0;
    uint32_t layer = 0;
  };


  class DxvkH264Decoder : public RcObject {

  public:

    static bool supports(const DxvkDevice* device);

    DxvkH264Decoder(DxvkDevice* device, VkExtent2D codedExtent);
    ~DxvkH264Decoder();

    DxvkH264DecodedFrame decode(
      const DxvkH264PictureParameters& params,
      const std::vector<uint8_t>& bytes);

  private:

    DxvkDevice* m_device;
    Rc<vk::DeviceFn> m_vkd;

    VkVideoDecodeH264ProfileInfoKHR m_h264Profile = { VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR };
    VkVideoProfileInfoKHR m_profile = { VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR, &m_h264Profile };
    VkVideoProfileListInfoKHR m_profiles = { VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR };

    VkExtent2D m_extent;
    VkVideoSessionKHR m_session = VK_NULL_HANDLE;
    VkCommandPool m_pool = VK_NULL_HANDLE;
    struct Submission;
    std::array<std::unique_ptr<Submission>, 3> m_submissions;
    Rc<DxvkFence> m_complete;
    uint64_t m_completeValue = 0;

    std::vector<std::unique_ptr<DxvkH264Memory>> m_sessionMemory;
    Rc<DxvkH264Image> m_dpb;
    std::vector<int32_t> m_pictureIds;
    uint32_t m_maxSlots = 0;
    uint32_t m_maxReferences = 0;
    bool m_initialized = false;

    void init();
    void destroy();

    DxvkH264DecodedFrame dropPicture(const DxvkH264PictureParameters& params);

    Rc<DxvkH264Image> createImage(
      const VkVideoFormatPropertiesKHR& format,
            VkImageUsageFlags           usage,
            uint32_t                    layers,
            bool                        graphicsAccess);

  };

}
