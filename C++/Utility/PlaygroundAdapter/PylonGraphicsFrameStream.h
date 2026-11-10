#pragma once

/**
 * @file PylonGraphicsFrameStream.h
 * @brief Owns Camera grab callbacks and publishes owned GraphicsFrame values.
 */

#include "engine/GraphicsFrameAdapter.h"

#include "CameraCaptureTracker.h"

#include <functional>
#include <memory>

class Camera;

/** @brief Receives one owned frame, its source index and its pylon capture identity. */
using PylonFrameCallback = std::function<void(
    GraphicsFrame&&, unsigned int sourceIndex, const CameraFrameCapture& capture)>;

/** Owns Camera SDK callback registration and emits only owned GraphicsFrame values. */
class PylonGraphicsFrameStream final
{
public:
    PylonGraphicsFrameStream(Camera* camera, PylonFrameCallback callback);
    ~PylonGraphicsFrameStream();

    PylonGraphicsFrameStream(const PylonGraphicsFrameStream&) = delete;
    PylonGraphicsFrameStream& operator=(const PylonGraphicsFrameStream&) = delete;

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};
