#include <algorithm>
#include <cstring>
#include <limits>

#include "dxvk_device.h"
#include "dxvk_h264.h"

namespace dxvk {

  namespace {

    constexpr VkExtensionProperties H264HeaderVersion = {
      VK_STD_VULKAN_VIDEO_CODEC_H264_DECODE_EXTENSION_NAME,
      VK_STD_VULKAN_VIDEO_CODEC_H264_DECODE_API_VERSION_1_0_0,
    };

    constexpr int32_t EmptyPictureSlot = -1;
    constexpr int32_t InvalidPictureSlot = -2;

    void checkVideo(VkResult result, const char* operation) {
      if (result != VK_SUCCESS)
        throw DxvkError(str::format("DxvkH264: ", operation, " failed: ", result));
    }

  }


  class DxvkH264Memory {

  public:

    DxvkH264Memory(
            DxvkDevice*           device,
      const VkMemoryAllocateInfo& info,
            uint32_t              heap)
    : m_adapter(device->adapter()), m_vkd(device->vkd()),
      m_heap(heap), m_size(int64_t(info.allocationSize)),
      m_coherent(device->adapter()->memoryProperties().memoryTypes[info.memoryTypeIndex].propertyFlags
        & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) {
      checkVideo(m_vkd->vkAllocateMemory(m_vkd->device(), &info, nullptr, &m_memory), "vkAllocateMemory");
      m_adapter->notifyMemoryStats(m_heap, m_size, m_size);
    }

    ~DxvkH264Memory() {
      m_vkd->vkFreeMemory(m_vkd->device(), m_memory, nullptr);
      m_adapter->notifyMemoryStats(m_heap, -m_size, -m_size);
    }

    DxvkH264Memory(const DxvkH264Memory&) = delete;
    DxvkH264Memory& operator = (const DxvkH264Memory&) = delete;

    bool isHostCoherent() const { return m_coherent; }

    VkDeviceMemory handle() const {
      return m_memory;
    }

  private:

    Rc<DxvkAdapter>  m_adapter;
    Rc<vk::DeviceFn> m_vkd;
    VkDeviceMemory   m_memory = VK_NULL_HANDLE;
    uint32_t         m_heap;
    int64_t          m_size;
    bool             m_coherent;

  };


  namespace {

    std::unique_ptr<DxvkH264Memory> allocateVideoMemory(
            DxvkDevice*           device,
      const VkMemoryRequirements& requirements,
            VkMemoryPropertyFlags required,
            VkMemoryPropertyFlags preferred,
      const void*                 next = nullptr) {
      const auto& memory = device->adapter()->memoryProperties();
      uint32_t index = memory.memoryTypeCount;

      for (uint32_t i = 0; i < memory.memoryTypeCount; i++) {
        const auto flags = memory.memoryTypes[i].propertyFlags;
        if (!(requirements.memoryTypeBits & (1u << i)) || (flags & required) != required)
          continue;

        if (index == memory.memoryTypeCount || (flags & preferred) == preferred)
          index = i;

        if ((flags & preferred) == preferred)
          break;
      }

      if (index == memory.memoryTypeCount)
        throw DxvkError("DxvkH264: No compatible memory type");

      if (requirements.size > VkDeviceSize(std::numeric_limits<int64_t>::max()))
        throw DxvkError("DxvkH264: Invalid memory allocation size");

      VkMemoryAllocateInfo info = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, next };
      info.allocationSize = requirements.size;
      info.memoryTypeIndex = index;

      return std::make_unique<DxvkH264Memory>(device, info, memory.memoryTypes[index].heapIndex);
    }

    struct VideoBitstream {
      explicit VideoBitstream(const Rc<vk::DeviceFn>& vk)
      : vkd(vk) { }

      ~VideoBitstream() {
        if (mapped)
          vkd->vkUnmapMemory(vkd->device(), memory->handle());
        if (buffer)
          vkd->vkDestroyBuffer(vkd->device(), buffer, nullptr);
      }

      Rc<vk::DeviceFn> vkd;
      VkBuffer buffer = VK_NULL_HANDLE;
      VkDeviceSize capacity = 0;
      void* mapped = nullptr;
      std::unique_ptr<DxvkH264Memory> memory;
    };

    VkImageMemoryBarrier2 videoBarrier(
            VkImage               image,
            uint32_t              layers,
            VkImageLayout         oldLayout,
            VkImageLayout         newLayout,
            VkAccessFlags2        srcAccess,
            VkAccessFlags2        dstAccess) {
      VkImageMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
      barrier.srcStageMask = srcAccess ? VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR : VK_PIPELINE_STAGE_2_NONE;
      barrier.srcAccessMask = srcAccess;
      barrier.dstStageMask = dstAccess ? VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR : VK_PIPELINE_STAGE_2_NONE;
      barrier.dstAccessMask = dstAccess;
      barrier.oldLayout = oldLayout;
      barrier.newLayout = newLayout;
      barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.image = image;
      barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers };
      return barrier;
    }

  }


  struct DxvkH264Decoder::Submission {
    explicit Submission(const Rc<vk::DeviceFn>& vkd) : vk(vkd) { }

    void setParameters(VkVideoSessionKHR session, const DxvkH264PictureParameters& params);
    VkDeviceSize uploadBitstream(DxvkDevice* device, const VkVideoProfileListInfoKHR& profiles,
      const std::vector<uint8_t>& bytes);

    VkResult wait() const {
      if (!pending)
        return VK_SUCCESS;
      VkResult status = vk->vkGetFenceStatus(vk->device(), fence);
      if (status == VK_NOT_READY)
        status = vk->vkWaitForFences(vk->device(), 1, &fence, VK_TRUE, UINT64_MAX);
      return status;
    }

    ~Submission() {
      // A failed host wait can leave work pending on a live device. Keep its
      // resources alive until the fence signals or the device reports loss.
      auto status = wait();
      while (status != VK_SUCCESS && status != VK_ERROR_DEVICE_LOST) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        status = vk->vkGetFenceStatus(vk->device(), fence);
      }
      if (parameters)
        vk->vkDestroyVideoSessionParametersKHR(vk->device(), parameters, nullptr);
      if (fence)
        vk->vkDestroyFence(vk->device(), fence, nullptr);
    }

    Rc<vk::DeviceFn> vk;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool pending = false;
    Rc<DxvkH264Image> dpb;
    Rc<DxvkH264Image> output;
    Rc<DxvkImage> image;
    std::unique_ptr<VideoBitstream> bitstream;
    VkVideoSessionParametersKHR parameters = VK_NULL_HANDLE;
    StdVideoH264SequenceParameterSet sps = { };
    StdVideoH264PictureParameterSet pps = { };
    StdVideoH264ScalingLists scalingLists = { };
  };


  void DxvkH264Decoder::Submission::setParameters(
          VkVideoSessionKHR          session,
    const DxvkH264PictureParameters& params) {
    // The translator leaves optional pointers null and supplies the PPS scaling
    // lists separately. Compare their contents before fixing up the Vulkan PPS.
    if (parameters && !std::memcmp(&sps, &params.sps, sizeof(sps))
     && !std::memcmp(&pps, &params.pps, sizeof(pps))
     && !std::memcmp(&scalingLists, &params.scalingLists, sizeof(scalingLists)))
      return;

    auto vkPps = params.pps;
    if (vkPps.flags.pic_scaling_matrix_present_flag)
      vkPps.pScalingLists = &params.scalingLists;

    VkVideoDecodeH264SessionParametersAddInfoKHR add = { VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_ADD_INFO_KHR };
    add.stdSPSCount = 1;
    add.pStdSPSs = &params.sps;
    add.stdPPSCount = 1;
    add.pStdPPSs = &vkPps;

    VkVideoDecodeH264SessionParametersCreateInfoKHR h264Parameters = { VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_CREATE_INFO_KHR };
    h264Parameters.maxStdSPSCount = 1;
    h264Parameters.maxStdPPSCount = 1;
    h264Parameters.pParametersAddInfo = &add;

    VkVideoSessionParametersCreateInfoKHR parameterInfo = { VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR, &h264Parameters };
    parameterInfo.videoSession = session;

    if (parameters)
      vk->vkDestroyVideoSessionParametersKHR(vk->device(), parameters, nullptr);
    parameters = VK_NULL_HANDLE;
    checkVideo(vk->vkCreateVideoSessionParametersKHR(vk->device(), &parameterInfo, nullptr, &parameters),
      "vkCreateVideoSessionParametersKHR");
    sps = params.sps;
    pps = params.pps;
    scalingLists = params.scalingLists;
  }


  VkDeviceSize DxvkH264Decoder::Submission::uploadBitstream(
          DxvkDevice*                device,
    const VkVideoProfileListInfoKHR& profiles,
    const std::vector<uint8_t>&      bytes) {
    const auto alignment = device->properties().h264Decode.capabilities.minBitstreamBufferSizeAlignment;
    VkDeviceSize size = align(VkDeviceSize(bytes.size()), alignment);

    if (size < bytes.size() || size > std::numeric_limits<size_t>::max())
      throw DxvkError("DxvkH264: Invalid bitstream size");

    if (!bitstream || bitstream->capacity < size) {
      auto upload = std::make_unique<VideoBitstream>(vk);
      VkBufferCreateInfo buffer = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &profiles };
      buffer.size = align(size, VkDeviceSize(65536));
      buffer.usage = VK_BUFFER_USAGE_VIDEO_DECODE_SRC_BIT_KHR;
      buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      checkVideo(vk->vkCreateBuffer(vk->device(), &buffer, nullptr, &upload->buffer), "vkCreateBuffer");

      VkMemoryRequirements requirements;
      vk->vkGetBufferMemoryRequirements(vk->device(), upload->buffer, &requirements);
      VkMemoryDedicatedAllocateInfo dedicated = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
      dedicated.buffer = upload->buffer;
      upload->memory = allocateVideoMemory(device, requirements,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &dedicated);
      checkVideo(vk->vkBindBufferMemory(vk->device(), upload->buffer, upload->memory->handle(), 0), "vkBindBufferMemory");

      checkVideo(vk->vkMapMemory(vk->device(), upload->memory->handle(), 0, VK_WHOLE_SIZE, 0, &upload->mapped), "vkMapMemory");
      upload->capacity = buffer.size;
      bitstream = std::move(upload);
    }

    void* mapped = bitstream->mapped;
    std::memcpy(mapped, bytes.data(), bytes.size());
    std::memset(static_cast<uint8_t*>(mapped) + bytes.size(), 0, size - bytes.size());

    if (!bitstream->memory->isHostCoherent()) {
      VkMappedMemoryRange range = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE };
      range.memory = bitstream->memory->handle();
      range.size = VK_WHOLE_SIZE;
      checkVideo(vk->vkFlushMappedMemoryRanges(vk->device(), 1, &range), "vkFlushMappedMemoryRanges");
    }
    return size;
  }


  DxvkH264Image::DxvkH264Image(const Rc<vk::DeviceFn>& vkd)
  : m_vkd(vkd) { }


  DxvkH264Image::~DxvkH264Image() {
    if (m_view)
      m_vkd->vkDestroyImageView(m_vkd->device(), m_view, nullptr);
    if (m_image)
      m_vkd->vkDestroyImage(m_vkd->device(), m_image, nullptr);
  }


  DxvkH264Decoder::DxvkH264Decoder(DxvkDevice* device, VkExtent2D codedExtent)
  : m_device(device), m_vkd(device->vkd()), m_extent(codedExtent) {
    try {
      init();
    } catch (...) {
      destroy();
      throw;
    }
  }


  DxvkH264Decoder::~DxvkH264Decoder() {
    destroy();
  }


  void DxvkH264Decoder::destroy() {
    for (auto& slot : m_submissions)
      slot.reset();
    if (m_pool)
      m_vkd->vkDestroyCommandPool(m_vkd->device(), m_pool, nullptr);
    if (m_session)
      m_vkd->vkDestroyVideoSessionKHR(m_vkd->device(), m_session, nullptr);
  }


  bool DxvkH264Decoder::supports(const DxvkDevice* device) {
    const auto& video = device->properties().h264Decode;
    const auto& header = video.capabilities.stdHeaderVersion;
    const auto& vk = device->vkd();
    return device->queues().videoDecode.queueHandle && video.distinctOutput
        && video.capabilities.maxDpbSlots >= 2 && video.capabilities.maxActiveReferencePictures
        && header.specVersion >= H264HeaderVersion.specVersion
        && !std::strcmp(header.extensionName, H264HeaderVersion.extensionName)
        && vk->vkCreateVideoSessionKHR && vk->vkDestroyVideoSessionKHR
        && vk->vkGetVideoSessionMemoryRequirementsKHR && vk->vkBindVideoSessionMemoryKHR
        && vk->vkCreateVideoSessionParametersKHR && vk->vkDestroyVideoSessionParametersKHR
        && vk->vkCmdBeginVideoCodingKHR && vk->vkCmdControlVideoCodingKHR
        && vk->vkCmdDecodeVideoKHR && vk->vkCmdEndVideoCodingKHR;
  }


  void DxvkH264Decoder::init() {
    const auto& video = m_device->properties().h264Decode;
    const auto& queue = m_device->queues().videoDecode;
    const auto& capabilities = video.capabilities;

    if (!supports(m_device))
      throw DxvkError("DxvkH264: Video decode is unavailable");

    if ((m_extent.width | m_extent.height) & 15u)
      throw DxvkError("DxvkH264: Coded extent must be macroblock aligned");

    if (m_extent.width < capabilities.minCodedExtent.width || m_extent.width > capabilities.maxCodedExtent.width
     || m_extent.height < capabilities.minCodedExtent.height || m_extent.height > capabilities.maxCodedExtent.height)
      throw DxvkError("DxvkH264: Coded extent is outside device limits");

    m_h264Profile.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_HIGH;
    m_h264Profile.pictureLayout = VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_PROGRESSIVE_KHR;
    m_profile.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
    m_profile.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
    m_profile.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    m_profile.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    m_profiles.profileCount = 1;
    m_profiles.pProfiles = &m_profile;

    m_maxSlots = std::min(capabilities.maxDpbSlots, 17u);
    m_maxReferences = std::min(capabilities.maxActiveReferencePictures, m_maxSlots - 1u);

    VkVideoSessionCreateInfoKHR session = { VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR };
    session.queueFamilyIndex = queue.queueFamily;
    session.pVideoProfile = &m_profile;
    session.pictureFormat = video.outputFormat.format;
    session.maxCodedExtent = m_extent;
    session.referencePictureFormat = video.referenceFormat.format;
    session.maxDpbSlots = m_maxSlots;
    session.maxActiveReferencePictures = m_maxReferences;
    session.pStdHeaderVersion = &H264HeaderVersion;
    checkVideo(m_vkd->vkCreateVideoSessionKHR(m_vkd->device(), &session, nullptr, &m_session),
      "vkCreateVideoSessionKHR");

    uint32_t memoryCount = 0;
    checkVideo(m_vkd->vkGetVideoSessionMemoryRequirementsKHR(m_vkd->device(), m_session, &memoryCount, nullptr),
      "vkGetVideoSessionMemoryRequirementsKHR");

    std::vector<VkVideoSessionMemoryRequirementsKHR> requirements(memoryCount,
      { VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR });
    checkVideo(m_vkd->vkGetVideoSessionMemoryRequirementsKHR(m_vkd->device(), m_session, &memoryCount, requirements.data()),
      "vkGetVideoSessionMemoryRequirementsKHR");

    m_sessionMemory.reserve(memoryCount);
    std::vector<VkBindVideoSessionMemoryInfoKHR> bindings(memoryCount,
      { VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR });

    for (uint32_t i = 0; i < memoryCount; i++) {
      const auto& requirement = requirements[i];
      auto allocation = allocateVideoMemory(m_device, requirement.memoryRequirements, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      bindings[i].memoryBindIndex = requirement.memoryBindIndex;
      bindings[i].memory = allocation->handle();
      bindings[i].memorySize = requirement.memoryRequirements.size;
      m_sessionMemory.push_back(std::move(allocation));
    }

    if (memoryCount)
      checkVideo(m_vkd->vkBindVideoSessionMemoryKHR(m_vkd->device(), m_session, memoryCount, bindings.data()),
        "vkBindVideoSessionMemoryKHR");

    VkCommandPoolCreateInfo pool = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = queue.queueFamily;
    checkVideo(m_vkd->vkCreateCommandPool(m_vkd->device(), &pool, nullptr, &m_pool), "vkCreateCommandPool");

    VkCommandBufferAllocateInfo command = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    command.commandPool = m_pool;
    command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command.commandBufferCount = 1;
    VkFenceCreateInfo fence = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    for (auto& slot : m_submissions) {
      slot = std::make_unique<Submission>(m_vkd);
      checkVideo(m_vkd->vkAllocateCommandBuffers(m_vkd->device(), &command, &slot->cmd), "vkAllocateCommandBuffers");
      checkVideo(m_vkd->vkCreateFence(m_vkd->device(), &fence, nullptr, &slot->fence), "vkCreateFence");
    }
    m_complete = m_device->createFence({ 0 });
  }


  Rc<DxvkH264Image> DxvkH264Decoder::createImage(
    const VkVideoFormatPropertiesKHR& format,
          VkImageUsageFlags           usage,
          uint32_t                    layers,
          bool                        graphicsAccess) {
    Rc<DxvkH264Image> image = new DxvkH264Image(m_vkd);

    const auto& queues = m_device->queues();
    const std::array<uint32_t, 2> families = { queues.videoDecode.queueFamily, queues.graphics.queueFamily };

    VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &m_profiles };
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format.format;
    info.extent = { m_extent.width, m_extent.height, 1 };
    info.mipLevels = 1;
    info.arrayLayers = layers;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (graphicsAccess && families[0] != families[1]) {
      info.sharingMode = VK_SHARING_MODE_CONCURRENT;
      info.queueFamilyIndexCount = families.size();
      info.pQueueFamilyIndices = families.data();
    }

    checkVideo(m_vkd->vkCreateImage(m_vkd->device(), &info, nullptr, &image->m_image), "vkCreateImage");

    VkMemoryRequirements requirements;
    m_vkd->vkGetImageMemoryRequirements(m_vkd->device(), image->m_image, &requirements);
    VkMemoryDedicatedAllocateInfo dedicated = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
    dedicated.image = image->m_image;
    image->m_memory = allocateVideoMemory(m_device, requirements, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &dedicated);
    checkVideo(m_vkd->vkBindImageMemory(m_vkd->device(), image->m_image, image->m_memory->handle(), 0), "vkBindImageMemory");

    VkImageViewCreateInfo view = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    view.image = image->m_image;
    view.viewType = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    view.format = format.format;
    view.components = format.componentMapping;
    view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers };
    checkVideo(m_vkd->vkCreateImageView(m_vkd->device(), &view, nullptr, &image->m_view), "vkCreateImageView");
    return image;
  }


  DxvkH264DecodedFrame DxvkH264Decoder::dropPicture(const DxvkH264PictureParameters& params) {
    // The Vulkan slot stays active until it is explicitly recycled or reset,
    // but its old contents can no longer satisfy a reference to this ID.
    for (auto& id : m_pictureIds) {
      if (id >= 0 && (params.picture.flags.IdrPicFlag || id == params.currentPictureId))
        id = InvalidPictureSlot;
    }

    return { };
  }


  DxvkH264DecodedFrame DxvkH264Decoder::decode(
    const DxvkH264PictureParameters& params,
    const std::vector<uint8_t>& bytes) {
    const bool reset = !m_initialized || params.picture.flags.IdrPicFlag;
    if (bytes.empty() || params.sliceOffsets.empty()
     || params.referenceCount > m_maxReferences || params.referenceCount > params.references.size()
     || (!m_initialized && !params.picture.flags.is_intra)
     || (params.referenceCount && reset)
     || params.sps.pic_width_in_mbs_minus1 + 1u != m_extent.width / 16u
     || params.sps.pic_height_in_map_units_minus1 + 1u != m_extent.height / 16u)
      return dropPicture(params);

    // The active SPS stays fixed within a coded video sequence. Grow a layered
    // DPB only when starting or resetting a sequence, when no references survive.
    const uint32_t allocatedSlots = uint32_t(m_pictureIds.size());
    const uint32_t requiredSlots = std::min(m_maxSlots, uint32_t(params.sps.max_num_ref_frames) + 1u);
    const uint32_t slotCount = reset ? std::max(allocatedSlots, requiredSlots) : allocatedSlots;
    if (params.referenceCount >= slotCount)
      return dropPicture(params);

    const auto& video = m_device->properties().h264Decode;
    const bool replaceDpb = slotCount > allocatedSlots;
    auto dpb = replaceDpb
      ? createImage(video.referenceFormat, VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR, slotCount, false)
      : m_dpb;
    std::vector<int32_t> pictureIds = reset ? std::vector<int32_t>(slotCount, EmptyPictureSlot) : m_pictureIds;

    std::array<bool, 17> used = { };
    std::array<VkVideoPictureResourceInfoKHR, 16> referencePictures = { };
    std::array<VkVideoDecodeH264DpbSlotInfoKHR, 16> referenceInfo = { };
    std::array<VkVideoReferenceSlotInfoKHR, 16> references = { };

    VkVideoPictureResourceInfoKHR pictureResource = { VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR };
    pictureResource.codedExtent = m_extent;
    pictureResource.imageViewBinding = dpb->m_view;

    for (uint32_t i = 0; i < params.referenceCount; i++) {
      const auto& reference = params.references[i];
      auto entry = std::find(pictureIds.begin(), pictureIds.end(), reference.pictureId);

      if (entry == pictureIds.end() || reference.pictureId == params.currentPictureId)
        return dropPicture(params);

      uint32_t slot = uint32_t(entry - pictureIds.begin());
      if (used[slot])
        return dropPicture(params);

      used[slot] = true;
      referencePictures[i] = pictureResource;
      referencePictures[i].baseArrayLayer = slot;
      referenceInfo[i] = { VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_DPB_SLOT_INFO_KHR, nullptr, &reference.info };
      references[i] = { VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR, &referenceInfo[i],
        int32_t(slot), &referencePictures[i] };
    }

    uint32_t currentSlot = slotCount;

    for (uint32_t i = 0; i < slotCount; i++) {
      if (!used[i] && (currentSlot == slotCount || pictureIds[i] < 0))
        currentSlot = i;
      if (pictureIds[i] == params.currentPictureId) {
        currentSlot = i;
        break;
      }
    }

    if (currentSlot == slotCount)
      return dropPicture(params);

    std::array<VkVideoReferenceSlotInfoKHR, 18> bound = { };
    std::copy_n(references.begin(), params.referenceCount, bound.begin());
    uint32_t boundCount = params.referenceCount;

    if (pictureIds[currentSlot] != EmptyPictureSlot)
      bound[boundCount++] = { VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR, nullptr, int32_t(currentSlot), nullptr };

    pictureResource.baseArrayLayer = currentSlot;
    bound[boundCount++] = { VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR, nullptr, -1, &pictureResource };

    VkVideoDecodeH264DpbSlotInfoKHR currentInfo = { VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_DPB_SLOT_INFO_KHR };
    currentInfo.pStdReferenceInfo = &params.currentReference;

    VkVideoReferenceSlotInfoKHR current = { VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR, &currentInfo };
    current.slotIndex = int32_t(currentSlot);
    current.pPictureResource = &pictureResource;

    auto& slot = *m_submissions[m_completeValue % m_submissions.size()];
    checkVideo(slot.wait(), "wait for decode slot");
    slot.pending = false;

    slot.setParameters(m_session, params);
    const VkDeviceSize size = slot.uploadBitstream(m_device, m_profiles, bytes);

    // Graphics may still be copying a decoded picture even after its video
    // fence signals. The command list retains both the image and its storage.
    if (!slot.image || slot.image->isInUse(DxvkAccess::Read)) {
      const auto outputUsage = VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
      auto storage = createImage(video.outputFormat, outputUsage, 1, true);

      DxvkImageCreateInfo imageInfo;
      imageInfo.format = video.outputFormat.format;
      imageInfo.sampleCount = VK_SAMPLE_COUNT_1_BIT;
      imageInfo.extent = { m_extent.width, m_extent.height, 1 };
      imageInfo.numLayers = 1;
      imageInfo.mipLevels = 1;
      imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
      imageInfo.stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
      imageInfo.access = VK_ACCESS_TRANSFER_READ_BIT;
      imageInfo.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      imageInfo.initialLayout = imageInfo.layout;
      auto image = m_device->importImage(imageInfo, storage->m_image, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      slot.image = std::move(image);
      slot.output = std::move(storage);
    }

    VkVideoDecodeH264PictureInfoKHR codecPicture = { VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PICTURE_INFO_KHR };
    codecPicture.pStdPictureInfo = &params.picture;
    codecPicture.sliceCount = uint32_t(params.sliceOffsets.size());
    codecPicture.pSliceOffsets = params.sliceOffsets.data();

    VkVideoDecodeInfoKHR decode = { VK_STRUCTURE_TYPE_VIDEO_DECODE_INFO_KHR, &codecPicture };
    decode.srcBuffer = slot.bitstream->buffer;
    decode.srcBufferRange = size;
    decode.dstPictureResource = { VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR, nullptr,
      { 0, 0 }, m_extent, 0, slot.output->m_view };
    decode.pSetupReferenceSlot = &current;
    decode.referenceSlotCount = params.referenceCount;
    decode.pReferenceSlots = references.data();

    checkVideo(m_vkd->vkResetCommandBuffer(slot.cmd, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo beginCommand = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    beginCommand.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    checkVideo(m_vkd->vkBeginCommandBuffer(slot.cmd, &beginCommand), "vkBeginCommandBuffer");

    const auto readWrite = VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR | VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR;
    std::array<VkImageMemoryBarrier2, 2> barriers = {{
      videoBarrier(dpb->m_image, slotCount,
        replaceDpb ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR,
        VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR, m_initialized ? readWrite : 0, readWrite),
      videoBarrier(slot.output->m_image, 1, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR, 0, VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR),
    }};

    VkDependencyInfo dependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dependency.imageMemoryBarrierCount = barriers.size();
    dependency.pImageMemoryBarriers = barriers.data();
    m_vkd->vkCmdPipelineBarrier2(slot.cmd, &dependency);

    VkVideoBeginCodingInfoKHR beginCoding = { VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR };
    beginCoding.videoSession = m_session;
    beginCoding.videoSessionParameters = slot.parameters;
    beginCoding.referenceSlotCount = boundCount;
    beginCoding.pReferenceSlots = bound.data();
    m_vkd->vkCmdBeginVideoCodingKHR(slot.cmd, &beginCoding);

    if (reset) {
      VkVideoCodingControlInfoKHR control = { VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR };
      control.flags = VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR;
      m_vkd->vkCmdControlVideoCodingKHR(slot.cmd, &control);
    }

    m_vkd->vkCmdDecodeVideoKHR(slot.cmd, &decode);

    VkVideoEndCodingInfoKHR endCoding = { VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR };
    m_vkd->vkCmdEndVideoCodingKHR(slot.cmd, &endCoding);

    auto ready = videoBarrier(slot.output->m_image, 1, VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR,
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR, 0);
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &ready;
    m_vkd->vkCmdPipelineBarrier2(slot.cmd, &dependency);
    checkVideo(m_vkd->vkEndCommandBuffer(slot.cmd), "vkEndCommandBuffer");

    const uint64_t value = m_completeValue + 1;
    VkSemaphoreSubmitInfo signal = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
    signal.semaphore = m_complete->handle();
    signal.value = value;
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkCommandBufferSubmitInfo command = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
    command.commandBuffer = slot.cmd;

    VkSubmitInfo2 submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &command;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &signal;

    checkVideo(m_vkd->vkResetFences(m_vkd->device(), 1, &slot.fence), "vkResetFences");
    checkVideo(m_device->submitVideoDecode(submit, slot.fence), "vkQueueSubmit2");
    slot.pending = true;
    slot.dpb = dpb;

    pictureIds[currentSlot] = params.picture.flags.is_reference ? int32_t(params.currentPictureId) : EmptyPictureSlot;
    m_dpb = std::move(dpb);
    m_pictureIds = std::move(pictureIds);
    m_initialized = true;
    m_completeValue = value;
    return { slot.output, slot.image, m_complete, value, 0 };
  }

}
