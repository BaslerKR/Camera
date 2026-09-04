#pragma once

#include "BlazeGraphicsFrameAdapter.h"
#include "Camera.h"
#include "PylonScene3DProfile.h"
#include "engine/GraphicsFrame.h"

#include <optional>
#include <cstdint>

class PylonGraphicsFrameAdapter final
{
public:
    PylonGraphicsFrameAdapter() = default;

    [[nodiscard]] std::optional<GraphicsFrame> convertGraphicsFrame(
        const Pylon::CPylonDataContainer& container,
        const GraphicsFrameRequest& request,
        const PylonScene3DProfile& profile) const;

    /** Converts one owned 2D pylon image into the canonical GraphicsImage. */
    [[nodiscard]] GraphicsImage convertGraphicsImage(
        const Pylon::CPylonImage& image,
        std::uint64_t frameSequence = 0U) const;

private:
    BlazeGraphicsFrameAdapter _blazeAdapter;
};

/** Owns Camera SDK callback registration and emits only owned GraphicsFrame values. */
class PylonGraphicsFrameStream final
{
public:
    PylonGraphicsFrameStream(Camera* camera, GraphicsFrameCallback callback);
    ~PylonGraphicsFrameStream();

    PylonGraphicsFrameStream(const PylonGraphicsFrameStream&) = delete;
    PylonGraphicsFrameStream& operator=(const PylonGraphicsFrameStream&) = delete;

private:
    Camera* _camera = nullptr;
    GraphicsFrameCallback _callback;
    PylonGraphicsFrameAdapter _adapter;
    GraphicsFrameCallbackGate _callbackGate;
    Camera::CallbackId _grabCallbackId = 0;
    Camera::CallbackId _grab3DCallbackId = 0;
};
