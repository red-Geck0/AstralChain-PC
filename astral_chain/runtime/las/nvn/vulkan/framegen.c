// Frame generation (Astral Chain, AC_FRAMEGEN=1): AMD FSR 3.1 frame generation through the AMD FSR SDK's frame
// generation swapchain for Vulkan. One generated frame is shown between two of the game's, paced by the swapchain's
// own threads. docs/framegen-notes.md.
//
// The SDK comes as signed DLLs (tools/fidelityfx: amd_fidelityfx_loader_vk.dll, which loads
// amd_fidelityfx_framegeneration_vk.dll next to it), loaded at run time; without them, or without the queues it
// needs, frame generation stays off and the game runs as before.
//
// Inputs, per game frame:
// - motion vectors: exec.c's mv_img (AC_MOTION_VECTORS: the game's velocity programs patched to write exact
//   ndc_now - ndc_prev, NDC y up);
// - depth: the game's scene depth (D24S8), as its object velocity pass uses it;
// - the image before the HUD: the game's final composite target, right after its composite draw (the HUD is drawn
//   over it in the same pass), drawn into a window-sized image the way the present draws the game image;
// - camera: near, far and vertical field of view from the camera velocity draw's constants (cb13).
// The game sends the next frame's rendering before it presents a frame, so these are copied when they are complete
// (fg_capture, keyed by the composite target) and used when that target is presented (fg_present_record).
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "framegen.h"
#include "api/include/ffx_api.h"
#include "api/include/vk/ffx_api_vk.h"
typedef struct VkQueueInfoFFXAPI VkQueueInfoFFXAPI;  // (the SDK's Vulkan headers name it as C++ would)
#include "framegeneration/include/ffx_framegeneration.h"
#include "framegeneration/include/vk/ffx_api_framegeneration_vk.h"

static struct {
    int state;  // 0 not tried, 1 loaded, -1 off
    PfnFfxCreateContext create;
    PfnFfxDestroyContext destroy;
    PfnFfxConfigure configure;
    PfnFfxQuery query;
    PfnFfxDispatch dispatch;
    bool queues;                    // the swapchain's queues were added to the device
    uint32_t cfam, cidx, pfam, pidx, afam, aidx;
    VkQueue cq, pq, aq;             // async compute, present, image acquire
    ffxContext sc, fg;              // swapchain context, frame generation context
    struct ffxQueryDescSwapchainReplacementFunctionsVK fn;
    VkExtent2D ext;
    VkFormat fmt;
    uint32_t create_flags, debug_flags;
    uint64_t frame_id;
} F;

// ---- the game queue: the swapchain submits to it from its own threads (through fg_game_submit)
static CRITICAL_SECTION qlock;
static INIT_ONCE qlock_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK qlock_init(PINIT_ONCE o, PVOID p, PVOID *c) {
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&qlock);
    return TRUE;
}
void fg_queue_lock(void) {
    InitOnceExecuteOnce(&qlock_once, qlock_init, NULL, NULL);
    EnterCriticalSection(&qlock);
}
void fg_queue_unlock(void) { LeaveCriticalSection(&qlock); }
static VkResult fg_game_submit(uint32_t n, const VkSubmitInfo *s, VkFence f) {
    fg_queue_lock();
    VkResult r = vkQueueSubmit(g_vk.queue, n, s, f);
    fg_queue_unlock();
    return r;
}

static void *proc(HMODULE m, const char *name) { return (void *)GetProcAddress(m, name); }

bool fg_wanted(void) {
    if (F.state) return F.state > 0;
    F.state = -1;
    const char *e = getenv("AC_FRAMEGEN");
    if (!e || e[0] != '1') return false;
    wchar_t path[MAX_PATH + 64];
    DWORD n = GetEnvironmentVariableW(L"AC_FFX_DIR", path, MAX_PATH);
    if (!n || n >= MAX_PATH) {  // <project>\build\game\round-00\trace.exe -> <project>\tools\fidelityfx
        n = GetModuleFileNameW(NULL, path, MAX_PATH);
        for (int up = 0; up < 4 && n; up++) {
            wchar_t *s = wcsrchr(path, L'\\');
            if (!s) n = 0;
            else *s = 0;
        }
        if (!n) { LOG("[fg] frame generation off: the game's folder is unknown\n"); return false; }
        wcscat(path, L"\\tools\\fidelityfx");
    }
    wcscat(path, L"\\amd_fidelityfx_loader_vk.dll");
    HMODULE m = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) { LOG("[fg] frame generation off: %ls not loaded (error %lu)\n", path, GetLastError()); return false; }
    F.create = (PfnFfxCreateContext)proc(m, "ffxCreateContext");
    F.destroy = (PfnFfxDestroyContext)proc(m, "ffxDestroyContext");
    F.configure = (PfnFfxConfigure)proc(m, "ffxConfigure");
    F.query = (PfnFfxQuery)proc(m, "ffxQuery");
    F.dispatch = (PfnFfxDispatch)proc(m, "ffxDispatch");
    if (!F.create || !F.destroy || !F.configure || !F.query || !F.dispatch) {
        LOG("[fg] frame generation off: %ls lacks the AMD FSR API\n", path);
        return false;
    }
    const char *d = getenv("AC_FG_DEBUG");  // 1: tear, reset and pacing lines; 2: the debug view
    F.debug_flags = !d ? 0 : d[0] == '2' ? FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_VIEW
                  : FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_TEAR_LINES | FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_RESET_INDICATORS |
                    FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_PACING_LINES;
    const char *inv = getenv("AC_FG_DEPTH_INVERTED");
    if (inv && inv[0] == '1') F.create_flags |= FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED;
    const char *as = getenv("AC_FG_ASYNC");  // 1: generation on the async compute queue
    if (as && as[0] == '1') F.create_flags |= FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT;
    F.state = 1;
    LOG("[fg] AMD FSR SDK loaded (%ls)\n", path);
    return true;
}

// ---- device queues: async compute, present (a transfer queue that can present) and image acquire
uint32_t fg_device_queues(VkDeviceQueueCreateInfo *out, uint32_t max) {
    if (!fg_wanted()) return 0;
    VkQueueFamilyProperties qp[16];
    uint32_t nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &nq, qp);
    VkBool32 pres[16] = {0};
    int cf = -1, pf = -1;
    for (uint32_t i = 0; i < nq; i++) {
        vkGetPhysicalDeviceSurfaceSupportKHR(g_vk.phys, i, g_vk.surface, &pres[i]);
        if (i == g_vk.qfam) continue;
        VkQueueFlags f = qp[i].queueFlags;
        if (cf < 0 && (f & VK_QUEUE_COMPUTE_BIT) && !(f & VK_QUEUE_GRAPHICS_BIT)) cf = (int)i;
        if (pf < 0 && pres[i] && (f & VK_QUEUE_TRANSFER_BIT) && !(f & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))) pf = (int)i;
    }
    uint32_t used[16] = {0};
    if (cf < 0) { LOG("[fg] frame generation off: no compute queue family\n"); F.state = -1; return 0; }
    F.cfam = (uint32_t)cf; F.cidx = used[cf]++;
    // Present and acquire from the compute family where it can present: from the transfer family the Arc B390's
    // driver crashed (igvk64.dll, null + 0xc0) right after the swapchain was made. AC_FG_PRESENT=transfer: that one.
    const char *pe = getenv("AC_FG_PRESENT");
    if (!(pe && !strcmp(pe, "transfer")) && pres[cf] && qp[cf].queueCount >= 3) pf = cf;
    if (pf < 0 && pres[cf] && qp[cf].queueCount > used[cf]) pf = cf;  // (no transfer family that can present)
    if (pf < 0) { LOG("[fg] frame generation off: no queue to present from\n"); F.state = -1; return 0; }
    F.pfam = (uint32_t)pf; F.pidx = used[pf]++;
    int af = qp[pf].queueCount > used[pf] ? pf : qp[cf].queueCount > used[cf] ? cf : -1;
    if (af < 0) { LOG("[fg] frame generation off: no queue left for image acquisition\n"); F.state = -1; return 0; }
    F.afam = (uint32_t)af; F.aidx = used[af]++;
    static float prio[16][8];
    uint32_t n = 0;
    for (uint32_t i = 0; i < nq; i++) {
        if (!used[i]) continue;
        if (n == max || used[i] > 8) { F.state = -1; return 0; }
        for (uint32_t k = 0; k < used[i]; k++) prio[i][k] = 1.0f;
        out[n++] = (VkDeviceQueueCreateInfo){.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = i,
                                             .queueCount = used[i], .pQueuePriorities = prio[i]};
    }
    F.queues = true;
    return n;
}

void fg_device_created(void) {
    if (!F.queues) return;
    vkGetDeviceQueue(g_vk.dev, F.cfam, F.cidx, &F.cq);
    vkGetDeviceQueue(g_vk.dev, F.pfam, F.pidx, &F.pq);
    vkGetDeviceQueue(g_vk.dev, F.afam, F.aidx, &F.aq);
    LOG("[fg] queues: game %u.0, async compute %u.%u, present %u.%u, image acquire %u.%u\n", g_vk.qfam, F.cfam, F.cidx,
        F.pfam, F.pidx, F.afam, F.aidx);
}

// ---- formats and resources
static uint32_t ffx_format(VkFormat f) {
    switch (f) {
    case VK_FORMAT_B8G8R8A8_UNORM: return FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB: return FFX_API_SURFACE_FORMAT_B8G8R8A8_SRGB;
    case VK_FORMAT_R8G8B8A8_UNORM: return FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8A8_SRGB: return FFX_API_SURFACE_FORMAT_R8G8B8A8_SRGB;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM;
    case VK_FORMAT_R16G16_SFLOAT: return FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
    case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_X8_D24_UNORM_PACK32: return FFX_API_SURFACE_FORMAT_R32_UINT;
    case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_D32_SFLOAT_S8_UINT: return FFX_API_SURFACE_FORMAT_R32_FLOAT;
    case VK_FORMAT_D16_UNORM: return FFX_API_SURFACE_FORMAT_R16_UNORM;
    default: return FFX_API_SURFACE_FORMAT_UNKNOWN;
    }
}
static bool is_depth(VkFormat f) {
    return f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_X8_D24_UNORM_PACK32 || f == VK_FORMAT_D32_SFLOAT ||
           f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D16_UNORM;
}
// Every image of the port is in VK_IMAGE_LAYOUT_GENERAL, which is the SDK's unordered-access state.
static struct FfxApiResource res(const TexBackend *b) {
    struct FfxApiResource r;
    memset(&r, 0, sizeof r);
    if (!b || !b->image) return r;
    r.resource = (void *)b->image;
    r.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    r.description.format = ffx_format(b->format);
    r.description.width = b->width;
    r.description.height = b->height;
    r.description.depth = 1;
    r.description.mipCount = 1;
    r.description.flags = FFX_API_RESOURCE_FLAGS_NONE;
    r.description.usage = FFX_API_RESOURCE_USAGE_READ_ONLY;
    if (is_depth(b->format)) r.description.usage |= FFX_API_RESOURCE_USAGE_DEPTHTARGET;
    if (b->format == VK_FORMAT_D24_UNORM_S8_UINT || b->format == VK_FORMAT_D32_SFLOAT_S8_UINT)
        r.description.usage |= FFX_API_RESOURCE_USAGE_STENCILTARGET;
    r.state = FFX_API_RESOURCE_STATE_UNORDERED_ACCESS;
    return r;
}

// A port-owned image (GENERAL layout), made again when its size or format changes.
static bool own_image(TexBackend *d, VkCommandBuffer cb, VkFormat f, uint32_t w, uint32_t h, VkImageUsageFlags usage) {
    if (d->image && (d->width != w || d->height != h || d->format != f)) {
        vk_views_forget(d);
        vk_defer_image(d->image, d->mem);
        d->image = VK_NULL_HANDLE;
        d->mem = VK_NULL_HANDLE;
    }
    if (d->image) return true;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL) != VK_SUCCESS) {
        d->image = VK_NULL_HANDLE;
        return false;
    }
    VkImageAspectFlags aspect = !is_depth(f) ? VK_IMAGE_ASPECT_COLOR_BIT
                              : f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT
                              ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
    d->format = f; d->width = w; d->height = h; d->depth = 1; d->layers = 1; d->levels = 1; d->samples = 1;
    d->aspect = aspect; d->base_type = VK_IMAGE_VIEW_TYPE_2D;
    vk_image_init_layout(cb, d->image, aspect);
    return true;
}

// ---- contexts
static void fg_message(uint32_t type, const wchar_t *message) {
    static int n;
    if (n++ < 200) LOG("[fg] SDK %s: %ls\n", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", message);
}
static void debug_messages(ffxContext *c, uint64_t effect) {
    struct ffxConfigureDescGlobalDebug d;
    memset(&d, 0, sizeof d);
    d.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG;
    d.effectId = effect;
    d.fpMessage = fg_message;
    d.debugLevel = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_WARNINGS;
    F.configure(c, &d.header);
}

static ffxReturnCode_t fg_dispatch_cb(ffxDispatchDescFrameGeneration *p, void *ctx) {
    return F.dispatch((ffxContext *)ctx, &p->header);
}

// The SDK's Vulkan functions, through this: on the Arc B390 it asked for descriptor pools of 4 and 8 million
// descriptors of each type, and the second failed (VK_ERROR_OUT_OF_HOST_MEMORY), so the frame generation context was
// never created. Pools are capped at 262144 of each type (contexts were created at 65536 too). The KHR aliases of core
// functions it asks for (vkGetBufferMemoryRequirements2KHR) are given the core ones.
#define FG_POOL_CAP 262144u
static PFN_vkCreateDescriptorPool real_create_pool;
static VKAPI_ATTR VkResult VKAPI_CALL fg_create_pool(VkDevice d, const VkDescriptorPoolCreateInfo *ci, const VkAllocationCallbacks *a,
                                                     VkDescriptorPool *p) {
    VkDescriptorPoolCreateInfo c = *ci;
    VkDescriptorPoolSize sz[32];
    if (ci->poolSizeCount <= 32) {
        bool capped = false;
        for (uint32_t i = 0; i < ci->poolSizeCount; i++) {
            sz[i] = ci->pPoolSizes[i];
            if (sz[i].descriptorCount > FG_POOL_CAP) { sz[i].descriptorCount = FG_POOL_CAP; capped = true; }
        }
        c.pPoolSizes = sz;
        static int logged;
        if (capped && !logged++) LOG("[fg] the SDK's descriptor pools capped at %u descriptors of each type\n", FG_POOL_CAP);
    }
    VkResult r = real_create_pool(d, &c, a, p);
    if (r != VK_SUCCESS) LOG("[fg] descriptor pool not created: %d (%u sets)\n", r, ci->maxSets);
    return r;
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL fg_device_proc(VkDevice d, const char *name) {
    PFN_vkVoidFunction f = vkGetDeviceProcAddr(d, name);
    size_t n = strlen(name);
    if (!f && n > 3 && n < 120 && !strcmp(name + n - 3, "KHR")) {
        char core[128];
        memcpy(core, name, n - 3);
        core[n - 3] = 0;
        f = vkGetDeviceProcAddr(d, core);
    }
    if (f && !strcmp(name, "vkCreateDescriptorPool")) {
        real_create_pool = (PFN_vkCreateDescriptorPool)f;
        return (PFN_vkVoidFunction)fg_create_pool;
    }
    return f;
}

static void log_version(ffxContext *c, const char *what) {
    struct ffxQueryGetProviderVersion v;
    memset(&v, 0, sizeof v);
    v.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (F.query(c, &v.header) == FFX_API_RETURN_OK && v.versionName) LOG("[fg] %s: %s\n", what, v.versionName);
}

static bool make_fg_context(void) {
    VkDeviceContext dc = {g_vk.dev, g_vk.phys, fg_device_proc};
    struct ffxCreateBackendVKDesc be;
    memset(&be, 0, sizeof be);
    be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    be.deviceContext = dc;
    struct ffxCreateContextDescFrameGenerationVersion ver;
    memset(&ver, 0, sizeof ver);
    ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
    ver.version = FFX_FRAMEGENERATION_VERSION;
    struct ffxCreateContextDescFrameGeneration c;
    memset(&c, 0, sizeof c);
    c.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    c.flags = F.create_flags;
    c.displaySize.width = F.ext.width;
    c.displaySize.height = F.ext.height;
    c.maxRenderSize.width = F.ext.width;  // (the game's image is letterboxed into the window: never larger)
    c.maxRenderSize.height = F.ext.height;
    c.backBufferFormat = ffx_format(F.fmt);
    c.header.pNext = &be.header;
    be.header.pNext = &ver.header;
    debug_messages(NULL, FFX_API_EFFECT_ID_FRAMEGENERATION);
    ffxReturnCode_t r = F.create(&F.fg, &c.header, NULL);
    if (r != FFX_API_RETURN_OK) {  // the newest version may need hardware this is not (AMD FSR Frame Generation 4: RDNA 4)
        uint64_t count = 0, ids[16];
        const char *names[16];
        struct ffxQueryDescGetVersions q;
        memset(&q, 0, sizeof q);
        q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        q.device = &dc;
        q.outputCount = &count;
        F.query(NULL, &q.header);
        if (count > 16) count = 16;
        q.versionIds = ids;
        q.versionNames = names;
        F.query(NULL, &q.header);
        LOG("[fg] frame generation context not created with the default version (%u); %llu versions\n", r, (unsigned long long)count);
        for (uint64_t i = 0; i < count && r != FFX_API_RETURN_OK; i++) {
            struct ffxOverrideVersion ov;
            memset(&ov, 0, sizeof ov);
            ov.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
            ov.versionId = ids[i];
            ver.header.pNext = &ov.header;
            F.fg = NULL;
            r = F.create(&F.fg, &c.header, NULL);
            LOG("[fg]   %s: %s\n", names[i] ? names[i] : "?", r == FFX_API_RETURN_OK ? "created" : "not created");
        }
    }
    if (r != FFX_API_RETURN_OK) { F.fg = NULL; LOG("[fg] frame generation off: no frame generation context\n"); return false; }
    log_version(&F.fg, "frame generation");
    debug_messages(&F.fg, FFX_API_EFFECT_ID_FRAMEGENERATION);
    return true;
}

bool fg_create_swapchain(VkSwapchainCreateInfoKHR *ci, VkSwapchainKHR *swapchain) {
    if (F.state <= 0 || !F.queues) return false;
    if (F.sc) {  // made again in place (window size)
        if (F.fg) { F.destroy(&F.fg, NULL); F.fg = NULL; }
        VkResult r = F.fn.pOutCreateSwapchainFFXAPI(g_vk.dev, ci, NULL, swapchain, F.sc);
        if (r != VK_SUCCESS) LOG("[fg] swapchain not made again: %d\n", r);
    } else {
        F.ext = ci->imageExtent;
        F.fmt = ci->imageFormat;
        if (!make_fg_context()) { F.state = -1; return false; }  // (no frames would be generated: Vulkan's swapchain)
        struct ffxCreateContextDescFrameGenerationSwapChainVersionVK ver;
        memset(&ver, 0, sizeof ver);
        ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_VK;
        ver.version = FFX_FRAMEGENERATION_SWAPCHAIN_VK_VERSION;
        struct ffxCreateContextDescFrameGenerationSwapChainVK d;
        memset(&d, 0, sizeof d);
        d.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK;
        d.header.pNext = &ver.header;
        d.physicalDevice = g_vk.phys;
        d.device = g_vk.dev;
        d.swapchain = swapchain;
        d.createInfo = *ci;
        d.gameQueue = (struct VkQueueInfoFFXAPI){g_vk.queue, g_vk.qfam, fg_game_submit};
        d.asyncComputeQueue = (struct VkQueueInfoFFXAPI){F.cq, F.cfam, NULL};
        d.presentQueue = (struct VkQueueInfoFFXAPI){F.pq, F.pfam, NULL};
        d.imageAcquireQueue = (struct VkQueueInfoFFXAPI){F.aq, F.afam, NULL};
        LOG("[fg] making the frame generation swapchain (thread %lu)\n", GetCurrentThreadId());
        ffxReturnCode_t r = F.create(&F.sc, &d.header, NULL);
        LOG("[fg] frame generation swapchain: %u\n", r);
        if (r != FFX_API_RETURN_OK) {
            LOG("[fg] frame generation off: the frame generation swapchain was not created (%u)\n", r);
            F.destroy(&F.fg, NULL);
            F.fg = NULL;
            F.sc = NULL;
            F.state = -1;
            return false;
        }
        memset(&F.fn, 0, sizeof F.fn);
        F.fn.header.type = FFX_API_QUERY_DESC_TYPE_FGSWAPCHAIN_FUNCTIONS_VK;
        F.query(&F.sc, &F.fn.header);
        log_version(&F.sc, "frame generation swapchain");
        debug_messages(&F.sc, FFX_API_EFFECT_ID_FRAMEGENERATIONSWAPCHAIN_VK);
        {   // Pacing: the swapchain spins on the CPU until each present's time and for GPU fences by default. The game
            // is CPU-bound on a handheld sharing its power between CPU and GPU (the spinning thread took 73% of a
            // core and the game's frame rate fell from 64 to 50): let it sleep (hybrid spin, fence waits).
            // AC_FG_SPIN=1: the default spinning.
            const char *sp = getenv("AC_FG_SPIN");
            static FfxApiSwapchainFramePacingTuning t = {0.1f, 0.1f, true, 2, true};
            if (sp && sp[0] == '1') { t.allowHybridSpin = false; t.allowWaitForSingleObjectOnFence = false; }
            struct ffxConfigureDescFrameGenerationSwapChainKeyValueVK kv;
            memset(&kv, 0, sizeof kv);
            kv.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_KEYVALUE_VK;
            kv.key = FFX_API_CONFIGURE_FG_SWAPCHAIN_KEY_FRAMEPACINGTUNING;
            kv.ptr = &t;
            ffxReturnCode_t tr = F.configure(&F.sc, &kv.header);
            LOG("[fg] pacing: %s (%u)\n", t.allowHybridSpin ? "hybrid spin, fence waits" : "spinning", tr);
        }
        return true;
    }
    F.ext = ci->imageExtent;  // (made again in place)
    F.fmt = ci->imageFormat;
    if (!make_fg_context()) LOG("[fg] (the swapchain stays; no frames are generated)\n");
    return true;
}

bool fg_active(void) { return F.sc != NULL; }
static int trace_calls = 3;  // (the first calls of each, with the thread: where a driver fault happens)
VkResult fg_get_images(uint32_t *count, VkImage *images) {
    uint32_t cap = *count, n = 0;  // (the usual two calls: how many, then those)
    VkResult r = F.fn.pOutGetSwapchainImagesKHR(g_vk.dev, g_vk.swapchain, &n, NULL);
    if (n > cap) n = cap;
    for (uint32_t i = 0; i < cap; i++) images[i] = VK_NULL_HANDLE;
    if (r == VK_SUCCESS) r = F.fn.pOutGetSwapchainImagesKHR(g_vk.dev, g_vk.swapchain, &n, images);
    *count = n;
    for (uint32_t i = 0; i < n; i++)
        if (!images[i]) { LOG("[fg] swapchain image %u is null\n", i); *count = i; break; }
    LOG("[fg] swapchain images: %d, %u of them\n", r, *count);
    return r;
}
VkResult fg_acquire(VkSemaphore sem, uint32_t *image) {
    static int n;
    if (n < trace_calls) LOG("[fg] acquire (thread %lu)\n", GetCurrentThreadId());
    VkResult r = F.fn.pOutAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, UINT64_MAX, sem, VK_NULL_HANDLE, image);
    if (n++ < trace_calls) LOG("[fg] acquire: %d, image %u\n", r, *image);
    return r;
}
VkResult fg_present(const VkPresentInfoKHR *pi) {
    static int n;
    if (n < trace_calls) LOG("[fg] present (thread %lu)\n", GetCurrentThreadId());
    VkResult r = F.fn.pOutQueuePresentKHR(g_vk.queue, pi);
    if (n++ < trace_calls) LOG("[fg] present: %d\n", r);
    return r;
}
void fg_wait_presents(void) {
    if (!F.sc) return;
    struct ffxDispatchDescFrameGenerationSwapChainWaitForPresentsVK w;
    memset(&w, 0, sizeof w);
    w.header.type = FFX_API_DISPATCH_DESC_TYPE_FGSWAPCHAIN_WAIT_FOR_PRESENTS_VK;
    F.dispatch(&F.sc, &w.header);
}

// ---- per frame inputs
typedef struct {
    const void *key;              // the game's composite target
    TexBackend color, mv, depth;  // copies
    uint64_t seq;                 // capture number (0: none)
    float near_, far_, tan_y;
} Slot;
#define SLOTS 4
static Slot slots[SLOTS];
static uint64_t cap_seq;
static float cam_near = 0.1f, cam_far = 10000.0f, cam_tan_y = 0.4877f;
static TexBackend hudless;  // window-sized

void fg_note_camera(float near_, float far_, float tan_half_fov_y) {
    if (near_ > 0 && far_ > near_ && tan_half_fov_y > 0.01f && tan_half_fov_y < 10) {
        cam_near = near_;
        cam_far = far_;
        cam_tan_y = tan_half_fov_y;
    }
}

static void copy_image(VkCommandBuffer cb, const TexBackend *s, const TexBackend *d, VkImageAspectFlags aspect) {
    VkImageCopy c = {.srcSubresource = {aspect, 0, 0, 1}, .dstSubresource = {aspect, 0, 0, 1},
                     .extent = {d->width, d->height, 1}};
    vkCmdCopyImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
}

void fg_capture(VkCommandBuffer cb, const void *target, TexBackend *color, TexBackend *mv, TexBackend *depth) {
    if (!F.fg || !target || !color || !color->image || !mv || !mv->image || !depth || !depth->image) return;
    Slot *s = NULL;
    for (int i = 0; i < SLOTS && !s; i++) if (slots[i].key == target) s = &slots[i];
    for (int i = 0; i < SLOTS && !s; i++) if (!slots[i].key) s = &slots[i];
    if (!s) { s = &slots[0]; for (int i = 1; i < SLOTS; i++) if (slots[i].seq < s->seq) s = &slots[i]; }
    s->key = target;
    const VkImageUsageFlags u = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (!own_image(&s->color, cb, color->format, color->width, color->height, u) ||
        !own_image(&s->mv, cb, mv->format, mv->width, mv->height, u) ||
        !own_image(&s->depth, cb, depth->format, depth->width, depth->height, u | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
        s->seq = 0;
        return;
    }
    vk_full_barrier(cb);
    copy_image(cb, color, &s->color, VK_IMAGE_ASPECT_COLOR_BIT);
    copy_image(cb, mv, &s->mv, VK_IMAGE_ASPECT_COLOR_BIT);
    copy_image(cb, depth, &s->depth, VK_IMAGE_ASPECT_DEPTH_BIT);
    vk_full_barrier(cb);
    s->seq = ++cap_seq;
    s->near_ = cam_near;
    s->far_ = cam_far;
    s->tan_y = cam_tan_y;
}

// (exec.c) the present shader drawing b into (vx, vy, vw, vh) of target, cleared to black, without post-processing
void vk_present_draw_raw(VkCommandBuffer cb, TexBackend *b, VkImageView target, VkExtent2D ext, float vx, float vy, float vw,
                         float vh, const float pv[8]);

void fg_present_record(VkCommandBuffer cb, const void *presented, TexBackend *presented_b, float vx, float vy, float vw,
                       float vh, const float pv[8]) {
    if (!F.sc || !F.fg) return;
    F.frame_id++;
    Slot *s = NULL;
    for (int i = 0; i < SLOTS && !s; i++) if (slots[i].key && slots[i].key == presented && slots[i].seq) s = &slots[i];
    static uint64_t last_seq;
    bool on = s && s->seq != last_seq && presented_b && s->color.width == presented_b->width &&
              s->color.height == presented_b->height;
    if (on) last_seq = s->seq;
    if (on) {  // the image before the HUD, where the present draws the game image
        static const VkComponentMapping id = {0};
        on = own_image(&hudless, cb, g_vk.sc_format, g_vk.sc_extent.width, g_vk.sc_extent.height,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        VkImageView v = on ? vk_image_view(&hudless, VK_IMAGE_VIEW_TYPE_2D, hudless.format, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT)
                           : VK_NULL_HANDLE;
        if (v) vk_present_draw_raw(cb, &s->color, v, g_vk.sc_extent, vx, vy, vw, vh, pv);
        else on = false;
    }
    static LARGE_INTEGER qf, last;
    LARGE_INTEGER now;
    if (!qf.QuadPart) QueryPerformanceFrequency(&qf);
    QueryPerformanceCounter(&now);
    float ms = last.QuadPart ? (float)((double)(now.QuadPart - last.QuadPart) * 1000.0 / (double)qf.QuadPart) : 16.7f;
    last = now;

    struct ffxConfigureDescFrameGeneration c;
    memset(&c, 0, sizeof c);
    c.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    c.swapChain = (void *)g_vk.swapchain;
    c.frameGenerationCallback = fg_dispatch_cb;
    c.frameGenerationCallbackUserContext = &F.fg;
    c.frameGenerationEnabled = on;
    c.allowAsyncWorkloads = (F.create_flags & FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT) != 0;
    if (on) c.HUDLessColor = res(&hudless);
    c.flags = F.debug_flags;
    c.onlyPresentGenerated = false;
    c.generationRect.left = (int32_t)lroundf(vx);
    c.generationRect.top = (int32_t)lroundf(vy);
    c.generationRect.width = (int32_t)lroundf(vw);
    c.generationRect.height = (int32_t)lroundf(vh);
    c.frameID = F.frame_id;
    ffxReturnCode_t rc = F.configure(&F.fg, &c.header);
    ffxReturnCode_t rp = FFX_API_RETURN_OK;
    if (on && rc == FFX_API_RETURN_OK) {
        struct ffxDispatchDescFrameGenerationPrepareV2 p;
        memset(&p, 0, sizeof p);
        p.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
        p.frameID = F.frame_id;
        p.flags = F.debug_flags;
        p.commandList = (void *)cb;
        p.renderSize.width = s->mv.width;
        p.renderSize.height = s->mv.height;
        // mv_img: ndc_now - ndc_prev, NDC y up; the SDK's: pixels from the current position to the previous one
        p.motionVectorScale.x = -0.5f * (float)s->mv.width;
        p.motionVectorScale.y = 0.5f * (float)s->mv.height;
        p.frameTimeDelta = ms;
        p.cameraNear = s->near_;
        p.cameraFar = s->far_;
        p.cameraFovAngleVertical = 2.0f * atanf(s->tan_y);
        p.viewSpaceToMetersFactor = 1.0f;
        p.depth = res(&s->depth);
        p.motionVectors = res(&s->mv);
        rp = F.dispatch(&F.fg, &p.header);
    }
    static uint64_t n_on, n_off, logged_at;
    static int errs;
    if (on) n_on++;
    else n_off++;
    if ((rc != FFX_API_RETURN_OK || rp != FFX_API_RETURN_OK) && errs++ < 10)
        LOG("[fg] configure %u, prepare %u (frame %llu)\n", rc, rp, (unsigned long long)F.frame_id);
    if (F.frame_id - logged_at >= 600) {
        uint64_t shown = F.fn.pOutGetLastPresentCountFFXAPI ? F.fn.pOutGetLastPresentCountFFXAPI(g_vk.swapchain) : 0;
        static uint64_t shown_at;
        LOG("[fg] frames %llu: generation on for %llu, off for %llu; presents %llu (%llu since)\n",
            (unsigned long long)F.frame_id, (unsigned long long)n_on, (unsigned long long)n_off, (unsigned long long)shown,
            (unsigned long long)(shown - shown_at));
        shown_at = shown;
        logged_at = F.frame_id;
        n_on = n_off = 0;
    }
}
