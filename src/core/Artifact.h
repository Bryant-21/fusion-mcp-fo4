#pragma once
// Project-owned GPL-3.0 code. Verifies a finished image file off engine owners.
#include <cstdint>
#include <string>

namespace fusionmcp {
struct ArtifactInfo {
    bool ok=false;
    std::string error;
    std::uint64_t bytes=0;
    std::uint32_t width=0,height=0;
    std::uint8_t bitDepth=0,colorType=0;
    std::string sha256;
};
// Reads the whole file: PNG signature/IHDR and a SHA-256 digest. Blocking I/O;
// never call on the Game or Render owner.
ArtifactInfo InspectPng(const std::string& path);

struct ImagePreview {
    bool ok=false;
    std::string error;
    std::uint32_t sourceWidth=0,sourceHeight=0,width=0,height=0;
    int quality=0;
    std::string base64;
};
// Decodes an image file and re-encodes it as a JPEG no wider than maxWidth whose
// base64 form fits maxBase64Bytes (lowering quality, then size). Blocking; off owners.
ImagePreview EncodePreview(const std::string& path,std::uint32_t maxWidth,std::size_t maxBase64Bytes);
}
