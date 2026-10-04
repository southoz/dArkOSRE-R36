// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "frontend/sdl/video/presenter.h"
#include "frontend/sdl/gpu_present.h"
#include "frontend/sdl/scanout.h"

#include <SDL.h>
#include <cstring>

namespace ds::sdl {

namespace {

SDL_Rect sdl(const frontend::Rect& r) { return SDL_Rect{r.x, r.y, r.w, r.h}; }

class VkImportPresenter final : public FramePresenter {
public:
  VkImportPresenter(ScanoutOut& sink, std::unique_ptr<GpuPresent> gpu) : sink_(sink), gpu_(std::move(gpu)) {
    name_ = std::string("GPU present on ") + gpu_->device_name() + " (dma-buf import, " +
            (std::strcmp(SDL_GetCurrentVideoDriver(), "KMSDRM") == 0 ? "kms" : "dmabuf") + " scanout)";
    sink_.set_gpu_writes(true);
  }
  ~VkImportPresenter() override { if (!lost_) gpu_->flush(sink_); sink_.set_gpu_writes(false); }

  Kind kind() const override { return Kind::Vulkan; }
  Fit fit(SDL_Window* win, int w, int h) override {
    if (w == sink_.width() && h == sink_.height()) return Fit::Same;
    if (sink_.reopen(win, w, h) && gpu_->reimport(sink_)) return Fit::Changed;
    lost_ = true;
    return Fit::Lost;
  }
  bool present(const u32* const fb[2], const View* views, int nviews, const Params& p) override {
    GpuPresent::View v[frontend::SCREENS];
    for (int i = 0; i < nviews && i < frontend::SCREENS; ++i) v[i] = GpuPresent::View{views[i].screen, sdl(views[i].rect), views[i].shown, views[i].blends, views[i].grid};
    return gpu_->present(sink_, fb, v, nviews, p.rot, p.lw, p.lh, p.inset_alpha, sdl(p.drawn), p.grid);
  }
  u32* overlay(int lw, int lh) override { return gpu_->overlay(lw, lh); }
  void flush() override { gpu_->flush(sink_); sink_.flush(); }
  void retire() override { gpu_->flush(sink_); }

private:
  ScanoutOut& sink_;
  std::unique_ptr<GpuPresent> gpu_;
  bool lost_ = false;   // the sink failed to reopen: nothing in flight to hand it
};

} // namespace

std::unique_ptr<FramePresenter> open_vk_import_presenter(ScanoutOut& sink, std::string* why) {
  std::unique_ptr<GpuPresent> gpu = GpuPresent::open(sink, why);
  if (!gpu) return nullptr;
  return std::make_unique<VkImportPresenter>(sink, std::move(gpu));
}

} // namespace ds::sdl
