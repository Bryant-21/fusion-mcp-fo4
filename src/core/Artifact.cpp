#include <Windows.h>
#include <bcrypt.h>
#include <wincodec.h>
#include <wrl/client.h>
#include "Artifact.h"
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace fusionmcp {
namespace {
std::uint32_t BigEndian(const unsigned char* p) {
    return (std::uint32_t(p[0])<<24)|(std::uint32_t(p[1])<<16)|(std::uint32_t(p[2])<<8)|p[3];
}
bool Sha256(const std::vector<unsigned char>& data,std::string& hex) {
    BCRYPT_ALG_HANDLE algorithm{};
    if (BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)!=0) return false;
    std::array<unsigned char,32> digest{};
    const bool ok=BCryptHash(algorithm,nullptr,0,const_cast<PUCHAR>(data.data()),static_cast<ULONG>(data.size()),
        digest.data(),static_cast<ULONG>(digest.size()))==0;
    BCryptCloseAlgorithmProvider(algorithm,0);
    if (!ok) return false;
    static constexpr char digits[]="0123456789abcdef";
    hex.clear();
    for (const auto byte:digest) {hex+=digits[byte>>4];hex+=digits[byte&15];}
    return true;
}
std::string Base64(const std::vector<unsigned char>& data) {
    static constexpr char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size()+2)/3*4);
    for (std::size_t i=0;i<data.size();i+=3) {
        const std::uint32_t n=(std::uint32_t(data[i])<<16)|(i+1<data.size()?std::uint32_t(data[i+1])<<8:0)|(i+2<data.size()?data[i+2]:0);
        out+=alphabet[(n>>18)&63];out+=alphabet[(n>>12)&63];
        out+=i+1<data.size()?alphabet[(n>>6)&63]:'=';
        out+=i+2<data.size()?alphabet[n&63]:'=';
    }
    return out;
}
using Microsoft::WRL::ComPtr;
bool EncodeJpeg(IWICImagingFactory* factory,IWICBitmapSource* source,float quality,std::vector<unsigned char>& out) {
    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr,TRUE,&stream))) return false;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> options;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatJpeg,nullptr,&encoder))||
        FAILED(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache))||
        FAILED(encoder->CreateNewFrame(&frame,&options))) return false;
    PROPBAG2 option{};
    option.pstrName=const_cast<LPOLESTR>(L"ImageQuality");
    VARIANT value{};
    value.vt=VT_R4;value.fltVal=quality;
    UINT width=0,height=0;
    WICPixelFormatGUID format=GUID_WICPixelFormat24bppBGR;
    if (FAILED(options->Write(1,&option,&value))||FAILED(frame->Initialize(options.Get()))||
        FAILED(source->GetSize(&width,&height))||FAILED(frame->SetSize(width,height))||
        FAILED(frame->SetPixelFormat(&format))||FAILED(frame->WriteSource(source,nullptr))||
        FAILED(frame->Commit())||FAILED(encoder->Commit())) return false;
    HGLOBAL memory{};
    if (FAILED(GetHGlobalFromStream(stream.Get(),&memory))) return false;
    STATSTG stat{};
    if (FAILED(stream->Stat(&stat,STATFLAG_NONAME))) return false;
    const auto* bytes=static_cast<const unsigned char*>(GlobalLock(memory));
    if (!bytes) return false;
    out.assign(bytes,bytes+stat.cbSize.QuadPart);
    GlobalUnlock(memory);
    return true;
}
// COM is initialised per call because control tools run on transport worker threads.
struct ComScope {
    HRESULT hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    ~ComScope() {if (SUCCEEDED(hr)) CoUninitialize();}
    bool Usable() const {return SUCCEEDED(hr)||hr==RPC_E_CHANGED_MODE;}
};
}

ImagePreview EncodePreview(const std::string& path,std::uint32_t maxWidth,std::size_t maxBase64Bytes) {
    ImagePreview preview;
    ComScope com;
    if (!com.Usable()) {preview.error="COM unavailable";return preview;}
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)))) {
        preview.error="Windows Imaging Component unavailable";return preview;
    }
    if (FAILED(factory->CreateDecoderFromFilename(std::filesystem::path(path).c_str(),nullptr,GENERIC_READ,
            WICDecodeMetadataCacheOnDemand,&decoder))||FAILED(decoder->GetFrame(0,&frame))||
        FAILED(frame->GetSize(&preview.sourceWidth,&preview.sourceHeight))||!preview.sourceWidth||!preview.sourceHeight) {
        preview.error="Image cannot be decoded";return preview;
    }
    ComPtr<IWICFormatConverter> converted;
    if (FAILED(factory->CreateFormatConverter(&converted))||
        FAILED(converted->Initialize(frame.Get(),GUID_WICPixelFormat24bppBGR,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom))) {
        preview.error="Image pixel format cannot be converted";return preview;
    }
    auto width=std::min(maxWidth,preview.sourceWidth);
    for (int attempt=0;attempt<6&&width>=16;++attempt,width=width*3/4) {
        const auto height=std::max<std::uint32_t>(1,static_cast<std::uint32_t>(std::uint64_t(preview.sourceHeight)*width/preview.sourceWidth));
        ComPtr<IWICBitmapScaler> scaler;
        if (FAILED(factory->CreateBitmapScaler(&scaler))||
            FAILED(scaler->Initialize(converted.Get(),width,height,WICBitmapInterpolationModeFant))) {
            preview.error="Image cannot be scaled";return preview;
        }
        for (const int quality:{85,70,55}) {
            std::vector<unsigned char> jpeg;
            if (!EncodeJpeg(factory.Get(),scaler.Get(),quality/100.0f,jpeg)) {preview.error="JPEG encoding failed";return preview;}
            if ((jpeg.size()+2)/3*4>maxBase64Bytes) continue;
            preview.ok=true;preview.width=width;preview.height=height;preview.quality=quality;
            preview.base64=Base64(jpeg);
            return preview;
        }
    }
    preview.error="Image does not fit the response budget";
    return preview;
}

ArtifactInfo InspectPng(const std::string& path) {
    ArtifactInfo info;
    std::ifstream file(path,std::ios::binary);
    if (!file) {info.error="Artifact file cannot be opened";return info;}
    const std::vector<unsigned char> data((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());
    info.bytes=data.size();
    static constexpr unsigned char signature[]={0x89,'P','N','G','\r','\n',0x1A,'\n'};
    if (data.size()<33||!std::equal(std::begin(signature),std::end(signature),data.begin())||
        BigEndian(&data[8])!=13||std::string(data.begin()+12,data.begin()+16)!="IHDR") {
        info.error="Artifact is not a PNG with a leading IHDR chunk";return info;
    }
    info.width=BigEndian(&data[16]);info.height=BigEndian(&data[20]);
    info.bitDepth=data[24];info.colorType=data[25];
    if (!info.width||!info.height) {info.error="PNG has zero dimensions";return info;}
    if (!Sha256(data,info.sha256)) {info.error="SHA-256 unavailable";return info;}
    info.ok=true;
    return info;
}
}
