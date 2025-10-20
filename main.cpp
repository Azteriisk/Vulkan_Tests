#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>


#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include <limits>

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "third_party/imgui_filebrowser/ImGuiFileBrowser.h"
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>

namespace fs = std::filesystem;

namespace {
constexpr uint32_t kWidth = 1280;
constexpr uint32_t kHeight = 720;
constexpr uint64_t kFenceTimeoutNs = std::numeric_limits<uint64_t>::max();
constexpr int kMaxFramesInFlight = 3;
constexpr float kDefaultVerticalFovDegrees = 60.0f;
constexpr float kRotationSpeedDegreesPerSecond = 20.0f;

inline void FocusWindow(GLFWwindow* window) {
    if (window) {
        glfwFocusWindow(window);
    }
}

std::optional<std::string> GetEnvVar(const char* name) {
#ifdef _WIN32
    char* buffer = nullptr;
    size_t length = 0;
    if (_dupenv_s(&buffer, &length, name) != 0 || buffer == nullptr) {
        if (buffer != nullptr) {
            std::free(buffer);
        }
        return std::nullopt;
    }
    std::string value(buffer);
    std::free(buffer);
    return value;
#else
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string(value);
#endif
}


void CheckVkResult(VkResult err) {
    if (err == VK_SUCCESS) {
        return;
    }
    std::cerr << "[ImGui] Vulkan error: " << err << std::endl;
    if (err < 0) {
        throw std::runtime_error("ImGui Vulkan backend reported a fatal Vulkan error");
    }
}

#ifndef SHADER_DIR
#define SHADER_DIR "."
#endif

#ifdef NDEBUG
constexpr bool kEnableValidationLayers = false;
#else
constexpr bool kEnableValidationLayers = true;
#endif

const std::vector<const char*> kValidationLayers = {"VK_LAYER_KHRONOS_validation"};

const std::vector<const char*> kDeviceExtensions = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
};

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
    void*) {
    if (callbackData != nullptr) {
        if (callbackData->pMessageIdName != nullptr &&
            std::strcmp(callbackData->pMessageIdName, "Loader Message") == 0 &&
            callbackData->pMessage != nullptr &&
            std::strstr(callbackData->pMessage, "SocialClubVulkanLayer.json") != nullptr) {
            return VK_FALSE;
        }
    }
    if (messageSeverity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        std::cerr << "[Validation] " << callbackData->pMessage << std::endl;
    }
    return VK_FALSE;
}

VkResult CreateDebugUtilsMessengerEXT(
    VkInstance instance,
    const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDebugUtilsMessengerEXT* pMessenger) {
    auto func = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
    if (func != nullptr) {
        return func(instance, pCreateInfo, pAllocator, pMessenger);
    }
    return VK_ERROR_EXTENSION_NOT_PRESENT;
}

void DestroyDebugUtilsMessengerEXT(
    VkInstance instance,
    VkDebugUtilsMessengerEXT messenger,
    const VkAllocationCallbacks* pAllocator) {
    auto func = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
    if (func != nullptr) {
        func(instance, messenger, pAllocator);
    }
}

struct Vertex {
    glm::vec3 position{};
    glm::vec3 normal{};

    static VkVertexInputBindingDescription bindingDescription() {
        VkVertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = sizeof(Vertex);
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        return binding;
    }

    static std::array<VkVertexInputAttributeDescription, 2> attributeDescriptions() {
        std::array<VkVertexInputAttributeDescription, 2> attrs{};

        attrs[0].binding = 0;
        attrs[0].location = 0;
        attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
        attrs[0].offset = offsetof(Vertex, position);

        attrs[1].binding = 0;
        attrs[1].location = 1;
        attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT;
        attrs[1].offset = offsetof(Vertex, normal);

        return attrs;
    }
};

struct alignas(16) UniformBufferObject {
    glm::mat4 model{1.0f};
    glm::mat4 view{1.0f};
    glm::mat4 proj{1.0f};
};

glm::mat4 AiMatrixToGlm(const aiMatrix4x4& matrix) {
    return glm::transpose(glm::make_mat4(&matrix.a1));
}


struct QueueFamilyIndices {
    std::optional<uint32_t> graphicsFamily;
    std::optional<uint32_t> presentFamily;

    bool isComplete() const {
        return graphicsFamily.has_value() && presentFamily.has_value();
    }
};

struct SwapChainSupportDetails {
    VkSurfaceCapabilitiesKHR capabilities{};
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;
};

std::vector<char> readFile(const fs::path& filePath) {
    std::ifstream file(filePath, std::ios::ate | std::ios::binary);
    if (!file) {
        throw std::runtime_error("Failed to open file: " + filePath.string());
    }
    size_t fileSize = static_cast<size_t>(file.tellg());
    std::vector<char> buffer(fileSize);
    file.seekg(0);
    file.read(buffer.data(), buffer.size());
    return buffer;
}

}  // namespace

class TriangleApp {
public:
    void run() {
        initWindow();
        initVulkan();
        mainLoop();
        cleanup();
    }

private:
    void initWindow() {
        if (!glfwInit()) {
            throw std::runtime_error("Failed to initialize GLFW");
        }

        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
        window_ = glfwCreateWindow(static_cast<int>(kWidth), static_cast<int>(kHeight),
                                   "Vulkan Mesh Viewer", nullptr, nullptr);
        if (!window_) {
            glfwTerminate();
            throw std::runtime_error("Failed to create GLFW window");
        }

        glfwSetWindowUserPointer(window_, this);
        glfwSetFramebufferSizeCallback(window_, framebufferResizeCallback);
        glfwSetWindowSizeCallback(window_, windowResizeCallback);
        glfwSetWindowAspectRatio(window_, static_cast<int>(kWidth), static_cast<int>(kHeight));
        glfwSetKeyCallback(window_, keyCallback);
        glfwSetCharCallback(window_, charCallback);
        glfwSetMouseButtonCallback(window_, mouseButtonCallback);
        glfwSetScrollCallback(window_, scrollCallback);
        glfwSetCursorPosCallback(window_, cursorPosCallback);
        glfwSetCursorEnterCallback(window_, cursorEnterCallback);
    }

    void initVulkan() {
        configureLoaderEnvironment();
        createInstance();
        setupDebugMessenger();
        createSurface();
        pickPhysicalDevice();
        depthImageFormat_ = findDepthFormat();
        createLogicalDevice();
        createSwapChain();
        createImageViews();
        createDescriptorSetLayout();
        createPipelineLayout();
        createGraphicsPipeline();
        createCommandPool();
        createUniformBuffers();
        createDescriptorPool();
        createDescriptorSets();
        createDepthResources();
        createCommandBuffers();
        createImGuiDescriptorPool();
        initImGui();
        createSyncObjects();
        enterModelSelectionMode();
    }

    void mainLoop() {
        while (!glfwWindowShouldClose(window_)) {
            glfwPollEvents();

            drawFrame();
        }

        vkDeviceWaitIdle(device_);
    }

    void cleanup() {
        cleanupSwapChain();
        cleanupImGui();

        if (indexBuffer_ != VK_NULL_HANDLE) {
            vkDestroyBuffer(device_, indexBuffer_, nullptr);
            indexBuffer_ = VK_NULL_HANDLE;
        }
        if (indexBufferMemory_ != VK_NULL_HANDLE) {
            vkFreeMemory(device_, indexBufferMemory_, nullptr);
            indexBufferMemory_ = VK_NULL_HANDLE;
        }

        vkDestroyBuffer(device_, vertexBuffer_, nullptr);
        vkFreeMemory(device_, vertexBufferMemory_, nullptr);

        for (size_t i = 0; i < kMaxFramesInFlight; ++i) {
            if (uniformBuffers_[i] != VK_NULL_HANDLE) {
                vkDestroyBuffer(device_, uniformBuffers_[i], nullptr);
                uniformBuffers_[i] = VK_NULL_HANDLE;
            }
            if (uniformBuffersMapped_[i] != nullptr) {
                vkUnmapMemory(device_, uniformBuffersMemory_[i]);
                uniformBuffersMapped_[i] = nullptr;
            }
            if (uniformBuffersMemory_[i] != VK_NULL_HANDLE) {
                vkFreeMemory(device_, uniformBuffersMemory_[i], nullptr);
                uniformBuffersMemory_[i] = VK_NULL_HANDLE;
            }
        }

        if (descriptorPool_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
            descriptorPool_ = VK_NULL_HANDLE;
        }

        for (int i = 0; i < kMaxFramesInFlight; ++i) {
            vkDestroySemaphore(device_, renderFinishedSemaphores_[i], nullptr);
            vkDestroySemaphore(device_, imageAvailableSemaphores_[i], nullptr);
            vkDestroyFence(device_, inFlightFences_[i], nullptr);
        }

        vkDestroyCommandPool(device_, commandPool_, nullptr);
        vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
        pipelineLayout_ = VK_NULL_HANDLE;

        if (descriptorSetLayout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device_, descriptorSetLayout_, nullptr);
            descriptorSetLayout_ = VK_NULL_HANDLE;
        }

        vkDestroyDevice(device_, nullptr);

        if (kEnableValidationLayers) {
            DestroyDebugUtilsMessengerEXT(instance_, debugMessenger_, nullptr);
        }

        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        vkDestroyInstance(instance_, nullptr);

        glfwDestroyWindow(window_);
        glfwTerminate();
    }

    void recreateSwapChain() {
        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window_, &width, &height);
        while (width == 0 || height == 0) {
            glfwGetFramebufferSize(window_, &width, &height);
            glfwWaitEvents();
        }

        vkDeviceWaitIdle(device_);

        cleanupSwapChain();

        createSwapChain();
        createImageViews();
        createDepthResources();
        createGraphicsPipeline();
        createCommandBuffers();
        if (imguiInitialized_) {
            ImGui_ImplVulkan_SetMinImageCount(static_cast<uint32_t>(swapchainImages_.size()));
        }
    }

    void cleanupSwapChain() {
        if (!swapchainImageViews_.empty()) {
            for (auto imageView : swapchainImageViews_) {
                vkDestroyImageView(device_, imageView, nullptr);
            }
            swapchainImageViews_.clear();
        }

        if (!commandBuffers_.empty()) {
            vkFreeCommandBuffers(device_, commandPool_,
                                 static_cast<uint32_t>(commandBuffers_.size()), commandBuffers_.data());
            commandBuffers_.clear();
        }

        if (!depthImageViews_.empty()) {
            for (auto view : depthImageViews_) {
                vkDestroyImageView(device_, view, nullptr);
            }
            depthImageViews_.clear();
        }

        if (!depthImages_.empty()) {
            for (size_t i = 0; i < depthImages_.size(); ++i) {
                if (depthImages_[i] != VK_NULL_HANDLE) {
                    vkDestroyImage(device_, depthImages_[i], nullptr);
                }
                if (i < depthImageMemory_.size() && depthImageMemory_[i] != VK_NULL_HANDLE) {
                    vkFreeMemory(device_, depthImageMemory_[i], nullptr);
                }
            }
            depthImages_.clear();
            depthImageMemory_.clear();
        }
        depthImageLayouts_.clear();

        if (graphicsPipeline_ != VK_NULL_HANDLE) {
            vkDestroyPipeline(device_, graphicsPipeline_, nullptr);
            graphicsPipeline_ = VK_NULL_HANDLE;
        }

        if (swapchain_ != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(device_, swapchain_, nullptr);
            swapchain_ = VK_NULL_HANDLE;
        }

        swapchainImages_.clear();
        swapchainImageLayouts_.clear();
        imagesInFlight_.clear();
        currentImGuiDrawData_ = nullptr;
    }

    void createInstance();
    void populateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& createInfo) const;
    void setupDebugMessenger();
    void createSurface();
    void pickPhysicalDevice();
    void createLogicalDevice();
    void createSwapChain();
    void createImageViews();
    void createDescriptorSetLayout();
    void createPipelineLayout();
    void createGraphicsPipeline();
    void createCommandPool();
    void loadModel(const fs::path& modelPath);
    void processAssimpNode(const aiNode* node, const aiScene* scene, const glm::mat4& parentTransform,
                           glm::vec3& minBounds, glm::vec3& maxBounds);
    void processAssimpMesh(const aiMesh* mesh, const glm::mat4& transform,
                           glm::vec3& minBounds, glm::vec3& maxBounds);
    void enterModelSelectionMode();
    void unloadModel();
    void promptModelSelection();
    void createImGuiDescriptorPool();
    void initImGui();
    void cleanupImGui();
    void buildImGuiFrame();
    void uploadImGuiFonts();
    static void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods);
    static void charCallback(GLFWwindow* window, unsigned int c);
    static void mouseButtonCallback(GLFWwindow* window, int button, int action, int mods);
    static void scrollCallback(GLFWwindow* window, double xoffset, double yoffset);
    static void cursorPosCallback(GLFWwindow* window, double xpos, double ypos);
    static void cursorEnterCallback(GLFWwindow* window, int entered);
    void createVertexBuffer();
    void createIndexBuffer();
    void createUniformBuffers();
    void createDescriptorPool();
    void createDescriptorSets();
    void createDepthResources();
    void createCommandBuffers();
    void createSyncObjects();
    void updateUniformBuffer(uint32_t frameIndex);
    void recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex, uint32_t frameIndex);
    void drawFrame();
    bool isDeviceSuitable(VkPhysicalDevice device);
    QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device);
    bool checkDeviceExtensionSupport(VkPhysicalDevice device);
    SwapChainSupportDetails querySwapChainSupport(VkPhysicalDevice device);
    VkSurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats);
    VkPresentModeKHR chooseSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes);
    VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities);
    VkShaderModule createShaderModule(const std::vector<char>& code);
    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);
    VkFormat findSupportedFormat(const std::vector<VkFormat>& candidates, VkImageTiling tiling,
                                 VkFormatFeatureFlags features);
    VkFormat findDepthFormat();
    void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
                      VkBuffer& buffer, VkDeviceMemory& bufferMemory);
    VkCommandBuffer beginSingleTimeCommands();
    void endSingleTimeCommands(VkCommandBuffer commandBuffer);
    void copyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size);
    std::vector<const char*> getRequiredExtensions();
    bool checkValidationLayerSupport();
    static void framebufferResizeCallback(GLFWwindow* window, int, int);
    static void windowResizeCallback(GLFWwindow* window, int, int);
    void configureLoaderEnvironment() const;

private:
    GLFWwindow* window_ = nullptr;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;

    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;

    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    VkQueue presentQueue_ = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    std::vector<VkImage> swapchainImages_;
    std::vector<VkImageView> swapchainImageViews_;
    std::vector<VkImageLayout> swapchainImageLayouts_;
    std::vector<VkFence> imagesInFlight_;
    VkFormat swapchainImageFormat_{};
    VkExtent2D swapchainExtent_{};

    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline graphicsPipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorPool imguiDescriptorPool_ = VK_NULL_HANDLE;
    bool imguiInitialized_ = false;
    ImDrawData* currentImGuiDrawData_ = nullptr;
    std::array<VkDescriptorSet, kMaxFramesInFlight> descriptorSets_{};

    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers_;

    VkBuffer vertexBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory vertexBufferMemory_ = VK_NULL_HANDLE;
    VkBuffer indexBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory indexBufferMemory_ = VK_NULL_HANDLE;

    std::array<VkBuffer, kMaxFramesInFlight> uniformBuffers_{};
    std::array<VkDeviceMemory, kMaxFramesInFlight> uniformBuffersMemory_{};
    std::array<void*, kMaxFramesInFlight> uniformBuffersMapped_{};

    std::vector<Vertex> vertices_;
    std::vector<uint32_t> indices_;

    std::vector<VkImage> depthImages_;
    std::vector<VkDeviceMemory> depthImageMemory_;
    std::vector<VkImageView> depthImageViews_;
    std::vector<VkImageLayout> depthImageLayouts_;
    VkFormat depthImageFormat_ = VK_FORMAT_UNDEFINED;

    std::array<VkSemaphore, kMaxFramesInFlight> imageAvailableSemaphores_{};
    std::array<VkSemaphore, kMaxFramesInFlight> renderFinishedSemaphores_{};
    std::array<VkFence, kMaxFramesInFlight> inFlightFences_{};

    ImGui::FileBrowser fileBrowser_{ImGui::FileBrowser::DialogMode::Open};
    size_t currentFrame_ = 0;
    bool framebufferResized_ = false;
    bool swapchainOutOfDate_ = false;
    bool modelLoaded_ = false;
    bool dialogInProgress_ = false;
    bool lastUiVisible_ = false;

    enum class AppMode {
        AwaitingModel,
        DisplayingModel,
    };

    AppMode appMode_ = AppMode::AwaitingModel;
    std::string lastErrorMessage_;
    bool showLoadErrorPopup_ = false;
    uint32_t graphicsQueueFamilyIndex_ = 0;

    float modelScale_ = 1.0f;
    float modelRadius_ = 1.0f;
    float cameraDistance_ = 3.0f;
    float rotationSpeedRadiansPerSecond_ = glm::radians(kRotationSpeedDegreesPerSecond);
    float verticalFovRadians_ = glm::radians(kDefaultVerticalFovDegrees);
    std::chrono::steady_clock::time_point startTime_ = std::chrono::steady_clock::now();
    fs::path loadedModelPath_;
};

void TriangleApp::configureLoaderEnvironment() const {
#ifdef _WIN32
    auto setEnvVar = [](const char* name, const std::string& value) {
        if (_putenv_s(name, value.c_str()) != 0) {
            std::cerr << "Warning: failed to set environment variable '" << name << "'" << std::endl;
        }
    };
#else
    auto setEnvVar = [](const char* name, const std::string& value) {
        if (setenv(name, value.c_str(), 1) != 0) {
            std::cerr << "Warning: failed to set environment variable '" << name << "'" << std::endl;
        }
    };
#endif

    setEnvVar("VK_LOADER_DISABLE_IMPLICIT_LAYERS", "1");

    auto sdkRootOpt = GetEnvVar("VULKAN_SDK");
    if (!sdkRootOpt.has_value() || sdkRootOpt->empty()) {
        return;
    }
    const fs::path sdkPath(*sdkRootOpt);

    std::vector<std::string> validPaths;
    const std::array<fs::path, 6> candidateDirs = {
        sdkPath / "Bin",
        sdkPath / "bin",
        sdkPath / "Lib",
        sdkPath / "lib",
        sdkPath / "etc" / "vulkan" / "explicit_layer.d",
        sdkPath / "share" / "vulkan" / "explicit_layer.d",
    };

    for (const auto& dir : candidateDirs) {
        try {
            if (!fs::is_directory(dir)) {
                continue;
            }
            for (const auto& entry : fs::directory_iterator(dir)) {
                if (entry.is_regular_file() && entry.path().extension() == ".json") {
                    validPaths.push_back(dir.string());
                    break;
                }
            }
        } catch (const std::exception&) {
            continue;
        }
    }

    if (validPaths.empty()) {
        return;
    }

    std::string combined;
    combined.reserve(validPaths.size() * 32);

    std::sort(validPaths.begin(), validPaths.end());
    validPaths.erase(std::unique(validPaths.begin(), validPaths.end()), validPaths.end());

#ifdef _WIN32
    constexpr char kPathSeparator = ';';
#else
    constexpr char kPathSeparator = ':';
#endif

    for (size_t i = 0; i < validPaths.size(); ++i) {
        if (i > 0) {
            combined += kPathSeparator;
        }
        combined += validPaths[i];
    }
    setEnvVar("VK_LAYER_PATH", combined);
}

void TriangleApp::createInstance() {
    if (kEnableValidationLayers && !checkValidationLayerSupport()) {
        throw std::runtime_error("Validation layers requested but not available");
    }

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "Vulkan Mesh Viewer";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "No Engine";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_MAKE_API_VERSION(0, 1, 3, 0);

    std::vector<const char*> extensions = getRequiredExtensions();

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

    VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
    if (kEnableValidationLayers) {
        createInfo.enabledLayerCount = static_cast<uint32_t>(kValidationLayers.size());
        createInfo.ppEnabledLayerNames = kValidationLayers.data();

        populateDebugMessengerCreateInfo(debugCreateInfo);
        createInfo.pNext = &debugCreateInfo;
    }

#ifdef __APPLE__
#ifdef VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR
        createInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif
#endif

    if (vkCreateInstance(&createInfo, nullptr, &instance_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create Vulkan instance");
    }
}

void TriangleApp::populateDebugMessengerCreateInfo(
    VkDebugUtilsMessengerCreateInfoEXT& createInfo) const {
    createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    createInfo.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    createInfo.messageType =
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    createInfo.pfnUserCallback = debugCallback;
}

void TriangleApp::setupDebugMessenger() {
    if (!kEnableValidationLayers) {
        return;
    }
    VkDebugUtilsMessengerCreateInfoEXT createInfo{};
    populateDebugMessengerCreateInfo(createInfo);
    if (CreateDebugUtilsMessengerEXT(instance_, &createInfo, nullptr, &debugMessenger_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to set up debug messenger");
    }
}

void TriangleApp::createSurface() {
    if (glfwCreateWindowSurface(instance_, window_, nullptr, &surface_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create window surface");
    }
}

void TriangleApp::pickPhysicalDevice() {
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance_, &deviceCount, nullptr);
    if (deviceCount == 0) {
        throw std::runtime_error("Failed to find GPUs with Vulkan support");
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(instance_, &deviceCount, devices.data());

    for (const auto& device : devices) {
        if (isDeviceSuitable(device)) {
            physicalDevice_ = device;
            break;
        }
    }

    if (physicalDevice_ == VK_NULL_HANDLE) {
        throw std::runtime_error("Failed to find a suitable GPU");
    }
}

void TriangleApp::createLogicalDevice() {
    QueueFamilyIndices indices = findQueueFamilies(physicalDevice_);

    std::set<uint32_t> uniqueQueueFamilies = {
        indices.graphicsFamily.value(),
        indices.presentFamily.value(),
    };

    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    queueCreateInfos.reserve(uniqueQueueFamilies.size());

    float queuePriority = 1.0f;
    for (uint32_t queueFamily : uniqueQueueFamilies) {
        VkDeviceQueueCreateInfo queueCreateInfo{};
        queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueCreateInfo.queueFamilyIndex = queueFamily;
        queueCreateInfo.queueCount = 1;
        queueCreateInfo.pQueuePriorities = &queuePriority;
        queueCreateInfos.push_back(queueCreateInfo);
    }

    VkPhysicalDeviceDynamicRenderingFeatures dynamicRenderingFeatures{};
    dynamicRenderingFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;
    dynamicRenderingFeatures.dynamicRendering = VK_TRUE;

    VkPhysicalDeviceFeatures deviceFeatures{};

    std::vector<const char*> enabledExtensions = kDeviceExtensions;

#ifdef __APPLE__
    enabledExtensions.push_back("VK_KHR_portability_subset");
#endif

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos = queueCreateInfos.data();
    createInfo.pEnabledFeatures = &deviceFeatures;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
    createInfo.ppEnabledExtensionNames = enabledExtensions.data();
    createInfo.pNext = &dynamicRenderingFeatures;

    if (kEnableValidationLayers) {
        createInfo.enabledLayerCount = static_cast<uint32_t>(kValidationLayers.size());
        createInfo.ppEnabledLayerNames = kValidationLayers.data();
    }

    if (vkCreateDevice(physicalDevice_, &createInfo, nullptr, &device_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create logical device");
    }

    graphicsQueueFamilyIndex_ = indices.graphicsFamily.value();
    vkGetDeviceQueue(device_, graphicsQueueFamilyIndex_, 0, &graphicsQueue_);
    vkGetDeviceQueue(device_, indices.presentFamily.value(), 0, &presentQueue_);
}

void TriangleApp::createSwapChain() {
    SwapChainSupportDetails swapChainSupport = querySwapChainSupport(physicalDevice_);

    VkSurfaceFormatKHR surfaceFormat = chooseSwapSurfaceFormat(swapChainSupport.formats);
    VkPresentModeKHR presentMode = chooseSwapPresentMode(swapChainSupport.presentModes);
    VkExtent2D extent = chooseSwapExtent(swapChainSupport.capabilities);
    uint32_t imageCount = swapChainSupport.capabilities.minImageCount + 1;
    if (swapChainSupport.capabilities.maxImageCount > 0 &&
        imageCount > swapChainSupport.capabilities.maxImageCount) {
        imageCount = swapChainSupport.capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = surface_;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = surfaceFormat.format;
    createInfo.imageColorSpace = surfaceFormat.colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    QueueFamilyIndices indices = findQueueFamilies(physicalDevice_);
    uint32_t queueFamilyIndices[] = {indices.graphicsFamily.value(), indices.presentFamily.value()};

    if (indices.graphicsFamily != indices.presentFamily) {
        createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = 2;
        createInfo.pQueueFamilyIndices = queueFamilyIndices;
    } else {
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    createInfo.preTransform = swapChainSupport.capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;
    createInfo.oldSwapchain = swapchain_;

    if (vkCreateSwapchainKHR(device_, &createInfo, nullptr, &swapchain_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create swap chain");
    }

    vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, nullptr);
    swapchainImages_.resize(imageCount);
    vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, swapchainImages_.data());

    swapchainImageFormat_ = surfaceFormat.format;
    swapchainExtent_ = extent;

    swapchainImageLayouts_.assign(imageCount, VK_IMAGE_LAYOUT_UNDEFINED);
    imagesInFlight_.assign(imageCount, VK_NULL_HANDLE);

    if (imguiInitialized_) {
        ImGui_ImplVulkan_SetMinImageCount(imageCount);
    }
}

void TriangleApp::createImageViews() {
    swapchainImageViews_.resize(swapchainImages_.size());

    for (size_t i = 0; i < swapchainImages_.size(); ++i) {
        VkImageViewCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        createInfo.image = swapchainImages_[i];
        createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        createInfo.format = swapchainImageFormat_;
        createInfo.components = {
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY,
        };
        createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        createInfo.subresourceRange.baseMipLevel = 0;
        createInfo.subresourceRange.levelCount = 1;
        createInfo.subresourceRange.baseArrayLayer = 0;
        createInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(device_, &createInfo, nullptr, &swapchainImageViews_[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create image views");
        }
    }
}

void TriangleApp::createDescriptorSetLayout() {
    if (descriptorSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, descriptorSetLayout_, nullptr);
        descriptorSetLayout_ = VK_NULL_HANDLE;
    }

    VkDescriptorSetLayoutBinding uboLayoutBinding{};
    uboLayoutBinding.binding = 0;
    uboLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uboLayoutBinding.descriptorCount = 1;
    uboLayoutBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    uboLayoutBinding.pImmutableSamplers = nullptr;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &uboLayoutBinding;

    if (vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &descriptorSetLayout_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create descriptor set layout");
    }
}

void TriangleApp::createPipelineLayout() {
    if (pipelineLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
        pipelineLayout_ = VK_NULL_HANDLE;
    }

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &descriptorSetLayout_;

    if (vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr, &pipelineLayout_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create pipeline layout");
    }
}

void TriangleApp::createGraphicsPipeline() {
    if (graphicsPipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, graphicsPipeline_, nullptr);
        graphicsPipeline_ = VK_NULL_HANDLE;
    }

    fs::path shaderRoot{SHADER_DIR};
    auto vertShaderCode = readFile(shaderRoot / "triangle.vert.spv");
    auto fragShaderCode = readFile(shaderRoot / "triangle.frag.spv");

    VkShaderModule vertShaderModule = createShaderModule(vertShaderCode);
    VkShaderModule fragShaderModule = createShaderModule(fragShaderCode);

    VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
    vertShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertShaderStageInfo.module = vertShaderModule;
    vertShaderStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
    fragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragShaderStageInfo.module = fragShaderModule;
    fragShaderStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo shaderStages[] = {vertShaderStageInfo, fragShaderStageInfo};

    auto bindingDescription = Vertex::bindingDescription();
    auto attributeDescriptions = Vertex::attributeDescriptions();

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount = 1;
    vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
    vertexInputInfo.vertexAttributeDescriptionCount =
        static_cast<uint32_t>(attributeDescriptions.size());
    vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;
    rasterizer.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;
    depthStencil.front = {};
    depthStencil.back = {};

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.logicOp = VK_LOGIC_OP_COPY;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;
    colorBlending.blendConstants[0] = 0.0f;
    colorBlending.blendConstants[1] = 0.0f;
    colorBlending.blendConstants[2] = 0.0f;
    colorBlending.blendConstants[3] = 0.0f;

    std::array<VkDynamicState, 2> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };

    VkPipelineDynamicStateCreateInfo dynamicState{};
   dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    VkPipelineRenderingCreateInfo pipelineRenderingInfo{};
    pipelineRenderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    pipelineRenderingInfo.colorAttachmentCount = 1;
    pipelineRenderingInfo.pColorAttachmentFormats = &swapchainImageFormat_;
    pipelineRenderingInfo.depthAttachmentFormat = depthImageFormat_;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = pipelineLayout_;
    pipelineInfo.pNext = &pipelineRenderingInfo;

    if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                                  &graphicsPipeline_) != VK_SUCCESS) {
        vkDestroyShaderModule(device_, fragShaderModule, nullptr);
        vkDestroyShaderModule(device_, vertShaderModule, nullptr);
        throw std::runtime_error("Failed to create graphics pipeline");
    }

    vkDestroyShaderModule(device_, fragShaderModule, nullptr);
    vkDestroyShaderModule(device_, vertShaderModule, nullptr);
}

void TriangleApp::createCommandPool() {
    QueueFamilyIndices queueFamilyIndices = findQueueFamilies(physicalDevice_);

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamilyIndices.graphicsFamily.value();

    if (vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create command pool");
    }
}

void TriangleApp::loadModel(const fs::path& modelPath) {
    unloadModel();

    Assimp::Importer importer;
    const unsigned int importFlags =
        aiProcess_Triangulate |
        aiProcess_GenSmoothNormals |
        aiProcess_JoinIdenticalVertices |
        aiProcess_ImproveCacheLocality;

    const aiScene* scene = importer.ReadFile(modelPath.string(), importFlags);

    if (!scene || !scene->mRootNode || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE)) {
        throw std::runtime_error("Failed to load model: " + std::string(importer.GetErrorString()));
    }

    vertices_.clear();
    indices_.clear();

    glm::vec3 minBounds(std::numeric_limits<float>::max());
    glm::vec3 maxBounds(std::numeric_limits<float>::lowest());

    processAssimpNode(scene->mRootNode, scene, glm::mat4(1.0f), minBounds, maxBounds);

    if (vertices_.empty() || indices_.empty()) {
        throw std::runtime_error("Loaded model contained no geometry");
    }

    glm::vec3 center = (minBounds + maxBounds) * 0.5f;
    float maxRadiusSquared = 0.0f;

    for (auto& vertex : vertices_) {
        vertex.position -= center;
        maxRadiusSquared = std::max(maxRadiusSquared, glm::dot(vertex.position, vertex.position));
    }

    modelRadius_ = std::sqrt(std::max(maxRadiusSquared, 1e-6f));
    modelScale_ = 1.0f / modelRadius_;

    const float scaledRadius = modelRadius_ * modelScale_;
    const float halfFov = verticalFovRadians_ * 0.5f;
    const float minDistance = scaledRadius / std::tan(halfFov);
    cameraDistance_ = std::max(minDistance + scaledRadius * 0.75f, scaledRadius + 0.5f);

    createVertexBuffer();
    createIndexBuffer();

    startTime_ = std::chrono::steady_clock::now();
    loadedModelPath_ = fs::absolute(modelPath);
    modelLoaded_ = true;
    appMode_ = AppMode::DisplayingModel;
    lastErrorMessage_.clear();
    showLoadErrorPopup_ = false;

    if (window_) {
        std::string title = "Vulkan Mesh Viewer - " + loadedModelPath_.filename().string();
        glfwSetWindowTitle(window_, title.c_str());
    }

    std::cout << "Loaded model \"" << loadedModelPath_.string() << "\" with "
              << vertices_.size() << " vertices and "
              << indices_.size() / 3 << " triangles" << std::endl;
}

void TriangleApp::processAssimpNode(const aiNode* node,
                                    const aiScene* scene,
                                    const glm::mat4& parentTransform,
                                    glm::vec3& minBounds,
                                    glm::vec3& maxBounds) {
    if (!node) {
        return;
    }

    glm::mat4 nodeTransform = parentTransform * AiMatrixToGlm(node->mTransformation);

    for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
        const aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
        processAssimpMesh(mesh, nodeTransform, minBounds, maxBounds);
    }

    for (unsigned int i = 0; i < node->mNumChildren; ++i) {
        processAssimpNode(node->mChildren[i], scene, nodeTransform, minBounds, maxBounds);
    }
}

void TriangleApp::processAssimpMesh(const aiMesh* mesh,
                                    const glm::mat4& transform,
                                    glm::vec3& minBounds,
                                    glm::vec3& maxBounds) {
    if (!mesh) {
        return;
    }

    const size_t baseVertex = vertices_.size();
    vertices_.reserve(vertices_.size() + mesh->mNumVertices);
    indices_.reserve(indices_.size() + mesh->mNumFaces * 3);

    glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(transform)));

    for (unsigned int v = 0; v < mesh->mNumVertices; ++v) {
        Vertex vertex{};
        const aiVector3D& pos = mesh->mVertices[v];
        glm::vec4 transformedPosition = transform * glm::vec4(pos.x, pos.y, pos.z, 1.0f);
        vertex.position = glm::vec3(transformedPosition);

        glm::vec3 normal(0.0f, 1.0f, 0.0f);
        if (mesh->HasNormals()) {
            const aiVector3D& n = mesh->mNormals[v];
            normal = glm::vec3(n.x, n.y, n.z);
        }
        normal = glm::normalize(normalMatrix * normal);
        if (!std::isfinite(normal.x) || !std::isfinite(normal.y) || !std::isfinite(normal.z)) {
            normal = glm::vec3(0.0f, 1.0f, 0.0f);
        }
        vertex.normal = normal;

        vertices_.push_back(vertex);
        minBounds = glm::min(minBounds, vertex.position);
        maxBounds = glm::max(maxBounds, vertex.position);
    }

    for (unsigned int f = 0; f < mesh->mNumFaces; ++f) {
        const aiFace& face = mesh->mFaces[f];
        if (face.mNumIndices < 3) {
            continue;
        }
        indices_.push_back(static_cast<uint32_t>(baseVertex + face.mIndices[0]));
        indices_.push_back(static_cast<uint32_t>(baseVertex + face.mIndices[1]));
        indices_.push_back(static_cast<uint32_t>(baseVertex + face.mIndices[2]));
    }
}

void TriangleApp::enterModelSelectionMode() {
    appMode_ = AppMode::AwaitingModel;
    fileBrowser_.Close();
    fileBrowser_.ClearSelected();
    dialogInProgress_ = false;
    unloadModel();
    if (window_) {
        glfwSetWindowTitle(window_, "Vulkan Mesh Viewer - Select a model");
        FocusWindow(window_);
    }
}

void TriangleApp::unloadModel() {
    if (!modelLoaded_) {
        loadedModelPath_.clear();
        startTime_ = std::chrono::steady_clock::now();
        return;
    }

    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
    }

    if (indexBuffer_ != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, indexBuffer_, nullptr);
        indexBuffer_ = VK_NULL_HANDLE;
    }
    if (indexBufferMemory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, indexBufferMemory_, nullptr);
        indexBufferMemory_ = VK_NULL_HANDLE;
    }
    if (vertexBuffer_ != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, vertexBuffer_, nullptr);
        vertexBuffer_ = VK_NULL_HANDLE;
    }
    if (vertexBufferMemory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, vertexBufferMemory_, nullptr);
        vertexBufferMemory_ = VK_NULL_HANDLE;
    }

    vertices_.clear();
    indices_.clear();
    modelLoaded_ = false;
    modelRadius_ = 1.0f;
    modelScale_ = 1.0f;
    cameraDistance_ = 3.0f;
    loadedModelPath_.clear();
    startTime_ = std::chrono::steady_clock::now();
}


void TriangleApp::keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    ImGui_ImplGlfw_KeyCallback(window, key, scancode, action, mods);
}

void TriangleApp::charCallback(GLFWwindow* window, unsigned int c) {
    ImGui_ImplGlfw_CharCallback(window, c);
}

void TriangleApp::mouseButtonCallback(GLFWwindow* window, int button, int action, int mods) {
    ImGui_ImplGlfw_MouseButtonCallback(window, button, action, mods);
}

void TriangleApp::scrollCallback(GLFWwindow* window, double xoffset, double yoffset) {
    ImGui_ImplGlfw_ScrollCallback(window, xoffset, yoffset);
}

void TriangleApp::cursorPosCallback(GLFWwindow* window, double xpos, double ypos) {
    ImGui_ImplGlfw_CursorPosCallback(window, xpos, ypos);
}

void TriangleApp::cursorEnterCallback(GLFWwindow* window, int entered) {
    ImGui_ImplGlfw_CursorEnterCallback(window, entered);
}

void TriangleApp::createVertexBuffer() {
    if (vertices_.empty()) {
        throw std::runtime_error("No vertex data loaded");
    }

    VkDeviceSize bufferSize = sizeof(Vertex) * vertices_.size();

    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingBufferMemory = VK_NULL_HANDLE;
    createBuffer(bufferSize,
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 stagingBuffer,
                 stagingBufferMemory);

    void* data = nullptr;
    vkMapMemory(device_, stagingBufferMemory, 0, bufferSize, 0, &data);
    std::memcpy(data, vertices_.data(), static_cast<size_t>(bufferSize));
    vkUnmapMemory(device_, stagingBufferMemory);

    createBuffer(bufferSize,
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                 vertexBuffer_,
                 vertexBufferMemory_);

    copyBuffer(stagingBuffer, vertexBuffer_, bufferSize);

    vkDestroyBuffer(device_, stagingBuffer, nullptr);
    vkFreeMemory(device_, stagingBufferMemory, nullptr);
}

void TriangleApp::createIndexBuffer() {
    if (indices_.empty()) {
        throw std::runtime_error("No index data loaded");
    }

    VkDeviceSize bufferSize = sizeof(uint32_t) * indices_.size();

    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingBufferMemory = VK_NULL_HANDLE;
    createBuffer(bufferSize,
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 stagingBuffer,
                 stagingBufferMemory);

    void* data = nullptr;
    vkMapMemory(device_, stagingBufferMemory, 0, bufferSize, 0, &data);
    std::memcpy(data, indices_.data(), static_cast<size_t>(bufferSize));
    vkUnmapMemory(device_, stagingBufferMemory);

    createBuffer(bufferSize,
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                 indexBuffer_,
                 indexBufferMemory_);

    copyBuffer(stagingBuffer, indexBuffer_, bufferSize);

    vkDestroyBuffer(device_, stagingBuffer, nullptr);
    vkFreeMemory(device_, stagingBufferMemory, nullptr);
}

void TriangleApp::createUniformBuffers() {
    VkDeviceSize bufferSize = sizeof(UniformBufferObject);

    for (size_t i = 0; i < kMaxFramesInFlight; ++i) {
        if (uniformBuffers_[i] != VK_NULL_HANDLE) {
            vkDestroyBuffer(device_, uniformBuffers_[i], nullptr);
            uniformBuffers_[i] = VK_NULL_HANDLE;
        }
        if (uniformBuffersMapped_[i] != nullptr) {
            vkUnmapMemory(device_, uniformBuffersMemory_[i]);
            uniformBuffersMapped_[i] = nullptr;
        }
        if (uniformBuffersMemory_[i] != VK_NULL_HANDLE) {
            vkFreeMemory(device_, uniformBuffersMemory_[i], nullptr);
            uniformBuffersMemory_[i] = VK_NULL_HANDLE;
        }

        createBuffer(bufferSize,
                     VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     uniformBuffers_[i],
                     uniformBuffersMemory_[i]);

        void* data = nullptr;
        vkMapMemory(device_, uniformBuffersMemory_[i], 0, bufferSize, 0, &data);
        uniformBuffersMapped_[i] = data;
    }
}

void TriangleApp::createDescriptorPool() {
    if (descriptorPool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
        descriptorPool_ = VK_NULL_HANDLE;
    }

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSize.descriptorCount = kMaxFramesInFlight;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = kMaxFramesInFlight;

    if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create descriptor pool");
    }
}

void TriangleApp::createDescriptorSets() {
    std::array<VkDescriptorSetLayout, kMaxFramesInFlight> layouts{};
    layouts.fill(descriptorSetLayout_);

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = descriptorPool_;
    allocInfo.descriptorSetCount = kMaxFramesInFlight;
    allocInfo.pSetLayouts = layouts.data();

    if (vkAllocateDescriptorSets(device_, &allocInfo, descriptorSets_.data()) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate descriptor sets");
    }

    for (size_t i = 0; i < kMaxFramesInFlight; ++i) {
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = uniformBuffers_[i];
        bufferInfo.offset = 0;
        bufferInfo.range = sizeof(UniformBufferObject);

        VkWriteDescriptorSet descriptorWrite{};
        descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrite.dstSet = descriptorSets_[i];
        descriptorWrite.dstBinding = 0;
        descriptorWrite.dstArrayElement = 0;
        descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        descriptorWrite.descriptorCount = 1;
        descriptorWrite.pBufferInfo = &bufferInfo;
        descriptorWrite.pImageInfo = nullptr;
        descriptorWrite.pTexelBufferView = nullptr;

        vkUpdateDescriptorSets(device_, 1, &descriptorWrite, 0, nullptr);
    }
}

void TriangleApp::createImGuiDescriptorPool() {
    if (imguiDescriptorPool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, imguiDescriptorPool_, nullptr);
        imguiDescriptorPool_ = VK_NULL_HANDLE;
    }

    std::array<VkDescriptorPoolSize, 11> poolSizes = {
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLER, 1000},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000},
    };

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = 1000 * static_cast<uint32_t>(poolSizes.size());
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();

    if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &imguiDescriptorPool_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create Dear ImGui descriptor pool");
    }
}

void TriangleApp::initImGui() {
    if (imguiInitialized_) {
        return;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();

    if (!ImGui_ImplGlfw_InitForVulkan(window_, false)) {
        throw std::runtime_error("Failed to initialize ImGui GLFW backend");
    }

    ImGui_ImplVulkan_InitInfo initInfo{};
    initInfo.Instance = instance_;
    initInfo.PhysicalDevice = physicalDevice_;
    initInfo.Device = device_;
    initInfo.QueueFamily = graphicsQueueFamilyIndex_;
    initInfo.Queue = graphicsQueue_;
    initInfo.PipelineCache = VK_NULL_HANDLE;
    initInfo.DescriptorPool = imguiDescriptorPool_;
    initInfo.Subpass = 0;
    initInfo.MinImageCount = static_cast<uint32_t>(swapchainImages_.size());
    initInfo.ImageCount = static_cast<uint32_t>(swapchainImages_.size());
    initInfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    initInfo.Allocator = nullptr;
    initInfo.CheckVkResultFn = CheckVkResult;
    initInfo.RenderPass = VK_NULL_HANDLE;
    initInfo.UseDynamicRendering = true;
    initInfo.PipelineRenderingCreateInfo = {};
    initInfo.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    initInfo.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    initInfo.PipelineRenderingCreateInfo.pColorAttachmentFormats = &swapchainImageFormat_;
    initInfo.PipelineRenderingCreateInfo.depthAttachmentFormat = depthImageFormat_;
    initInfo.PipelineRenderingCreateInfo.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;

    if (!ImGui_ImplVulkan_Init(&initInfo)) {
        throw std::runtime_error("Failed to initialize ImGui Vulkan backend");
    }

    uploadImGuiFonts();
    imguiInitialized_ = true;
}

void TriangleApp::cleanupImGui() {
    currentImGuiDrawData_ = nullptr;
    if (imguiInitialized_) {
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        imguiInitialized_ = false;
    }

    if (imguiDescriptorPool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, imguiDescriptorPool_, nullptr);
        imguiDescriptorPool_ = VK_NULL_HANDLE;
    }
}

void TriangleApp::buildImGuiFrame() {
    currentImGuiDrawData_ = nullptr;
    if (!imguiInitialized_) {
        return;
    }

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    ImGuiIO& io = ImGui::GetIO();

    if (!io.WantCaptureKeyboard) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            if (appMode_ == AppMode::DisplayingModel) {
                enterModelSelectionMode();
            } else if (appMode_ == AppMode::AwaitingModel) {
                glfwSetWindowShouldClose(window_, GLFW_TRUE);
            }
        }

        if (appMode_ == AppMode::AwaitingModel &&
            (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter) ||
             ImGui::IsKeyPressed(ImGuiKey_Space))) {
            promptModelSelection();
        }
    }


    if (appMode_ == AppMode::AwaitingModel) {
        const ImVec2 windowSize(520.0f, 260.0f);
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(windowSize);
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;

        if (ImGui::Begin("Model Loader", nullptr, flags)) {
            ImGui::TextWrapped("Load a mesh from the assets folder (.fbx / .obj). The mesh will be "
                               "centered, scaled to fit the camera, and placed on a turntable.");
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            if (ImGui::Button("Browse assets...", ImVec2(-1.0f, 0.0f))) {
                promptModelSelection();
            }

            ImGui::Spacing();
            ImGui::TextUnformatted("Shortcuts:");
            ImGui::BulletText("Enter / Space — Browse for a model");
            ImGui::BulletText("Esc — Quit the viewer");

            if (!lastErrorMessage_.empty()) {
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
                ImGui::TextWrapped("Last error: %s", lastErrorMessage_.c_str());
                ImGui::PopStyleColor();
            }
        }
        ImGui::End();
    } else if (appMode_ == AppMode::DisplayingModel) {
        ImGui::SetNextWindowBgAlpha(0.35f);
        ImGui::SetNextWindowPos(ImVec2(12.0f, 12.0f), ImGuiCond_Always);
        ImGui::Begin("ViewerOverlay", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
        ImGui::TextUnformatted("Press Esc to return to the loader");
        ImGui::End();
    }

    if (fileBrowser_.IsOpened()) {
        bool wasOpen = fileBrowser_.IsOpened();
        fileBrowser_.Display();
        bool stillOpen = fileBrowser_.IsOpened();
        if (fileBrowser_.HasSelected()) {
            auto selected = fileBrowser_.GetSelected();
            fileBrowser_.ClearSelected();
            fileBrowser_.Close();
            dialogInProgress_ = false;
            try {
                loadModel(selected);
                FocusWindow(window_);
            } catch (const std::exception& e) {
                lastErrorMessage_ = e.what();
                showLoadErrorPopup_ = true;
                appMode_ = AppMode::AwaitingModel;
                FocusWindow(window_);
            }
        } else if (wasOpen && !stillOpen) {
            dialogInProgress_ = false;
            fileBrowser_.ClearSelected();
        }
    }

    if (showLoadErrorPopup_ && !lastErrorMessage_.empty()) {
        ImGui::OpenPopup("Load Error");
        showLoadErrorPopup_ = false;
    }

    if (ImGui::BeginPopupModal("Load Error", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("%s", lastErrorMessage_.c_str());
        ImGui::Spacing();
        if (ImGui::Button("OK", ImVec2(120.0f, 0.0f))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::Render();
    currentImGuiDrawData_ = ImGui::GetDrawData();
    if (currentImGuiDrawData_ && (io.DisplayFramebufferScale.x != 1.0f || io.DisplayFramebufferScale.y != 1.0f)) {
        currentImGuiDrawData_->ScaleClipRects(io.DisplayFramebufferScale);
    }
    lastUiVisible_ = currentImGuiDrawData_ && currentImGuiDrawData_->CmdListsCount > 0;
}

void TriangleApp::uploadImGuiFonts() {
    if (!ImGui_ImplVulkan_CreateFontsTexture()) {
        throw std::runtime_error("Failed to create ImGui font texture");
    }
}

void TriangleApp::promptModelSelection() {
    if (dialogInProgress_) {
        return;
    }
    dialogInProgress_ = true;

    fs::path initialDir = fs::current_path() / "assets";
    if (!fs::exists(initialDir)) {
        initialDir = fs::current_path();
    }

    fileBrowser_.ClearSelected();
    fileBrowser_.SetTitle("Select Model");
    const std::vector<const char*> filters = {".fbx", ".obj"};
    fileBrowser_.SetTypeFilters(filters);
    fileBrowser_.SetPwd(initialDir);
    fileBrowser_.Open();
}

void TriangleApp::createDepthResources() {
    if (depthImageFormat_ == VK_FORMAT_UNDEFINED) {
        throw std::runtime_error("Depth image format not initialized");
    }

    if (!depthImageViews_.empty()) {
        for (auto view : depthImageViews_) {
            vkDestroyImageView(device_, view, nullptr);
        }
        depthImageViews_.clear();
    }
    if (!depthImages_.empty()) {
        for (size_t i = 0; i < depthImages_.size(); ++i) {
            if (depthImages_[i] != VK_NULL_HANDLE) {
                vkDestroyImage(device_, depthImages_[i], nullptr);
            }
            if (i < depthImageMemory_.size() && depthImageMemory_[i] != VK_NULL_HANDLE) {
                vkFreeMemory(device_, depthImageMemory_[i], nullptr);
            }
        }
        depthImages_.clear();
        depthImageMemory_.clear();
    }

    const size_t imageCount = swapchainImages_.size();
    depthImages_.resize(imageCount, VK_NULL_HANDLE);
    depthImageMemory_.resize(imageCount, VK_NULL_HANDLE);
    depthImageViews_.resize(imageCount, VK_NULL_HANDLE);
    depthImageLayouts_.assign(imageCount, VK_IMAGE_LAYOUT_UNDEFINED);

    for (size_t i = 0; i < imageCount; ++i) {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent.width = swapchainExtent_.width;
        imageInfo.extent.height = swapchainExtent_.height;
        imageInfo.extent.depth = 1;
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = depthImageFormat_;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateImage(device_, &imageInfo, nullptr, &depthImages_[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create depth image");
        }

        VkMemoryRequirements memRequirements;
        vkGetImageMemoryRequirements(device_, depthImages_[i], &memRequirements);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex =
            findMemoryType(memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (vkAllocateMemory(device_, &allocInfo, nullptr, &depthImageMemory_[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to allocate depth image memory");
        }

        if (vkBindImageMemory(device_, depthImages_[i], depthImageMemory_[i], 0) != VK_SUCCESS) {
            throw std::runtime_error("Failed to bind depth image memory");
        }

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = depthImages_[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = depthImageFormat_;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(device_, &viewInfo, nullptr, &depthImageViews_[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create depth image view");
        }
    }
}

void TriangleApp::createCommandBuffers() {
    if (!commandBuffers_.empty()) {
        vkFreeCommandBuffers(device_, commandPool_,
                             static_cast<uint32_t>(commandBuffers_.size()), commandBuffers_.data());
        commandBuffers_.clear();
    }

    commandBuffers_.resize(swapchainImages_.size());

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commandPool_;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = static_cast<uint32_t>(commandBuffers_.size());

    if (vkAllocateCommandBuffers(device_, &allocInfo, commandBuffers_.data()) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate command buffers");
    }
}

void TriangleApp::createSyncObjects() {
    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (int i = 0; i < kMaxFramesInFlight; ++i) {
        if (vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &imageAvailableSemaphores_[i]) != VK_SUCCESS ||
            vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &renderFinishedSemaphores_[i]) != VK_SUCCESS ||
            vkCreateFence(device_, &fenceInfo, nullptr, &inFlightFences_[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create synchronization objects");
        }
    }
}

void TriangleApp::updateUniformBuffer(uint32_t frameIndex) {
    if (!modelLoaded_) {
        return;
    }

    auto now = std::chrono::steady_clock::now();
    float elapsedSeconds = std::chrono::duration<float>(now - startTime_).count();

    UniformBufferObject ubo{};
    float angle = rotationSpeedRadiansPerSecond_ * elapsedSeconds;
    glm::mat4 model = glm::rotate(glm::mat4(1.0f), angle, glm::vec3(0.0f, 1.0f, 0.0f));
    model = glm::scale(model, glm::vec3(modelScale_));
    ubo.model = model;

    const float scaledRadius = modelRadius_ * modelScale_;
    glm::vec3 eyePosition(0.0f, scaledRadius * 0.35f, cameraDistance_);
    ubo.view = glm::lookAt(eyePosition, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));

    float aspectRatio = static_cast<float>(swapchainExtent_.width) /
                        static_cast<float>(swapchainExtent_.height);
    ubo.proj = glm::perspective(verticalFovRadians_, aspectRatio, 0.1f, 500.0f);
    ubo.proj[1][1] *= -1.0f;

    if (uniformBuffersMapped_[frameIndex] != nullptr) {
        std::memcpy(uniformBuffersMapped_[frameIndex], &ubo, sizeof(ubo));
    }
}

void TriangleApp::recordCommandBuffer(VkCommandBuffer commandBuffer,
                                      uint32_t imageIndex,
                                      uint32_t frameIndex) {
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = 0;
    beginInfo.pInheritanceInfo = nullptr;

    if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
        throw std::runtime_error("Failed to begin recording command buffer");
    }

    std::array<VkImageMemoryBarrier, 2> imageBarriers{};
    VkPipelineStageFlags srcStageMask = 0;
    const VkPipelineStageFlags dstStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;

    VkImageMemoryBarrier& colorBarrier = imageBarriers[0];
    colorBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    colorBarrier.oldLayout = swapchainImageLayouts_[imageIndex];
    colorBarrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    colorBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    colorBarrier.image = swapchainImages_[imageIndex];
    colorBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    colorBarrier.subresourceRange.baseMipLevel = 0;
    colorBarrier.subresourceRange.levelCount = 1;
    colorBarrier.subresourceRange.baseArrayLayer = 0;
    colorBarrier.subresourceRange.layerCount = 1;
    colorBarrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkPipelineStageFlags colorSrcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    if (colorBarrier.oldLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
        colorSrcStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        colorBarrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    } else {
        colorBarrier.srcAccessMask = 0;
    }

    srcStageMask |= colorSrcStage;

    size_t barrierCount = 1;
    if (imageIndex < depthImages_.size()) {
        VkImageMemoryBarrier& depthBarrier = imageBarriers[1];
        depthBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        depthBarrier.oldLayout = depthImageLayouts_[imageIndex];
        depthBarrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depthBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        depthBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        depthBarrier.image = depthImages_[imageIndex];
        depthBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        depthBarrier.subresourceRange.baseMipLevel = 0;
        depthBarrier.subresourceRange.levelCount = 1;
        depthBarrier.subresourceRange.baseArrayLayer = 0;
        depthBarrier.subresourceRange.layerCount = 1;
        depthBarrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        VkPipelineStageFlags depthSrcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        if (depthBarrier.oldLayout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL) {
            depthSrcStage = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            depthBarrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        } else {
            depthBarrier.srcAccessMask = 0;
        }

        srcStageMask |= depthSrcStage;
        depthImageLayouts_[imageIndex] = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        barrierCount = 2;
    }

    vkCmdPipelineBarrier(commandBuffer,
                         srcStageMask,
                         dstStageMask,
                         0,
                         0, nullptr,
                         0, nullptr,
                         static_cast<uint32_t>(barrierCount),
                         imageBarriers.data());

    VkClearValue clearColor{};
    clearColor.color = {{0.05f, 0.05f, 0.08f, 1.0f}};
    VkClearValue depthClear{};
    depthClear.depthStencil = {1.0f, 0};

    VkRenderingAttachmentInfo colorAttachment{};
    colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttachment.imageView = swapchainImageViews_[imageIndex];
    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.clearValue = clearColor;

    VkRenderingAttachmentInfo depthAttachment{};
    depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depthAttachment.imageView = depthImageViews_[imageIndex];
    depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.clearValue = depthClear;

    VkRenderingInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    renderingInfo.renderArea.offset = {0, 0};
    renderingInfo.renderArea.extent = swapchainExtent_;
    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachments = &colorAttachment;
    renderingInfo.pDepthAttachment = &depthAttachment;

    vkCmdBeginRendering(commandBuffer, &renderingInfo);

    const float targetAspect = static_cast<float>(kWidth) / static_cast<float>(kHeight);
    const float extentWidth = static_cast<float>(swapchainExtent_.width);
    const float extentHeight = static_cast<float>(swapchainExtent_.height);
    const float currentAspect = extentWidth / extentHeight;

    float viewportWidth = extentWidth;
    float viewportHeight = extentHeight;
    float viewportOffsetX = 0.0f;
    float viewportOffsetY = 0.0f;

    if (currentAspect > targetAspect) {
        viewportHeight = extentHeight;
        viewportWidth = viewportHeight * targetAspect;
        viewportOffsetX = (extentWidth - viewportWidth) * 0.5f;
    } else if (currentAspect < targetAspect) {
        viewportWidth = extentWidth;
        viewportHeight = viewportWidth / targetAspect;
        viewportOffsetY = (extentHeight - viewportHeight) * 0.5f;
    }

    VkViewport viewport{};
    viewport.x = viewportOffsetX;
    viewport.y = viewportOffsetY;
    viewport.width = viewportWidth;
    viewport.height = viewportHeight;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

    const int32_t scissorX = std::clamp(static_cast<int32_t>(std::floor(viewportOffsetX)), 0,
                                        static_cast<int32_t>(swapchainExtent_.width));
    const int32_t scissorY = std::clamp(static_cast<int32_t>(std::floor(viewportOffsetY)), 0,
                                        static_cast<int32_t>(swapchainExtent_.height));
    const int32_t scissorRight = std::clamp(static_cast<int32_t>(std::ceil(viewportOffsetX + viewportWidth)), scissorX,
                                            static_cast<int32_t>(swapchainExtent_.width));
    const int32_t scissorBottom = std::clamp(static_cast<int32_t>(std::ceil(viewportOffsetY + viewportHeight)), scissorY,
                                             static_cast<int32_t>(swapchainExtent_.height));

    VkRect2D scissor{};
    scissor.offset = {scissorX, scissorY};
    scissor.extent = {
        static_cast<uint32_t>(std::max(scissorRight - scissorX, 1)),
        static_cast<uint32_t>(std::max(scissorBottom - scissorY, 1)),
    };
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline_);

    if (modelLoaded_) {
        VkBuffer vertexBuffers[] = {vertexBuffer_};
        VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
        vkCmdBindIndexBuffer(commandBuffer, indexBuffer_, 0, VK_INDEX_TYPE_UINT32);
        vkCmdBindDescriptorSets(commandBuffer,
                                VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelineLayout_,
                                0,
                                1,
                                &descriptorSets_[frameIndex],
                                0,
                                nullptr);

        vkCmdDrawIndexed(commandBuffer, static_cast<uint32_t>(indices_.size()), 1, 0, 0, 0);
    }
    if (imguiInitialized_ && currentImGuiDrawData_ != nullptr &&
        currentImGuiDrawData_->CmdListsCount > 0) {
        ImGui_ImplVulkan_RenderDrawData(currentImGuiDrawData_, commandBuffer);
    }

    vkCmdEndRendering(commandBuffer);

    VkImageMemoryBarrier presentBarrier{};
    presentBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    presentBarrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    presentBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    presentBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    presentBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    presentBarrier.image = swapchainImages_[imageIndex];
    presentBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    presentBarrier.subresourceRange.baseMipLevel = 0;
    presentBarrier.subresourceRange.levelCount = 1;
    presentBarrier.subresourceRange.baseArrayLayer = 0;
    presentBarrier.subresourceRange.layerCount = 1;
    presentBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    presentBarrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;

    vkCmdPipelineBarrier(commandBuffer,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0,
                         0, nullptr,
                         0, nullptr,
                         1, &presentBarrier);

    if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
        throw std::runtime_error("Failed to record command buffer");
    }

    currentImGuiDrawData_ = nullptr;
    swapchainImageLayouts_[imageIndex] = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
}

void TriangleApp::drawFrame() {
    while (true) {
        int fbWidth = 0;
        int fbHeight = 0;
        glfwGetFramebufferSize(window_, &fbWidth, &fbHeight);
        if (fbWidth > 0 && fbHeight > 0 &&
            (static_cast<uint32_t>(fbWidth) != swapchainExtent_.width ||
             static_cast<uint32_t>(fbHeight) != swapchainExtent_.height)) {
            framebufferResized_ = true;
            swapchainOutOfDate_ = true;
        }

        if (framebufferResized_ || swapchainOutOfDate_) {
            framebufferResized_ = false;
            swapchainOutOfDate_ = false;
            recreateSwapChain();
            continue;
        }

        VkFence fence = inFlightFences_[currentFrame_];
        VkResult fenceResult = vkWaitForFences(device_, 1, &fence, VK_TRUE, kFenceTimeoutNs);
        if (fenceResult != VK_SUCCESS) {
            throw std::runtime_error("Failed to wait for in-flight fence");
        }

        uint32_t imageIndex = 0;
        VkResult result =
            vkAcquireNextImageKHR(device_, swapchain_, kFenceTimeoutNs,
                                  imageAvailableSemaphores_[currentFrame_], VK_NULL_HANDLE, &imageIndex);

        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            framebufferResized_ = true;
            continue;
        }
        if (result == VK_SUBOPTIMAL_KHR) {
            swapchainOutOfDate_ = true;
        } else if (result != VK_SUCCESS) {
            throw std::runtime_error("Failed to acquire swap chain image");
        }

        if (!imagesInFlight_.empty()) {
            VkFence& imageFence = imagesInFlight_.at(imageIndex);
            if (imageFence != VK_NULL_HANDLE && imageFence != inFlightFences_[currentFrame_]) {
                VkResult imageFenceResult =
                    vkWaitForFences(device_, 1, &imageFence, VK_TRUE, kFenceTimeoutNs);
                if (imageFenceResult != VK_SUCCESS) {
                    throw std::runtime_error("Failed to wait for previous usage of swap chain image");
                }
            }
            imageFence = inFlightFences_[currentFrame_];
        }

        vkResetFences(device_, 1, &inFlightFences_[currentFrame_]);

        updateUniformBuffer(static_cast<uint32_t>(currentFrame_));
        buildImGuiFrame();

        vkResetCommandBuffer(commandBuffers_[imageIndex], 0);
        recordCommandBuffer(commandBuffers_[imageIndex], imageIndex, static_cast<uint32_t>(currentFrame_));

        VkPipelineStageFlags waitStages[] = {
            static_cast<VkPipelineStageFlags>(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT)};

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = &imageAvailableSemaphores_[currentFrame_];
        submitInfo.pWaitDstStageMask = waitStages;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffers_[imageIndex];
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &renderFinishedSemaphores_[currentFrame_];

        if (vkQueueSubmit(graphicsQueue_, 1, &submitInfo, inFlightFences_[currentFrame_]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to submit draw command buffer");
        }

        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &renderFinishedSemaphores_[currentFrame_];
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &swapchain_;
        presentInfo.pImageIndices = &imageIndex;

        result = vkQueuePresentKHR(presentQueue_, &presentInfo);

        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            framebufferResized_ = true;
            continue;
        }
        if (result == VK_SUBOPTIMAL_KHR) {
            swapchainOutOfDate_ = true;
        } else if (result != VK_SUCCESS) {
            throw std::runtime_error("Failed to present swap chain image");
        }

        currentFrame_ = (currentFrame_ + 1) % kMaxFramesInFlight;
        break;
    }
}

bool TriangleApp::isDeviceSuitable(VkPhysicalDevice device) {
    QueueFamilyIndices indices = findQueueFamilies(device);

    bool extensionsSupported = checkDeviceExtensionSupport(device);

    bool swapChainAdequate = false;
    if (extensionsSupported) {
        SwapChainSupportDetails swapChainSupport = querySwapChainSupport(device);
        swapChainAdequate = !swapChainSupport.formats.empty() &&
                            !swapChainSupport.presentModes.empty();
    }

    VkPhysicalDeviceDynamicRenderingFeatures dynamicRenderingFeatures{};
    dynamicRenderingFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;

    VkPhysicalDeviceFeatures2 deviceFeatures2{};
    deviceFeatures2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    deviceFeatures2.pNext = &dynamicRenderingFeatures;

    vkGetPhysicalDeviceFeatures2(device, &deviceFeatures2);

    bool supportsDynamicRendering = dynamicRenderingFeatures.dynamicRendering == VK_TRUE;

    return indices.isComplete() && extensionsSupported && swapChainAdequate && supportsDynamicRendering;
}

QueueFamilyIndices TriangleApp::findQueueFamilies(VkPhysicalDevice device) {
    QueueFamilyIndices indices;

    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);

    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

    int i = 0;
    for (const auto& queueFamily : queueFamilies) {
        if (queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            indices.graphicsFamily = i;
        }

        VkBool32 presentSupport = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface_, &presentSupport);

        if (presentSupport) {
            indices.presentFamily = i;
        }

        if (indices.isComplete()) {
            break;
        }

        ++i;
    }

    return indices;
}

bool TriangleApp::checkDeviceExtensionSupport(VkPhysicalDevice device) {
    uint32_t extensionCount = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);

    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, availableExtensions.data());

    std::set<std::string> requiredExtensions(kDeviceExtensions.begin(), kDeviceExtensions.end());

    for (const auto& extension : availableExtensions) {
        requiredExtensions.erase(extension.extensionName);
    }

#ifdef __APPLE__
    requiredExtensions.erase("VK_KHR_portability_subset");
#endif

    return requiredExtensions.empty();
}

SwapChainSupportDetails TriangleApp::querySwapChainSupport(VkPhysicalDevice device) {
    SwapChainSupportDetails details;

    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface_, &details.capabilities);

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface_, &formatCount, nullptr);
    if (formatCount != 0) {
        details.formats.resize(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface_, &formatCount, details.formats.data());
    }

    uint32_t presentModeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface_, &presentModeCount, nullptr);
    if (presentModeCount != 0) {
        details.presentModes.resize(presentModeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface_, &presentModeCount,
                                                  details.presentModes.data());
    }

    return details;
}

VkSurfaceFormatKHR TriangleApp::chooseSwapSurfaceFormat(
    const std::vector<VkSurfaceFormatKHR>& availableFormats) {
    for (const auto& availableFormat : availableFormats) {
        if (availableFormat.format == VK_FORMAT_B8G8R8A8_SRGB &&
            availableFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return availableFormat;
        }
    }
    return availableFormats.front();
}

VkPresentModeKHR TriangleApp::chooseSwapPresentMode(
    const std::vector<VkPresentModeKHR>& availablePresentModes) {
    for (const auto& availablePresentMode : availablePresentModes) {
        if (availablePresentMode == VK_PRESENT_MODE_MAILBOX_KHR) {
            return availablePresentMode;
        }
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D TriangleApp::chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities) {
    if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
        return capabilities.currentExtent;
    }

    VkExtent2D actualExtent = {kWidth, kHeight};

    actualExtent.width =
        std::clamp(actualExtent.width, capabilities.minImageExtent.width,
                   capabilities.maxImageExtent.width);
    actualExtent.height =
        std::clamp(actualExtent.height, capabilities.minImageExtent.height,
                   capabilities.maxImageExtent.height);

    return actualExtent;
}

VkShaderModule TriangleApp::createShaderModule(const std::vector<char>& code) {
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = code.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule shaderModule;
    if (vkCreateShaderModule(device_, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create shader module");
    }
    return shaderModule;
}

uint32_t TriangleApp::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memProperties);

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; ++i) {
        if ((typeFilter & (1 << i)) &&
            (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    throw std::runtime_error("Failed to find suitable memory type");
}

VkFormat TriangleApp::findSupportedFormat(const std::vector<VkFormat>& candidates,
                                          VkImageTiling tiling,
                                          VkFormatFeatureFlags features) {
    for (VkFormat format : candidates) {
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(physicalDevice_, format, &props);

        if (tiling == VK_IMAGE_TILING_LINEAR &&
            (props.linearTilingFeatures & features) == features) {
            return format;
        }
        if (tiling == VK_IMAGE_TILING_OPTIMAL &&
            (props.optimalTilingFeatures & features) == features) {
            return format;
        }
    }

    throw std::runtime_error("Failed to find supported format");
}

VkFormat TriangleApp::findDepthFormat() {
    return findSupportedFormat(
        {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT},
        VK_IMAGE_TILING_OPTIMAL,
        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
}

void TriangleApp::createBuffer(VkDeviceSize size,
                               VkBufferUsageFlags usage,
                               VkMemoryPropertyFlags properties,
                               VkBuffer& buffer,
                               VkDeviceMemory& bufferMemory) {
    if (buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
    }
    if (bufferMemory != VK_NULL_HANDLE) {
        vkFreeMemory(device_, bufferMemory, nullptr);
        bufferMemory = VK_NULL_HANDLE;
    }

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device_, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create buffer");
    }

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(device_, buffer, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, properties);

    if (vkAllocateMemory(device_, &allocInfo, nullptr, &bufferMemory) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate buffer memory");
    }

    if (vkBindBufferMemory(device_, buffer, bufferMemory, 0) != VK_SUCCESS) {
        throw std::runtime_error("Failed to bind buffer memory");
    }
}

VkCommandBuffer TriangleApp::beginSingleTimeCommands() {
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = commandPool_;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(device_, &allocInfo, &commandBuffer) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate command buffer");
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
        throw std::runtime_error("Failed to begin command buffer");
    }

    return commandBuffer;
}

void TriangleApp::endSingleTimeCommands(VkCommandBuffer commandBuffer) {
    if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
        throw std::runtime_error("Failed to end command buffer");
    }

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    if (vkQueueSubmit(graphicsQueue_, 1, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS) {
        throw std::runtime_error("Failed to submit command buffer");
    }
    vkQueueWaitIdle(graphicsQueue_);

    vkFreeCommandBuffers(device_, commandPool_, 1, &commandBuffer);
}

void TriangleApp::copyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size) {
    VkCommandBuffer commandBuffer = beginSingleTimeCommands();

    VkBufferCopy copyRegion{};
    copyRegion.srcOffset = 0;
    copyRegion.dstOffset = 0;
    copyRegion.size = size;
    vkCmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, 1, &copyRegion);

    endSingleTimeCommands(commandBuffer);
}

std::vector<const char*> TriangleApp::getRequiredExtensions() {
    uint32_t glfwExtensionCount = 0;
    const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
    if (!glfwExtensions) {
        throw std::runtime_error("Failed to get required GLFW extensions");
    }

    std::vector<const char*> extensions(glfwExtensions, glfwExtensions + glfwExtensionCount);

    if (kEnableValidationLayers) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

#ifdef __APPLE__
#ifdef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
        extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
#else
        extensions.push_back("VK_KHR_portability_enumeration");
#endif
#endif

    return extensions;
}

bool TriangleApp::checkValidationLayerSupport() {
    uint32_t layerCount;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);

    std::vector<VkLayerProperties> availableLayers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

    for (const char* layerName : kValidationLayers) {
        bool layerFound = false;
        for (const auto& layerProperties : availableLayers) {
            if (std::strcmp(layerName, layerProperties.layerName) == 0) {
                layerFound = true;
                break;
            }
        }
        if (!layerFound) {
            return false;
        }
    }

    return true;
}

void TriangleApp::framebufferResizeCallback(GLFWwindow* window, int, int) {
    auto app = reinterpret_cast<TriangleApp*>(glfwGetWindowUserPointer(window));
    app->framebufferResized_ = true;
    app->swapchainOutOfDate_ = true;
}

void TriangleApp::windowResizeCallback(GLFWwindow* window, int, int) {
    auto app = reinterpret_cast<TriangleApp*>(glfwGetWindowUserPointer(window));
    if (app) {
        app->framebufferResized_ = true;
        app->swapchainOutOfDate_ = true;
    }
}

int main() {
    TriangleApp app;

    try {
        app.run();
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
