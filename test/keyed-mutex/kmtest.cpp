// Keyed mutex stress test for DXVK_KEYED_MUTEX_BLOCKING A/B comparisons.
//
// Two D3D11 devices in one process share a texture through a keyed mutex
// (the same MiscFlags AVPro Video uses: SHARED_KEYEDMUTEX | SHARED_NTHANDLE).
// The producer queues a lot of GPU work, then writes a frame number into the
// shared texture and releases the mutex. The consumer acquires it, copies the
// texture to a staging texture and checks that it sees the same frame number.
//
// Reports how long ReleaseSync/AcquireSync block the calling thread, and how many
// frames the consumer saw stale data in (a missing GPU-side wait shows up here).
//
// Usage: kmtest.exe [frames=300] [load=20] [size=1920]
//   load = amount of GPU work (64MB copies) the producer queues per frame
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <algorithm>

#define CHECK(x) do { HRESULT _hr = (x); if (FAILED(_hr)) { \
  printf("FAILED %s -> 0x%08lx (line %d)\n", #x, (unsigned long)_hr, __LINE__); return 1; } } while (0)

static double now_ms() {
  static LARGE_INTEGER f;
  if (!f.QuadPart) QueryPerformanceFrequency(&f);
  LARGE_INTEGER t; QueryPerformanceCounter(&t);
  return 1000.0 * double(t.QuadPart) / double(f.QuadPart);
}

static void wait_gpu(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
  D3D11_QUERY_DESC qd = { D3D11_QUERY_EVENT, 0 };
  ID3D11Query* q = nullptr;
  if (FAILED(dev->CreateQuery(&qd, &q))) return;
  ctx->End(q);
  ctx->Flush();
  BOOL done = FALSE;
  while (ctx->GetData(q, &done, sizeof(done), 0) != S_OK || !done) Sleep(0);
  q->Release();
}

struct Stat {
  std::vector<double> v;
  void add(double x) { v.push_back(x); }
  void print(const char* name) {
    if (v.empty()) return;
    std::sort(v.begin(), v.end());
    double sum = 0; for (double x : v) sum += x;
    printf("  %-22s avg %7.3f ms   p95 %7.3f ms   max %7.3f ms\n",
      name, sum / v.size(), v[size_t(v.size() * 0.95)], v.back());
  }
};

static int make_device(IDXGIAdapter* adapter, ID3D11Device** dev, ID3D11DeviceContext** ctx) {
  D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
    nullptr, 0, &fl, 1, D3D11_SDK_VERSION, dev, nullptr, ctx));
  return 0;
}

int main(int argc, char** argv) {
  // A console window under Proton can close before its output is readable, so
  // everything is written to kmtest-<mode>.log next to the exe (run.sh prints it).
  {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) slash[1] = 0;
    const char* m = getenv("DXVK_KEYED_MUTEX_BLOCKING");
    strcat(path, (m && m[0] == '1') ? "kmtest-blocking.log" : "kmtest-fixed.log");
    if (!freopen(path, "w", stdout)) return 3;
    setvbuf(stdout, nullptr, _IONBF, 0);
  }
  printf("kmtest started\n");
  int frames = argc > 1 ? atoi(argv[1]) : 300;
  int load   = argc > 2 ? atoi(argv[2]) : 20;
  int size   = argc > 3 ? atoi(argv[3]) : 1920;
  const char* mode = getenv("DXVK_KEYED_MUTEX_BLOCKING");
  printf("kmtest: %d frames, load=%d, %dx%d, DXVK_KEYED_MUTEX_BLOCKING=%s\n",
    frames, load, size, size * 9 / 16, mode ? mode : "(unset)");

  IDXGIFactory1* factory; CHECK(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory));
  IDXGIAdapter* adapter; CHECK(factory->EnumAdapters(0, &adapter));

  ID3D11Device *devA, *devB; ID3D11DeviceContext *ctxA, *ctxB;
  if (make_device(adapter, &devA, &ctxA) || make_device(adapter, &devB, &ctxB)) return 1;

  int w = size, h = size * 9 / 16;
  D3D11_TEXTURE2D_DESC d = {};
  d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
  d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

  ID3D11Texture2D* sharedA; CHECK(devA->CreateTexture2D(&d, nullptr, &sharedA));
  IDXGIResource1* res; CHECK(sharedA->QueryInterface(__uuidof(IDXGIResource1), (void**)&res));
  HANDLE handle;
  CHECK(res->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle));
  ID3D11Device1* devB1; CHECK(devB->QueryInterface(__uuidof(ID3D11Device1), (void**)&devB1));
  ID3D11Texture2D* sharedB; CHECK(devB1->OpenSharedResource1(handle, __uuidof(ID3D11Texture2D), (void**)&sharedB));

  IDXGIKeyedMutex *kmA, *kmB;
  CHECK(sharedA->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&kmA));
  CHECK(sharedB->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&kmB));

  ID3D11RenderTargetView* rtv; CHECK(devA->CreateRenderTargetView(sharedA, nullptr, &rtv));

  D3D11_TEXTURE2D_DESC sd = d;
  sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.MiscFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Texture2D* staging; CHECK(devB->CreateTexture2D(&sd, nullptr, &staging));

  // Producer-side scratch textures for load: 4096x4096 RGBA8 = 64MB each.
  D3D11_TEXTURE2D_DESC ld = {};
  ld.Width = 4096; ld.Height = 4096; ld.MipLevels = 1; ld.ArraySize = 1;
  ld.Format = DXGI_FORMAT_R8G8B8A8_UNORM; ld.SampleDesc.Count = 1; ld.Usage = D3D11_USAGE_DEFAULT;
  ID3D11Texture2D *l0, *l1;
  CHECK(devA->CreateTexture2D(&ld, nullptr, &l0));
  CHECK(devA->CreateTexture2D(&ld, nullptr, &l1));

  // Sanity phase, no reliance on GPU synchronisation: write a value, wait for the GPU
  // to finish with an event query, hand over with the keyed mutex, read it back. If this
  // already fails, sharing itself does not work here and the results below mean nothing.
  int sanityGot = -1;
  {
    CHECK(kmA->AcquireSync(0, 5000));
    float c[4] = { 77.f / 255.f, 0.f, 0.f, 1.f };
    ctxA->ClearRenderTargetView(rtv, c);
    wait_gpu(devA, ctxA);
    CHECK(kmA->ReleaseSync(1));
    CHECK(kmB->AcquireSync(1, 5000));
    ctxB->CopyResource(staging, sharedB);
    D3D11_MAPPED_SUBRESOURCE m;
    CHECK(ctxB->Map(staging, 0, D3D11_MAP_READ, 0, &m));
    sanityGot = ((const unsigned char*)m.pData)[(h / 2) * m.RowPitch + (w / 2) * 4 + 2];
    ctxB->Unmap(staging, 0);
    CHECK(kmB->ReleaseSync(0));
    printf("sanity check (value 77 written, waited for GPU, read back): got %d -> %s\n",
      sanityGot, sanityGot == 77 ? "ok" : "SHARING DOES NOT WORK");
  }

  // How long does the producer's load take on the GPU?
  {
    wait_gpu(devA, ctxA);
    double t = now_ms();
    for (int k = 0; k < load; k++) ctxA->CopyResource((k & 1) ? l1 : l0, (k & 1) ? l0 : l1);
    wait_gpu(devA, ctxA);
    printf("GPU time of one frame's load: %.2f ms (should be several ms for the test to mean anything;\n"
           "  raise the 'load' argument if it is not)\n", now_ms() - t);
  }

  Stat prodAcquire, prodRelease, consAcquire, consTotal, producerFrame;
  int stale = 0, bad = 0;

  for (int i = 1; i <= frames; i++) {
    double t0 = now_ms();

    double a0 = now_ms();
    HRESULT hr = kmA->AcquireSync(0, 5000);
    prodAcquire.add(now_ms() - a0);
    if (hr != S_OK) { printf("producer AcquireSync failed 0x%08lx at frame %d\n", (unsigned long)hr, i); return 1; }

    // Busy GPU first, shared write last: if the consumer does not wait for the
    // producer's GPU work, it reads the previous frame's value.
    for (int k = 0; k < load; k++) ctxA->CopyResource((k & 1) ? l1 : l0, (k & 1) ? l0 : l1);
    float color[4] = { float(i & 0xff) / 255.f, 0.f, 0.f, 1.f };  // BGRA: written to R
    ctxA->ClearRenderTargetView(rtv, color);

    double r0 = now_ms();
    hr = kmA->ReleaseSync(1);
    prodRelease.add(now_ms() - r0);
    if (hr != S_OK) { printf("producer ReleaseSync failed 0x%08lx at frame %d\n", (unsigned long)hr, i); return 1; }
    producerFrame.add(now_ms() - t0);

    double c0 = now_ms();
    hr = kmB->AcquireSync(1, 5000);
    consAcquire.add(now_ms() - c0);
    if (hr != S_OK) { printf("consumer AcquireSync failed 0x%08lx at frame %d\n", (unsigned long)hr, i); return 1; }

    ctxB->CopyResource(staging, sharedB);
    D3D11_MAPPED_SUBRESOURCE m;
    CHECK(ctxB->Map(staging, 0, D3D11_MAP_READ, 0, &m));
    const unsigned char* px = (const unsigned char*)m.pData + (h / 2) * m.RowPitch + (w / 2) * 4;
    int got = px[2];  // BGRA: red
    ctxB->Unmap(staging, 0);
    if (got != (i & 0xff)) { if (got == ((i - 1) & 0xff)) stale++; else bad++; }

    hr = kmB->ReleaseSync(0);
    if (hr != S_OK) { printf("consumer ReleaseSync failed 0x%08lx at frame %d\n", (unsigned long)hr, i); return 1; }
    consTotal.add(now_ms() - c0);
  }

  printf("\nResults (%d frames):\n", frames);
  prodAcquire.print("producer AcquireSync");
  prodRelease.print("producer ReleaseSync");
  consAcquire.print("consumer AcquireSync");
  producerFrame.print("producer frame time");
  printf("\n  stale frames seen by consumer: %d\n  wrong pixel values:            %d\n", stale, bad);
  if (sanityGot != 77) {
    printf("\nINCONCLUSIVE: the shared texture did not carry data to the second device even with explicit\n"
           "GPU waits, so sharing itself is not working in this setup (check the DXVK log).\n");
    return 4;
  }
  printf("\n%s\n", (stale || bad) ? "FAIL: consumer saw data from before the producer's GPU work finished"
                                   : "PASS: consumer always saw the current frame");
  return (stale || bad) ? 2 : 0;
}
