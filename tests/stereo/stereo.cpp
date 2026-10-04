#include <nall/nall.hpp>
#include <n64/n64.hpp>
// termios also defines NCCS, which is a PlayStation GTE instruction name.
#undef NCCS
#include <ps1/ps1.hpp>
#include <nall/main.hpp>
#include <n64/vulkan/parallel-rdp/parallel-rdp/rdp_stereo.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace ares;

static auto require(bool condition, const char* message) -> void {
  if(!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

static auto testMath() -> void {
  Stereo::Settings settings;
  settings.depth = 100;
  settings.convergence = 10;
  require(settings.offset(0.1, 640, 0) == 0, "convergence plane has zero disparity");
  require(settings.offset(0.2, 640, 0) > 0, "near geometry has crossed parallax");
  require(settings.offset(0.05, 640, 0) < 0, "far geometry has uncrossed parallax");
  require(settings.offset(0, 640, 0) == 0, "missing depth remains flat");
  require(settings.offset(0.2, 640, 0) == -settings.offset(0.2, 640, 1), "eyes are symmetric");
  settings.depth = 0;
  require(settings.offset(0.2, 640, 0) == 0, "zero depth is mono");

  Stereo::ProjectionCache cache;
  cache.reset(true);
  cache.record(10, 20, 0.1f);
  require(cache.find(10, 20) > 0, "GTE projection is cached");
  require(cache.find(11, 20) == 0, "unmatched coordinate stays flat");
  cache.nextFrame();
  require(cache.find(10, 20) > 0, "buffered packets retain one previous frame");
  cache.nextFrame();
  require(cache.find(10, 20) == 0, "stale projections expire");
  cache.record(10, 20, 0.1f);
  cache.record(10, 20, 0.3f);
  require(cache.find(10, 20) == 0, "ambiguous projections are rejected");

  RDP::TriangleSetup triangle = {};
  triangle.xh = 40 * 32768;
  triangle.xm = 80 * 32768;
  triangle.xl = 60 * 32768;
  triangle.yh = 20 * 4;
  triangle.ym = 60 * 4;
  triangle.yl = 100 * 4;
  triangle.dxhdy = 4096;
  RDP::AttributeSetup attr = {};
  attr.z = int32_t(0.9 * 32767.0 * 65536.0);
  attr.dzdx = 100000;
  attr.dzdy = 200000;
  attr.dsdx = 65536;
  attr.dsdy = 32768;
  auto originalTriangle = triangle;
  auto originalAttr = attr;
  require(RDP::stereo_transform(triangle, attr, 0, 10), "zero-depth RDP transform succeeds");
  require(!std::memcmp(&triangle, &originalTriangle, sizeof(triangle)) &&
          !std::memcmp(&attr, &originalAttr, sizeof(attr)), "zero-depth RDP state is bit exact");
  require(RDP::stereo_transform(triangle, attr, 8, 10), "RDP depth shear succeeds");
  // A surface point must retain its texture coordinate after the change of basis.
  double dx = 23, dy = 31;
  double gain = 80.0 / (32767.0 * 65536.0);
  double newDx = dx * (1 + gain * originalAttr.dzdx) + dy * gain * originalAttr.dzdy;
  double before = originalAttr.dsdx * dx + originalAttr.dsdy * dy;
  double after = attr.dsdx * newDx + attr.dsdy * dy;
  require(std::abs(before - after) < 64, "RDP texture gradients follow stereo geometry");
  std::puts("PASS: stereo projection, cache and RDP interpolation");
}

static auto testPlayStation() -> void {
  using namespace ares::PlayStation;
  gpu.vram.allocate(1_MiB, 0);
  for(unsigned y = 0; y < 512; ++y) gpu.vram2D[y] = (u16*)(gpu.vram.data + y * 2048);
  gpu.generateTables();
  gpu.display.width = 320;
  gpu.stereo.enabled = true;
  gpu.stereo.depth = 100;
  gpu.stereo.convergence = 10;
  gpu.resetStereo();

  auto triangle = [&] {
    GPU::Render render{};
    render.command = 0x20;
    render.drawingAreaOriginX2 = 319;
    render.drawingAreaOriginY2 = 239;
    render.v0.setPoint(100, 50).setColor(255, 0, 0);
    render.v1.setPoint(160, 50).setColor(255, 0, 0);
    render.v2.setPoint(130, 100).setColor(255, 0, 0);
    return render;
  };
  auto render = triangle();
  for(auto v : {render.v0, render.v1, render.v2}) gpu.projections.record(v.x, v.y, 0.2f);
  auto mono = render;
  mono.execute();
  std::vector<u8> expected(gpu.vram.data, gpu.vram.data + 1_MiB);
  gpu.vram.fill(0);
  render.executeStereo();
  require(!std::memcmp(gpu.vram.data, expected.data(), expected.size()), "PS1 guest VRAM stays mono");
  auto left = std::vector<u8>(gpu.eyeVRAM[0].data, gpu.eyeVRAM[0].data + 1_MiB);
  auto right = std::vector<u8>(gpu.eyeVRAM[1].data, gpu.eyeVRAM[1].data + 1_MiB);
  require(left != right, "PS1 renders distinct eye views");
  require(gpu.eyeVRAM[0].readHalf((60 * 1024 + 155) * 2) != 0, "PS1 near polygon shifts right for left eye");
  require(gpu.eyeVRAM[1].readHalf((60 * 1024 + 155) * 2) == 0, "PS1 near polygon shifts left for right eye");
  gpu.stereo.swapEyes = true;
  for(auto& eye : gpu.eyeVRAM) eye.fill(0);
  render = triangle();
  render.executeStereo();
  require(!std::memcmp(gpu.eyeVRAM[0].data, right.data(), 1_MiB) &&
          !std::memcmp(gpu.eyeVRAM[1].data, left.data(), 1_MiB), "PS1 eye swap exchanges complete views");
  gpu.stereo.depth = 0;
  for(auto& eye : gpu.eyeVRAM) eye.fill(0);
  render = triangle();
  render.executeStereo();
  for(auto& eye : gpu.eyeVRAM) require(!std::memcmp(eye.data, expected.data(), 1_MiB), "PS1 zero-depth eyes equal mono");

  // CPU uploads must reach both auxiliary VRAMs, including mask semantics.
  gpu.io.mode = GPU::Mode::Normal;
  gpu.queue.gp0.reset();
  gpu.writeGP0(0xa0000000);
  gpu.writeGP0(20u << 16 | 10u);
  gpu.writeGP0(1u << 16 | 2u);
  gpu.writeGP0(0x03e0001f);
  for(auto& eye : gpu.eyeVRAM) require(eye.readWord((20 * 1024 + 10) * 2) == 0x03e0001f, "PS1 CPU uploads reach both eyes");
  gpu.writeGP0(0x80000000);
  gpu.writeGP0(20u << 16 | 10u);
  gpu.writeGP0(30u << 16 | 30u);
  gpu.writeGP0(1u << 16 | 2u);
  for(auto& eye : gpu.eyeVRAM) require(eye.readWord((30 * 1024 + 30) * 2) == 0x03e0001f, "PS1 VRAM copies reach both eyes");

  cpu.gte.constructTable();
  cpu.gte.rotation = {{4096, 0, 0}, {0, 4096, 0}, {0, 0, 4096}};
  cpu.gte.translation = {0, 0, 0};
  cpu.gte.h = 256;
  cpu.gte.ofx = 160 * 65536;
  cpu.gte.ofy = 120 * 65536;
  cpu.gte.v.a = {100, 0, 2560};
  cpu.RTPS(false, 12);
  require(gpu.projections.find(cpu.gte.screen[2].x, cpu.gte.screen[2].y) > 0,
          "actual GTE projection feeds stereo cache");
  gpu.resetStereo();
  require(gpu.projections.find(100, 50) == 0, "state-load reset discards stale PS1 depth");
  for(auto& eye : gpu.eyeVRAM) require(!std::memcmp(eye.data, gpu.vram.data, 1_MiB), "state-load reset seeds both PS1 eyes");

  // Exercise the real SBS blitter, including the last 24-bit pixel wrapping VRAM.
  gpu.screen = std::make_shared<ares::Core::Video::Screen>("Stereo test", 1280, 512);
  gpu.blitter.power();
  auto& blitter = gpu.blitter;
  blitter.blank = false;
  blitter.width = 320;
  blitter.height = 240;
  blitter.tx = blitter.ty = 0;
  blitter.tw = 2;
  blitter.th = 1;
  blitter.sx = 1023;
  blitter.sy = 511;
  blitter.depth = 0;
  for(unsigned eye = 0; eye < 2; ++eye) {
    blitter.eyeVRAM[eye].writeHalf(1_MiB - 2, eye ? 0x03e0 : 0x001f);
    blitter.eyeVRAM[eye].writeHalf(0, 0x7c00);
  }
  blitter.refresh();
  auto pixels = gpu.screen->pixels(1);
  require(pixels[0] == 0x100001f && pixels[320] == 0x10003e0,
          "PS1 blitter packs left then right using the screen canvas pitch");
  require(pixels[1] == 0x1007c00 && pixels[321] == 0x1007c00, "PS1 15-bit scanout wraps VRAM");
  blitter.depth = 1;
  for(auto& eye : blitter.eyeVRAM) {
    eye.writeByte(1_MiB - 2, 0x12);
    eye.writeByte(1_MiB - 1, 0x34);
    eye.writeByte(0, 0x56);
  }
  blitter.refresh();
  require(pixels[0] == 0x563412 && pixels[320] == 0x563412, "PS1 24-bit SBS scanout wraps each byte safely");
  gpu.screen.reset();
  blitter.vram.reset();
  for(auto& eye : blitter.eyeVRAM) eye.reset();
  gpu.vram.reset();
  for(auto& eye : gpu.eyeVRAM) eye.reset();
  std::puts("PASS: PS1 rasterization, guest isolation, eye swap, uploads, copies and GTE");
}

struct Scanout {
  unsigned width = 0, height = 0;
  std::vector<u8> rgba;
};

static auto renderNintendo64(unsigned depth, bool swap, unsigned upscale = 1) -> std::vector<Scanout> {
  using namespace ares::Nintendo64;
  auto root = std::make_shared<ares::Core::Object>("Stereo test");
  mi.load(root);
  rdram.ram.allocate(8_MiB, 0);
  rdram.mapIdentity = true;
  vulkan.stereo.enabled = true;
  vulkan.stereo.depth = depth;
  vulkan.stereo.convergence = 10;
  vulkan.stereo.swapEyes = swap;
  vulkan.stereo.fullWidth = upscale == 2;
  vulkan.internalUpscale = vulkan.outputUpscale = upscale;
  vulkan.disableVideoInterfaceProcessing = true;
  vulkan.load({});
  require(vulkan.stereoActive(), "Vulkan initializes both N64 eye processors");

  std::vector<u32> words;
  auto command = [&](u32 high, u32 low) { words.push_back(high); words.push_back(low); };
  command(0x3f100000 | 319, 0x10000); // RGBA5551 color image
  command(0x2d000000, (320 * 4) << 12 | (240 * 4));
  command(0x2f300000, 0); // fill cycle
  command(0x37000000, 0x00010001);
  command(0x36000000 | (319 * 4) << 12 | (239 * 4), 0);
  command(0x27000000, 0);
  command(0x2f0000f0, 0); // one cycle, no dither
  command(0x3c000000 | 8 << 20 | 16 << 15 | 7 << 12 | 7 << 9 | 8 << 5 | 16,
          8u << 28 | 8u << 24 | 7 << 21 | 7 << 18 | 3 << 15 | 7 << 12 | 3 << 9 | 3 << 6 | 7 << 3 | 3);
  command(0x3a000000, 0xff0000ff); // primitive red
  // Right triangle: (100,50), (160,50), (100,100); projected Z = .8.
  command(0x09800000 | 100 * 4, (50 * 4) << 16 | (50 * 4));
  command(160 << 16, u32(-78643)); // lower edge slope -1.2 pixels/line
  command(100 << 16, 0);          // major edge
  command(160 << 16, 0);          // upper edge (zero height)
  command(u32(0.8 * 32767.0 * 65536.0), 0);
  command(0, 0);
  command(0x29000000, 0);
  for(unsigned i = 0; i < words.size(); ++i) rdram.ram.write<Word>(i * 4, words[i], RBusDevice::PI_DMA);
  rdp.command = {};
  rdp.command.end = words.size() * 4;
  require(vulkan.render(), "N64 RDP command stream executes");
  require(!vulkan.crashed(), "N64 RDP did not report a crash");

  // Exercise CPU framebuffer uploads as well as rasterizer output.
  rdram.ram.write<Half>(0x10000 + (120 * 320 + 100) * 2, 0x07c1, RBusDevice::PI_DMA);
  vulkan.writeWord(0, 0x302);
  vulkan.writeWord(1, 0x10000);
  vulkan.writeWord(2, 320);
  vulkan.writeWord(6, 525);
  vulkan.writeWord(7, 3093);
  vulkan.writeWord(9, ::RDP::make_default_h_start());
  vulkan.writeWord(10, ::RDP::make_default_v_start());
  vulkan.writeWord(12, 512);
  vulkan.writeWord(13, 1024);
  require(vulkan.scanoutAsync(false), "N64 stereo scanout starts");
  std::vector<Scanout> result(3);
  for(int eye = -1; eye < 2; ++eye) {
    const u8* data = nullptr;
    auto& output = result[eye + 1];
    vulkan.mapScanoutRead(data, output.width, output.height, eye);
    require(data && output.width && output.height, "N64 scanout has pixels");
    output.rgba.assign(data, data + output.width * output.height * 4);
    vulkan.unmapScanoutRead(eye);
  }
  vi.screen = std::make_shared<ares::Core::Video::Screen>("Stereo test", 1280 * upscale, 576 * upscale);
  vi.screen->setSize(640 * upscale, 480 * upscale);
  vi.gpuOutputValid = true;
  vi.refresh(); // Packs both mapped scanouts and completes their frame fences.
  require(vi.screen->width() == 640 * upscale * (vulkan.stereo.fullWidth ? 2 : 1),
          "N64 full/half SBS logical width is correct");
  auto pixels = vi.screen->pixels(1);
  for(unsigned eye = 0; eye < 2; ++eye) {
    auto& output = result[eye + 1];
    for(unsigned y = 0; y < output.height; ++y) for(unsigned x = 0; x < output.width; ++x) {
      auto source = &output.rgba[(y * output.width + x) * 4];
      u32 expected = source[0] << 16 | source[1] << 8 | source[2];
      require(pixels[y * vi.screen->canvasWidth() + eye * output.width + x] == expected,
              "N64 VI packs both complete eye scanouts without gaps or overlap");
    }
  }
  vi.screen.reset();
  vulkan.frame();
  vulkan.unload();
  rdram.ram.reset();
  mi.unload();
  return result;
}

static auto testNintendo64() -> void {
  auto zero = renderNintendo64(0, false);
  require(zero[0].rgba == zero[1].rgba && zero[0].rgba == zero[2].rgba, "N64 zero-depth eyes equal mono pixel for pixel");
  auto normal = renderNintendo64(100, false);
  require(normal[0].rgba == zero[0].rgba, "N64 stereo leaves guest image unchanged");
  require(normal[1].rgba != normal[2].rgba, "N64 renders distinct eye views");
  auto swapped = renderNintendo64(100, true);
  require(normal[1].rgba == swapped[2].rgba && normal[2].rgba == swapped[1].rgba, "N64 eye swap exchanges complete views");
  auto high = renderNintendo64(0, false, 2);
  require(high[0].rgba == high[1].rgba && high[0].rgba == high[2].rgba, "N64 2x zero-depth eyes equal mono");
  std::puts("PASS: N64 Vulkan rendering, guest isolation, zero depth, eye swap and 2x scanout");
}

auto nall::main(Arguments) -> void {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  ares::Platform platform;
  ares::platform = &platform;
  testMath();
  testPlayStation();
  testNintendo64();
  std::puts("All stereo tests passed.");
}
