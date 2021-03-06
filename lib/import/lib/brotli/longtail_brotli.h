#pragma once

#include "../../src/longtail.h"

#ifdef __cplusplus
extern "C" {
#endif

extern struct Longtail_CompressionAPI* Longtail_CreateBrotliCompressionAPI();
extern Longtail_CompressionAPI_HSettings LONGTAIL_BROTLI_GENERIC_MIN_QUALITY;
extern Longtail_CompressionAPI_HSettings LONGTAIL_BROTLI_GENERIC_DEFAULT_QUALITY;
extern Longtail_CompressionAPI_HSettings LONGTAIL_BROTLI_GENERIC_MAX_QUALITY;
extern Longtail_CompressionAPI_HSettings LONGTAIL_BROTLI_TEXT_MIN_QUALITY;
extern Longtail_CompressionAPI_HSettings LONGTAIL_BROTLI_TEXT_DEFAULT_QUALITY;
extern Longtail_CompressionAPI_HSettings LONGTAIL_BROTLI_TEXT_MAX_QUALITY;

#ifdef __cplusplus
}
#endif
