namespace ares::Nintendo64 {

struct Vulkan {
  auto load(Node::Object) -> bool;
  auto unload() -> void;

  auto render() -> bool;
  auto frame() -> void;
  auto writeWord(u32 address, u32 data) -> void;
  auto scanoutAsync(bool field) -> bool;
  auto mapScanoutRead(const u8*& rgba, u32& width, u32& height, int eye = -1) -> void;
  auto unmapScanoutRead(int eye = -1) -> void;
  auto endScanout() -> void;
  auto crashed() -> const char*;

  struct Implementation;
  Implementation* implementation = nullptr;

  Stereo::Settings stereo;
  Implementation* eyes[2] = {};
  auto writeRdram(u32 address, u32 size, u64 value, bool ebus = false) -> void;
  auto resetStereo() -> void;
  auto stereoActive() const -> bool { return eyes[0] && eyes[1]; }

  bool enable = true;
  bool disableVideoInterfaceProcessing = false;
  bool weaveDeinterlacing = false;
  u32  internalUpscale = 1;  //1, 2, 4, 8
  bool supersampleScanout = false;
  u32  outputUpscale = supersampleScanout ? 1 : internalUpscale;
};

extern Vulkan vulkan;

}
