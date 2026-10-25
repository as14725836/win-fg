// record_apicount — offline harness that measures what FrameGen::record() emits.
//
// Why this exists: there is no Android GPU inside CI, and a software Vulkan
// (lavapipe) can only prove "the commands record without crashing". The two
// per-frame optimizations in record_impl.inc (pipeline-bind cache, merged
// initial barrier, coarsest-only flow clear, lazy flowLvl_ allocation) are all
// *emit-side* wins, so the honest thing to measure is the exact number of API
// calls and the exact number of bytes of device memory. Both are observable
// without executing a single shader:
//
//   * record() only needs the vkCmd* entry points to exist — it never submits,
//     so no pipeline ever runs and no pixel is produced here.
//   * allocation size is read from vkGetImageMemoryRequirements, which is also
//     a pure query.
//
// Build (needs a Vulkan loader + a physical device; lavapipe is fine):
//     cmake -DWINFG_BUILD_TESTS=ON ... && cmake --build build
//     VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
//         ./build/record_apicount 1920 1080 4 1
//
// The last two arguments are model (0..4) and perf_preset (0..2). Set
// DUMP_TRANS=1 to print the per-image layout-transition table, and FRAMES=n to
// record n frames instead of 1 (the counters are cumulative).
#include <vulkan/vulkan.h>
#include "framegen.hpp"
#include "config.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <utility>
#include <vector>

using namespace winfg;

#define VKOK(x) do{ VkResult r_=(x); if(r_!=VK_SUCCESS){ \
    printf("VKFAIL %s -> %d\n", #x, (int)r_); exit(1);} }while(0)

static InstanceDispatch g_id{};
static DeviceDispatch   g_dd{};

struct Cnt {
    unsigned long long bindPipe=0, bindSets=0, dispatches=0;
    unsigned long long barrierCalls=0, barriers=0, clears=0, copies=0;
    unsigned long long imgCreates=0, imgBytes=0, bufCreates=0, bufBytes=0;
    std::vector<VkImage> order;                                  // creation order
    std::map<VkImage, std::map<std::pair<int,int>,int>> trans;    // [img][(from,to)] -> n
} C;

static PFN_vkCmdBindPipeline       r_bindPipe=nullptr;
static PFN_vkCmdBindDescriptorSets r_bindSets=nullptr;
static PFN_vkCmdDispatch           r_dispatch=nullptr;
static PFN_vkCmdPipelineBarrier    r_barrier=nullptr;
static PFN_vkCmdClearColorImage    r_clear=nullptr;
static PFN_vkCmdCopyImage          r_copyImage=nullptr;
static PFN_vkCreateImage           r_createImage=nullptr;
static PFN_vkGetImageMemoryRequirements r_imgReq=nullptr;
static PFN_vkCreateBuffer          r_createBuffer=nullptr;
static PFN_vkGetBufferMemoryRequirements r_bufReq=nullptr;

static void VKAPI_CALL k_bindPipe(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p){
    ++C.bindPipe; r_bindPipe(cb,bp,p);
}
static void VKAPI_CALL k_bindSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout pl,
        uint32_t first, uint32_t n, const VkDescriptorSet* s, uint32_t dn, const uint32_t* d){
    ++C.bindSets; r_bindSets(cb,bp,pl,first,n,s,dn,d);
}
static void VKAPI_CALL k_dispatch(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z){
    ++C.dispatches; r_dispatch(cb,x,y,z);
}
static void VKAPI_CALL k_barrier(VkCommandBuffer cb, VkPipelineStageFlags s, VkPipelineStageFlags d,
        VkDependencyFlags f, uint32_t mc, const VkMemoryBarrier* m, uint32_t bc,
        const VkBufferMemoryBarrier* b, uint32_t ic, const VkImageMemoryBarrier* i){
    ++C.barrierCalls; C.barriers += ic;
    for (uint32_t k = 0; k < ic; ++k)
        C.trans[i[k].image][{(int)i[k].oldLayout, (int)i[k].newLayout}]++;
    r_barrier(cb,s,d,f,mc,m,bc,b,ic,i);
}
static void VKAPI_CALL k_clear(VkCommandBuffer cb, VkImage im, VkImageLayout l,
        const VkClearColorValue* c, uint32_t n, const VkImageSubresourceRange* r){
    ++C.clears; r_clear(cb,im,l,c,n,r);
}
static void VKAPI_CALL k_copyImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d,
        VkImageLayout dl, uint32_t n, const VkImageCopy* r){
    ++C.copies; r_copyImage(cb,s,sl,d,dl,n,r);
}
static VkResult VKAPI_CALL k_createImage(VkDevice dv, const VkImageCreateInfo* ci,
        const VkAllocationCallbacks* ac, VkImage* out){
    VkResult r = r_createImage(dv, ci, ac, out);
    if (r == VK_SUCCESS) { ++C.imgCreates; C.order.push_back(*out); }
    return r;
}
static void VKAPI_CALL k_imgReq(VkDevice dv, VkImage im, VkMemoryRequirements* mr){
    r_imgReq(dv, im, mr); C.imgBytes += mr->size;
}
static VkResult VKAPI_CALL k_createBuffer(VkDevice dv, const VkBufferCreateInfo* bi,
        const VkAllocationCallbacks* ac, VkBuffer* out){
    VkResult r = r_createBuffer(dv, bi, ac, out);
    if (r == VK_SUCCESS) ++C.bufCreates;
    return r;
}
static void VKAPI_CALL k_bufReq(VkDevice dv, VkBuffer b, VkMemoryRequirements* mr){
    r_bufReq(dv, b, mr); C.bufBytes += mr->size;
}

struct Dev { VkInstance inst; VkPhysicalDevice phys; VkDevice dev; VkQueue queue;
             uint32_t qfam; VkCommandPool pool; };

static uint32_t memType(VkPhysicalDevice phys, uint32_t bits, VkMemoryPropertyFlags want){
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(phys,&mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return 0;
}

static Dev makeDevice(){
    Dev d{};
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &ai;
    VKOK(vkCreateInstance(&ci,nullptr,&d.inst));

    uint32_t n = 0; vkEnumeratePhysicalDevices(d.inst,&n,nullptr);
    if (!n) { printf("no physical device\n"); exit(1); }
    std::vector<VkPhysicalDevice> pds(n); vkEnumeratePhysicalDevices(d.inst,&n,pds.data());
    d.phys = pds[0];

    uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(d.phys,&qn,nullptr);
    std::vector<VkQueueFamilyProperties> qs(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(d.phys,&qn,qs.data());
    d.qfam = UINT32_MAX;
    for (uint32_t i = 0; i < qn; ++i) if (qs[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { d.qfam=i; break; }
    if (d.qfam == UINT32_MAX) { printf("no compute queue\n"); exit(1); }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex=d.qfam; qi.queueCount=1; qi.pQueuePriorities=&prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount=1; dci.pQueueCreateInfos=&qi;
    VKOK(vkCreateDevice(d.phys,&dci,nullptr,&d.dev));
    vkGetDeviceQueue(d.dev,d.qfam,0,&d.queue);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex=d.qfam; pci.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VKOK(vkCreateCommandPool(d.dev,&pci,nullptr,&d.pool));

    g_id.GetInstanceProcAddr = vkGetInstanceProcAddr;
    g_id.DestroyInstance = vkDestroyInstance;
    g_id.GetPhysicalDeviceMemoryProperties = vkGetPhysicalDeviceMemoryProperties;
    g_id.GetPhysicalDeviceQueueFamilyProperties = vkGetPhysicalDeviceQueueFamilyProperties;
    g_id.GetPhysicalDeviceProperties = vkGetPhysicalDeviceProperties;

    // Field names mirror vk_dispatch.hpp (they carry no "vk" prefix).
#define L(field) g_dd.field = (PFN_vk##field)vkGetDeviceProcAddr(d.dev, "vk" #field)
    L(GetDeviceProcAddr); L(DestroyDevice); L(GetDeviceQueue); L(QueueSubmit); L(DeviceWaitIdle);
    L(CreateImage); L(DestroyImage); L(CreateImageView); L(DestroyImageView);
    L(AllocateMemory); L(FreeMemory); L(BindImageMemory); L(GetImageMemoryRequirements);
    L(CreateBuffer); L(DestroyBuffer); L(GetBufferMemoryRequirements); L(BindBufferMemory);
    L(MapMemory); L(UnmapMemory); L(CreateSampler); L(DestroySampler);
    L(CreateShaderModule); L(DestroyShaderModule);
    L(CreateDescriptorSetLayout); L(DestroyDescriptorSetLayout);
    L(CreatePipelineLayout); L(DestroyPipelineLayout);
    L(CreateComputePipelines); L(DestroyPipeline);
    L(CreateDescriptorPool); L(DestroyDescriptorPool); L(ResetDescriptorPool);
    L(AllocateDescriptorSets); L(UpdateDescriptorSets);
    L(CreateCommandPool); L(DestroyCommandPool);
    L(AllocateCommandBuffers); L(FreeCommandBuffers);
    L(BeginCommandBuffer); L(EndCommandBuffer);
    L(CmdBindPipeline); L(CmdBindDescriptorSets); L(CmdDispatch); L(CmdPipelineBarrier);
    L(CmdCopyImage); L(CmdCopyImageToBuffer); L(CmdClearColorImage);
    L(CreateFence); L(DestroyFence); L(WaitForFences); L(ResetFences);
#undef L
    return d;
}

struct Tex { VkImage img; VkDeviceMemory mem; VkImageView view; };

static Tex makeTex(Dev& d, uint32_t W, uint32_t H, VkFormat fmt, VkImageUsageFlags usage){
    Tex t{};
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType=VK_IMAGE_TYPE_2D; ici.format=fmt; ici.extent={W,H,1};
    ici.mipLevels=1; ici.arrayLayers=1; ici.samples=VK_SAMPLE_COUNT_1_BIT;
    ici.tiling=VK_IMAGE_TILING_OPTIMAL; ici.usage=usage;
    ici.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    VKOK(vkCreateImage(d.dev,&ici,nullptr,&t.img));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(d.dev,t.img,&mr);
    VkMemoryAllocateInfo ami{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ami.allocationSize=mr.size;
    ami.memoryTypeIndex=memType(d.phys,mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VKOK(vkAllocateMemory(d.dev,&ami,nullptr,&t.mem));
    VKOK(vkBindImageMemory(d.dev,t.img,t.mem,0));
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image=t.img; vi.viewType=VK_IMAGE_VIEW_TYPE_2D; vi.format=fmt;
    vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VKOK(vkCreateImageView(d.dev,&vi,nullptr,&t.view));
    return t;
}

int main(int argc, char** argv){
    const int W      = (argc > 1) ? atoi(argv[1]) : 320;
    const int H      = (argc > 2) ? atoi(argv[2]) : 180;
    const int model  = (argc > 3) ? atoi(argv[3]) : 4;
    const int preset = (argc > 4) ? atoi(argv[4]) : 1;
    const int frames = getenv("FRAMES") ? atoi(getenv("FRAMES")) : 1;

    Dev d = makeDevice();

    // Swap the counters in after the device exists but before FrameGen::init, so
    // the per-resolution allocations are counted too.
    r_bindPipe=g_dd.CmdBindPipeline;          g_dd.CmdBindPipeline=k_bindPipe;
    r_bindSets=g_dd.CmdBindDescriptorSets;    g_dd.CmdBindDescriptorSets=k_bindSets;
    r_dispatch=g_dd.CmdDispatch;              g_dd.CmdDispatch=k_dispatch;
    r_barrier=g_dd.CmdPipelineBarrier;        g_dd.CmdPipelineBarrier=k_barrier;
    r_clear=g_dd.CmdClearColorImage;          g_dd.CmdClearColorImage=k_clear;
    r_copyImage=g_dd.CmdCopyImage;            g_dd.CmdCopyImage=k_copyImage;
    r_createImage=g_dd.CreateImage;           g_dd.CreateImage=k_createImage;
    r_imgReq=g_dd.GetImageMemoryRequirements; g_dd.GetImageMemoryRequirements=k_imgReq;
    r_createBuffer=g_dd.CreateBuffer;         g_dd.CreateBuffer=k_createBuffer;
    r_bufReq=g_dd.GetBufferMemoryRequirements; g_dd.GetBufferMemoryRequirements=k_bufReq;

    Config cfg;
    cfg.enabled = true; cfg.model = model; cfg.perfPreset = preset;
    cfg.gmMode = 2;   // C1 off: isolate the flow+synth graph
    cfg.frMode = 2;   // C2 off
    cfg.sanitize();

    FrameGen fg;
    if (!fg.init(&g_dd,&g_id,d.phys,d.dev,d.qfam,d.queue)) { printf("init FAILED\n"); return 1; }
    fg.configure(cfg);
    if (!fg.onResize({(uint32_t)W,(uint32_t)H}, VK_FORMAT_R8G8B8A8_UNORM)) {
        printf("onResize FAILED\n"); return 1;
    }

    printf("ALLOC images=%llu bytes=%llu (%.2f MB)  buffers=%llu bytes=%llu\n",
           C.imgCreates, C.imgBytes, (double)C.imgBytes/1048576.0, C.bufCreates, C.bufBytes);
    // Reset the record-side counters so they cover record() only.
    C.bindPipe=C.bindSets=C.dispatches=0;
    C.barrierCalls=C.barriers=C.clears=C.copies=0;

    const VkFormat FMT = VK_FORMAT_R8G8B8A8_UNORM;
    Tex prev = makeTex(d,W,H,FMT,VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    Tex curr = makeTex(d,W,H,FMT,VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    Tex out  = makeTex(d,W,H,FMT,VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool=d.pool; cai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount=1;
    VkCommandBuffer cb; VKOK(vkAllocateCommandBuffers(d.dev,&cai,&cb));

    for (int fi = 0; fi < frames; ++fi) {
        VkCommandBufferBeginInfo bbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VKOK(vkBeginCommandBuffer(cb,&bbi));
        fg.record(cb, prev.view, curr.view, out.view, 0.5f, 0);
        VKOK(vkEndCommandBuffer(cb));
        VKOK(vkResetCommandBuffer(cb,0));
    }

    printf("RESULT W=%d H=%d model=%d preset=%d flowFinest=%d frames=%d\n",
           W, H, model, preset, fg.flowFinest(), frames);
    printf("  CmdBindPipeline      = %llu\n", C.bindPipe);
    printf("  CmdBindDescriptorSets= %llu\n", C.bindSets);
    printf("  CmdDispatch          = %llu\n", C.dispatches);
    printf("  CmdPipelineBarrier   = %llu (carrying %llu image barriers)\n",
           C.barrierCalls, C.barriers);
    printf("  CmdClearColorImage   = %llu\n", C.clears);
    printf("  CmdCopyImage         = %llu\n", C.copies);

    if (getenv("DUMP_TRANS")) {
        printf("TRANSITIONS (img# from->to xN; <none> = allocated but never transitioned):\n");
        for (size_t k = 0; k < C.order.size(); ++k) {
            auto it = C.trans.find(C.order[k]);
            if (it == C.trans.end()) { printf("  img%-2zu : <none>\n", k); continue; }
            printf("  img%-2zu :", k);
            for (auto& e : it->second)
                printf(" %d->%d x%d;", e.first.first, e.first.second, e.second);
            printf("\n");
        }
    }

    fg.destroy();
    printf("OK\n");
    return 0;
}