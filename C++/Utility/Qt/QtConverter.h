#ifndef CONVERTERQT_H
#define CONVERTERQT_H

/**
 * @file QtConverter.h
 * @brief Converts Basler pylon image buffers into Qt QImage frames.
 *
 * Selects direct-copy or Pylon ImageFormatConverter paths for Mono/RGB pixel
 * types so 2D grab results can be passed to GraphicsEngine::setImage().
 */

#include <pylon/ImageFormatConverter.h>
#include <pylon/PylonImage.h>

#ifdef QT_GUI_LIB
#include <QImage>

#include <cstring>

namespace QtConverterDetail {

inline bool isDirectMono8CopyPixelType(const Pylon::EPixelType pixelType)
{
    switch(pixelType){
    case Pylon::PixelType_Mono8:
    case Pylon::PixelType_Mono8signed:
#if defined(PYLON_VERSION_MAJOR) && PYLON_VERSION_MAJOR >= 8
    case Pylon::PixelType_Confidence8:
    case Pylon::PixelType_Data8:
    case Pylon::PixelType_Data8s:
    case Pylon::PixelType_Error8:
#endif
        return true;
    default:
        return false;
    }
}

inline bool isDirectMono16CopyPixelType(const Pylon::EPixelType pixelType)
{
    switch(pixelType){
    case Pylon::PixelType_Mono16:
#if defined(PYLON_VERSION_MAJOR) && PYLON_VERSION_MAJOR >= 8
    case Pylon::PixelType_Confidence16:
    case Pylon::PixelType_Data16:
    case Pylon::PixelType_Data16s:
#endif
        return true;
    default:
        return false;
    }
}

inline bool shouldConvertToMono8(const Pylon::EPixelType pixelType)
{
    switch(pixelType){
    case Pylon::PixelType_Undefined:
    case Pylon::PixelType_Mono1packed:
    case Pylon::PixelType_Mono2packed:
    case Pylon::PixelType_Mono4packed:
        return true;
    default:
        return false;
    }
}

inline bool shouldConvertToMono16(const Pylon::EPixelType pixelType)
{
    switch(pixelType){
    case Pylon::PixelType_Mono10:
    case Pylon::PixelType_Mono10packed:
    case Pylon::PixelType_Mono10p:
    case Pylon::PixelType_Mono12:
    case Pylon::PixelType_Mono12packed:
    case Pylon::PixelType_Mono12p:
        return true;
    default:
        return false;
    }
}

inline bool shouldConvertToColor(const Pylon::EPixelType pixelType)
{
    switch(pixelType){
    case Pylon::PixelType_BayerGR8:
    case Pylon::PixelType_BayerRG8:
    case Pylon::PixelType_BayerGB8:
    case Pylon::PixelType_BayerBG8:
    case Pylon::PixelType_BayerGR10:
    case Pylon::PixelType_BayerRG10:
    case Pylon::PixelType_BayerGB10:
    case Pylon::PixelType_BayerBG10:
    case Pylon::PixelType_BayerGR12:
    case Pylon::PixelType_BayerRG12:
    case Pylon::PixelType_BayerGB12:
    case Pylon::PixelType_BayerBG12:
    case Pylon::PixelType_BayerGR12Packed:
    case Pylon::PixelType_BayerRG12Packed:
    case Pylon::PixelType_BayerGB12Packed:
    case Pylon::PixelType_BayerBG12Packed:
    case Pylon::PixelType_BayerGR10p:
    case Pylon::PixelType_BayerRG10p:
    case Pylon::PixelType_BayerGB10p:
    case Pylon::PixelType_BayerBG10p:
    case Pylon::PixelType_BayerGR12p:
    case Pylon::PixelType_BayerRG12p:
    case Pylon::PixelType_BayerGB12p:
    case Pylon::PixelType_BayerBG12p:
    case Pylon::PixelType_BayerGR16:
    case Pylon::PixelType_BayerRG16:
    case Pylon::PixelType_BayerGB16:
    case Pylon::PixelType_BayerBG16:
    case Pylon::PixelType_RGB8packed:
    case Pylon::PixelType_BGR8packed:
    case Pylon::PixelType_RGBA8packed:
    case Pylon::PixelType_BGRA8packed:
    case Pylon::PixelType_RGB10packed:
    case Pylon::PixelType_BGR10packed:
    case Pylon::PixelType_RGB12packed:
    case Pylon::PixelType_BGR12packed:
    case Pylon::PixelType_RGB16packed:
    case Pylon::PixelType_BGR10V1packed:
    case Pylon::PixelType_BGR10V2packed:
    case Pylon::PixelType_YUV411packed:
    case Pylon::PixelType_YUV422packed:
    case Pylon::PixelType_YUV444packed:
    case Pylon::PixelType_RGB8planar:
    case Pylon::PixelType_RGB10planar:
    case Pylon::PixelType_RGB12planar:
    case Pylon::PixelType_RGB16planar:
    case Pylon::PixelType_YUV422_YUYV_Packed:
    case Pylon::PixelType_YUV444planar:
    case Pylon::PixelType_YUV422planar:
    case Pylon::PixelType_YUV420planar:
    case Pylon::PixelType_YCbCr420_8_YY_CbCr_Semiplanar:
    case Pylon::PixelType_YCbCr422_8_YY_CbCr_Semiplanar:
    case Pylon::PixelType_RGB12V1packed:
#if defined(PYLON_VERSION_MAJOR) && PYLON_VERSION_MAJOR >= 8
    case Pylon::PixelType_BiColorRGBG8:
    case Pylon::PixelType_BiColorBGRG8:
    case Pylon::PixelType_BiColorRGBG10:
    case Pylon::PixelType_BiColorRGBG10p:
    case Pylon::PixelType_BiColorBGRG10:
    case Pylon::PixelType_BiColorBGRG10p:
    case Pylon::PixelType_BiColorRGBG12:
    case Pylon::PixelType_BiColorRGBG12p:
    case Pylon::PixelType_BiColorBGRG12:
    case Pylon::PixelType_BiColorBGRG12p:
#endif
        return true;
    default:
        return false;
    }
}

inline QImage convertViaPylon(const Pylon::CPylonImage& pylonImg,
                              Pylon::CImageFormatConverter& converter,
                              const Pylon::EPixelType outputPixelType,
                              const QImage::Format outputFormat)
{
    QImage outImage(pylonImg.GetWidth(), pylonImg.GetHeight(), outputFormat);
    converter.OutputPixelFormat = outputPixelType;
    converter.Convert(outImage.bits(), outImage.sizeInBytes(), pylonImg);
    return outImage;
}

inline QImage copyMonoImage(const Pylon::CPylonImage& pylonImg, const QImage::Format format)
{
    QImage outImage(pylonImg.GetWidth(), pylonImg.GetHeight(), format);
    std::memcpy(outImage.bits(), pylonImg.GetBuffer(), static_cast<size_t>(outImage.sizeInBytes()));
    return outImage;
}

} // namespace QtConverterDetail

inline QImage convertPylonImageToQImage(Pylon::CPylonImage pylonImg)
{
    if(pylonImg.GetWidth() == 0 || pylonImg.GetHeight() == 0 || pylonImg.GetBuffer() == nullptr){
        return {};
    }

    Pylon::CImageFormatConverter converter;
    converter.OutputBitAlignment = Pylon::OutputBitAlignment_MsbAligned;

    const auto pixelType = pylonImg.GetPixelType();
    if(QtConverterDetail::isDirectMono8CopyPixelType(pixelType)){
        return QtConverterDetail::copyMonoImage(pylonImg, QImage::Format_Grayscale8);
    }
    if(QtConverterDetail::isDirectMono16CopyPixelType(pixelType)){
        return QtConverterDetail::copyMonoImage(pylonImg, QImage::Format_Grayscale16);
    }
    if(QtConverterDetail::shouldConvertToMono8(pixelType)){
        return QtConverterDetail::convertViaPylon(
            pylonImg, converter, Pylon::PixelType_Mono8, QImage::Format_Grayscale8);
    }
    if(QtConverterDetail::shouldConvertToMono16(pixelType)){
        return QtConverterDetail::convertViaPylon(
            pylonImg, converter, Pylon::PixelType_Mono16, QImage::Format_Grayscale16);
    }
    if(QtConverterDetail::shouldConvertToColor(pixelType)){
        return QtConverterDetail::convertViaPylon(
            pylonImg, converter, Pylon::PixelType_BGRA8packed, QImage::Format_ARGB32);
    }

    try{
        return QtConverterDetail::convertViaPylon(
            pylonImg, converter, Pylon::PixelType_BGRA8packed, QImage::Format_ARGB32);
    }catch(const Pylon::GenericException&){
    }
    try{
        return QtConverterDetail::convertViaPylon(
            pylonImg, converter, Pylon::PixelType_Mono16, QImage::Format_Grayscale16);
    }catch(const Pylon::GenericException&){
    }
    try{
        return QtConverterDetail::convertViaPylon(
            pylonImg, converter, Pylon::PixelType_Mono8, QImage::Format_Grayscale8);
    }catch(const Pylon::GenericException&){
    }

    return {};
}
#endif

#endif // CONVERTERQT_H
