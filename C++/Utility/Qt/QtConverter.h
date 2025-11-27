#ifndef CONVERTERQT_H
#define CONVERTERQT_H
#include <pylon/PylonImage.h>
#include <pylon/ImageFormatConverter.h>

#ifdef QT_GUI_LIB
#include <QImage>
inline QImage convertPylonImageToQImage(Pylon::CPylonImage pylonImg){
    Pylon::CImageFormatConverter converter;
    QImage::Format format = QImage::Format_Invalid;
    switch(pylonImg.GetPixelType()){
    case Pylon::PixelType_Undefined:
    case Pylon::PixelType_Mono1packed:
    case Pylon::PixelType_Mono2packed:
    case Pylon::PixelType_Mono4packed:
    case Pylon::PixelType_Mono8:
    case Pylon::PixelType_Mono8signed:
        format = QImage::Format_Grayscale8;
        break;
    case Pylon::PixelType_Mono10:
    case Pylon::PixelType_Mono10packed:
    case Pylon::PixelType_Mono10p:
    case Pylon::PixelType_Mono12:
    case Pylon::PixelType_Mono12packed:
    case Pylon::PixelType_Mono12p:
    case Pylon::PixelType_Mono16:
        format = QImage::Format_Grayscale16;
        break;
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
    case Pylon::PixelType_RGB12V1packed:
        format = QImage::Format_RGB32;
        break;
    case Pylon::PixelType_Double:
    case Pylon::PixelType_Confidence8:
    case Pylon::PixelType_Confidence16:
    case Pylon::PixelType_Coord3D_C8:
    case Pylon::PixelType_Coord3D_C16:
    case Pylon::PixelType_Coord3D_ABC32f:
#if defined(PYLON_VERSION_MAJOR) && PYLON_VERSION_MAJOR >=8
    case Pylon::PixelType_Data8:
    case Pylon::PixelType_Data8s:
    case Pylon::PixelType_Data16:
    case Pylon::PixelType_Data16s:
    case Pylon::PixelType_Data32:
    case Pylon::PixelType_Data32s:
    case Pylon::PixelType_Data64:
    case Pylon::PixelType_Data64s:
    case Pylon::PixelType_Data32f:
    case Pylon::PixelType_Data64f:
    case Pylon::PixelType_Error8:
#endif
        break;
    }
    QImage outImage(pylonImg.GetWidth(), pylonImg.GetHeight(), format);
    if(format != QImage::Format_RGB32){
        int width = pylonImg.GetWidth();
        int height = pylonImg.GetHeight();
        int bpp = (format == QImage::Format_Grayscale16) ? 2 : 1;
        const int stride = width * bpp;
        const uchar* buffer = static_cast<const uchar*>(pylonImg.GetBuffer());
        outImage = QImage(buffer, width, height, stride, format).copy();
    }else{
        converter.OutputPixelFormat = Pylon::PixelType_BGRA8packed;
        converter.Convert(outImage.bits(), outImage.sizeInBytes(), pylonImg);
    }
    return outImage;
}
#endif
#endif // CONVERTERQT_H
