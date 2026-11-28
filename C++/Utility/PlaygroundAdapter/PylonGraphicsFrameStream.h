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
using CameraStorageOutcomeCallback = std::function<void(GraphicsFrameError)>;
using CameraRangeRejectionCallback = std::function<void(const PylonRangeRejection&)>;

/** Owns Camera SDK callback registration and emits only owned GraphicsFrame values. */
class PylonGraphicsFrameStream final
{
public:
    PylonGraphicsFrameStream(Camera* camera, GraphicsFrameCallback callback,
        CameraImageRejectionCallback imageRejection = {},
        CameraRangeRejectionCallback rangeRejection = {},
        CameraStorageOutcomeCallback storageOutcome = {});
    ~PylonGraphicsFrameStream();

    /** Cancels this acquisition run before stopping the camera. */
    void cancel() noexcept;

    /** Installs the source for whole-frame transactions. */
    void setFrameContextSource(std::shared_ptr<GraphicsFrameContextSource> source);

    PylonGraphicsFrameStream(const PylonGraphicsFrameStream&) = delete;
    PylonGraphicsFrameStream& operator=(const PylonGraphicsFrameStream&) = delete;

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};
