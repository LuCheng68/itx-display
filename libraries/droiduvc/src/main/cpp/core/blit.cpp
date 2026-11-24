#include "blit.h"
#include <algorithm>
#include <array>
#include <cstdint>
namespace droiduvc {
namespace {
struct CurvePoint { uint8_t input; uint8_t output; };
constexpr std::array<CurvePoint,10> kPoints{{
 {0,0},{6,16},{26,32},{63,64},{100,96},{137,128},{174,160},{211,192},{243,220},{255,248}
}};
constexpr std::array<uint8_t,256> makeLut() {
 std::array<uint8_t,256> lut{};
 for(int v=0;v<256;++v) {
  size_t upper=1;
  while(upper<kPoints.size()-1 && v>kPoints[upper].input) ++upper;
  const auto a=kPoints[upper-1], b=kPoints[upper];
  const int dx=static_cast<int>(b.input)-a.input, dy=static_cast<int>(b.output)-a.output;
  const int offset=v-a.input;
  const int mapped=static_cast<int>(a.output)+(dx>0?(offset*dy+dx/2)/dx:0);
  lut[static_cast<size_t>(v)]=static_cast<uint8_t>(std::clamp(mapped,0,255));
 }
 return lut;
}
constexpr auto kRgbLut=makeLut();
}
void blitRgbToRgbx(const uint8_t *src,size_t srcStrideBytes,uint32_t srcWidth,uint32_t srcHeight,
 uint8_t *dst,size_t dstStrideBytes,uint32_t dstWidth,uint32_t dstHeight,bool applyRgbExperimentCompensation) {
 const uint32_t rows=std::min(srcHeight,dstHeight), cols=std::min(srcWidth,dstWidth);
 if(!applyRgbExperimentCompensation) {
  for(uint32_t y=0;y<rows;++y) {
   const uint8_t *s=src+y*srcStrideBytes; uint8_t *d=dst+y*dstStrideBytes;
   for(uint32_t x=0;x<cols;++x,s+=3,d+=4) { d[0]=s[0];d[1]=s[1];d[2]=s[2];d[3]=0xFF; }
  }
  return;
 }
 // Fuse lookup into existing copy to avoid a second full-frame pass.
 for(uint32_t y=0;y<rows;++y) {
  const uint8_t *s=src+y*srcStrideBytes; uint8_t *d=dst+y*dstStrideBytes;
  for(uint32_t x=0;x<cols;++x,s+=3,d+=4) {
   d[0]=kRgbLut[s[0]];d[1]=kRgbLut[s[1]];d[2]=kRgbLut[s[2]];d[3]=0xFF;
  }
 }
}
}
