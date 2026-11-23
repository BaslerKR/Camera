#pragma once

/**
 * @file PylonGraphicsFrameStream.h
 * @brief Owns Camera grab callbacks and publishes owned GraphicsFrame values.
 */

#include "PylonAssemblyRejection.h"
#include "engine/GraphicsFrameAdapter.h"
#include "engine/GraphicsImageWriter.h"

#include <functional>
#include <memory>

class Camera;

/** @brief Neutral best-effort observers of failed 2D and range assembly. */
using CameraImageRejectionCallback = std::function<void(const PylonImageRejection&)>;
using CameraRangeRejectionCallback = std::function<void(const PylonRangeRejection&)>;

/** Owns Camera SDK callback registration and emits only owned GraphicsFrame values. */
class PylonGraphicsFrameStream final
{
public:
    PylonGraphicsFrameStream(Camera* camera, GraphicsFrameCallback callback,
        CameraImageRejectionCallback imageRejection = {},
        CameraRangeRejectionCallback rangeRejection = {});
    ~PylonGraphicsFrameStream();

    /** Forwards the storage 2D images are copied into; callable while grabbing. */
    void setFrameStorage(GraphicsImageStorage storage);

    PylonGraphicsFrameStream(const PylonGraphicsFrameStream&) = delete;
    PylonGraphicsFrameStream& operator=(const PylonGraphicsFrameStream&) = delete;

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};
