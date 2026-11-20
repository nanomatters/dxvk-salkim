#pragma once

#include <array>
#include <memory>
#include <vector>

#include "dxvk_fence.h"
#include "dxvk_image.h"
#include "dxvk_video_av1.h"

namespace dxvk {

  class DxvkVideoMemory;
  struct DxvkVideoDecodeInfo;
  struct DxvkDeviceQueue;

  enum class DxvkVideoProfile {
    H264,
    HevcMain,
    HevcMain10,
    Av1Main,
    Av1Main10,
  };

  struct DxvkH264Reference {
    uint8_t pictureId = 0;
    StdVideoDecodeH264ReferenceInfo info = { };
  };


  struct DxvkH264SessionParameters {
    StdVideoH264SequenceParameterSet sps = { };
    StdVideoH264PictureParameterSet pps = { };
    StdVideoH264ScalingLists scalingLists = { };
  };

  struct DxvkH264PictureParameters : DxvkH264SessionParameters {
    StdVideoDecodeH264PictureInfo picture = { };
    StdVideoDecodeH264ReferenceInfo currentReference = { };
    std::array<DxvkH264Reference, 16> references = { };
    uint32_t referenceCount = 0;
    uint8_t currentPictureId = 0;
    std::vector<uint32_t> sliceOffsets;
  };


  struct DxvkH265Reference {
    uint8_t pictureId = 0;
    StdVideoDecodeH265ReferenceInfo info = { };
  };

  struct DxvkH265SessionParameters {
    StdVideoH265VideoParameterSet vps = { };
    StdVideoH265SequenceParameterSet sps = { };
    StdVideoH265PictureParameterSet pps = { };
    StdVideoH265DecPicBufMgr dpbMgr = { };
    StdVideoH265ProfileTierLevel profileTierLevel = { };
    StdVideoH265ScalingLists scalingLists = { };
  };

  struct DxvkH265PictureParameters : DxvkH265SessionParameters {
    StdVideoDecodeH265PictureInfo picture = { };
    StdVideoDecodeH265ReferenceInfo currentReference = { };
    std::array<DxvkH265Reference, 15> references = { };
    uint32_t referenceCount = 0;
    uint8_t currentPictureId = 0;
    std::vector<uint32_t> sliceOffsets;
  };


  class DxvkVideoImage : public RcObject {

  public:

    explicit DxvkVideoImage(const Rc<vk::DeviceFn>& vkd);
    ~DxvkVideoImage();

  private:

    friend class DxvkVideoDecoder;

    Rc<vk::DeviceFn> m_vkd;
    VkImage m_image = VK_NULL_HANDLE;
    std::unique_ptr<DxvkVideoMemory> m_memory;
    VkImageView m_view = VK_NULL_HANDLE;

  };


  struct DxvkDecodedFrame {
    Rc<DxvkVideoImage> storage;
    Rc<DxvkImage> image;
    Rc<DxvkFence> fence;
    uint64_t fenceValue = 0;
    uint32_t layer = 0;
  };


  class DxvkVideoDecoder : public RcObject {

  public:

    static bool supports(const DxvkDevice* device, DxvkVideoProfile profile);

    DxvkVideoDecoder(DxvkDevice* device, DxvkVideoProfile profile, VkExtent2D codedExtent);
    ~DxvkVideoDecoder();

    DxvkDecodedFrame decode(
      const DxvkAv1PictureParameters& params,
      const std::vector<uint8_t>& bytes);

    DxvkDecodedFrame decode(
      const DxvkH265PictureParameters& params,
      const std::vector<uint8_t>& bytes);

    DxvkDecodedFrame decode(
      const DxvkH264PictureParameters& params,
      const std::vector<uint8_t>& bytes);

  private:

    DxvkDevice* m_device;
    Rc<vk::DeviceFn> m_vkd;

    DxvkVideoProfile m_codec;
    const DxvkVideoDecodeInfo& m_caps;
    const DxvkDeviceQueue& m_queue;
    VkVideoDecodeAV1ProfileInfoKHR m_av1Profile = { VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_PROFILE_INFO_KHR };
    VkVideoDecodeH265ProfileInfoKHR m_hevcProfile = { VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PROFILE_INFO_KHR };
    VkVideoDecodeH264ProfileInfoKHR m_h264Profile = { VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR };
    VkVideoProfileInfoKHR m_profile = { VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR };
    VkVideoProfileListInfoKHR m_profiles = { VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR };

    VkExtent2D m_extent;
    VkExtent2D m_dpbExtent = { };
    VkVideoSessionKHR m_session = VK_NULL_HANDLE;
    VkCommandPool m_pool = VK_NULL_HANDLE;
    struct Submission;
    std::array<std::unique_ptr<Submission>, 3> m_submissions;
    Rc<DxvkFence> m_complete;
    uint64_t m_completeValue = 0;

    std::vector<std::unique_ptr<DxvkVideoMemory>> m_sessionMemory;
    Rc<DxvkVideoImage> m_dpb;
    std::vector<int32_t> m_pictureIds;
    std::array<StdVideoDecodeAV1ReferenceInfo, STD_VIDEO_AV1_NUM_REF_FRAMES + 1> m_av1References = { };
    std::array<VkExtent2D, STD_VIDEO_AV1_NUM_REF_FRAMES + 1> m_av1ReferenceExtents = { };
    uint32_t m_maxSlots = 0;
    uint32_t m_maxReferences = 0;
    bool m_initialized = false;

    void init();
    void destroy();

    struct Picture;

    template<typename Parameters, typename SlotInfo, typename Reference>
    DxvkDecodedFrame decodePicture(const Parameters& params, const std::vector<uint8_t>& bytes,
      const Picture& picture, const SlotInfo& currentInfo, uint32_t referenceCount, const Reference* sourceReferences);

    DxvkDecodedFrame dropPicture(uint8_t currentPictureId, bool reset);

    Rc<DxvkVideoImage> createImage(
      const VkVideoFormatPropertiesKHR& format,
            VkImageUsageFlags           usage,
            uint32_t                    layers,
            bool                        graphicsAccess);

  };

}
