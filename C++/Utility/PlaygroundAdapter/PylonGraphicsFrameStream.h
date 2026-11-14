#pragma once

/**
 * @file PylonGraphicsFrameStream.h
 * @brief Owns Camera grab callbacks and publishes owned GraphicsFrame values.
 */

#include "engine/GraphicsFrameAdapter.h"
#include "engine/GraphicsImageWriter.h"

#include <memory>

class Camera;

/** Owns Camera SDK callback registration and emits only owned GraphicsFrame values. */
class PylonGraphicsFrameStream final
{
public:
    PylonGraphicsFrameStream(Camera* camera, GraphicsFrameCallback callback);
    ~PylonGraphicsFrameStream();

    /** Forwards the storage 2D images are copied into; callable while grabbing. */
    void setFrameStorage(GraphicsImageStorage storage);

    PylonGraphicsFrameStream(const PylonGraphicsFrameStream&) = delete;
    PylonGraphicsFrameStream& operator=(const PylonGraphicsFrameStream&) = delete;

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};
