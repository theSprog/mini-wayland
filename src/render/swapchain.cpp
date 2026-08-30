#include "mw/render/swapchain.hpp"

#include <utility>

#include "mw/core/log.hpp"
#include "mw/drm/error.hpp"

namespace mw::render {

Result<Swapchain> Swapchain::allocate(BufferSource& source, const SwapchainDesc& desc,
                                      const egl::Display* display) {
    if (desc.count < 2u || desc.count > Swapchain::kMaxBuffers) {
        return Err(drm::Errc::Unsupported,
                   fmt("buffer count {} out of range [2, {}]; single buffering always tears",
                       desc.count, kMaxBuffers));
    }
    if (desc.size.empty()) {
        return Err(drm::Errc::Unsupported, "swapchain size is zero");
    }

    Swapchain chain;
    for (uint32_t i = 0; i < desc.count; ++i) {
        AllocRequest request;
        request.size = desc.size;
        request.format = desc.format;
        request.modifiers = desc.modifiers;
        request.need_cpu_write = desc.need_cpu_write;

        auto buffer = source.allocate(request);
        if (! buffer) {
            return Err(drm::Errc::Unsupported,
                       fmt("swapchain buffer {}/{} could not be allocated: {}", i + 1u,
                           desc.count, buffer.error().message));
        }

        Slot* slot = chain.at(i);
        slot->buffer = std::move(buffer).value();

        if (display != nullptr) {
            auto target = GlRenderTarget::create(*display, slot->buffer);
            if (! target) {
                return Err(drm::Errc::Unsupported,
                           fmt("swapchain buffer {}/{} was allocated but cannot be used as a "
                               "GL render target: {}",
                               i + 1u, desc.count, target.error().message));
            }
            slot->target = std::move(target).value();
        }
    }

    // count_ 最后才置，中途失败时 chain 的析构只会碰到已经建好的槽位。
    // 不过 Slot 的成员本来就是\"空即无操作\"，这里主要是让 to_string() 诚实。
    chain.count_ = desc.count;

    LOG_INFO("{}", chain.to_string());
    if (! chain.modifiers_uniform()) {
        // 同一条 swapchain 里 modifier 不一致，意味着 plane 分配器在不同帧
        // 面对的是不同布局。TEST_ONLY 可能这一帧过下一帧不过，症状是偶发闪烁。
        LOG_WARN("the allocator returned different modifiers across the swapchain; "
                 "plane assignment results may differ from frame to frame");
    }
    return Ok(std::move(chain));
}

Result<Swapchain> Swapchain::create(BufferSource& source, const SwapchainDesc& desc) {
    return allocate(source, desc, nullptr);
}

Result<Swapchain> Swapchain::create_with_targets(BufferSource& source,
                                                 const egl::Display& display,
                                                 const SwapchainDesc& desc) {
    return allocate(source, desc, &display);
}

Swapchain::Slot& Swapchain::acquire() noexcept {
    return slots_[next_];
}

const Swapchain::Slot& Swapchain::acquire() const noexcept {
    return slots_[next_];
}

Swapchain::Slot* Swapchain::at(uint32_t index) noexcept {
    if (index >= kMaxBuffers) {
        return nullptr;
    }
    return &slots_[index];
}

void Swapchain::mark_submitted() noexcept {
    if (count_ == 0u) {
        return;
    }
    next_ = (next_ + 1u) % count_;
    ++in_flight_;
}

void Swapchain::on_flip_complete() noexcept {
    if (in_flight_ > 0u) {
        --in_flight_;
    } else {
        // 收到了没有对应提交的完成事件。通常是事件循环少记了一次提交，
        // 或者内核补发了旧事件。不致命，但值得知道。
        LOG_WARN("flip completion with no outstanding submission");
    }
}

bool Swapchain::modifiers_uniform() const noexcept {
    if (count_ < 2u) {
        return true;
    }
    const Modifier first = slots_[0].buffer.modifier();
    for (uint32_t i = 1; i < count_; ++i) {
        if (slots_[i].buffer.modifier() != first) {
            return false;
        }
    }
    return true;
}

std::string Swapchain::to_string() const {
    if (count_ == 0u) {
        return "<empty swapchain>";
    }
    std::string out = fmt("swapchain of {} buffer(s):", count_);
    for (uint32_t i = 0; i < count_; ++i) {
        out += fmt("\n  [{}] {}", i, slots_[i].buffer.to_string());
        if (slots_[i].target.valid()) {
            out += fmt("\n      render target {}", slots_[i].target.to_string());
        }
    }
    return out;
}

} // namespace mw::render
