#include <n64/n64.hpp>

//included here rather than in vulkan.hpp so that parallel-rdp headers are only
//parsed in this translation unit
#include "rdp_device.hpp"

namespace ares::Nintendo64 {

Vulkan vulkan;

struct LoggingInterface : Util::LoggingInterface {
  auto log(const char* tag, const char* fmt, va_list va) -> bool {
    char buffer[8192];
    vsnprintf(buffer, sizeof(buffer), fmt, va);
  //print(terminal::color::yellow(tag), buffer);
    return true;
  }
} loggingInterface;

struct Vulkan::Implementation {
  Implementation(u8* data, u32 size, int eye = -1);
  Memory::Writable ram;
  HiddenRAM hidden;
  ~Implementation();

  ::Vulkan::Context context;
  ::Vulkan::Device device;
  ::RDP::CommandProcessor* processor = nullptr;
  atomic<const char*> crash_error = nullptr;

  struct Validation : public ::RDP::ValidationInterface {
    Implementation& self;
    Validation(Implementation& i) : self(i) {}
    void report_rdp_crash(::RDP::ValidationError err, const char *msg) override {
      self.crash_error = msg;
    }
  } validator{*this};

  //commands are u64 words, but the backend uses u32 swapped words.
  //size and offset are in u64 words.
  u32 buffer[0x10000] = {};
  u32 queueSize = 0;
  u32 queueOffset = 0;

  ::RDP::VIScanoutBuffer scanout;
  std::mutex lock;
  std::condition_variable condition;
  u32 scanoutCount = 0;
  u32 endCount = 0;
};

auto Vulkan::load(Node::Object) -> bool {
  unload();
  if (vulkan.enable) {
    Util::set_thread_logging_interface(&loggingInterface);
    implementation = new Vulkan::Implementation(rdram.ram.data, rdram.ram.size);
    if(!implementation->processor) {
      delete implementation;
      implementation = nullptr;
    }

    if (!implementation) {
      platform->status("Vulkan init failed: No RDP rendering support");
      vulkan.enable = false;
      rdram.hidden.data = nullptr;
    } else {
      platform->status("Vulkan Enabled: using paraLLEl-RDP");
      rdram.hidden.data = (u8*)implementation->processor->begin_read_hidden_rdram();
      if(stereo.enabled) {
        for(unsigned eye = 0; eye < 2; ++eye) {
          eyes[eye] = new Implementation(rdram.ram.data, rdram.ram.size, eye);
        }
        if(!eyes[0]->processor || !eyes[1]->processor) {
          for(auto& eye : eyes) { delete eye; eye = nullptr; }
          platform->status("SBS initialization failed; using mono rendering");
        }
      }
    }
  } else {
    platform->status("Vulkan Disabled: No RDP rendering support");
    rdram.hidden.data = nullptr;
  }

  return true;
}

auto Vulkan::unload() -> void {
  for(auto& eye : eyes) { delete eye; eye = nullptr; }
  rdram.hidden.data = nullptr;
  if (implementation) delete implementation;
  implementation = nullptr;
}

auto Vulkan::render() -> bool {
  if(!implementation) return false;

  static constexpr u32 commandLength[64] = {
    1, 1, 1, 1, 1, 1, 1, 1, 4, 6,12,14,12,14,20,22,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  };

  auto& command = rdp.command;

  u32 current = command.current & ~7;
  u32 end = command.end & ~7;
  u32 length = (end - current) / 8;
  if(current >= end) return true;

  u32* buffer = implementation->buffer;
  u32& queueSize = implementation->queueSize;
  u32& queueOffset = implementation->queueOffset;
  if(queueSize + length >= 0x8000) return true;

  if(!command.source) {
    do {
      buffer[queueSize * 2 + 0] = rdram.ram.read<Word>(current, RBusDevice::DP_DMA); current += 4;
      buffer[queueSize * 2 + 1] = rdram.ram.read<Word>(current, RBusDevice::DP_DMA); current += 4;
      queueSize++;
    } while(--length);
  } else {
    do {
      buffer[queueSize * 2 + 0] = rsp.dmem.read<Word>(current); current += 4;
      buffer[queueSize * 2 + 1] = rsp.dmem.read<Word>(current); current += 4;
      if(system.homebrewMode) {
        rsp.debugger.dmemReadWord(current - 8, 8, "RDP XBUS");
      }
      queueSize++;
    } while(--length);
  }

  while(queueOffset < queueSize) {
    u32 op = buffer[queueOffset * 2];
    u32 code = op >> 24 & 63;
    u32 length = commandLength[code];

    if(queueOffset + length > queueSize) {
      //partial command, keep data around for next processing call
      command.start = command.current = command.end;
      return true;
    }

    if(code >= 8) {
      implementation->processor->enqueue_command(length * 2, buffer + queueOffset * 2);
      for(auto eye : eyes) if(eye) eye->processor->enqueue_command(length * 2, buffer + queueOffset * 2);
    }

    if(::RDP::Op(code) == ::RDP::Op::SyncFull) {
      implementation->processor->wait_for_timeline(implementation->processor->signal_timeline());
      for(auto eye : eyes) if(eye) eye->processor->wait_for_timeline(eye->processor->signal_timeline());
      rdp.syncFull();
    }

    queueOffset += length;
  }

  queueOffset = 0;
  queueSize = 0;
  command.current = command.end;
  return true;
}

auto Vulkan::frame() -> void {
  if(!implementation) return;
  implementation->processor->begin_frame_context();
  for(auto eye : eyes) if(eye) eye->processor->begin_frame_context();
}

auto Vulkan::writeWord(u32 address, u32 data) -> void {
  if(!implementation) return;
  implementation->processor->set_vi_register(::RDP::VIRegister(address), data);
  for(auto eye : eyes) if(eye) eye->processor->set_vi_register(::RDP::VIRegister(address), data);
}

auto Vulkan::scanoutAsync(bool field) -> bool {
  if(!implementation) return false;

  for(auto current : {implementation, eyes[0], eyes[1]}) {
    if(!current) continue;
    { //wait until we're done reading in thread before we clobber the readback buffer
      std::unique_lock<std::mutex> lock{current->lock};
      current->condition.wait(lock, [current]() {
        return current->scanoutCount == current->endCount;
      });
    }

    current->processor->set_vi_register(::RDP::VIRegister::VCurrentLine, field);

    //0 steps if scanning out at upscaled resolution.
    //each downscale step reduces output resolution to [width, height] * max(1, upscale >> downscale_steps)
    ::RDP::ScanoutOptions options;
    options.downscale_steps = supersampleScanout ? 16 : 0;
    options.persist_frame_on_invalid_input = true;  //this is a compatibility hack, but I'm not sure what for ...
    if(disableVideoInterfaceProcessing) {
      options.vi = {false, false, true, false, false, false};
    }
    if(!supersampleScanout){
      options.blend_previous_frame = weaveDeinterlacing;
      options.upscale_deinterlacing = !weaveDeinterlacing;
    }
    else {
      options.blend_previous_frame = false;
      options.upscale_deinterlacing = true;
    }


    if(current->scanout.fence) {
      current->scanout.fence->wait();
    }
    current->processor->scanout_async_buffer(current->scanout, options);
    current->scanoutCount++;
  }
  return true;
}

auto Vulkan::mapScanoutRead(const u8*& rgba, u32& width, u32& height, int eye) -> void {
  auto current = eye >= 0 && eye < 2 ? eyes[eye] : implementation;
  if(!current || !current->scanout.fence || !current->scanout.width || !current->scanout.height) {
    rgba = nullptr;
    width = 0;
    height = 0;
  } else {
    current->scanout.fence->wait();
    rgba = (const u8*)current->device.map_host_buffer(*current->scanout.buffer, ::Vulkan::MEMORY_ACCESS_READ_BIT);
    width = current->scanout.width;
    height = current->scanout.height;
  }
}

auto Vulkan::unmapScanoutRead(int eye) -> void {
  auto current = eye >= 0 && eye < 2 ? eyes[eye] : implementation;
  if(current && current->scanout.buffer) {
    current->device.unmap_host_buffer(*current->scanout.buffer, ::Vulkan::MEMORY_ACCESS_READ_BIT);
  }
}

auto Vulkan::endScanout() -> void {
  for(auto current : {implementation, eyes[0], eyes[1]}) if(current) {
    //notify main thread that we're done reading
    std::lock_guard<std::mutex> lock{current->lock};
    current->endCount++;
    current->condition.notify_one();
  }
}

auto Vulkan::writeRdram(u32 address, u32 size, u64 value, bool ebus) -> void {
  for(auto eye : eyes) if(eye && eye->processor && address < eye->ram.size) {
    auto write = [&]<u32 Size>() {
      eye->ram.write<Size>(address, value);
      if(ebus) eye->hidden.ebusScatter<Size>(address, value);
      else eye->hidden.update<Size>(address, value);
    };
    switch(size) {
    case Byte: write.operator()<Byte>(); break;
    case Half: write.operator()<Half>(); break;
    case Word: write.operator()<Word>(); break;
    case Dual: write.operator()<Dual>(); break;
    }
  }
}

auto Vulkan::resetStereo() -> void {
  // Auxiliary render targets are not guest state. Seed them after a state load;
  // subsequent draws rebuild their eye-specific content.
  for(auto eye : eyes) if(eye && eye->processor) {
    eye->processor->idle();
    memory::copy(eye->ram.data, rdram.ram.data, rdram.ram.size);
    memory::copy(eye->hidden.data, rdram.hidden.data, rdram.ram.size / 2);
  }
}

auto Vulkan::crashed() -> const char* {
  if(implementation) return implementation->crash_error;
  return nullptr;
}

Vulkan::Implementation::Implementation(u8* data, u32 size, int eye) {
  if(eye >= 0) {
    ram.allocate(size);
    memory::copy(ram.data, data, size);
    data = ram.data;
  }
  if(!::Vulkan::Context::init_loader(nullptr)) return;
  if(!context.init_instance_and_device(nullptr, 0, nullptr, 0, 0)) return;
  device.set_context(context);
  device.init_frame_contexts(3);

  ::RDP::CommandProcessorFlags flags = ::RDP::COMMAND_PROCESSOR_FLAG_HOST_VISIBLE_HIDDEN_RDRAM_BIT;
  switch(vulkan.internalUpscale) {
  case 2: flags |= ::RDP::COMMAND_PROCESSOR_FLAG_UPSCALING_2X_BIT; break;
  case 4: flags |= ::RDP::COMMAND_PROCESSOR_FLAG_UPSCALING_4X_BIT; break;
  case 8: flags |= ::RDP::COMMAND_PROCESSOR_FLAG_UPSCALING_8X_BIT; break;
  }

  if(vulkan.internalUpscale > 1) {
    flags |= ::RDP::COMMAND_PROCESSOR_FLAG_SUPER_SAMPLED_DITHER_BIT;
    //rasky: this is explicitly disabled because we want to make sure we don't
    // read back the super sampled version, as it can cause artifacts. We want
    // parallelRDP to also produce a 1x render to use for readbacks.
    //flags |= ::RDP::COMMAND_PROCESSOR_FLAG_SUPER_SAMPLED_READ_BACK_BIT;
  }

  processor = new ::RDP::CommandProcessor(device, data, 0, size, size / 2, flags);
  if(!processor->device_is_supported()) {
    delete processor;
    processor = nullptr;
    return;
  }

  if(eye >= 0) {
    processor->set_stereo(vulkan.stereo.separation(1, eye), vulkan.stereo.convergence);
    hidden.data = (u8*)processor->begin_read_hidden_rdram();
  }
  processor->set_validation_interface(&validator);
}

Vulkan::Implementation::~Implementation() {
  if(processor) delete processor;
  ram.reset();
}

}
