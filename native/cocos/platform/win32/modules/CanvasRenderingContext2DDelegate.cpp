/****************************************************************************
 Copyright (c) 2018-2023 Xiamen Yaji Software Co., Ltd.

 http://www.cocos.com

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights to
 use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 of the Software, and to permit persons to whom the Software is furnished to do so,
 subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
****************************************************************************/

#include "platform/win32/modules/CanvasRenderingContext2DDelegate.h"
#include "base/memory/Memory.h"
#include "platform/win32/modules/DirectWriteTextRasterizer.h"

#include <algorithm>
#include <cmath>

namespace {
void fillRectWithColor(uint8_t *buf, uint32_t totalWidth, uint32_t totalHeight, uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    CC_ASSERT(x + width <= totalWidth);
    CC_ASSERT(y + height <= totalHeight);

    uint32_t y0 = y;
    uint32_t y1 = y + height;
    uint8_t *p;
    for (uint32_t offsetY = y0; offsetY < y1; ++offsetY) {
        for (uint32_t offsetX = x; offsetX < (x + width); ++offsetX) {
            p = buf + (totalWidth * offsetY + offsetX) * 4;
            *p++ = r;
            *p++ = g;
            *p++ = b;
            *p++ = a;
        }
    }
}

void blendStraightAlpha(uint8_t *destination, uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha) {
    if (alpha == 0U) {
        return;
    }

    const float sourceAlpha = static_cast<float>(alpha) / 255.0F;
    const float destinationAlpha = static_cast<float>(destination[3]) / 255.0F;
    const float outputAlpha = sourceAlpha + destinationAlpha * (1.0F - sourceAlpha);
    if (outputAlpha <= 0.0F) {
        return;
    }

    const float destinationFactor = destinationAlpha * (1.0F - sourceAlpha);
    destination[0] = static_cast<uint8_t>(std::round((static_cast<float>(red) * sourceAlpha + static_cast<float>(destination[0]) * destinationFactor) / outputAlpha));
    destination[1] = static_cast<uint8_t>(std::round((static_cast<float>(green) * sourceAlpha + static_cast<float>(destination[1]) * destinationFactor) / outputAlpha));
    destination[2] = static_cast<uint8_t>(std::round((static_cast<float>(blue) * sourceAlpha + static_cast<float>(destination[2]) * destinationFactor) / outputAlpha));
    destination[3] = static_cast<uint8_t>(std::round(outputAlpha * 255.0F));
}

std::vector<uint8_t> dilateCoverage(const std::vector<uint8_t> &source, int width, int height, uint32_t radius) {
    if (radius == 0U || source.empty()) {
        return source;
    }

    std::vector<uint8_t> result(source.size(), 0U);
    const int signedRadius = static_cast<int>(radius);
    const int radiusSquared = signedRadius * signedRadius;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            uint8_t maximumCoverage = 0U;
            const int minimumY = std::max(0, y - signedRadius);
            const int maximumY = std::min(height - 1, y + signedRadius);
            const int minimumX = std::max(0, x - signedRadius);
            const int maximumX = std::min(width - 1, x + signedRadius);
            for (int sampleY = minimumY; sampleY <= maximumY && maximumCoverage < 255U; ++sampleY) {
                const int deltaY = sampleY - y;
                for (int sampleX = minimumX; sampleX <= maximumX; ++sampleX) {
                    const int deltaX = sampleX - x;
                    if (deltaX * deltaX + deltaY * deltaY > radiusSquared) {
                        continue;
                    }
                    const size_t sampleIndex = static_cast<size_t>(sampleY) * static_cast<size_t>(width) + static_cast<size_t>(sampleX);
                    maximumCoverage = std::max(maximumCoverage, source[sampleIndex]);
                }
            }
            result[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)] = maximumCoverage;
        }
    }
    return result;
}
} // namespace

namespace cc {
CanvasRenderingContext2DDelegate::CanvasRenderingContext2DDelegate()
: _directWriteRasterizer(std::make_unique<DirectWriteTextRasterizer>()) {
    HDC hdc = GetDC(_wnd);
    _DC = CreateCompatibleDC(hdc);
    ReleaseDC(_wnd, hdc);
}

CanvasRenderingContext2DDelegate::~CanvasRenderingContext2DDelegate() {
    if (_previousPen) {
        SelectObject(_DC, _previousPen);
        _previousPen = nullptr;
    }
    if (_hpen) {
        DeleteObject(_hpen);
        _hpen = nullptr;
    }
    deleteBitmap();
    removeCustomFont();
    if (_DC)
        DeleteDC(_DC);
}

void CanvasRenderingContext2DDelegate::recreateBuffer(float w, float h) {
    _bufferWidth = w;
    _bufferHeight = h;
    if (_bufferWidth < 1.0F || _bufferHeight < 1.0F) {
        deleteBitmap();
        return;
    }

    auto textureSize = static_cast<int>(_bufferWidth * _bufferHeight * 4);
    auto *data = static_cast<uint8_t *>(malloc(sizeof(uint8_t) * textureSize));
    memset(data, 0x00, textureSize);
    _imageData.fastSet(data, textureSize);

    prepareBitmap(static_cast<int>(_bufferWidth), static_cast<int>(_bufferHeight));
}

void CanvasRenderingContext2DDelegate::beginPath() {
    // called: set_lineWidth() -> beginPath() -> moveTo() -> lineTo() -> stroke(), when draw line
    if (_previousPen) {
        SelectObject(_DC, _previousPen);
        _previousPen = nullptr;
    }
    if (_hpen) {
        DeleteObject(_hpen);
        _hpen = nullptr;
    }
    _hpen = CreatePen(PS_SOLID, static_cast<int>(_lineWidth), RGB(255, 255, 255));
    _previousPen = SelectObject(_DC, _hpen);

    SetBkMode(_DC, TRANSPARENT);
}

void CanvasRenderingContext2DDelegate::closePath() {
}

void CanvasRenderingContext2DDelegate::moveTo(float x, float y) {
    MoveToEx(_DC, static_cast<int>(x), static_cast<int>(-(y - _bufferHeight - _fontSize)), nullptr);
}

void CanvasRenderingContext2DDelegate::lineTo(float x, float y) {
    LineTo(_DC, static_cast<int>(x), static_cast<int>(-(y - _bufferHeight - _fontSize)));
}

void CanvasRenderingContext2DDelegate::stroke() {
    if (_previousPen) {
        SelectObject(_DC, _previousPen);
        _previousPen = nullptr;
    }
    if (_hpen) {
        DeleteObject(_hpen);
        _hpen = nullptr;
    }
    if (_bufferWidth < 1.0F || _bufferHeight < 1.0F) {
        return;
    }

    fillTextureData(_strokeStyle);
}

void CanvasRenderingContext2DDelegate::saveContext() {
    _savedDC = SaveDC(_DC);
}

void CanvasRenderingContext2DDelegate::restoreContext() {
    BOOL ret = RestoreDC(_DC, _savedDC);
    if (0 == ret) {
        SE_LOGD("CanvasRenderingContext2DImpl restore context failed.\n");
    }
}

void CanvasRenderingContext2DDelegate::clearRect(float /*x*/, float /*y*/, float w, float h) {
    if (_bufferWidth < 1.0F || _bufferHeight < 1.0F) {
        return;
    }

    if (_imageData.isNull()) {
        return;
    }

    recreateBuffer(w, h);
}

void CanvasRenderingContext2DDelegate::fillRect(float x, float y, float w, float h) {
    if (_bufferWidth < 1.0F || _bufferHeight < 1.0F) {
        return;
    }

    // not filled all Bits in buffer? the buffer length is _bufferWidth * _bufferHeight * 4, but it filled _bufferWidth * _bufferHeight * 3?
    uint8_t *buffer = _imageData.getBytes();
    if (buffer) {
        uint8_t r = static_cast<uint8_t>(_fillStyle[0] * 255.0f);
        uint8_t g = static_cast<uint8_t>(_fillStyle[1] * 255.0f);
        uint8_t b = static_cast<uint8_t>(_fillStyle[2] * 255.0f);
        uint8_t a = static_cast<uint8_t>(_fillStyle[3] * 255.0f);
        fillRectWithColor(buffer,
                          static_cast<uint32_t>(_bufferWidth),
                          static_cast<uint32_t>(_bufferHeight),
                          static_cast<uint32_t>(x),
                          static_cast<uint32_t>(y),
                          static_cast<uint32_t>(w),
                          static_cast<uint32_t>(h), r, g, b, a);
    }
}

void CanvasRenderingContext2DDelegate::fillText(const ccstd::string &text, float x, float y, float /*maxWidth*/) {
    if (text.empty() || _bufferWidth < 1.0F || _bufferHeight < 1.0F) {
        return;
    }

    Point offsetPoint = convertDrawPoint(Point{x, y}, text);

    std::vector<uint8_t> coverageMask;
    if (rasterizeText(text, offsetPoint[0], offsetPoint[1], &coverageMask)) {
        compositeCoverage(coverageMask, _fillStyle);
    } else {
        drawText(text, static_cast<int>(std::round(offsetPoint[0])), static_cast<int>(std::round(offsetPoint[1])));
        fillTextureData(_fillStyle);
    }
}

CanvasRenderingContext2DDelegate::Size CanvasRenderingContext2DDelegate::measureText(const ccstd::string &text) {
    if (text.empty())
        return ccstd::array<float, 2>{0.0f, 0.0f};

    float width = 0.0F;
    float height = 0.0F;
    if (_useDirectWrite && _directWriteRasterizer->measureText(text.data(), text.size(), &width, &height)) {
        return Size{width, height};
    }

    int bufferLen = 0;
    wchar_t *pwszBuffer = CanvasRenderingContext2DDelegate::utf8ToUtf16(text, &bufferLen);
    Size size = sizeWithText(pwszBuffer, bufferLen);
    // SE_LOGD("CanvasRenderingContext2DImpl::measureText: %s, %d, %d\n", text.c_str(), size.cx, size.cy);
    CC_SAFE_DELETE_ARRAY(pwszBuffer);
    return size;
}

void CanvasRenderingContext2DDelegate::updateFont(const ccstd::string &fontName,
                                                  float fontSize,
                                                  bool bold,
                                                  bool italic,
                                                  bool /* oblique */,
                                                  bool /* smallCaps */) {
    do {
        _fontName = fontName;
        _fontSize = static_cast<int>(fontSize);
        ccstd::string fontPath;
        LOGFONTA tFont = {0};
        if (!_fontName.empty()) {
            // firstly, try to create font from ttf file
            const auto &fontInfoMap = getFontFamilyNameMap();
            auto iter = fontInfoMap.find(_fontName);
            if (iter != fontInfoMap.end()) {
                fontPath = iter->second;
                ccstd::string tmpFontPath = fontPath;
                size_t nFindPos = tmpFontPath.rfind("/");
                tmpFontPath = &tmpFontPath[nFindPos + 1];
                nFindPos = tmpFontPath.rfind(".");
                // IDEA: draw ttf failed if font file name not equal font face name
                // for example: "DejaVuSansMono-Oblique" not equal "DejaVu Sans Mono"  when using DejaVuSansMono-Oblique.ttf
                _fontName = tmpFontPath.substr(0, nFindPos);
            } else {
                auto nFindPos = fontName.rfind("/");
                if (nFindPos != fontName.npos) {
                    if (fontName.length() == nFindPos + 1) {
                        _fontName = "";
                    } else {
                        _fontName = &_fontName[nFindPos + 1];
                    }
                }
            }
            tFont.lfCharSet = DEFAULT_CHARSET;
            strcpy_s(tFont.lfFaceName, LF_FACESIZE, _fontName.c_str());
        }

        if (_fontSize) {
            tFont.lfHeight = -_fontSize;
        }

        if (bold) {
            tFont.lfWeight = FW_BOLD;
        } else {
            tFont.lfWeight = FW_NORMAL;
        }

        tFont.lfItalic = italic;

        // DirectWrite is used for installed system fonts. Keep grayscale GDI as
        // a fallback for project fonts registered through AddFontResource.
        tFont.lfQuality = ANTIALIASED_QUALITY;

        // delete old font
        removeCustomFont();

        if (!fontPath.empty()) {
            _curFontPath = fontPath;
            wchar_t *pwszBuffer = utf8ToUtf16(_curFontPath);
            if (pwszBuffer) {
                if (AddFontResource(pwszBuffer)) {
                    SendMessage(_wnd, WM_FONTCHANGE, 0, 0);
                }
                delete[] pwszBuffer;
                pwszBuffer = nullptr;
            }
        }

        // create new font
        _font = CreateFontIndirectA(&tFont);
        if (!_font) {
            // create failed, use default font
            SE_LOGE("Failed to create custom font(font name: %s, font size: %f), use default font.\n",
                    _fontName.c_str(), fontSize);
        } else {
            SelectObject(_DC, _font);
            SendMessage(_wnd, WM_FONTCHANGE, 0, 0);
        }

        const ccstd::string directWriteFontFamily = _fontName.empty() ? ccstd::string("Arial") : _fontName;
        _useDirectWrite = fontPath.empty() && _directWriteRasterizer->updateFont(directWriteFontFamily.data(),
                                                                                 directWriteFontFamily.size(),
                                                                                 fontSize,
                                                                                 bold,
                                                                                 italic);
    } while (false);
}

void CanvasRenderingContext2DDelegate::setTextAlign(TextAlign align) {
    _textAlign = align;
}

void CanvasRenderingContext2DDelegate::setTextBaseline(TextBaseline baseline) {
    _textBaseLine = baseline;
}

void CanvasRenderingContext2DDelegate::setFillStyle(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    _fillStyle = {r / 255.0F, g / 255.0F, b / 255.0F, a / 255.0F};
}

void CanvasRenderingContext2DDelegate::setStrokeStyle(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    _strokeStyle = {r / 255.0F, g / 255.0F, b / 255.0F, a / 255.0F};
}

void CanvasRenderingContext2DDelegate::setLineWidth(float lineWidth) {
    _lineWidth = lineWidth;
}

const cc::Data &CanvasRenderingContext2DDelegate::getDataRef() const {
    return _imageData;
}

// change utf-8 string to utf-16, pRetLen is the string length after changing
wchar_t *CanvasRenderingContext2DDelegate::utf8ToUtf16(const ccstd::string &str, int *pRetLen /* = nullptr*/) {
    wchar_t *pwszBuffer = nullptr;
    do {
        if (str.empty()) {
            break;
        }
        int nLen = static_cast<int>(str.size());
        int nBufLen = nLen + 1;
        pwszBuffer = ccnew wchar_t[nBufLen];
        CC_BREAK_IF(!pwszBuffer);
        memset(pwszBuffer, 0, sizeof(wchar_t) * nBufLen);
        // str.size() not equal actuallyLen for Chinese char
        int actuallyLen = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), nLen, pwszBuffer, nBufLen);
        // SE_LOGE("_utf8ToUtf16, str:%s, strLen:%d, retLen:%d\n", str.c_str(), str.size(), actuallyLen);
        if (pRetLen != nullptr) {
            *pRetLen = actuallyLen;
        }
    } while (false);
    return pwszBuffer;
}

void CanvasRenderingContext2DDelegate::removeCustomFont() {
    HFONT hDefFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    if (hDefFont != _font) {
        DeleteObject(SelectObject(_DC, hDefFont));
    }
    // release temp font resource
    if (!_curFontPath.empty()) {
        wchar_t *pwszBuffer = utf8ToUtf16(_curFontPath);
        if (pwszBuffer) {
            RemoveFontResource(pwszBuffer);
            SendMessage(_wnd, WM_FONTCHANGE, 0, 0);
            delete[] pwszBuffer;
            pwszBuffer = nullptr;
        }
        _curFontPath.clear();
    }
}

// x, y offset value
int CanvasRenderingContext2DDelegate::drawText(const ccstd::string &text, int x, int y) {
    int nRet = 0;
    wchar_t *pwszBuffer = nullptr;
    do {
        CC_BREAK_IF(text.empty());

        DWORD dwFmt = DT_SINGLELINE | DT_NOPREFIX;

        int bufferLen = 0;
        pwszBuffer = utf8ToUtf16(text, &bufferLen);

        Size newSize = sizeWithText(pwszBuffer, bufferLen);

        _textSize = newSize;

        RECT rcText = {0};

        rcText.right = static_cast<int>(newSize[0]);
        rcText.bottom = static_cast<int>(newSize[1]);

        LONG offsetX = x;
        LONG offsetY = y;
        if (offsetX || offsetY) {
            OffsetRect(&rcText, offsetX, offsetY);
        }

        // SE_LOGE("_drawText text,%s size: (%d, %d) offset after convert: (%d, %d) \n", text.c_str(), newSize.cx, newSize.cy, offsetX, offsetY);

        SetBkMode(_DC, TRANSPARENT);
        SetTextColor(_DC, RGB(255, 255, 255)); // white color

        // draw text
        nRet = DrawTextW(_DC, pwszBuffer, bufferLen, &rcText, dwFmt);
    } while (false);
    CC_SAFE_DELETE_ARRAY(pwszBuffer);

    return nRet;
}

CanvasRenderingContext2DDelegate::Size CanvasRenderingContext2DDelegate::sizeWithText(const wchar_t *pszText, int nLen) {
    Size tRet{0, 0};
    do {
        CC_BREAK_IF(!pszText || nLen <= 0);

        RECT rc = {0, 0, 0, 0};
        DWORD dwCalcFmt = DT_CALCRECT | DT_NOPREFIX;

        // measure text size
        DrawTextW(_DC, pszText, nLen, &rc, dwCalcFmt);

        tRet[0] = static_cast<float>(rc.right);
        tRet[1] = static_cast<float>(rc.bottom);
    } while (false);

    return tRet;
}

void CanvasRenderingContext2DDelegate::prepareBitmap(int nWidth, int nHeight) {
    // release bitmap
    deleteBitmap();

    if (nWidth > 0 && nHeight > 0) {
        _bmp = CreateBitmap(nWidth, nHeight, 1, 32, nullptr);
        if (!_bmp) {
            return;
        }
        HGDIOBJ previousBitmap = SelectObject(_DC, _bmp);
        if (!previousBitmap || previousBitmap == HGDI_ERROR) {
            DeleteObject(_bmp);
            _bmp = nullptr;
            return;
        }
        if (!_defaultBitmap) {
            _defaultBitmap = static_cast<HBITMAP>(previousBitmap);
        }
        clearBitmapMask();
    }
}

void CanvasRenderingContext2DDelegate::deleteBitmap() {
    if (_bmp) {
        if (_DC && _defaultBitmap && GetCurrentObject(_DC, OBJ_BITMAP) == _bmp) {
            SelectObject(_DC, _defaultBitmap);
        }
        DeleteObject(_bmp);
        _bmp = nullptr;
    }
}

void CanvasRenderingContext2DDelegate::clearBitmapMask() {
    if (_DC && _bmp && _bufferWidth > 0.0F && _bufferHeight > 0.0F) {
        PatBlt(_DC, 0, 0, static_cast<int>(_bufferWidth), static_cast<int>(_bufferHeight), BLACKNESS);
    }
}

bool CanvasRenderingContext2DDelegate::readBitmapCoverage(std::vector<uint8_t> *coverageMask) {
    if (!coverageMask || !_DC || !_bmp || _bufferWidth < 1.0F || _bufferHeight < 1.0F) {
        return false;
    }

    const int width = static_cast<int>(_bufferWidth);
    const int height = static_cast<int>(_bufferHeight);
    std::vector<uint8_t> bitmapData(static_cast<size_t>(width) * static_cast<size_t>(height) * 4U);
    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = -height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
    if (!_defaultBitmap) {
        return false;
    }
    SelectObject(_DC, _defaultBitmap);
    const int scanLines = GetDIBits(_DC,
                                    _bmp,
                                    0,
                                    static_cast<UINT>(height),
                                    bitmapData.data(),
                                    &bitmapInfo,
                                    DIB_RGB_COLORS);
    SelectObject(_DC, _bmp);
    if (scanLines == 0) {
        return false;
    }

    coverageMask->resize(static_cast<size_t>(width) * static_cast<size_t>(height));
    for (size_t pixelIndex = 0; pixelIndex < coverageMask->size(); ++pixelIndex) {
        // GDI renders white text on a black bitmap, so every color channel is
        // the grayscale coverage value.
        (*coverageMask)[pixelIndex] = bitmapData[pixelIndex * 4U + 2U];
    }
    return true;
}

bool CanvasRenderingContext2DDelegate::rasterizeText(const ccstd::string &text,
                                                     float x,
                                                     float y,
                                                     std::vector<uint8_t> *coverageMask) const {
    return _useDirectWrite && _directWriteRasterizer->rasterize(text.data(),
                                                                text.size(),
                                                                x,
                                                                y,
                                                                static_cast<int>(_bufferWidth),
                                                                static_cast<int>(_bufferHeight),
                                                                coverageMask);
}

void CanvasRenderingContext2DDelegate::compositeCoverage(const std::vector<uint8_t> &coverageMask,
                                                         const Color4F &color,
                                                         uint32_t dilationRadius) {
    uint8_t *imageBuffer = _imageData.getBytes();
    const int width = static_cast<int>(_bufferWidth);
    const int height = static_cast<int>(_bufferHeight);
    if (!imageBuffer || width <= 0 || height <= 0 || coverageMask.size() != static_cast<size_t>(width) * static_cast<size_t>(height)) {
        return;
    }

    const std::vector<uint8_t> expandedCoverage = dilateCoverage(coverageMask, width, height, dilationRadius);
    const uint8_t red = static_cast<uint8_t>(std::round(color[0] * 255.0F));
    const uint8_t green = static_cast<uint8_t>(std::round(color[1] * 255.0F));
    const uint8_t blue = static_cast<uint8_t>(std::round(color[2] * 255.0F));
    const float colorAlpha = color[3];
    for (size_t pixelIndex = 0; pixelIndex < expandedCoverage.size(); ++pixelIndex) {
        const uint8_t alpha = static_cast<uint8_t>(std::round(static_cast<float>(expandedCoverage[pixelIndex]) * colorAlpha));
        blendStraightAlpha(imageBuffer + pixelIndex * 4U, red, green, blue, alpha);
    }
}

void CanvasRenderingContext2DDelegate::fillTextureData(const Color4F &color, uint32_t dilationRadius) {
    std::vector<uint8_t> coverageMask;
    if (readBitmapCoverage(&coverageMask)) {
        compositeCoverage(coverageMask, color, dilationRadius);
    }
    clearBitmapMask();
}

ccstd::array<float, 2> CanvasRenderingContext2DDelegate::convertDrawPoint(Point point, const ccstd::string &text) {
    Size textSize = measureText(text);
    if (_textAlign == TextAlign::CENTER) {
        point[0] -= textSize[0] / 2.0f;
    } else if (_textAlign == TextAlign::RIGHT) {
        point[0] -= textSize[0];
    }

    float directWriteLineHeight = 0.0F;
    float directWriteBaseline = 0.0F;
    const bool hasDirectWriteMetrics = _useDirectWrite && _directWriteRasterizer->getLineMetrics(&directWriteLineHeight, &directWriteBaseline);
    if (_textBaseLine == TextBaseline::TOP) {
        // DrawText default
        if (!hasDirectWriteMetrics) {
            GetTextMetrics(_DC, &_tm);
            point[1] += -_tm.tmInternalLeading;
        }
    } else if (_textBaseLine == TextBaseline::MIDDLE) {
        point[1] += -(hasDirectWriteMetrics ? directWriteLineHeight : textSize[1]) / 2.0F;
    } else if (_textBaseLine == TextBaseline::BOTTOM) {
        point[1] += -(hasDirectWriteMetrics ? directWriteLineHeight : textSize[1]);
    } else if (_textBaseLine == TextBaseline::ALPHABETIC) {
        if (hasDirectWriteMetrics) {
            point[1] -= directWriteBaseline;
        } else {
            GetTextMetrics(_DC, &_tm);
            point[1] -= _tm.tmAscent;
        }
    }

    return point;
}

void CanvasRenderingContext2DDelegate::fill() {
}

void CanvasRenderingContext2DDelegate::setLineCap(const ccstd::string & /* lineCap */) {
}

void CanvasRenderingContext2DDelegate::setLineJoin(const ccstd::string & /* lineCap */) {
}

void CanvasRenderingContext2DDelegate::fillImageData(const Data & /* imageData */,
                                                     float /* imageWidth */,
                                                     float /* imageHeight */,
                                                     float /* offsetX */,
                                                     float /* offsetY */) {
}

void CanvasRenderingContext2DDelegate::strokeText(const ccstd::string &text,
                                                  float x,
                                                  float y,
                                                  float /* maxWidth */) {
    if (text.empty() || _bufferWidth < 1.0F || _bufferHeight < 1.0F || _lineWidth <= 0.0F) {
        return;
    }

    Point offsetPoint = convertDrawPoint(Point{x, y}, text);
    const uint32_t outlineRadius = static_cast<uint32_t>(std::max(1.0F, std::ceil(_lineWidth * 0.5F)));
    std::vector<uint8_t> coverageMask;
    if (rasterizeText(text, offsetPoint[0], offsetPoint[1], &coverageMask)) {
        compositeCoverage(coverageMask, _strokeStyle, outlineRadius);
    } else {
        drawText(text, static_cast<int>(std::round(offsetPoint[0])), static_cast<int>(std::round(offsetPoint[1])));
        fillTextureData(_strokeStyle, outlineRadius);
    }
}

void CanvasRenderingContext2DDelegate::rect(float /* x */,
                                            float /* y */,
                                            float /* w */,
                                            float /* h */) {
}

} // namespace cc
