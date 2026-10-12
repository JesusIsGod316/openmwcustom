#include "interop-contract.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <vulkan/vulkan.h>
#include <osg/GraphicsContext>
#include <osg/GLExtensions>
#include <osgViewer/Viewer>
#endif

namespace
{
    constexpr int SkipCode = 77;
    struct Unsupported : std::runtime_error { using std::runtime_error::runtime_error; };

#ifdef _WIN32
    // Vulkan 1.1 dispatch is loaded from the trusted Windows loader. This tool
    // has no deployment dependency on a second Vulkan loader beside the game.
#define P9_VK_FUNCTIONS(X) \
    X(vkCreateInstance) X(vkDestroyInstance) X(vkEnumerateInstanceExtensionProperties) X(vkEnumeratePhysicalDevices) \
    X(vkGetPhysicalDeviceProperties2) X(vkEnumerateDeviceExtensionProperties) \
    X(vkGetPhysicalDeviceImageFormatProperties2) X(vkGetPhysicalDeviceExternalSemaphoreProperties) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkCreateDevice) X(vkDestroyDevice) \
    X(vkGetDeviceQueue) X(vkGetPhysicalDeviceMemoryProperties) X(vkGetDeviceProcAddr) \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkWaitForFences) \
    X(vkAllocateCommandBuffers) X(vkFreeCommandBuffers) X(vkCmdPipelineBarrier) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements2) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkBindImageMemory) X(vkCreateSemaphore) X(vkDestroySemaphore) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) \
    X(vkMapMemory) X(vkUnmapMemory) X(vkCreateFence) X(vkDestroyFence) X(vkResetFences) \
    X(vkQueueSubmit) X(vkCmdCopyImageToBuffer) X(vkCmdCopyBufferToImage)
#define P9_VK_DECLARE(name) PFN_##name name = nullptr;
    P9_VK_FUNCTIONS(P9_VK_DECLARE)
#undef P9_VK_DECLARE

    void loadVulkan()
    {
        static HMODULE loader = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!loader) throw Unsupported("installed Windows Vulkan driver loader unavailable");
#define P9_VK_LOAD(name) \
        name = reinterpret_cast<PFN_##name>(GetProcAddress(loader, #name)); \
        if (!name) throw Unsupported("installed Vulkan loader missing 1.1 entry point " #name);
        P9_VK_FUNCTIONS(P9_VK_LOAD)
#undef P9_VK_LOAD
    }
#undef P9_VK_FUNCTIONS

#endif

    std::string jsonString(const std::string& value)
    {
        std::ostringstream out;
        out << '"';
        for (unsigned char c : value)
        {
            if (c == '"' || c == '\\') out << '\\' << c;
            else if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c);
            else out << c;
        }
        return out.str() + '"';
    }

    class Evidence
    {
        std::ofstream mFile;
    public:
        explicit Evidence(const std::string& path)
        {
            if (!path.empty())
            {
                mFile.open(path, std::ios::trunc);
                if (!mFile) throw std::runtime_error("cannot create fixture evidence file");
            }
        }
        void event(const char* status, const char* check, const std::string& detail)
        {
            const auto row = "{\"schema\":1,\"status\":" + jsonString(status)
                + ",\"check\":" + jsonString(check) + ",\"detail\":" + jsonString(detail) + "}";
            std::cout << row << std::endl;
            if (mFile) { mFile << row << std::endl; if (!mFile) throw std::runtime_error("fixture evidence write failed"); }
        }
    };

#ifdef _WIN32
    constexpr GLenum TextureTiling = 0x9580, DedicatedMemoryObject = 0x9581;
    constexpr GLenum NumTilingTypes = 0x9582, TilingTypes = 0x9583, OptimalTiling = 0x9584;
    constexpr GLenum OpaqueWin32 = 0x9587, GeneralLayout = 0x958D;
    constexpr GLenum NumDeviceUuids = 0x9596, DeviceUuid = 0x9597, DriverUuid = 0x9598;
    constexpr GLenum DeviceLuid = 0x9599, DeviceNodeMask = 0x959A;
    constexpr VkExternalMemoryHandleTypeFlagBits MemoryHandle = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    constexpr VkExternalSemaphoreHandleTypeFlagBits SemaphoreHandle = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    constexpr VkImageUsageFlags Usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
        | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    constexpr VkImageSubresourceRange ImageRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    void require(bool condition, const std::string& message)
    {
        if (!condition) throw std::runtime_error(message);
    }
    void check(VkResult result, const char* operation)
    {
        if (result != VK_SUCCESS)
            throw std::runtime_error(std::string(operation) + " failed: VkResult=" + std::to_string(result));
    }
    void glCheck(const char* operation)
    {
        const auto error = glGetError();
        if (error != GL_NO_ERROR)
            throw std::runtime_error(std::string(operation) + " failed: GL error=" + std::to_string(error));
    }
    template<class Function> Function function(const char* name)
    {
        auto result = reinterpret_cast<Function>(osg::getGLExtensionFuncPtr(name));
        if (!result) throw Unsupported(std::string("missing GL function ") + name);
        return result;
    }

    struct Gl
    {
        void (APIENTRY* getId)(GLenum, GLubyte*) = function<decltype(getId)>("glGetUnsignedBytevEXT");
        void (APIENTRY* getIndexedId)(GLenum, GLuint, GLubyte*) = function<decltype(getIndexedId)>("glGetUnsignedBytei_vEXT");
        void (APIENTRY* getFormat)(GLenum, GLenum, GLenum, GLsizei, GLint*) = function<decltype(getFormat)>("glGetInternalformativ");
        void (APIENTRY* createMemory)(GLsizei, GLuint*) = function<decltype(createMemory)>("glCreateMemoryObjectsEXT");
        void (APIENTRY* deleteMemory)(GLsizei, const GLuint*) = function<decltype(deleteMemory)>("glDeleteMemoryObjectsEXT");
        void (APIENTRY* memoryParameter)(GLuint, GLenum, const GLint*) = function<decltype(memoryParameter)>("glMemoryObjectParameterivEXT");
        void (APIENTRY* importMemory)(GLuint, GLuint64, GLenum, void*) = function<decltype(importMemory)>("glImportMemoryWin32HandleEXT");
        void (APIENTRY* storage)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLuint, GLuint64) = function<decltype(storage)>("glTexStorageMem2DEXT");
        void (APIENTRY* createSemaphores)(GLsizei, GLuint*) = function<decltype(createSemaphores)>("glGenSemaphoresEXT");
        void (APIENTRY* deleteSemaphores)(GLsizei, const GLuint*) = function<decltype(deleteSemaphores)>("glDeleteSemaphoresEXT");
        void (APIENTRY* importSemaphore)(GLuint, GLenum, void*) = function<decltype(importSemaphore)>("glImportSemaphoreWin32HandleEXT");
        void (APIENTRY* wait)(GLuint, GLuint, const GLuint*, GLuint, const GLuint*, const GLenum*) = function<decltype(wait)>("glWaitSemaphoreEXT");
        void (APIENTRY* signal)(GLuint, GLuint, const GLuint*, GLuint, const GLuint*, const GLenum*) = function<decltype(signal)>("glSignalSemaphoreEXT");
        osg::GLsync (APIENTRY* fence)(GLenum, GLbitfield) = function<decltype(fence)>("glFenceSync");
        GLenum (APIENTRY* clientWait)(osg::GLsync, GLbitfield, GLuint64) = function<decltype(clientWait)>("glClientWaitSync");
        void (APIENTRY* deleteSync)(osg::GLsync) = function<decltype(deleteSync)>("glDeleteSync");
    };

    std::string hexId(const unsigned char* value, std::size_t size)
    {
        std::ostringstream out;
        for (std::size_t i = 0; i < size; ++i) out << std::hex << std::setw(2) << std::setfill('0') << unsigned(value[i]);
        return out.str();
    }

    // Owns Vulkan resources only. Normal destruction is called after every
    // slot has both API retirement proofs. Error paths terminate the standalone
    // process without an unbounded vkDeviceWaitIdle or unsafe in-flight destroy.
    class Vulkan
    {
    public:
        VkInstance instance = VK_NULL_HANDLE;
        VkPhysicalDevice physical = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        VkQueue queue = VK_NULL_HANDLE;
        std::uint32_t queueFamily = 0;
        VkCommandPool commandPool = VK_NULL_HANDLE;
        VkPhysicalDeviceMemoryProperties memory{};
        PFN_vkGetMemoryWin32HandleKHR getMemoryHandle = nullptr;
        PFN_vkGetSemaphoreWin32HandleKHR getSemaphoreHandle = nullptr;
        std::uint64_t timeoutNs;

        Vulkan(const Gl& gl, Evidence& evidence, unsigned timeoutMs) : timeoutNs(std::uint64_t(timeoutMs) * 1000000)
        {
            VkApplicationInfo application{};
            application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
            application.pApplicationName = "OptimizedMW Phase 9 standalone interop fixture";
            application.apiVersion = VK_API_VERSION_1_1;
            VkInstanceCreateInfo create{};
            create.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
            create.pApplicationInfo = &application;
            const char* instanceExtensions[] = { VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
                VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME };
            std::uint32_t instanceExtensionCount = 0;
            check(vkEnumerateInstanceExtensionProperties(nullptr, &instanceExtensionCount, nullptr), "instance extension count");
            std::vector<VkExtensionProperties> instanceProperties(instanceExtensionCount);
            check(vkEnumerateInstanceExtensionProperties(nullptr, &instanceExtensionCount, instanceProperties.data()), "instance extension list");
            for (const char* name : instanceExtensions)
                if (std::none_of(instanceProperties.begin(), instanceProperties.end(), [name](const auto& e) { return std::string(e.extensionName) == name; }))
                    throw Unsupported(std::string("missing qualified Vulkan instance extension ") + name);
            // Although promoted in Vulkan 1.1, explicitly select the advertised
            // KHR route for cross-driver GL imports; qualify that exact setup
            // together with retained NT handles through complete retirement.
            create.enabledExtensionCount = 2; create.ppEnabledExtensionNames = instanceExtensions;
            auto result = vkCreateInstance(&create, nullptr, &instance);
            if (result == VK_ERROR_INCOMPATIBLE_DRIVER) throw Unsupported("Vulkan 1.1 loader/device unavailable");
            check(result, "vkCreateInstance");

            GLint count = 0, nodeMask = 0;
            std::array<unsigned char, VK_UUID_SIZE> uuid{}, driver{};
            std::array<unsigned char, VK_LUID_SIZE> luid{};
            glGetIntegerv(NumDeviceUuids, &count);
            if (count != 1) throw Unsupported("fixture requires exactly one GL physical device, no linked/multi-device context");
            gl.getIndexedId(DeviceUuid, 0, uuid.data());
            gl.getId(DriverUuid, driver.data()); gl.getId(DeviceLuid, luid.data());
            glGetIntegerv(DeviceNodeMask, &nodeMask); glCheck("GL device identity");
            if (!Phase9Interop::validNodeMask(static_cast<std::uint32_t>(nodeMask)))
                throw Unsupported("GL node mask is not an individual device");
            std::uint32_t devices = 0;
            check(vkEnumeratePhysicalDevices(instance, &devices, nullptr), "vkEnumeratePhysicalDevices");
            std::vector<VkPhysicalDevice> candidates(devices);
            check(vkEnumeratePhysicalDevices(instance, &devices, candidates.data()), "vkEnumeratePhysicalDevices");
            VkPhysicalDeviceProperties properties{};
            for (auto candidate : candidates)
            {
                VkPhysicalDeviceIDProperties id{}; id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
                VkPhysicalDeviceProperties2 p{}; p.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2; p.pNext = &id;
                vkGetPhysicalDeviceProperties2(candidate, &p);
                if (p.properties.apiVersion < VK_API_VERSION_1_1) continue;
                if (Phase9Interop::equalNonzeroId(uuid, id.deviceUUID)
                    && Phase9Interop::equalNonzeroId(driver, id.driverUUID)
                    && id.deviceLUIDValid && Phase9Interop::equalNonzeroId(luid, id.deviceLUID)
                    && id.deviceNodeMask == static_cast<std::uint32_t>(nodeMask))
                { physical = candidate; properties = p.properties; break; }
            }
            if (!physical) throw Unsupported("no Vulkan device matches GL device UUID, driver UUID, valid LUID and node mask");
            evidence.event("pass", "same_device", std::string(properties.deviceName) + "; uuid=" + hexId(uuid.data(), uuid.size())
                + "; driver_uuid=" + hexId(driver.data(), driver.size()) + "; luid=" + hexId(luid.data(), luid.size())
                + "; node_mask=" + std::to_string(nodeMask));

            std::uint32_t extensionCount = 0;
            check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &extensionCount, nullptr), "device extension count");
            std::vector<VkExtensionProperties> extensions(extensionCount);
            check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &extensionCount, extensions.data()), "device extensions");
            const char* required[] = { VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
                VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME };
            for (const char* name : required)
                if (std::none_of(extensions.begin(), extensions.end(), [name](const auto& e) { return std::string(e.extensionName) == name; }))
                    throw Unsupported(std::string("missing Vulkan extension ") + name);

            VkPhysicalDeviceExternalImageFormatInfo external{};
            external.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO; external.handleType = MemoryHandle;
            VkPhysicalDeviceImageFormatInfo2 format{};
            format.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2; format.pNext = &external;
            format.format = VK_FORMAT_R8G8B8A8_UNORM; format.type = VK_IMAGE_TYPE_2D;
            format.tiling = VK_IMAGE_TILING_OPTIMAL; format.usage = Usage;
            VkExternalImageFormatProperties externalProperties{};
            externalProperties.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
            VkImageFormatProperties2 formatProperties{};
            formatProperties.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2; formatProperties.pNext = &externalProperties;
            result = vkGetPhysicalDeviceImageFormatProperties2(physical, &format, &formatProperties);
            if (result == VK_ERROR_FORMAT_NOT_SUPPORTED) throw Unsupported("RGBA8 optimal-tiling external image usage unsupported");
            check(result, "external image format properties");
            const auto& memoryProperties = externalProperties.externalMemoryProperties;
            if (!(memoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT)
                || !(memoryProperties.compatibleHandleTypes & MemoryHandle))
                throw Unsupported("opaque Win32 memory is not exportable for the exact image contract");
            VkPhysicalDeviceExternalSemaphoreInfo semaphoreInfo{};
            semaphoreInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO;
            semaphoreInfo.handleType = SemaphoreHandle;
            VkExternalSemaphoreProperties semaphoreProperties{};
            semaphoreProperties.sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES;
            vkGetPhysicalDeviceExternalSemaphoreProperties(physical, &semaphoreInfo, &semaphoreProperties);
            if (!(semaphoreProperties.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT)
                || !(semaphoreProperties.compatibleHandleTypes & SemaphoreHandle))
                throw Unsupported("opaque Win32 binary semaphore is not exportable");
            evidence.event("pass", "external_contract", "RGBA8_UNORM; optimal tiling; color_attachment|transfer_src|transfer_dst|sampled; opaque_win32; dedicated allocation; image_features="
                + std::to_string(memoryProperties.externalMemoryFeatures) + "; semaphore_features="
                + std::to_string(semaphoreProperties.externalSemaphoreFeatures));

            std::uint32_t families = 0; vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, nullptr);
            std::vector<VkQueueFamilyProperties> queues(families); vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, queues.data());
            auto family = std::find_if(queues.begin(), queues.end(), [](const auto& q) { return q.queueCount && (q.queueFlags & VK_QUEUE_GRAPHICS_BIT); });
            if (family == queues.end()) throw Unsupported("matching Vulkan device has no graphics/transfer queue");
            queueFamily = static_cast<std::uint32_t>(family - queues.begin());
            float priority = 1;
            VkDeviceQueueCreateInfo queueInfo{}; queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueInfo.queueFamilyIndex = queueFamily; queueInfo.queueCount = 1; queueInfo.pQueuePriorities = &priority;
            VkDeviceCreateInfo deviceInfo{}; deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
            deviceInfo.queueCreateInfoCount = 1; deviceInfo.pQueueCreateInfos = &queueInfo;
            deviceInfo.enabledExtensionCount = 4; deviceInfo.ppEnabledExtensionNames = required;
            check(vkCreateDevice(physical, &deviceInfo, nullptr, &device), "vkCreateDevice");
            vkGetDeviceQueue(device, queueFamily, 0, &queue);
            vkGetPhysicalDeviceMemoryProperties(physical, &memory);
            getMemoryHandle = reinterpret_cast<PFN_vkGetMemoryWin32HandleKHR>(vkGetDeviceProcAddr(device, "vkGetMemoryWin32HandleKHR"));
            getSemaphoreHandle = reinterpret_cast<PFN_vkGetSemaphoreWin32HandleKHR>(vkGetDeviceProcAddr(device, "vkGetSemaphoreWin32HandleKHR"));
            require(getMemoryHandle && getSemaphoreHandle, "Vulkan export function missing after supported extension enable");
            VkCommandPoolCreateInfo pool{}; pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            pool.queueFamilyIndex = queueFamily; pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            check(vkCreateCommandPool(device, &pool, nullptr, &commandPool), "vkCreateCommandPool");
        }

        std::uint32_t memoryType(std::uint32_t bits, VkMemoryPropertyFlags flags) const
        {
            for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i)
                if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags) return i;
            throw Unsupported("required device-local or host-visible coherent memory type unavailable");
        }
        void waitFence(VkFence fence) const { check(vkWaitForFences(device, 1, &fence, VK_TRUE, timeoutNs), "bounded Vulkan fence wait"); }
        VkCommandBuffer allocateCommand() const
        {
            VkCommandBufferAllocateInfo info{}; info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            info.commandPool = commandPool; info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; info.commandBufferCount = 1;
            VkCommandBuffer result = VK_NULL_HANDLE; check(vkAllocateCommandBuffers(device, &info, &result), "vkAllocateCommandBuffers"); return result;
        }
        ~Vulkan()
        {
            if (std::uncaught_exceptions() != 0) return;
            if (commandPool) vkDestroyCommandPool(device, commandPool, nullptr);
            if (device) vkDestroyDevice(device, nullptr);
            if (instance) vkDestroyInstance(instance, nullptr);
        }
    };

    void barrier(VkCommandBuffer command, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
        std::uint32_t from, std::uint32_t to, VkAccessFlags sourceAccess, VkAccessFlags destinationAccess,
        VkPipelineStageFlags sourceStage, VkPipelineStageFlags destinationStage)
    {
        VkImageMemoryBarrier b{}; b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask = sourceAccess; b.dstAccessMask = destinationAccess; b.oldLayout = oldLayout; b.newLayout = newLayout;
        b.srcQueueFamilyIndex = from; b.dstQueueFamilyIndex = to; b.image = image; b.subresourceRange = ImageRange;
        vkCmdPipelineBarrier(command, sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr, 1, &b);
    }
    void begin(VkCommandBuffer command)
    {
        VkCommandBufferBeginInfo info{}; info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(command, &info), "vkBeginCommandBuffer");
    }

    struct SharedSlot
    {
        Vulkan& vk;
        const Gl& gl;
        osg::GLExtensions& osgGl;
        Phase9Interop::Slot ownership;
        Phase9Interop::Token token{};
        std::uint32_t width, height;
        GLuint texture = 0, glMemory = 0, framebuffer = 0, glToVk = 0, vkToGl = 0;
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory imageMemory = VK_NULL_HANDLE, readbackMemory = VK_NULL_HANDLE;
        VkBuffer readback = VK_NULL_HANDLE;
        VkSemaphore ready = VK_NULL_HANDLE, complete = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkCommandBuffer command = VK_NULL_HANDLE;
        void* mapped = nullptr;
        HANDLE memoryHandle = nullptr, producerHandle = nullptr, consumerHandle = nullptr;

        SharedSlot(Vulkan& v, const Gl& g, osg::GLExtensions& ext, Evidence& evidence, unsigned w, unsigned h, std::uint64_t generation)
            : vk(v), gl(g), osgGl(ext), ownership(generation), width(w), height(h)
        {
            VkExternalMemoryImageCreateInfo external{}; external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
            external.handleTypes = MemoryHandle;
            VkImageCreateInfo create{}; create.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; create.pNext = &external;
            create.imageType = VK_IMAGE_TYPE_2D; create.format = VK_FORMAT_R8G8B8A8_UNORM;
            create.extent = { width, height, 1 }; create.mipLevels = 1; create.arrayLayers = 1;
            create.samples = VK_SAMPLE_COUNT_1_BIT; create.tiling = VK_IMAGE_TILING_OPTIMAL;
            create.usage = Usage; create.sharingMode = VK_SHARING_MODE_EXCLUSIVE; create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            check(vkCreateImage(vk.device, &create, nullptr, &image), "vkCreateImage");
            VkMemoryDedicatedRequirements dedicated{}; dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS;
            VkMemoryRequirements2 requirements{}; requirements.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2; requirements.pNext = &dedicated;
            VkImageMemoryRequirementsInfo2 imageInfo{}; imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2; imageInfo.image = image;
            vkGetImageMemoryRequirements2(vk.device, &imageInfo, &requirements);
            require(requirements.memoryRequirements.size <= 16u * 1024u * 1024u,
                "tiny interop image exceeds bounded 16 MiB dedicated allocation");
            VkMemoryDedicatedAllocateInfo dedicatedAllocation{}; dedicatedAllocation.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
            dedicatedAllocation.image = image;
            VkExportMemoryAllocateInfo exportInfo{}; exportInfo.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
            exportInfo.pNext = &dedicatedAllocation; exportInfo.handleTypes = MemoryHandle;
            VkMemoryAllocateInfo allocation{}; allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocation.pNext = &exportInfo; allocation.allocationSize = requirements.memoryRequirements.size;
            allocation.memoryTypeIndex = vk.memoryType(requirements.memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            // Always dedicate, including implementations where it is optional.
            check(vkAllocateMemory(vk.device, &allocation, nullptr, &imageMemory), "dedicated image allocation");
            check(vkBindImageMemory(vk.device, image, imageMemory, 0), "vkBindImageMemory");
            VkMemoryGetWin32HandleInfoKHR handleInfo{}; handleInfo.sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR;
            handleInfo.memory = imageMemory; handleInfo.handleType = MemoryHandle;
            check(vk.getMemoryHandle(vk.device, &handleInfo, &memoryHandle), "export image memory Win32 handle");
            require(memoryHandle != nullptr && memoryHandle != INVALID_HANDLE_VALUE, "Vulkan exported an invalid memory handle");
            DWORD handleFlags = 0; require(GetHandleInformation(memoryHandle, &handleFlags) != FALSE, "Vulkan exported an invalid NT handle");
            evidence.event("pass", "shared_allocation", "generation=" + std::to_string(generation)
                + "; extent=" + std::to_string(width) + "x" + std::to_string(height)
                + "; bytes=" + std::to_string(allocation.allocationSize)
                + "; memory_type=" + std::to_string(allocation.memoryTypeIndex)
                + "; requires_dedicated=" + std::to_string(dedicated.requiresDedicatedAllocation)
                + "; prefers_dedicated=" + std::to_string(dedicated.prefersDedicatedAllocation)
                + "; dedicated=1; offset=0; valid_exported_NT_handle");
            gl.createMemory(1, &glMemory); glCheck("GL memory object creation"); const GLint isDedicated = GL_TRUE;
            gl.memoryParameter(glMemory, DedicatedMemoryObject, &isDedicated);
            glCheck("GL dedicated memory parameter");
            gl.importMemory(glMemory, allocation.allocationSize, OpaqueWin32, memoryHandle);
            // Opaque Win32 imports retain a reference; unlike opaque FD imports,
            // application ownership of the exported HANDLE is not transferred.
            // Retain that bounded reference until both API consumers retire.
            glCheck("GL dedicated memory import");
            glGenTextures(1, &texture); glBindTexture(GL_TEXTURE_2D, texture);
            glTexParameteri(GL_TEXTURE_2D, TextureTiling, OptimalTiling);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            gl.storage(GL_TEXTURE_2D, 1, GL_RGBA8, static_cast<GLsizei>(width), static_cast<GLsizei>(height), glMemory, 0);
            osgGl.glGenFramebuffers(1, &framebuffer); osgGl.glBindFramebuffer(GL_FRAMEBUFFER_EXT, framebuffer);
            osgGl.glFramebufferTexture2D(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, texture, 0);
            require(osgGl.glCheckFramebufferStatus(GL_FRAMEBUFFER_EXT) == GL_FRAMEBUFFER_COMPLETE_EXT, "shared image GL FBO incomplete");
            glCheck("GL shared optimal texture and framebuffer creation");

            auto semaphore = [&](VkSemaphore& native, GLuint& imported, HANDLE& exportedHandle)
            {
                VkExportSemaphoreCreateInfo exportSemaphore{}; exportSemaphore.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
                exportSemaphore.handleTypes = SemaphoreHandle;
                VkSemaphoreCreateInfo info{}; info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO; info.pNext = &exportSemaphore;
                check(vkCreateSemaphore(vk.device, &info, nullptr, &native), "exportable binary semaphore");
                VkSemaphoreGetWin32HandleInfoKHR get{}; get.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR;
                get.semaphore = native; get.handleType = SemaphoreHandle;
                check(vk.getSemaphoreHandle(vk.device, &get, &exportedHandle), "export binary semaphore handle");
                gl.createSemaphores(1, &imported); gl.importSemaphore(imported, OpaqueWin32, exportedHandle);
                glCheck("GL binary semaphore import");
            };
            semaphore(ready, glToVk, producerHandle); semaphore(complete, vkToGl, consumerHandle);
            VkBufferCreateInfo bufferInfo{}; bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            // Separate ranges in one coherent allocation hold GL producer
            // readback and the Vulkan producer's spatially varying upload.
            bufferInfo.size = std::uint64_t(width) * height * 8;
            bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            check(vkCreateBuffer(vk.device, &bufferInfo, nullptr, &readback), "readback buffer creation");
            VkMemoryRequirements bufferMemory{}; vkGetBufferMemoryRequirements(vk.device, readback, &bufferMemory);
            VkMemoryAllocateInfo host{}; host.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            host.allocationSize = bufferMemory.size;
            host.memoryTypeIndex = vk.memoryType(bufferMemory.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            check(vkAllocateMemory(vk.device, &host, nullptr, &readbackMemory), "host readback allocation");
            check(vkBindBufferMemory(vk.device, readback, readbackMemory, 0), "readback memory bind");
            check(vkMapMemory(vk.device, readbackMemory, 0, VK_WHOLE_SIZE, 0, &mapped), "readback map");
            VkFenceCreateInfo fenceInfo{}; fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            check(vkCreateFence(vk.device, &fenceInfo, nullptr, &fence), "slot fence creation");
            command = vk.allocateCommand(); begin(command);
            barrier(command, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, vk.queueFamily, VK_QUEUE_FAMILY_EXTERNAL,
                0, 0, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
            check(vkEndCommandBuffer(command), "initial release command");
            VkSubmitInfo submit{}; submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
            check(vkQueueSubmit(vk.queue, 1, &submit, fence), "initial ownership release");
            vk.waitFence(fence);
            // Initial layout is established by the completed Vulkan release.
            // A signal/wait pair supplies GL its explicit layout metadata.
            check(vkResetFences(vk.device, 1, &fence), "reset initial release fence");
            VkSubmitInfo initialSignal{}; initialSignal.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            initialSignal.signalSemaphoreCount = 1; initialSignal.pSignalSemaphores = &complete;
            check(vkQueueSubmit(vk.queue, 1, &initialSignal, fence), "initial external semaphore signal");
            vk.waitFence(fence);
            const GLenum layout = GeneralLayout; gl.wait(vkToGl, 0, nullptr, 1, &texture, &layout);
            retireGl(); glCheck("initial GL ownership acquisition");
        }

        void retireGl() const
        {
            auto sync = gl.fence(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            require(sync != nullptr, "GL retirement fence creation failed");
            const auto result = gl.clientWait(sync, GL_SYNC_FLUSH_COMMANDS_BIT, vk.timeoutNs);
            gl.deleteSync(sync);
            require(result == GL_ALREADY_SIGNALED || result == GL_CONDITION_SATISFIED,
                "bounded GL retirement fence failed or timed out");
        }
        void submit(std::uint64_t sequence)
        {
            token = ownership.submit(sequence);
            osgGl.glBindFramebuffer(GL_FRAMEBUFFER_EXT, framebuffer);
            glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
            glEnable(GL_SCISSOR_TEST); glDisable(GL_DITHER); glDisable(GL_BLEND); glDisable(GL_FRAMEBUFFER_SRGB);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            for (unsigned quadrant = 0; quadrant < 4; ++quadrant)
            {
                const unsigned x = (quadrant & 1) ? width / 2 : 0;
                const unsigned y = (quadrant & 2) ? height / 2 : 0;
                const auto pixel = Phase9Interop::glPattern(sequence ^ quadrant);
                glScissor(static_cast<GLint>(x), static_cast<GLint>(y),
                    static_cast<GLsizei>((quadrant & 1) ? width - x : width / 2),
                    static_cast<GLsizei>((quadrant & 2) ? height - y : height / 2));
                glClearColor(pixel[0] / 255.f, pixel[1] / 255.f, pixel[2] / 255.f, 1);
                glClear(GL_COLOR_BUFFER_BIT);
            }
            glDisable(GL_SCISSOR_TEST);
            const GLenum layout = GeneralLayout;
            gl.signal(glToVk, 0, nullptr, 1, &texture, &layout); glCheck("GL producer and external semaphore signal");
            check(vkResetFences(vk.device, 1, &fence), "slot fence reset");
            check(vkResetCommandBuffer(command, 0), "slot command reset"); begin(command);
            const VkDeviceSize imageBytes = std::uint64_t(width) * height * 4;
            auto* upload = static_cast<unsigned char*>(mapped) + imageBytes;
            for (unsigned y = 0; y < height; ++y)
                for (unsigned x = 0; x < width; ++x)
                {
                    const auto pixel = Phase9Interop::vkPattern(Phase9Interop::spatialSequence(sequence, x, y, width, height));
                    std::copy(pixel.begin(), pixel.end(), upload + (std::size_t(y) * width + x) * 4);
                }
            VkBufferMemoryBarrier uploadBarrier{}; uploadBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            uploadBarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT; uploadBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            uploadBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; uploadBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            uploadBarrier.buffer = readback; uploadBarrier.offset = imageBytes; uploadBarrier.size = imageBytes;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 0, nullptr, 1, &uploadBarrier, 0, nullptr);
            barrier(command, image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_QUEUE_FAMILY_EXTERNAL, vk.queueFamily, 0, VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy copy{}; copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.imageExtent = { width, height, 1 };
            vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &copy);
            VkBufferMemoryBarrier host{}; host.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            host.buffer = readback; host.size = imageBytes;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &host, 0, nullptr);
            barrier(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            copy.bufferOffset = imageBytes;
            vkCmdCopyBufferToImage(command, readback, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            barrier(command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                vk.queueFamily, VK_QUEUE_FAMILY_EXTERNAL, VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
            check(vkEndCommandBuffer(command), "interop copy/clear command end");
            const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            VkSubmitInfo submit{}; submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.waitSemaphoreCount = 1; submit.pWaitSemaphores = &ready; submit.pWaitDstStageMask = &waitStage;
            submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
            submit.signalSemaphoreCount = 1; submit.pSignalSemaphores = &complete;
            check(vkQueueSubmit(vk.queue, 1, &submit, fence), "Vulkan external wait/copy/clear/release/signal");
        }
        void consume(Evidence& evidence)
        {
            vk.waitFence(fence);
            const auto* pixels = static_cast<const unsigned char*>(mapped);
            for (unsigned y = 0; y < height; ++y)
                for (unsigned x = 0; x < width; ++x)
                {
                    const auto expected = Phase9Interop::glPattern(Phase9Interop::spatialSequence(token.sequence, x, y, width, height));
                    require(std::equal(expected.begin(), expected.end(), pixels + (std::size_t(y) * width + x) * 4),
                        "GL-to-Vulkan spatial readback pixel mismatch");
                }
            const GLenum layout = GeneralLayout; gl.wait(vkToGl, 0, nullptr, 1, &texture, &layout);
            // Wait for the GL consumer to acquire memory before the synchronous
            // client readback. The parent process enforces a whole-fixture bound.
            retireGl(); glCheck("Vulkan-to-GL external wait and retirement");
            glBindTexture(GL_TEXTURE_2D, texture); glPixelStorei(GL_PACK_ALIGNMENT, 1);
            std::vector<unsigned char> result(std::size_t(width) * height * 4);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, result.data()); glCheck("GL returned image readback");
            for (unsigned y = 0; y < height; ++y)
                for (unsigned x = 0; x < width; ++x)
                {
                    const auto returned = Phase9Interop::vkPattern(Phase9Interop::spatialSequence(token.sequence, x, y, width, height));
                    require(std::equal(returned.begin(), returned.end(), result.data() + (std::size_t(y) * width + x) * 4),
                        "Vulkan-to-GL spatial readback pixel mismatch");
                }
            retireGl(); ownership.consume(token, true, true);
            evidence.event("pass", "round_trip_pixels", "generation=" + std::to_string(token.generation)
                + "; sequence=" + std::to_string(token.sequence) + "; extent=" + std::to_string(width) + "x" + std::to_string(height)
                + "; all_pixels_verified_both_directions; both_API_consumers_retired");
        }
        ~SharedSlot()
        {
            if (std::uncaught_exceptions() != 0) return;
            ownership.retire();
            if (framebuffer) osgGl.glDeleteFramebuffers(1, &framebuffer);
            if (texture) glDeleteTextures(1, &texture);
            if (glMemory) gl.deleteMemory(1, &glMemory);
            if (glToVk) gl.deleteSemaphores(1, &glToVk);
            if (vkToGl) gl.deleteSemaphores(1, &vkToGl);
            if (memoryHandle) CloseHandle(memoryHandle);
            if (producerHandle) CloseHandle(producerHandle);
            if (consumerHandle) CloseHandle(consumerHandle);
            if (command) vkFreeCommandBuffers(vk.device, vk.commandPool, 1, &command);
            if (fence) vkDestroyFence(vk.device, fence, nullptr);
            if (ready) vkDestroySemaphore(vk.device, ready, nullptr);
            if (complete) vkDestroySemaphore(vk.device, complete, nullptr);
            if (mapped) vkUnmapMemory(vk.device, readbackMemory);
            if (readback) vkDestroyBuffer(vk.device, readback, nullptr);
            if (readbackMemory) vkFreeMemory(vk.device, readbackMemory, nullptr);
            if (image) vkDestroyImage(vk.device, image, nullptr);
            if (imageMemory) vkFreeMemory(vk.device, imageMemory, nullptr);
        }
    };

    void run(Evidence& evidence, unsigned frames, unsigned timeoutMs)
    {
        loadVulkan();
        osgViewer::Viewer anchor;
        osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
        traits->readDISPLAY(); traits->setUndefinedScreenDetailsToDefaultScreen();
        traits->width = 64; traits->height = 64; traits->doubleBuffer = false; traits->windowDecoration = false;
        osg::ref_ptr<osg::GraphicsContext> context = osg::GraphicsContext::createGraphicsContext(traits);
        if (!context || !context->realize() || !context->makeCurrent()) throw Unsupported("real GL context unavailable");
        const auto id = context->getState()->getContextID();
        for (const char* extension : { "GL_EXT_memory_object", "GL_EXT_memory_object_win32", "GL_EXT_semaphore", "GL_EXT_semaphore_win32" })
            if (!osg::isGLExtensionSupported(id, extension)) throw Unsupported(std::string("missing extension ") + extension);
        auto* osgGl = context->getState()->get<osg::GLExtensions>();
        if (!osgGl || !osgGl->glGenFramebuffers || !osgGl->glBindFramebuffer
            || !osgGl->glFramebufferTexture2D || !osgGl->glCheckFramebufferStatus || !osgGl->glDeleteFramebuffers)
            throw Unsupported("GL framebuffer entry points unavailable");
        Gl gl;
        GLint tilingCount = 0; gl.getFormat(GL_TEXTURE_2D, GL_RGBA8, NumTilingTypes, 1, &tilingCount);
        if (tilingCount < 1 || tilingCount > 16) throw Unsupported("RGBA8 external GL tiling list missing or unbounded");
        std::vector<GLint> tilings(static_cast<std::size_t>(tilingCount));
        gl.getFormat(GL_TEXTURE_2D, GL_RGBA8, TilingTypes, tilingCount, tilings.data()); glCheck("GL optimal RGBA8 tiling query");
        if (std::find(tilings.begin(), tilings.end(), static_cast<GLint>(OptimalTiling)) == tilings.end())
            throw Unsupported("RGBA8 optimal external tiling unavailable in GL");
        evidence.event("pass", "gl_capability", "all external-object, Win32, FBO, format-query and retirement entry points available; vendor="
            + std::string(reinterpret_cast<const char*>(glGetString(GL_VENDOR))) + "; renderer="
            + std::string(reinterpret_cast<const char*>(glGetString(GL_RENDERER))) + "; version="
            + std::string(reinterpret_cast<const char*>(glGetString(GL_VERSION))));
        Vulkan vk(gl, evidence, timeoutMs);
        std::uint64_t sequence = 0;
        // Exact dimensions intentionally change both ways, with no live image
        // resized in place. Two queue submissions precede the first retirement.
        for (std::uint64_t generation = 1; generation <= 3; ++generation)
        {
            const unsigned width = generation == 2 ? 37 : 16, height = generation == 2 ? 19 : 16;
            {
                std::array<std::unique_ptr<SharedSlot>, 2> slots;
                for (auto& slot : slots) slot = std::make_unique<SharedSlot>(vk, gl, *osgGl, evidence, width, height, generation);
                for (unsigned frame = 0; frame < frames; frame += 2)
                {
                    const unsigned count = std::min(2u, frames - frame);
                    for (unsigned i = 0; i < count; ++i) slots[i]->submit(++sequence);
                    for (unsigned i = 0; i < count; ++i) slots[i]->consume(evidence);
                }
            }
            glCheck("retired generation deletion");
            evidence.event("pass", "resize_retirement", "generation=" + std::to_string(generation)
                + "; two_slot_resources_released_after_GL_and_Vulkan_completion");
        }
        evidence.event("pass", "fixture", "same-device GL->Vulkan->GL verified; submissions=" + std::to_string(sequence)
            + "; no engine/DLSS/NGX capability or performance promotion");
    }
#endif
}

int main(int argc, char** argv)
{
    std::unique_ptr<Evidence> evidence;
    try
    {
        std::string path; unsigned frames = 4, timeoutMs = 1500;
        for (int i = 1; i < argc; ++i)
        {
            const std::string option = argv[i];
            if ((option == "--output" || option == "--frames" || option == "--timeout-ms") && i + 1 < argc)
            {
                const std::string value = argv[++i];
                if (option == "--output") path = value;
                else
                {
                    std::size_t used = 0;
                    const auto number = std::stoul(value, &used);
                    if (used != value.size() || number == 0 || number > (option == "--frames" ? 64u : 10000u))
                        throw std::runtime_error("fixture count or timeout outside bounded range");
                    if (option == "--frames") frames = static_cast<unsigned>(number);
                    else timeoutMs = static_cast<unsigned>(number);
                }
            }
            else throw std::runtime_error("usage: p9-gl-vulkan-interop [--output path] [--frames 1..64] [--timeout-ms 1..10000]");
        }
        evidence = std::make_unique<Evidence>(path);
#ifdef _WIN32
        run(*evidence, frames, timeoutMs);
        return 0;
#else
        (void)frames; (void)timeoutMs;
        throw Unsupported("this fixture implements opaque Win32 handles; Linux software adapters are unsupported and cannot prove Windows sharing");
#endif
    }
    catch (const Unsupported& e)
    {
        if (evidence) evidence->event("skip", "fixture", e.what()); else std::cerr << e.what() << '\n';
        return SkipCode;
    }
    catch (const std::exception& e)
    {
        if (evidence) evidence->event("fail", "fixture", e.what()); else std::cerr << e.what() << '\n';
        // A failed GPU submission may still own the resource. No destructor may
        // perform an unbounded idle wait or free a resource still used by a GPU.
        // This is a standalone process, and the launcher owns its outer deadline.
        std::_Exit(1);
    }
}
