#include "mw/drm/error.hpp"

namespace mw::drm {

const char* DrmError::error_message() const noexcept {
    switch (code_) {
        case Errc::OpenFailed:
            return "failed to open DRM node";
        case Errc::NotKmsCapable:
            return "node has no KMS resources (render node, or a card with no display engine)";
        case Errc::NoAtomicSupport:
            return "DRM_CLIENT_CAP_ATOMIC not available; this project is atomic-only";
        case Errc::NoUniversalPlanes:
            return "DRM_CLIENT_CAP_UNIVERSAL_PLANES not available";
        case Errc::NoDumbBuffer:
            return "DRM_CAP_DUMB_BUFFER is 0; CPU-drawn buffers unavailable";
        case Errc::NotMaster:
            return "not DRM master";
        case Errc::ResourceQueryFailed:
            return "failed to query DRM resources";
        case Errc::NoConnectedConnector:
            return "no connected connector found";
        case Errc::NoModeAvailable:
            return "connector is connected but reports no modes";
        case Errc::NoCompatibleCrtc:
            return "no CRTC in the encoder's possible_crtcs bitmask is usable";
        case Errc::NoCompatiblePlane:
            return "no plane of the requested type is usable on this CRTC";
        case Errc::StaleSnapshot:
            return "resource snapshot is stale; rescan() happened after this path was computed";
        case Errc::PropertyNotFound:
            return "required KMS property not found on object";
        case Errc::PropertyTypeMismatch:
            return "KMS property has an unexpected type";
        case Errc::BlobCreateFailed:
            return "failed to create property blob";
        case Errc::BlobReadFailed:
            return "failed to read property blob";
        case Errc::DumbCreateFailed:
            return "DRM_IOCTL_MODE_CREATE_DUMB failed";
        case Errc::DumbMapFailed:
            return "DRM_IOCTL_MODE_MAP_DUMB or mmap failed";
        case Errc::AddFbFailed:
            return "drmModeAddFB2 failed";
        case Errc::AtomicTestFailed:
            return "atomic TEST_ONLY rejected by the kernel";
        case Errc::AtomicCommitFailed:
            return "atomic commit rejected by the kernel";
        case Errc::Unsupported:
            return "capability not present on this kernel or driver, and no fallback exists";
        case Errc::Internal:
            return "internal error";
    }
    return "unknown drm error";
}

} // namespace mw::drm
