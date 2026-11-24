#include <jni.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string apiVersionString(uint32_t version) {
    std::ostringstream out;
    out << VK_VERSION_MAJOR(version) << "."
        << VK_VERSION_MINOR(version) << "."
        << VK_VERSION_PATCH(version);
    return out.str();
}

const char *deviceTypeName(VkPhysicalDeviceType type) {
    switch (type) {
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "Integrated GPU";
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "Discrete GPU";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "Virtual GPU";
        case VK_PHYSICAL_DEVICE_TYPE_CPU: return "CPU";
        default: return "Other";
    }
}

bool hasExtension(const std::vector<VkExtensionProperties> &extensions, const char *name) {
    return std::any_of(extensions.begin(), extensions.end(),
            [name](const VkExtensionProperties &extension) {
                return std::string(extension.extensionName) == name;
            });
}

}  // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_com_mikimn_droiduvc_NativeUvc_vulkanDeviceInfo(JNIEnv *env, jobject) {
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "ITX Display";
    appInfo.applicationVersion = 1;
    appInfo.pEngineName = "ITX Display diagnostics";
    appInfo.engineVersion = 1;
    appInfo.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;

    VkInstance instance = VK_NULL_HANDLE;
    const VkResult createResult = vkCreateInstance(&createInfo, nullptr, &instance);
    if (createResult != VK_SUCCESS) {
        const std::string message = "系统 Vulkan loader 存在但实例初始化失败 (VkResult="
                + std::to_string(static_cast<int>(createResult)) + ")";
        return env->NewStringUTF(message.c_str());
    }

    std::ostringstream report;
    uint32_t physicalDeviceCount = 0;
    VkResult enumerateResult = vkEnumeratePhysicalDevices(instance, &physicalDeviceCount, nullptr);
    if (enumerateResult != VK_SUCCESS || physicalDeviceCount == 0) {
        report << "Vulkan 实例可创建，但没有枚举到可用物理设备 (VkResult="
               << static_cast<int>(enumerateResult) << ")";
        vkDestroyInstance(instance, nullptr);
        return env->NewStringUTF(report.str().c_str());
    }

    std::vector<VkPhysicalDevice> devices(physicalDeviceCount);
    enumerateResult = vkEnumeratePhysicalDevices(instance, &physicalDeviceCount, devices.data());
    if (enumerateResult != VK_SUCCESS && enumerateResult != VK_INCOMPLETE) {
        report << "枚举 Vulkan 物理设备失败 (VkResult="
               << static_cast<int>(enumerateResult) << ")";
        vkDestroyInstance(instance, nullptr);
        return env->NewStringUTF(report.str().c_str());
    }

    report << "系统 Vulkan loader：可用；物理设备数：" << physicalDeviceCount << "\n";
    for (uint32_t deviceIndex = 0; deviceIndex < physicalDeviceCount; ++deviceIndex) {
        const VkPhysicalDevice device = devices[deviceIndex];
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device, &properties);

        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        if (queueFamilyCount > 0) {
            vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());
        }

        bool hasComputeQueue = false;
        bool hasGraphicsQueue = false;
        bool hasDedicatedComputeQueue = false;
        for (const VkQueueFamilyProperties &queue : queueFamilies) {
            const bool compute = (queue.queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
            const bool graphics = (queue.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
            hasComputeQueue = hasComputeQueue || compute;
            hasGraphicsQueue = hasGraphicsQueue || graphics;
            hasDedicatedComputeQueue = hasDedicatedComputeQueue || (compute && !graphics);
        }

        VkPhysicalDeviceMemoryProperties memoryProperties{};
        vkGetPhysicalDeviceMemoryProperties(device, &memoryProperties);
        uint64_t localMemoryBytes = 0;
        for (uint32_t heapIndex = 0; heapIndex < memoryProperties.memoryHeapCount; ++heapIndex) {
            const VkMemoryHeap &heap = memoryProperties.memoryHeaps[heapIndex];
            if ((heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) {
                localMemoryBytes += heap.size;
            }
        }

        uint32_t extensionCount = 0;
        std::vector<VkExtensionProperties> extensions;
        if (vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr) == VK_SUCCESS
                && extensionCount > 0) {
            extensions.resize(extensionCount);
            if (vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, extensions.data()) != VK_SUCCESS) {
                extensions.clear();
            }
        }

        if (deviceIndex > 0) report << "\n";
        report << "GPU " << (deviceIndex + 1) << "：" << properties.deviceName << "\n";
        report << "  类型：" << deviceTypeName(properties.deviceType)
               << "；API：" << apiVersionString(properties.apiVersion) << "\n";
        report << "  Driver version：" << properties.driverVersion
               << "；Vendor ID：0x" << std::hex << properties.vendorID
               << "；Device ID：0x" << properties.deviceID << std::dec << "\n";
        report << "  队列：" << (hasComputeQueue ? "支持计算" : "无计算队列")
               << " / " << (hasGraphicsQueue ? "支持图形" : "无图形队列")
               << (hasDedicatedComputeQueue ? " / 有独立计算队列" : " / 无独立计算队列")
               << "；队列族数：" << queueFamilyCount << "\n";
        report << "  Device-local heap：" << (localMemoryBytes / (1024 * 1024)) << " MiB"
               << "；最大 2D 图像：" << properties.limits.maxImageDimension2D
               << "；最大 Compute invocations：" << properties.limits.maxComputeWorkGroupInvocations
               << "\n";
        report << "  扩展：16-bit storage="
               << (hasExtension(extensions, "VK_KHR_16bit_storage") ? "是" : "未声明")
               << "；shader float16/int8="
               << (hasExtension(extensions, "VK_KHR_shader_float16_int8") ? "是" : "未声明")
               << "；storage buffer class="
               << (hasExtension(extensions, "VK_KHR_storage_buffer_storage_class") ? "是" : "未声明")
               << "\n";
    }

    vkDestroyInstance(instance, nullptr);
    return env->NewStringUTF(report.str().c_str());
}
