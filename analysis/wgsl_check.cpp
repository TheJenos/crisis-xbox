// Validates translated shaders with wgpu-native (naga), like the browser's
// shader-check page does with Tint: every <dir>/*.wgsl file is compiled and a
// render pipeline is built for it with a pass-through shader for the other stage.
//
// usage: wgsl_check <dir>
#include <webgpu.h>
#include <wgpu.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static WGPUStringView Str(const char* s) { return { s, strlen(s) }; }
static WGPUAdapter g_adapter;
static WGPUDevice g_device;
static std::string g_error;

static void OnAdapter(WGPURequestAdapterStatus status, WGPUAdapter a, WGPUStringView, void* done, void*)
{
    if (status == WGPURequestAdapterStatus_Success) g_adapter = a;
    *(bool*)done = true;
}
static void OnDevice(WGPURequestDeviceStatus status, WGPUDevice d, WGPUStringView, void* done, void*)
{
    if (status == WGPURequestDeviceStatus_Success) g_device = d;
    *(bool*)done = true;
}
static void OnError(WGPUDevice const*, WGPUErrorType, WGPUStringView msg, void*, void*)
{
    if (g_error.empty()) g_error.assign(msg.data ? msg.data : "", msg.length);
}

static WGPUShaderModule Module(const std::string& code)
{
    WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
    wgsl.code = { code.data(), code.size() };
    WGPUShaderModuleDescriptor smd = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    smd.nextInChain = &wgsl.chain;
    return wgpuDeviceCreateShaderModule(g_device, &smd);
}

int main(int argc, char** argv)
{
    if (argc < 2) { printf("usage: wgsl_check <dir>\n"); return 1; }
    WGPUInstance instance = wgpuCreateInstance(nullptr);
    bool done = false;
    WGPURequestAdapterOptions ao = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
    WGPURequestAdapterCallbackInfo aci = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
    aci.mode = WGPUCallbackMode_AllowProcessEvents;
    aci.callback = OnAdapter;
    aci.userdata1 = &done;
    wgpuInstanceRequestAdapter(instance, &ao, aci);
    while (!done) wgpuInstanceProcessEvents(instance);
    if (!g_adapter) { printf("no adapter\n"); return 1; }
    done = false;
    WGPUDeviceDescriptor dd = WGPU_DEVICE_DESCRIPTOR_INIT;
    dd.uncapturedErrorCallbackInfo.callback = OnError;
    WGPURequestDeviceCallbackInfo dci = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
    dci.mode = WGPUCallbackMode_AllowProcessEvents;
    dci.callback = OnDevice;
    dci.userdata1 = &done;
    wgpuAdapterRequestDevice(g_adapter, &dd, dci);
    while (!done) wgpuInstanceProcessEvents(instance);
    if (!g_device) { printf("no device\n"); return 1; }

    std::string vsOut = "struct O { @builtin(position) p: vec4f,";
    for (int i = 0; i < 16; i++) vsOut += " @location(" + std::to_string(i) + ") i" + std::to_string(i) + ": vec4f,";
    vsOut += " }\n@vertex fn main() -> O { var o: O; return o; }\n";
    WGPUShaderModule passVs = Module(vsOut);
    WGPUShaderModule passPs = Module("@fragment fn main() -> @location(0) vec4f { return vec4f(1.0); }\n");

    int ok = 0, bad = 0;
    for (auto& e : std::filesystem::directory_iterator(argv[1]))
    {
        if (e.path().extension() != ".wgsl") continue;
        std::string name = e.path().stem().string();
        std::stringstream ss;
        ss << std::ifstream(e.path(), std::ios::binary).rdbuf();
        std::string code = ss.str();
        bool pixel = name.rfind("ps_", 0) == 0;
        g_error.clear();
        WGPUShaderModule m = Module(code);
        if (g_error.empty())
        {
            WGPUColorTargetState targets[4];
            size_t count = 0;
            for (int i = 0; i < 4; i++)
            {
                targets[i] = WGPU_COLOR_TARGET_STATE_INIT;
                targets[i].format = WGPUTextureFormat_RGBA8Unorm;
                if (!pixel ? i == 0 : code.find("@location(" + std::to_string(i) + ") c" + std::to_string(i) + ":") != std::string::npos)
                    count = i + 1;
                else
                    targets[i].format = WGPUTextureFormat_Undefined;
            }
            WGPUFragmentState fs = WGPU_FRAGMENT_STATE_INIT;
            fs.module = pixel ? m : passPs;
            fs.entryPoint = Str("main");
            fs.targetCount = count;
            fs.targets = targets;
            WGPUDepthStencilState ds = WGPU_DEPTH_STENCIL_STATE_INIT;
            ds.format = WGPUTextureFormat_Depth32Float;
            ds.depthWriteEnabled = WGPUOptionalBool_True;
            ds.depthCompare = WGPUCompareFunction_Always;
            WGPURenderPipelineDescriptor pd = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
            pd.vertex.module = pixel ? passVs : m;
            pd.vertex.entryPoint = Str("main");
            pd.fragment = &fs;
            if (code.find("frag_depth") != std::string::npos || (pixel && count == 0))
                pd.depthStencil = &ds;
            WGPURenderPipeline p = wgpuDeviceCreateRenderPipeline(g_device, &pd);
            if (p) wgpuRenderPipelineRelease(p);
        }
        if (m) wgpuShaderModuleRelease(m);
        wgpuInstanceProcessEvents(instance);
        if (g_error.empty())
            ok++;
        else
        {
            bad++;
            if (bad <= 8)
                printf("FAIL %s:\n%s\n", name.c_str(), g_error.substr(0, 600).c_str());
        }
    }
    printf("%d ok, %d failed\n", ok, bad);
    return bad ? 2 : 0;
}
