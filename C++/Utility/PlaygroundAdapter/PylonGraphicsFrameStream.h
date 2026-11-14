#pragma once

/**
 * @file PylonGraphicsFrameStream.h
 * @brief Owns Camera grab callbacks and publishes owned GraphicsFrame values.
 */

#include "engine/GraphicsFrameAdapter.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

class Camera;

/** Owns Camera SDK callback registration and emits only owned GraphicsFrame values. */
class PylonGraphicsFrameStream final
{
public:
    PylonGraphicsFrameStream(Camera* camera, GraphicsFrameCallback callback);
    ~PylonGraphicsFrameStream();

    /** Storage for one image; see PylonGraphicsFrameAdapter::FrameStorage. */
    using FrameStorage = std::function<std::shared_ptr<std::uint8_t>(std::size_t bytes)>;
    /** Forwards the storage 2D images are copied into; callable while grabbing. */
    void setFrameStorage(FrameStorage storage);

    PylonGraphicsFrameStream(const PylonGraphicsFrameStream&) = delete;
    PylonGraphicsFrameStream& operator=(const PylonGraphicsFrameStream&) = delete;

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};
