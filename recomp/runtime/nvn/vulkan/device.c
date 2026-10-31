// Vulkan device, window, swapchain and presentation (counterpart of the window/present half of metal.m).
#include <SDL3/SDL.h>
#include "../../game.h"
#include <SDL3/SDL_vulkan.h>
#include <math.h>
#include <pthread.h>
#ifdef __ANDROID__
#include <android/native_window.h>
#endif
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../paths.h"
#include "../../platform.h"
#include "../../settings.h"
#include "../../ui/menu.h"
#include "vk.h"
#include "../output_scale.h"
#include "../framegen.h"
#include "../renderer.h"

VkG g_vk;
#define X(n) PFN_vk##n vkl_##n;
VK_LATE_FUNCS(X)
#undef X
static SDL_Window *window;
static atomic_bool ui_ready;
static VkDebugUtilsMessengerEXT messenger;

// ================================================================== instance / device
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT type,
                                               const VkDebugUtilsMessengerCallbackDataEXT *d, void *user) {
    (void)type, (void)user;
    static int n;
    if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT && n++ < 500) LOG("[vk] %s\n", d->pMessage);
    return VK_FALSE;
}

// LARECOMP_VK_VALIDATION=1: Khronos validation layer (MSYS2 package vulkan-validation-layers)
static void create_instance(void) {
    Uint32 n_sdl = 0;
    const char *const *sdl_ext = SDL_Vulkan_GetInstanceExtensions(&n_sdl);
    const char *ext[16];
    uint32_t n_ext = 0;
    for (Uint32 i = 0; i < n_sdl && n_ext < 14; i++) ext[n_ext++] = sdl_ext[i];
    bool validate = getenv("LARECOMP_VK_VALIDATION") != NULL;
    if (validate) ext[n_ext++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "LARecomp",
                             .pEngineName = "LARecomp", .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                               .enabledExtensionCount = n_ext, .ppEnabledExtensionNames = ext,
                               .enabledLayerCount = validate ? 1 : 0, .ppEnabledLayerNames = layers};
    VkResult r = vkCreateInstance(&ci, NULL, &g_vk.inst);
    if (r != VK_SUCCESS && validate) {
        LOG("[vk] validation layer unavailable (%d); continuing without\n", r);
        ci.enabledLayerCount = 0;
        ci.enabledExtensionCount--;
        validate = false;
        r = vkCreateInstance(&ci, NULL, &g_vk.inst);
    }
    if (r != VK_SUCCESS) FATAL("vkCreateInstance: %d (is a Vulkan 1.3 driver installed?)\n", r);
    if (validate) {
        PFN_vkCreateDebugUtilsMessengerEXT f =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_vk.inst, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mi = {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = debug_cb};
        if (f) f(g_vk.inst, &mi, NULL, &messenger);
    }
}

static bool has_ext(VkExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return true;
    return false;
}

// Discrete GPU first (LARECOMP_GPU=<index> picks one), needs Vulkan 1.3 and a queue that can present.
static void pick_device(void) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g_vk.inst, &n, NULL);
    if (!n) FATAL("no Vulkan device\n");
    VkPhysicalDevice *pd = calloc(n, sizeof *pd);
    vkEnumeratePhysicalDevices(g_vk.inst, &n, pd);
    int want = getenv("LARECOMP_GPU") ? atoi(getenv("LARECOMP_GPU")) : -1, best = -1, best_score = -1;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd[i], &p);
        LOG("[vk] gpu %u: %s (api %u.%u.%u)\n", i, p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
            VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion));
        if (p.apiVersion < VK_API_VERSION_1_3) continue;
        int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if ((int)i == want) score = 100;
        if (score > best_score) { best_score = score; best = (int)i; }
    }
    if (best < 0) FATAL("no Vulkan 1.3 device\n");
    g_vk.phys = pd[best];
    free(pd);
    vkGetPhysicalDeviceProperties(g_vk.phys, &g_vk.props);
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &nq, NULL);
    VkQueueFamilyProperties *qp = calloc(nq, sizeof *qp);
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &nq, qp);
    g_vk.qfam = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(g_vk.phys, i, g_vk.surface, &present);
        if ((qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && present) { g_vk.qfam = i; break; }
    }
    free(qp);
    if (g_vk.qfam == UINT32_MAX) FATAL("no graphics+present queue\n");
    LOG("[vk] using %s\n", g_vk.props.deviceName);
}

static bool g_fault_ext;
// After VK_ERROR_DEVICE_LOST: what the driver says faulted (VK_EXT_device_fault), into the log.
void vk_device_fault_report(void) {
    static int once;
    if (once++ || !g_fault_ext) return;
    PFN_vkGetDeviceFaultInfoEXT get = (PFN_vkGetDeviceFaultInfoEXT)vkGetDeviceProcAddr(g_vk.dev, "vkGetDeviceFaultInfoEXT");
    if (!get) return;
    VkDeviceFaultCountsEXT c = {.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    if (get(g_vk.dev, &c, NULL) != VK_SUCCESS) { LOG("[vk] device fault: no info\n"); return; }
    VkDeviceFaultAddressInfoEXT addr[16];
    VkDeviceFaultVendorInfoEXT vend[16];
    if (c.addressInfoCount > 16) c.addressInfoCount = 16;
    if (c.vendorInfoCount > 16) c.vendorInfoCount = 16;
    c.vendorBinarySize = 0;
    VkDeviceFaultInfoEXT info = {.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT, .pAddressInfos = addr, .pVendorInfos = vend};
    if (get(g_vk.dev, &c, &info) != VK_SUCCESS && 0) return;
    LOG("[vk] device fault: %s\n", info.description);
    static const char *const kind[] = {"none", "read invalid", "write invalid", "execute invalid", "ip unknown", "ip invalid", "ip valid"};
    for (uint32_t i = 0; i < c.addressInfoCount; i++)
        LOG("[vk]   address %s %#llx (precision %#llx)\n", addr[i].addressType < 7 ? kind[addr[i].addressType] : "?",
            (unsigned long long)addr[i].reportedAddress, (unsigned long long)addr[i].addressPrecision);
    for (uint32_t i = 0; i < c.vendorInfoCount; i++)
        LOG("[vk]   vendor: %s (code %#llx data %#llx)\n", vend[i].description, (unsigned long long)vend[i].vendorFaultCode,
            (unsigned long long)vend[i].vendorFaultData);
}

static void create_device(void) {
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne, sizeof *ep);
    vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &ne, ep);
    const char *ext[8];
    uint32_t n_ext = 0;
    ext[n_ext++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    g_vk.custom_border = has_ext(ep, ne, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
    if (g_vk.custom_border) ext[n_ext++] = VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME;
    g_vk.divisor = has_ext(ep, ne, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
    if (g_vk.divisor) ext[n_ext++] = VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME;
    g_vk.host_import = has_ext(ep, ne, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);  // guest heap as a GPU buffer (exec.c)
    if (g_vk.host_import) ext[n_ext++] = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
    bool fault = has_ext(ep, ne, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);  // device-lost diagnostics (vk_device_fault_report)
    if (fault) ext[n_ext++] = VK_EXT_DEVICE_FAULT_EXTENSION_NAME;
    g_fault_ext = fault;
    // LARECOMP_VK_ROBUST=1: out-of-range image reads and null descriptors made harmless too (robustImageAccess,
    // VK_EXT_robustness2), not only buffer reads. A test for the RTX 50-series device loss in 3D World courses (#30,
    // #37: the driver reports an invalid read); it costs a little speed, so not the default until it's shown to help.
    bool robust = getenv("LARECOMP_VK_ROBUST") && atoi(getenv("LARECOMP_VK_ROBUST")) != 0;
    bool rob2 = robust && has_ext(ep, ne, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
    if (rob2) ext[n_ext++] = VK_EXT_ROBUSTNESS_2_EXTENSION_NAME;
    free(ep);

    // features the backend relies on (all present on current desktop drivers)
    VkPhysicalDeviceRobustness2FeaturesEXT have_r2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    VkPhysicalDeviceVulkan13Features have13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                               .pNext = rob2 ? &have_r2 : NULL};
    VkPhysicalDeviceVulkan12Features have12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &have13};
    VkPhysicalDeviceFeatures2 have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &have12};
    vkGetPhysicalDeviceFeatures2(g_vk.phys, &have);
    // Shader global memory (exec.c gmem_desc, translate.c): a storage buffer binding reaches only maxStorageBufferRange
    // of the 3.2 GB heap (128 MB on lavapipe and Adreno, 1-2 GB on some desktop GPUs), so the heap's import is reached
    // through its device address instead (bufferDeviceAddress: part of Vulkan 1.3). Without host memory import there is
    // no heap on the GPU at all: draw stages then read per-draw copies of their storage buffers (exec.c gfx_gmap).
    // LARECOMP_VK_NO_BDA=1: the old storage buffer binding; LARECOMP_VK_NO_GMEM=1: no import (the per-draw copies).
    // The import lands in host-visible memory. Where the largest heap holding host-visible memory is device-local (an
    // integrated GPU sharing one heap with the system, e.g. Intel UHD / HD with 8 GB of RAM: ~4 GB), the 3.2 GB import
    // takes most of the budget textures and render targets need, and image creation then fails with
    // VK_ERROR_OUT_OF_DEVICE_MEMORY (#57). There the per-draw copies are used. Discrete GPUs (and the Steam Deck) keep
    // the import: it sits in their separate system memory heap, not in the GPU's own memory.
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &mem_props);
    int host_heap = -1;
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        uint32_t h = mem_props.memoryTypes[i].heapIndex;
        if ((mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            (host_heap < 0 || mem_props.memoryHeaps[h].size > mem_props.memoryHeaps[host_heap].size))
            host_heap = (int)h;
    }
    bool low_vram = host_heap >= 0 && (mem_props.memoryHeaps[host_heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) &&
                    mem_props.memoryHeaps[host_heap].size < HEAP_MAX + (3ull << 30);
    if (low_vram && g_vk.host_import)
        LOG("[vk] shared GPU memory is %llu MB: the guest heap is not imported (it would leave too little for textures)\n",
            (unsigned long long)(mem_props.memoryHeaps[host_heap].size >> 20));
    bool no_gmem = !g_vk.host_import || low_vram || getenv("LARECOMP_VK_NO_GMEM");
    g_vk.bda = have12.bufferDeviceAddress && have.features.shaderInt64 && !no_gmem && !getenv("LARECOMP_VK_NO_BDA");
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT div = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT,
        .vertexAttributeInstanceRateDivisor = VK_TRUE, .vertexAttributeInstanceRateZeroDivisor = VK_FALSE};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT cbc = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT, .customBorderColors = VK_TRUE,
        .customBorderColorWithoutFormat = VK_TRUE, .pNext = g_vk.divisor ? &div : NULL};
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                            .dynamicRendering = VK_TRUE, .synchronization2 = VK_TRUE,
                                            .shaderDemoteToHelperInvocation = VK_TRUE, .shaderTerminateInvocation = VK_TRUE,
                                            .maintenance4 = VK_TRUE, .robustImageAccess = robust && have13.robustImageAccess,
                                            .pNext = g_vk.custom_border ? (void *)&cbc : g_vk.divisor ? (void *)&div : NULL};
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                            .timelineSemaphore = VK_TRUE, .shaderInt8 = VK_TRUE, .samplerMirrorClampToEdge = VK_TRUE,
                                            .uniformAndStorageBuffer8BitAccess = VK_TRUE, .storageBuffer8BitAccess = VK_TRUE,
                                            .scalarBlockLayout = VK_TRUE, .bufferDeviceAddress = g_vk.bda, .pNext = &f13};
    VkPhysicalDeviceVulkan11Features f11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
                                            .storageBuffer16BitAccess = VK_TRUE, .pNext = &f12};
    VkPhysicalDeviceFaultFeaturesEXT ff = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT, .deviceFault = VK_TRUE,
                                           .pNext = &f11};
    VkPhysicalDeviceRobustness2FeaturesEXT r2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT,
                                                 .robustBufferAccess2 = have_r2.robustBufferAccess2 && have.features.robustBufferAccess,
                                                 .robustImageAccess2 = have_r2.robustImageAccess2,
                                                 .nullDescriptor = have_r2.nullDescriptor,
                                                 .pNext = fault ? (void *)&ff : (void *)&f11};
    VkPhysicalDeviceFeatures2 f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                   .pNext = rob2 ? (void *)&r2 : fault ? (void *)&ff : (void *)&f11};
    if (robust)
        LOG("[vk] robust access: images %d, robustness2 buffers %d images %d null descriptors %d\n",
            (int)(have13.robustImageAccess != 0), (int)r2.robustBufferAccess2, (int)r2.robustImageAccess2,
            (int)r2.nullDescriptor);
    VkPhysicalDeviceFeatures *b = &f.features, *h = &have.features;
    b->independentBlend = h->independentBlend;
    b->dualSrcBlend = h->dualSrcBlend;
    b->fillModeNonSolid = h->fillModeNonSolid;
    b->samplerAnisotropy = h->samplerAnisotropy;
    b->textureCompressionBC = h->textureCompressionBC;
    b->shaderInt64 = h->shaderInt64;
    b->depthBiasClamp = h->depthBiasClamp;
    b->depthClamp = h->depthClamp;
    b->largePoints = h->largePoints;
    b->wideLines = h->wideLines;
    b->imageCubeArray = h->imageCubeArray;
    b->geometryShader = h->geometryShader;  // (NVN geometry stages: SMM2's 2D-style terrain; exec.c)
    g_vk.geometry = h->geometryShader;
    b->sampleRateShading = h->sampleRateShading;
    b->fragmentStoresAndAtomics = h->fragmentStoresAndAtomics;
    b->vertexPipelineStoresAndAtomics = h->vertexPipelineStoresAndAtomics;
    b->robustBufferAccess = h->robustBufferAccess;  // guest shaders index cbufs freely: keep OOB reads harmless
    b->shaderClipDistance = h->shaderClipDistance;
    if (!h->shaderInt64) LOG("[vk] warning: no shaderInt64 (64-bit address math in shaders)\n");
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = g_vk.qfam,
                                  .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f, .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &qi, .enabledExtensionCount = n_ext, .ppEnabledExtensionNames = ext};
    VK_CHECK(vkCreateDevice(g_vk.phys, &di, NULL, &g_vk.dev));
    maxwell_glsl_bda = g_vk.bda;
    maxwell_glsl_gmap_gfx = no_gmem;
    LOG("[vk] shader global memory: %s\n", g_vk.bda ? "the heap through its device address" :
        no_gmem ? "per-draw copies of the storage buffers (no host memory import)" : "the heap as a storage buffer");
    vkGetDeviceQueue(g_vk.dev, g_vk.qfam, 0, &g_vk.queue);
#define X(n) if (!(vkl_##n = (PFN_vk##n)vkGetDeviceProcAddr(g_vk.dev, "vk" #n))) \
        FATAL("the graphics driver lacks vk" #n " (is a Vulkan 1.3 driver installed?)\n");
    VK_LATE_FUNCS(X)
#undef X

    // (VMA fetches its functions too: its Vulkan 1.3 ones are not in an old loader either)
    VmaVulkanFunctions vf = {.vkGetInstanceProcAddr = vkGetInstanceProcAddr, .vkGetDeviceProcAddr = vkGetDeviceProcAddr};
    VmaAllocatorCreateInfo ai = {.physicalDevice = g_vk.phys, .device = g_vk.dev, .instance = g_vk.inst,
                                 .vulkanApiVersion = VK_API_VERSION_1_3, .pVulkanFunctions = &vf};
    VK_CHECK(vmaCreateAllocator(&ai, &g_vk.vma));

    VkSemaphoreTypeCreateInfo st = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                    .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 0};
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &st};
    VK_CHECK(vkCreateSemaphore(g_vk.dev, &si, NULL, &g_vk.timeline));
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                  .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = g_vk.qfam};
    VK_CHECK(vkCreateCommandPool(g_vk.dev, &pi, NULL, &g_vk.pool));

    // pipeline cache: cache/vk_pipelines.bin (written by vk_pipeline_cache_save)
    size_t sz = 0;
    void *data = NULL;
    FILE *fp = fopen("cache/vk_pipelines.bin", "rb");
    if (fp) {
        fseek(fp, 0, SEEK_END);
        sz = (size_t)ftell(fp);
        fseek(fp, 0, SEEK_SET);
        data = malloc(sz);
        if (fread(data, 1, sz, fp) != sz) sz = 0;
        fclose(fp);
    }
    VkPipelineCacheCreateInfo pc = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = sz,
                                    .pInitialData = data};
    if (vkCreatePipelineCache(g_vk.dev, &pc, NULL, &g_vk.pcache) != VK_SUCCESS) {
        pc.initialDataSize = 0;
        VK_CHECK(vkCreatePipelineCache(g_vk.dev, &pc, NULL, &g_vk.pcache));
    }
    free(data);
}

void vk_pipeline_cache_save(void) {
    size_t sz = 0;
    if (vkGetPipelineCacheData(g_vk.dev, g_vk.pcache, &sz, NULL) != VK_SUCCESS || !sz) return;
    void *data = malloc(sz);
    if (vkGetPipelineCacheData(g_vk.dev, g_vk.pcache, &sz, data) == VK_SUCCESS) {
        mkdir("cache", 0755);
        FILE *f = fopen("cache/vk_pipelines.bin.tmp", "wb");
        if (f) {
            bool ok = fwrite(data, 1, sz, f) == sz;
            fclose(f);
            if (ok) { remove("cache/vk_pipelines.bin"); rename("cache/vk_pipelines.bin.tmp", "cache/vk_pipelines.bin"); }
        }
    }
    free(data);
}

// ================================================================== swapchain
static VkPresentModeKHR present_mode(void) {
    // MAILBOX by default: the game paces itself (vi.c's emulated 60 Hz vsync), and with FIFO every submission
    // queued behind the one waiting for a window image, so the game's fence waits followed the presentation engine:
    // a throttled / captured / busy compositor slowed the whole game (4-21 fps with the GPU mostly idle; mailbox:
    // 60). No tearing. LARECOMP_VK_PRESENT=fifo|immediate|mailbox to choose.
    const char *e = getenv("LARECOMP_VK_PRESENT");
    VkPresentModeKHR want = !e || !strcmp(e, "mailbox") ? VK_PRESENT_MODE_MAILBOX_KHR
                          : !strcmp(e, "immediate") ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR;
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &n, NULL);
    VkPresentModeKHR m[16];
    if (n > 16) n = 16;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &n, m);
    for (uint32_t i = 0; i < n; i++) if (m[i] == want) return want;
    return VK_PRESENT_MODE_FIFO_KHR;
}

static void surface_lost(void);
static void create_swapchain(void) {
    VkSurfaceCapabilitiesKHR caps;
    VkResult cr = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_vk.phys, g_vk.surface, &caps);
    if (cr == VK_ERROR_SURFACE_LOST_KHR) { surface_lost(); return; }  // (Android, in the background: vk_frame_begin)
    VK_CHECK(cr);
    int w, h;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    VkExtent2D ext = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : (VkExtent2D){(uint32_t)w, (uint32_t)h};
    VkSurfaceTransformFlagBitsKHR xform = caps.currentTransform;
#ifdef __ANDROID__
    // A phone held in landscape reports a 90 / 270 degree transform (its display is portrait) with a landscape size:
    // the image is made upright at that size and the compositor turns it (no pre-rotation).
    static int logged;
    if (!logged++) LOG("[vk] surface %ux%u, transform %#x, window %dx%d\n", caps.currentExtent.width, caps.currentExtent.height,
                       caps.currentTransform, w, h);
    if (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) xform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
#endif
    if (!ext.width || !ext.height) return;  // minimised
    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &nf, NULL);
    VkSurfaceFormatKHR fmts[64];
    if (nf > 64) nf = 64;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &nf, fmts);
    // scanout shows the bytes the game wrote (see the raw views in the present pass): a UNORM swapchain like Metal's
    VkSurfaceFormatKHR sf = fmts[0];
    for (uint32_t i = 0; i < nf; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { sf = fmts[i]; break; }
    if (sf.format != VK_FORMAT_B8G8R8A8_UNORM)  // (Android offers RGBA: still UNORM, not an sRGB format that would re-encode)
        for (uint32_t i = 0; i < nf; i++)
            if (fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { sf = fmts[i]; break; }
    uint32_t count = caps.minImageCount + 1;
    if (count < 3) count = 3;  // (frame generation: two presents per real frame)
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    if (count > 8) count = 8;
    VkSwapchainKHR old = g_vk.swapchain;
    VkSwapchainCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = g_vk.surface, .minImageCount = count,
        .imageFormat = sf.format, .imageColorSpace = sf.colorSpace, .imageExtent = ext, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = xform,
        .compositeAlpha = caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                                                                                          : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        .presentMode = present_mode(), .clipped = VK_TRUE,
        .oldSwapchain = old};
    cr = vkCreateSwapchainKHR(g_vk.dev, &ci, NULL, &g_vk.swapchain);
    if (cr == VK_ERROR_SURFACE_LOST_KHR || cr == VK_ERROR_NATIVE_WINDOW_IN_USE_KHR) {
        g_vk.swapchain = old;
        surface_lost();
        return;
    }
    VK_CHECK(cr);
    for (uint32_t i = 0; i < g_vk.sc_count; i++) vkDestroyImageView(g_vk.dev, g_vk.sc_views[i], NULL);
    if (old) vkDestroySwapchainKHR(g_vk.dev, old, NULL);
    g_vk.sc_format = sf.format;
    g_vk.sc_extent = ext;
    g_vk.sc_count = 8;
    vkGetSwapchainImagesKHR(g_vk.dev, g_vk.swapchain, &g_vk.sc_count, g_vk.sc_images);
    for (uint32_t i = 0; i < g_vk.sc_count; i++) {
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g_vk.sc_images[i],
                                    .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = sf.format,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VK_CHECK(vkCreateImageView(g_vk.dev, &vi, NULL, &g_vk.sc_views[i]));
    }
}

static void device_wait_idle(void) {  // (with nothing left for the present thread to present)
    vk_present_flush();
    vk_queue_lock();
    vkDeviceWaitIdle(g_vk.dev);
    vk_queue_unlock();
}

static void recreate_swapchain(void) {
    device_wait_idle();
    create_swapchain();
}

// The window's surface went away (Android destroys it while the app is in the background and makes a new one on
// return): swapchain and surface dropped; vk_frame_begin makes new ones from the window once it has one again.
static void surface_lost(void) {
    device_wait_idle();
    for (uint32_t i = 0; i < g_vk.sc_count; i++) vkDestroyImageView(g_vk.dev, g_vk.sc_views[i], NULL);
    g_vk.sc_count = 0;
    if (g_vk.swapchain) vkDestroySwapchainKHR(g_vk.dev, g_vk.swapchain, NULL);
    g_vk.swapchain = VK_NULL_HANDLE;
    if (g_vk.surface) vkDestroySurfaceKHR(g_vk.inst, g_vk.surface, NULL);
    g_vk.surface = VK_NULL_HANDLE;
}

static bool recreate_surface(void) {
    surface_lost();
    if (!SDL_Vulkan_CreateSurface(window, g_vk.inst, NULL, &g_vk.surface)) {
        static int nlog;
        if (nlog++ < 5) LOG("[vk] surface lost; no new one yet: %s\n", SDL_GetError());
        return false;
    }
    VkBool32 present = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(g_vk.phys, g_vk.qfam, g_vk.surface, &present);
    LOG("[vk] surface recreated (present support %d)\n", present);
    create_swapchain();
    return g_vk.swapchain != VK_NULL_HANDLE;
}

// ================================================================== frames (start screen and game)
// Acquire semaphores rotate per frame in flight; a slot is reused once the submission that waited on it is done.
#define FRAMES 3
static VkSemaphore acquire_sem[FRAMES], done_sem[8];
static uint64_t slot_seq[FRAMES];
static int slot;

static VkSemaphore binary_semaphore(void) {
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore s;
    VK_CHECK(vkCreateSemaphore(g_vk.dev, &si, NULL, &s));
    return s;
}

static void image_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .oldLayout = from, .newLayout = to, .image = img,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &di);
}

// What is being rendered into: a window image (acquired), or one of our own images (vk_offscreen_begin)
typedef struct { uint32_t image; VkSemaphore acquired; bool ok; TexBackend *off; } Frame;
static Frame cur_frame;

static VkImageView frame_view(void) {
    static const VkComponentMapping id = {0};
    return cur_frame.off ? vk_image_view(cur_frame.off, VK_IMAGE_VIEW_TYPE_2D, cur_frame.off->format, 0, 1, 0, 1, id,
                                         VK_IMAGE_ASPECT_COLOR_BIT)
                         : g_vk.sc_views[cur_frame.image];
}
static VkImageLayout frame_layout(void) {
    return cur_frame.off ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
}
static VkExtent2D frame_extent(void) {
    return cur_frame.off ? (VkExtent2D){cur_frame.off->width, cur_frame.off->height} : g_vk.sc_extent;
}
VkFormat vk_frame_format(void) { return cur_frame.off ? cur_frame.off->format : g_vk.sc_format; }

static void frame_rendering(VkAttachmentLoadOp load, const float clear[4]) {
    VkCommandBuffer cb = vk_cmd();
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = frame_view(),
                                    .imageLayout = frame_layout(), .loadOp = load, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    if (clear) memcpy(ca.clearValue.color.float32, clear, 4 * sizeof(float));
    VkExtent2D ext = frame_extent();
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, ext}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    VkViewport vp = {0, 0, (float)ext.width, (float)ext.height, 0, 1};
    VkRect2D sc = {{0, 0}, ext};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
}

void vk_offscreen_begin(TexBackend *img, const float clear[4]) {
    cur_frame = (Frame){.ok = true, .off = img};
    vk_full_barrier(vk_cmd());
    frame_rendering(VK_ATTACHMENT_LOAD_OP_CLEAR, clear);
}
void vk_offscreen_end(void) {
    if (!cur_frame.ok || !cur_frame.off) return;
    vkCmdEndRendering(vk_cmd());
    vk_full_barrier(vk_cmd());
    cur_frame = (Frame){0};
}

// ---- the queue, shared with the present thread; the deferred present (frame generation's real frame)
static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
void vk_queue_lock(void) { pthread_mutex_lock(&queue_lock); }
void vk_queue_unlock(void) { pthread_mutex_unlock(&queue_lock); }

static pthread_mutex_t dp_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t dp_cond = PTHREAD_COND_INITIALIZER;
static struct { bool pending; uint32_t img; VkSemaphore sem; VkSwapchainKHR sc; uint64_t at, gen_seq; } dp;
static uint64_t last_frame_seq;  // the submission of the last window image (the in-between one, for a deferred present)
static _Atomic bool dp_stale;  // a deferred present found the swapchain out of date / the surface lost

static void *dp_main(void *arg) {
    (void)arg;
    pthread_mutex_lock(&dp_lock);
    for (;;) {
        while (!dp.pending) pthread_cond_wait(&dp_cond, &dp_lock);
        uint64_t at = dp.at, gen_seq = dp.gen_seq;
        VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &dp.sem,
                               .swapchainCount = 1, .pSwapchains = &dp.sc, .pImageIndices = &dp.img};
        pthread_mutex_unlock(&dp_lock);
        if (gen_seq) {  // (the in-between image ready on the GPU: the real one half a slot after that)
            VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1,
                                      .pSemaphores = &g_vk.timeline, .pValues = &gen_seq};
            if (vkWaitSemaphores(g_vk.dev, &wi, 1000000000ull) == VK_SUCCESS) at = framegen_real_present_at(at, plat_now_ns());
        }
        if (plat_now_ns() < at) plat_sleep_until_ns(at);
        vk_queue_lock();
        VkResult r = vkQueuePresentKHR(g_vk.queue, &pi);
        vk_queue_unlock();
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR || r == VK_ERROR_SURFACE_LOST_KHR) atomic_store(&dp_stale, true);
        pthread_mutex_lock(&dp_lock);
        dp.pending = false;
        pthread_cond_broadcast(&dp_cond);
    }
    return NULL;
}

void vk_present_flush(void) {
    pthread_mutex_lock(&dp_lock);
    while (dp.pending) pthread_cond_wait(&dp_cond, &dp_lock);
    pthread_mutex_unlock(&dp_lock);
}

// Acquire the next window image and begin rendering into it (cleared to `clear`). Returns false when there is
// nothing to draw into (minimised window).
bool vk_frame_begin(const float clear[4]) {
    cur_frame.ok = false;
    int w, h;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    if (!w || !h) return false;  // minimised
    if (!g_vk.surface && !recreate_surface()) return false;
    if (atomic_exchange(&dp_stale, false)) recreate_swapchain();
    if (!g_vk.swapchain || (uint32_t)w != g_vk.sc_extent.width || (uint32_t)h != g_vk.sc_extent.height) recreate_swapchain();
    if (!g_vk.swapchain) return false;
    static int trace = -1;
    if (trace < 0) trace = getenv("LARECOMP_VK_TRACE") != NULL;
    if (trace) LOG("[vk] frame begin %dx%d (swapchain %ux%u)\n", w, h, g_vk.sc_extent.width, g_vk.sc_extent.height);
    slot = (slot + 1) % FRAMES;
    if (!acquire_sem[slot]) acquire_sem[slot] = binary_semaphore();
    vk_present_flush();  // (the swapchain is the present thread's until its present is issued)
    vk_wait_seq(slot_seq[slot]);
    VkResult r = vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, UINT64_MAX, acquire_sem[slot], VK_NULL_HANDLE, &cur_frame.image);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_ERROR_SURFACE_LOST_KHR) {
        if (r == VK_ERROR_SURFACE_LOST_KHR ? !recreate_surface() : (recreate_swapchain(), !g_vk.swapchain)) return false;
        r = vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, UINT64_MAX, acquire_sem[slot], VK_NULL_HANDLE, &cur_frame.image);
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { LOG("[vk] acquire failed: %d\n", r); return false; }
    cur_frame.acquired = acquire_sem[slot];
    cur_frame.off = NULL;
    slot_seq[slot] = vk_pending_seq();
    image_barrier(vk_cmd(), g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    frame_rendering(VK_ATTACHMENT_LOAD_OP_CLEAR, clear);
    cur_frame.ok = true;
    return true;
}

// Passes of their own between vk_frame_begin and vk_frame_end (exec.c post passes): end the window image's
// rendering, and begin it again keeping what was drawn.
void vk_frame_suspend(void) {
    if (cur_frame.ok) vkCmdEndRendering(vk_cmd());
}
void vk_frame_resume(void) {
    if (cur_frame.ok) frame_rendering(VK_ATTACHMENT_LOAD_OP_LOAD, NULL);
}

VkExtent2D vk_frame_extent(void) { return g_vk.sc_extent; }
void *vk_frame_cmd(void) { return (void *)vk_cmd(); }

static void save_window_image(VkCommandBuffer cb, VkSpan *out) {
    uint32_t w = g_vk.sc_extent.width, h = g_vk.sc_extent.height;
    *out = vk_readback((uint64_t)w * h * 4);
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy rg = {.bufferOffset = out->off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .imageExtent = {w, h, 1}};
    vkCmdCopyImageToBuffer(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out->buf, 1, &rg);
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}

// End rendering into the window image and present it (present_at: on the present thread at that time; 0: now).
// shot_path: also save the window image as PNG.
static void frame_end(const char *shot_path, uint64_t present_at) {
    if (!cur_frame.ok || cur_frame.off) return;
    if (!present_at) vk_present_flush();  // (the real frame before it is shown first)
    VkCommandBuffer cb = vk_cmd();
    vkCmdEndRendering(cb);
    VkSpan shot = {0};
    if (shot_path) save_window_image(cb, &shot);
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    uint32_t img = cur_frame.image;
    if (!done_sem[img]) done_sem[img] = binary_semaphore();
    uint64_t seq = vk_pending_seq();
    vk_submit_extra(cur_frame.acquired, done_sem[img], VK_NULL_HANDLE);
    uint64_t prev_frame_seq = last_frame_seq;
    last_frame_seq = seq;
    static int started = -1;
    if (present_at && started < 0) {
        pthread_t th;
        started = pthread_create(&th, NULL, dp_main, NULL) == 0;
        if (started) pthread_detach(th);
        else LOG("[vk] no present thread: the real frame is presented at once\n");
    }
    if (present_at && started) {
        pthread_mutex_lock(&dp_lock);
        dp.img = img, dp.sem = done_sem[img], dp.sc = g_vk.swapchain, dp.at = present_at, dp.gen_seq = prev_frame_seq;
        dp.pending = true;
        pthread_cond_broadcast(&dp_cond);
        pthread_mutex_unlock(&dp_lock);
    } else {
        VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &done_sem[img],
                               .swapchainCount = 1, .pSwapchains = &g_vk.swapchain, .pImageIndices = &img};
        vk_queue_lock();
        VkResult r = vkQueuePresentKHR(g_vk.queue, &pi);
        vk_queue_unlock();
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) recreate_swapchain();
        else if (r == VK_ERROR_SURFACE_LOST_KHR) recreate_surface();
    }
    cur_frame.ok = false;
    if (shot_path) {
        vk_wait_seq(seq);
        uint32_t w = g_vk.sc_extent.width, h = g_vk.sc_extent.height;
        uint8_t *px = shot.ptr;
        bool bgr = g_vk.sc_format == VK_FORMAT_B8G8R8A8_UNORM || g_vk.sc_format == VK_FORMAT_B8G8R8A8_SRGB;
        for (uint64_t i = 0; bgr && i < (uint64_t)w * h; i++) { uint8_t t = px[i * 4]; px[i * 4] = px[i * 4 + 2]; px[i * 4 + 2] = t; }
        for (uint64_t i = 0; i < (uint64_t)w * h; i++) px[i * 4 + 3] = 255;
        vk_save_png(shot_path, px, w, h);
    }
}
void vk_frame_end(const char *shot_path) { frame_end(shot_path, 0); }
void vk_frame_end_at(const char *shot_path, uint64_t present_at_ns) { frame_end(shot_path, present_at_ns ? present_at_ns : 1); }

#if !SDL_VERSION_ATLEAST(3, 4, 0)
// (SDL_SavePNG is SDL 3.4; distributions ship 3.2) A plain PNG writer: RGBA8, deflate "stored" blocks.
static uint32_t png_crc(uint32_t c, const uint8_t *p, size_t n) {
    c = ~c;
    for (size_t i = 0; i < n; i++) { c ^= p[i]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & -(c & 1u)); }
    return ~c;
}
static void png_be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void png_chunk(FILE *f, const char *type, const uint8_t *d, uint32_t n) {
    uint8_t h[8];
    png_be32(h, n); memcpy(h + 4, type, 4);
    fwrite(h, 1, 8, f);
    if (n) fwrite(d, 1, n, f);
    uint32_t c = png_crc(png_crc(0, (const uint8_t *)type, 4), d, n);
    png_be32(h, c);
    fwrite(h, 1, 4, f);
}
static bool save_png_plain(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h) {
    size_t row = (size_t)w * 4 + 1, raw_n = row * h;
    uint8_t *raw = malloc(raw_n);
    if (!raw) return false;
    for (uint32_t y = 0; y < h; y++) { raw[y * row] = 0; memcpy(raw + y * row + 1, rgba + (size_t)y * w * 4, (size_t)w * 4); }
    size_t nblk = raw_n / 65535 + 1, z_n = 2 + raw_n + nblk * 5 + 4;
    uint8_t *z = malloc(z_n), *o = z;
    if (!z) { free(raw); return false; }
    *o++ = 0x78; *o++ = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_n; i++) { a = (a + raw[i]) % 65521u; b = (b + a) % 65521u; }
    for (size_t off = 0; off < raw_n || off == 0;) {
        size_t n = raw_n - off > 65535 ? 65535 : raw_n - off;
        *o++ = off + n >= raw_n ? 1 : 0;
        *o++ = n & 0xFF; *o++ = n >> 8; *o++ = ~n & 0xFF; *o++ = (~n >> 8) & 0xFF;
        memcpy(o, raw + off, n); o += n; off += n;
        if (!n) break;
    }
    png_be32(o, (b << 16) | a); o += 4;
    FILE *f = fopen(path, "wb");
    bool ok = f != NULL;
    if (f) {
        static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
        uint8_t ihdr[13];
        png_be32(ihdr, w); png_be32(ihdr + 4, h);
        ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = ihdr[11] = ihdr[12] = 0;
        fwrite(sig, 1, 8, f);
        png_chunk(f, "IHDR", ihdr, 13);
        png_chunk(f, "IDAT", z, (uint32_t)(o - z));
        png_chunk(f, "IEND", NULL, 0);
        fclose(f);
    }
    free(raw); free(z);
    return ok;
}
#endif

void vk_save_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h) {
#ifdef __ANDROID__  // (SDL takes a relative path as one in the app's internal storage: keep it in the data root)
    char abs[1200];
    if (path[0] != '/' && getcwd(abs, 1000)) { snprintf(abs + strlen(abs), sizeof abs - strlen(abs), "/%s", path); path = abs; }
#endif
#if SDL_VERSION_ATLEAST(3, 4, 0)
    SDL_Surface *s = SDL_CreateSurfaceFrom((int)w, (int)h, SDL_PIXELFORMAT_ABGR8888, (void *)rgba, (int)w * 4);
    if (!s || !SDL_SavePNG(s, path)) LOG("[vk] saving %s failed: %s\n", path, SDL_GetError());
    else LOG("[vk] saved %s (%ux%u)\n", path, w, h);
    SDL_DestroySurface(s);
#else
    if (!save_png_plain(path, rgba, w, h)) LOG("[vk] saving %s failed\n", path);
    else LOG("[vk] saved %s (%ux%u)\n", path, w, h);
#endif
}

// ================================================================== game presentation (metal.m backend_present)
// Draws the game's image into the window with the widescreen crop, then the overlay menu.
extern void vk_present_draw(VkCommandBuffer cb, TexBackend *b, float vx, float vy, float vw, float vh, const float pv[8]);
void renderer_submit(void);
void renderer_set_scale(float sx, float sy);
uint64_t renderer_present_count(void);
void renderer_save_texture_tex(TexBackend *b, const char *path);

static uint64_t frames, total_frames, g_frame_counter;
uint64_t renderer_present_count(void) { return total_frames; }
uint64_t vk_frame_counter(void) { return g_frame_counter; }
static float g_shot_crop[4] = {0, 0, 1, 1};
static double last_fps_time;
static NvnTexture *g_last_presented;

const float *vk_shot_crop(void) { return g_shot_crop; }

// (frame generation shots) one of our RGBA8 images (GENERAL layout) as PNG, at <base><suffix>; waits for the GPU
static void save_image(TexBackend *img, const char *base, const char *suffix) {
    char path[512];
    snprintf(path, sizeof path, "%s%s", base, suffix);
    VkSpan out = vk_readback((uint64_t)img->width * img->height * 4);
    VkBufferImageCopy rg = {.bufferOffset = out.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .imageExtent = {img->width, img->height, 1}};
    vk_full_barrier(vk_cmd());
    vkCmdCopyImageToBuffer(vk_cmd(), img->image, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &rg);
    uint64_t seq = vk_pending_seq();
    renderer_submit();
    vk_wait_seq(seq);
    uint8_t *px = out.ptr;
    for (uint64_t i = 0; i < (uint64_t)img->width * img->height; i++) px[i * 4 + 3] = 255;
    vk_save_png(path, px, img->width, img->height);
}

void backend_present(NvnTexture *t);
static bool g_held;                 // (backend_present from backend_represent: no generated frame)
static _Atomic float g_display_hz;  // the window's display refresh (ui_run)
void backend_represent(void) {
    g_held = true;
    if (g_last_presented) backend_present(g_last_presented);
    else SDL_Delay(16);
    g_held = false;
}

void backend_present(NvnTexture *t) {
    extern void vk_probe_drain(void);
    vk_probe_drain();  // (exec.c: LARECOMP_DEBUG_PROBE_LAZY)
    g_last_presented = t;
    TexBackend *b = vk_tex(t, true);
    b->presented = true;
    vk_end_pass();
    static const float black[4] = {0, 0, 0, 1};
    uint64_t t0 = SDL_GetTicksNS();
    // Frame generation (Video tab; runtime/nvn/framegen.h): the game image goes to an image of ours (fg_k) first, and
    // from there to an in-between window image (with the last frame's, fg_k ^ 1) and this frame's own
    static int fg_k;
    static bool fg_last_gen;
    bool fg_on = g_settings.frame_gen != FRAMEGEN_OFF;
#ifdef __ANDROID__
    {   // (the display's refresh follows what the app asks for: 120 while frame generation is on)
        // (the request belongs to the surface: asked again for a new one, after the app was in the background)
        static int asked = -1;
        static void *asked_win;
        ANativeWindow *aw = SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, NULL);
        if (aw && (asked != fg_on || aw != asked_win)) {
            asked = fg_on, asked_win = aw;
            if (__builtin_available(android 30, *))
                ANativeWindow_setFrameRate(aw, fg_on ? 120.0f : 0.0f, ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT);
        }
    }
#endif
    TexBackend *fgc = NULL;
    bool have;
    if (fg_on && g_vk.swapchain) {  // (the swapchain at the window's size first: our image is made at its size)
        int ww, wh;
        SDL_GetWindowSizeInPixels(window, &ww, &wh);
        if (ww && wh && (atomic_exchange(&dp_stale, false) || (uint32_t)ww != g_vk.sc_extent.width ||
                         (uint32_t)wh != g_vk.sc_extent.height))
            recreate_swapchain();
    }
    extern bool g_frame_hidden;  // (objects.c: a mod's fast-forward, no window image)
    if (g_frame_hidden) {
        have = false;
    } else if (fg_on && g_vk.swapchain && g_vk.sc_extent.width && g_vk.sc_extent.height) {
        fgc = vk_framegen_image(fg_k, g_vk.sc_extent.width, g_vk.sc_extent.height);
        vk_offscreen_begin(fgc, black);
        have = true;
    } else {
        have = vk_frame_begin(black);
    }
    uint64_t nd = SDL_GetTicksNS() - t0;
    if (nd > 30000000 && !fgc) {  // (with frame generation the acquires come later)
        static int warned;
        if (warned++ < 20) LOG("[vk] acquire blocked %llums\n", (unsigned long long)(nd / 1000000));
    }
    if (have) {
        double dw = fgc ? fgc->width : g_vk.sc_extent.width, dh = fgc ? fgc->height : g_vk.sc_extent.height;
        extern void widescreen_frame_end(uint64_t frame);
        widescreen_frame_end(total_frames);
        extern void widescreen_set_factor(float f);
        extern bool widescreen_allowed(void);
        // render scale + placement in the window: runtime/nvn/output_scale.c (Video tab "Internal resolution");
        // no GPU timer here (-1): dynamic resolution goes by the frame time
        OutScale os;
        output_scale(dw, dh, widescreen_allowed(), SDL_GetTicksNS() / 1e9, -1, &os);
        double sx = os.sx, sy = os.sy, vx = os.vx, vy = os.vy, vw = os.vw, vh = os.vh, shape = os.shape;
        // interiors / dungeons: their 16:9 centre, or the part inside a room wider than one screen
        extern void widescreen_present_crop(uint64_t, double, float *, double *, double *, double *, double *);
        float crop[4];  // uv origin and size
        widescreen_present_crop(total_frames, shape, crop, &vx, &vy, &vw, &vh);
        { extern void input_set_game_rect(double, double, double, double, double, double); input_set_game_rect(vx, vy, vw, vh, dw, dh); }
        widescreen_set_factor((float)shape);
        renderer_set_scale((float)sx, (float)sy);
        VkCommandBuffer cb = vk_cmd();
        float pv[8] = {renderer_origin_lower_left() ? 1.0f : 0.0f, 0, 0, 0, crop[0], crop[1], crop[2], crop[3]};
        memcpy(g_shot_crop, crop, sizeof crop);
        vk_present_draw(cb, b, (float)vx, (float)vy, (float)vw, (float)vh, pv);
        static long menu_at = -1;
        if (menu_at < 0) menu_at = getenv("LARECOMP_MENU_AT") ? atol(getenv("LARECOMP_MENU_AT")) : 0;
        if (menu_at && (long)total_frames + 1 == menu_at) { extern void menu_test_open(void); menu_test_open(); }
        static FgState fg_state;
        FgFrame ff = {.w = (uint32_t)dw, .h = (uint32_t)dh, .fmt = (uint32_t)g_vk.sc_format, .now_ns = plat_now_ns(),
                      .display_hz = atomic_load(&g_display_hz), .enabled = fgc != NULL, .held = g_held,
                      .any_pace = getenv("LARECOMP_FRAMEGEN_ANY_PACE") != NULL};
        memcpy(ff.crop, crop, sizeof crop);
        bool fg_gen = framegen_step(&fg_state, &ff);
        // LARECOMP_FRAMEGEN_SHOT=<frame>: at the first in-between frame from there, the last real frame's game image,
        // the in-between window image and this frame's game image (LARECOMP_SHOT_PATH + _prev / _gen / _cur.png;
        // default build/framegen)
        static long fg_shot = -1;
        if (fg_shot < 0) fg_shot = getenv("LARECOMP_FRAMEGEN_SHOT") ? atol(getenv("LARECOMP_FRAMEGEN_SHOT")) : 0;
        char fg_base[480] = "";
        if (fgc && fg_gen && fg_shot && (long)total_frames + 1 >= fg_shot) {
            fg_shot = 0;
            snprintf(fg_base, sizeof fg_base, "%s", getenv("LARECOMP_SHOT_PATH") ? getenv("LARECOMP_SHOT_PATH") : "build/framegen");
        }
        uint64_t t_gen = 0;
        if (fgc) {
            vk_offscreen_end();
            TexBackend *fgp = fg_gen ? vk_framegen_image(fg_k ^ 1, fgc->width, fgc->height) : NULL;  // (outside any rendering)
            bool in_hand = false;  // (a window image acquired for the in-between frame, used for this frame's instead)
            if (fg_gen && vk_frame_begin(black) &&
                (g_vk.sc_extent.width != fgc->width || g_vk.sc_extent.height != fgc->height)) {
                LOG("[framegen] the window changed size under the in-between frame: this frame's own goes there\n");
                fg_gen = false, in_hand = true;
            } else if (fg_gen && cur_frame.ok) {
                vk_framegen_encode(vk_cmd(), fgp, fgc, 0.5f, !fg_last_gen);
                menu_render((void *)vk_cmd(), g_vk.sc_extent.width, g_vk.sc_extent.height);
                char gen_path[512];
                snprintf(gen_path, sizeof gen_path, "%s_gen.png", fg_base);
                vk_frame_end(fg_base[0] ? gen_path : NULL);
                if (fg_base[0]) {
                    save_image(fgp, fg_base, "_prev.png");
                    save_image(fgc, fg_base, "_cur.png");
                    LOG("[framegen] shot %s_prev / _gen / _cur.png\n", fg_base);
                }
                t_gen = plat_now_ns();
                static bool said;
                if (!said) said = true, LOG("[framegen] on %ux%u display %.0f Hz\n", fgc->width, fgc->height, ff.display_hz);
            } else {
                fg_gen = false;
            }
            fg_last_gen = fg_gen;
            fg_k ^= 1;
            if (!in_hand && !vk_frame_begin(black)) goto presented;
            cb = vk_cmd();
            vk_present_copy(cb, fgc);
            if (fg_gen) menu_redraw((void *)cb, g_vk.sc_extent.width, g_vk.sc_extent.height);
            else menu_render((void *)cb, g_vk.sc_extent.width, g_vk.sc_extent.height);
        } else {
            menu_render((void *)cb, g_vk.sc_extent.width, g_vk.sc_extent.height);
        }
        // LARECOMP_WINDOW_SHOT=<frame> saves the window image at that frame (LARECOMP_SHOT_PATH, default
        // build/window.png); <first>:<last>[:<step>] every step-th frame in between, LARECOMP_SHOT_PATH then a printf
        // pattern taking the frame number (build/win_%05d.png) - as metal.m's LARECOMP_CG_SHOT, for comparisons
        static long win_a = -1, win_b, win_step = 1;
        if (win_a < 0) {
            const char *e = getenv("LARECOMP_WINDOW_SHOT");
            win_a = e ? atol(e) : 0;
            win_b = win_a;
            if (e && strchr(e, ':')) sscanf(e, "%ld:%ld:%ld", &win_a, &win_b, &win_step);
            if (win_step < 1) win_step = 1;
        }
        long f1 = (long)total_frames + 1;
        bool shot = win_a && f1 >= win_a && f1 <= win_b && (f1 - win_a) % win_step == 0;
        char shot_path[512] = "build/window.png";
        const char *pat = getenv("LARECOMP_SHOT_PATH");
        if (shot && pat) {
            if (strchr(pat, '%')) snprintf(shot_path, sizeof shot_path, pat, (int)f1);
            else snprintf(shot_path, sizeof shot_path, "%s", pat);
        }
        // test hooks (LARECOMP_HOST_INPUT "<frame>:shot:<name>", runtime/hle/input.c): this frame's window image there
        extern char g_window_shot_request[256];
        bool req = !shot && g_window_shot_request[0];
        if (req) {
            snprintf(shot_path, sizeof shot_path, "%s", g_window_shot_request);
            g_window_shot_request[0] = 0;
        }
        if (fg_gen) vk_frame_end_at(shot || req ? shot_path : NULL, t_gen + FG_HALF_SLOT_NS);
        else vk_frame_end(shot || req ? shot_path : NULL);
        if (shot && f1 + win_step > win_b && getenv("LARECOMP_EXIT_AFTER_SHOT")) { fflush(stderr); _exit(0); }
    } else {
        renderer_submit();
    }
presented:;  // (frame generation: no window image for this frame's own present)
    extern void vk_prof_frame(uint64_t frame, uint64_t present_ns);
    vk_prof_frame(total_frames, SDL_GetTicksNS() - t0);
    extern void prefetch_stats_tick(uint64_t presented);
    prefetch_stats_tick(total_frames);
    frames++;
    total_frames++;
    g_frame_counter++;
    extern void vk_pace_frame(void);
    vk_pace_frame();  // (exec.c: the game's 30 fps mode when frames don't fit 60)
    extern void input_frame(uint64_t frame);
    input_frame(total_frames);
    static long slow_ms = -1;
    if (slow_ms < 0) slow_ms = getenv("LARECOMP_SLOW_FRAME_MS") ? atol(getenv("LARECOMP_SLOW_FRAME_MS")) : 0;
    static long slow_until = -1;  // LARECOMP_SLOW_FRAME_UNTIL=<frame>: the stall ends there (frame pacing recovery tests)
    if (slow_until < 0) slow_until = getenv("LARECOMP_SLOW_FRAME_UNTIL") ? atol(getenv("LARECOMP_SLOW_FRAME_UNTIL")) : 0;
    if (slow_ms > 0 && total_frames > 2600 && (!slow_until || (long)total_frames < slow_until)) SDL_Delay((Uint32)slow_ms);
    extern void fnprof_frame(uint64_t frame);
    fnprof_frame(total_frames);
    extern void warp_frame(uint64_t frame);
    warp_frame(total_frames);
    extern void reboot_test_frame(uint64_t frame);  // runtime/main.c (LARECOMP_REBOOT_AT)
    reboot_test_frame(total_frames);
    extern void save_editor_frame(uint64_t frame);
    save_editor_frame(total_frames);
    extern void vk_level_change_check(void);
    vk_level_change_check();
    static long shot_every = -1;
    if (shot_every < 0) { const char *e = getenv("LARECOMP_SCREENSHOT_EVERY"); shot_every = e ? atol(e) : 0; }
    extern bool g_debug_frame;
    static long debug_at = -1;
    if (debug_at < 0) { const char *e = getenv("LARECOMP_DEBUG_FRAME"); debug_at = e ? atol(e) : 0; }
    extern volatile int g_debug_frame_request;
    if (g_debug_frame_request) { g_debug_frame_request = 0; debug_at = (long)total_frames + 1; }
    // LARECOMP_DEBUG_FRAMES=<n> (with LARECOMP_DEBUG_FRAME / F9): dump n consecutive frames (build/pass_<frame>_NNN.png)
    static long debug_n = -1;
    if (debug_n < 0) { const char *e = getenv("LARECOMP_DEBUG_FRAMES"); debug_n = e && atol(e) > 1 ? atol(e) : 1; }
    g_debug_frame = debug_at && total_frames >= (uint64_t)debug_at && total_frames < (uint64_t)debug_at + (uint64_t)debug_n;
    {   // LARECOMP_GPUPROF_AT=<frame>[,<frame>...]: GPU time per pass of those frames ("[gpuprof]" in the log)
        extern bool g_gpuprof_frame;
        extern void vk_gpuprof_report(void);
        if (g_gpuprof_frame) { g_gpuprof_frame = false; vk_gpuprof_report(); }
        const char *e = getenv("LARECOMP_GPUPROF_AT");
        for (const char *p = e; p && *p;) {
            long at = strtol(p, (char **)&p, 10);
            if (at == (long)total_frames) { g_gpuprof_frame = true; break; }
            if (*p == ',') p++; else break;
        }
    }
    if (g_debug_frame) LOG("===== debug frame %llu =====\n", (unsigned long long)total_frames);
    {   // LARECOMP_SHOT_AT=<frame>,<frame>,... (ascending): the game image at those frames -> build/frames/frame_N.png
        static const char *next;
        static int init;
        if (!init) { init = 1; next = getenv("LARECOMP_SHOT_AT"); if (next) mkdir("build/frames", 0755); }
        while (next && *next) {
            long at = strtol(next, (char **)&next, 10);
            if ((long)total_frames < at) { while (next > getenv("LARECOMP_SHOT_AT") && next[-1] != ',') next--; break; }
            if ((long)total_frames == at) {
                char path[256];
                snprintf(path, sizeof path, "build/frames/frame_%05llu.png", (unsigned long long)total_frames);
                renderer_save_texture_tex(b, path);
            }
            if (*next == ',') next++;
        }
    }
    static long shot_from = -1;
    if (shot_from < 0) { const char *e = getenv("LARECOMP_SCREENSHOT_FROM"); shot_from = e ? atol(e) : 0; }
    if (shot_every && total_frames % shot_every == 0 && (long)total_frames >= shot_from) {
        char path[256];
        if (shot_from) mkdir("build/frames", 0755);
        snprintf(path, sizeof path, shot_from ? "build/frames/frame_%05llu.png" : "build/frame_%05llu.png",
                 (unsigned long long)total_frames);
        renderer_save_texture_tex(b, path);
        // a series (SCREENSHOT_FROM) runs on until LARECOMP_WINDOW_SHOT exits
        if (getenv("LARECOMP_EXIT_AFTER_SHOT") && !shot_from) { fflush(stderr); _exit(0); }
    }
    double now = SDL_GetTicks() / 1000.0;
    static double fps_iv = -1, prev_present, max_gap;
    if (fps_iv < 0) fps_iv = getenv("LARECOMP_FPS_INTERVAL") ? atof(getenv("LARECOMP_FPS_INTERVAL")) : 2.0;
    if (prev_present > 0 && now - prev_present > max_gap) max_gap = now - prev_present;
    prev_present = now;
    if (now - last_fps_time >= fps_iv) {
        extern const char *game_level_name(void);
        static uint64_t fg_real0, fg_gen0;  // frame generation: generated / real frames since the last line (as metal.m)
        char fgs[64] = "";
        if (g_settings.frame_gen != FRAMEGEN_OFF)
            snprintf(fgs, sizeof fgs, " gen %llu/%llu", (unsigned long long)(g_fg_generated - fg_gen0),
                     (unsigned long long)(g_fg_real - fg_real0));
        fg_real0 = g_fg_real, fg_gen0 = g_fg_generated;
        if (last_fps_time > 0)
            LOG("[vk] t=%.1f frame %llu %.1f fps%s max %.0fms level %s (%s)\n", now, (unsigned long long)total_frames,
                frames / (now - last_fps_time), fgs, max_gap * 1000.0, game_level_name(), renderer_stats());
        max_gap = 0;
        frames = 0;
        last_fps_time = now;
    }
}

// ================================================================== UI thread
void vk_exec_init(void);
void vk_shaders_init(void);

// exit() from any thread: let the GPU finish, keep the pipeline cache, then end the process without the DLL
// teardown (driver / validation-layer threads can stall it for minutes while the game's threads still run).
static void quick_exit_handler(void) {
    static int trace = -1;
    if (trace < 0) trace = getenv("LARECOMP_VK_TRACE") != NULL;
    if (trace) LOG("[vk] exit: waiting for the GPU\n");
    if (g_vk.dev) {
        device_wait_idle();
        if (trace) LOG("[vk] exit: saving the pipeline cache\n");
        vk_pipeline_cache_save();
    }
    if (trace) LOG("[vk] exit: terminating\n");
    fflush(stdout);
    fflush(stderr);
#if defined(_WIN32) || defined(__linux__)
    plat_terminate();
#endif
}

void ui_window_init(void) {
    if (window) return;
    atexit(quick_exit_handler);
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    settings_load();
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) FATAL("SDL_Init: %s\n", SDL_GetError());
    int win_w = 1280, win_h = 720;
    if (getenv("LARECOMP_WINDOW")) sscanf(getenv("LARECOMP_WINDOW"), "%dx%d", &win_w, &win_h);
    SDL_WindowFlags wflags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#ifdef __ANDROID__
    wflags |= SDL_WINDOW_FULLSCREEN;  // (immersive: no status / navigation bar over the game)
#endif
    if (getenv("LARECOMP_HIDDEN")) wflags |= SDL_WINDOW_HIDDEN;
    {   // scripted test runs (user input ignored: runtime/hle/input.c input_user_blocked) open without taking the
        // keyboard focus from whatever the player is doing
        const char *v = getenv("LARECOMP_NO_USER_INPUT");
        bool scripted = v ? atoi(v) != 0 : (getenv("LARECOMP_INPUT") || getenv("LARECOMP_DOORJUMP_TEST") || getenv("LARECOMP_PLACE") ||
                                            getenv("LARECOMP_HOST_INPUT") || getenv("LARECOMP_BIND_TEST"));
        if (scripted) SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    }
    window = SDL_CreateWindow(GAME_TITLE, win_w, win_h, wflags);
    if (!window) FATAL("SDL_CreateWindow: %s\n", SDL_GetError());
    rt_window_icon(window);
    // the Fullscreen setting applies from the start screen on (not only once the game runs); test windows stay windows
    if (g_settings.fullscreen && !getenv("LARECOMP_WINDOW") && !getenv("LARECOMP_HIDDEN")) SDL_SetWindowFullscreen(window, true);
    create_instance();
    if (!SDL_Vulkan_CreateSurface(window, g_vk.inst, NULL, &g_vk.surface)) FATAL("SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
    pick_device();
    create_device();
    create_swapchain();
    vk_exec_init();
    vk_shaders_init();
}

SDL_Window *ui_window(void) { return window; }

void backend_init(void) {
    while (!atomic_load(&ui_ready)) SDL_Delay(1);
    LOG("[vk] %s, swapchain %ux%u\n", g_vk.props.deviceName, g_vk.sc_extent.width, g_vk.sc_extent.height);
}

// ---- in-place reboot (runtime/main.c teardown; main thread, guest threads stopped)
void backend_reset(void) {
    if (!g_vk.dev) return;
    device_wait_idle();
    extern void vk_textures_reset(void);
    extern void vk_exec_reset(void);
    vk_textures_reset();
    vk_exec_reset();
    g_last_presented = NULL;
}
void backend_guest_memory_reset(void) {}  // (exec.c re-imports the heap on first use)
void backend_clear_window(void) {
    static const float black[4] = {0, 0, 0, 1};
    if (g_vk.dev && vk_frame_begin(black)) vk_frame_end(NULL);
}

int ui_run(void) {
    ui_window_init();
    atomic_store(&ui_ready, true);
    for (;;) {
        if (rt_reboot_requested()) return rt_reboot_requested();  // (runtime/main.c tears the game down)
        SDL_Event e;
        while (SDL_WaitEventTimeout(&e, 16)) {
            if (e.type == SDL_EVENT_QUIT) {
                LOG("window closed\n");
                exit(0);
            }
            extern bool input_user_blocked(const SDL_Event *e);
            if (input_user_blocked(&e)) continue;  // scripted test runs: the real keyboard / mouse / pads do nothing (as metal.m)
            if (menu_handle_event(&e)) continue;
            input_handle_event(&e);
        }
        menu_apply_window_requests(window);
        atomic_store(&g_display_hz, framegen_display_hz(window));  // (SDL's display state: read on this thread)
    }
}
