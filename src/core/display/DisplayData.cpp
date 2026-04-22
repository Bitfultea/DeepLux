#include "DisplayData.h"
#include "../model/ImageData.h"

namespace DeepLux {

DisplayData::DisplayData(const ImageData& img) : m_variant(img) {}

} // namespace DeepLux