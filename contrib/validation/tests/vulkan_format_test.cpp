// SPDX-License-Identifier: MIT
#include "../../../src/addons/mfgunlock/output_state.hpp"
#include "../../../src/addons/mfgunlock/vulkan_contract.hpp"
#include <cassert>
#include <cstdio>
namespace o=mfgunlock::outputstate;
namespace v=mfgunlock::vulkancontract;
int main() {
  o::SetResourceApi(o::ResourceApi::kD3D12);
  assert(o::LowPrecisionUiAlpha(24)); assert(!o::LowPrecisionUiAlpha(58));
  o::SetResourceApi(o::ResourceApi::kVulkan);
  assert(o::LowPrecisionUiAlpha(58)); assert(o::LowPrecisionUiAlpha(64));
  assert(!o::LowPrecisionUiAlpha(24)); assert(!o::LowPrecisionUiAlpha(37));
  for(auto f:{87u,91u}) {
    const auto state=o::Classify(o::ClassifyFormat(f),f,true,o::kColorSpaceSrgbNonlinear,true);
    assert(state.dlssg_supported && !state.hdr);
  }
  auto hdr=o::Classify(o::ClassifyFormat(24),24,true,o::kColorSpaceHdr10Pq,true);
  assert(hdr.hdr&&hdr.dlssg_supported);
  auto scrgb=o::Classify(o::ClassifyFormat(10),10,true,o::kColorSpaceScRgbLinear,true);
  assert(scrgb.hdr&&!scrgb.dlssg_supported);
  v::Image image{1,2,37,64,48,1,0,1,0,1,false};
  assert(v::ImageMetadataValid(image));
  image.mip_count=UINT32_MAX;assert(!v::ImageMetadataValid(image));
  image.mip_count=1;image.first_layer=UINT32_MAX;assert(!v::ImageMetadataValid(image));
  image.first_layer=0;image.view=0;assert(!v::ImageMetadataValid(image));
  assert(!v::DefinedLayout(0)&&!v::DefinedLayout(8)&&!v::DefinedLayout(UINT32_MAX));
  assert(v::DefinedLayout(1)&&v::DefinedLayout(5));
  o::SetResourceApi(o::ResourceApi::kD3D12);
  puts("vulkan_format_test PASS (format domains / HDR / subresource metadata)");
}
