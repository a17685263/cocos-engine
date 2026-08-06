/****************************************************************************
 Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.

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

#include "platform/win32/modules/DirectWriteTextRasterizer.h"

#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <Windows.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

#include <dwrite.h>

namespace cc {
namespace {

using Microsoft::WRL::ComPtr;

bool utf8ToWide(const char *source, size_t sourceSize, std::wstring *destination) {
    if (!destination || !source || sourceSize == 0U || sourceSize > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return false;
    }

    const int sourceLength = static_cast<int>(sourceSize);
    const int wideLength = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, source, sourceLength, nullptr, 0);
    if (wideLength <= 0) {
        return false;
    }

    destination->resize(static_cast<size_t>(wideLength));
    return MultiByteToWideChar(CP_UTF8,
                               MB_ERR_INVALID_CHARS,
                               source,
                               sourceLength,
                               &(*destination)[0],
                               wideLength) == wideLength;
}

class GlyphMaskRenderer final : public IDWriteTextRenderer {
public:
    GlyphMaskRenderer(IDWriteFactory *factory, int canvasWidth, int canvasHeight, std::vector<uint8_t> *mask)
    : _factory(factory),
      _canvasWidth(canvasWidth),
      _canvasHeight(canvasHeight),
      _mask(mask) {
        _factory->AddRef();
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override {
        if (!object) {
            return E_INVALIDARG;
        }

        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWritePixelSnapping) || riid == __uuidof(IDWriteTextRenderer)) {
            *object = static_cast<IDWriteTextRenderer *>(this);
            AddRef();
            return S_OK;
        }

        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&_referenceCount));
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG referenceCount = static_cast<ULONG>(InterlockedDecrement(&_referenceCount));
        if (referenceCount == 0) {
            delete this;
        }
        return referenceCount;
    }

    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void *, BOOL *isDisabled) override {
        if (!isDisabled) {
            return E_INVALIDARG;
        }
        *isDisabled = FALSE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void *, DWRITE_MATRIX *transform) override {
        if (!transform) {
            return E_INVALIDARG;
        }
        *transform = DWRITE_MATRIX{1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F};
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void *, FLOAT *pixelsPerDip) override {
        if (!pixelsPerDip) {
            return E_INVALIDARG;
        }
        *pixelsPerDip = 1.0F;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void *,
                                           FLOAT baselineOriginX,
                                           FLOAT baselineOriginY,
                                           DWRITE_MEASURING_MODE measuringMode,
                                           const DWRITE_GLYPH_RUN *glyphRun,
                                           const DWRITE_GLYPH_RUN_DESCRIPTION *,
                                           IUnknown *) override {
        if (!glyphRun || !_mask || _canvasWidth <= 0 || _canvasHeight <= 0) {
            return E_INVALIDARG;
        }

        ComPtr<IDWriteGlyphRunAnalysis> analysis;
        HRESULT result = _factory->CreateGlyphRunAnalysis(glyphRun,
                                                          1.0F,
                                                          nullptr,
                                                          DWRITE_RENDERING_MODE_GDI_CLASSIC,
                                                          measuringMode,
                                                          baselineOriginX,
                                                          baselineOriginY,
                                                          &analysis);
        if (FAILED(result)) {
            return result;
        }

        RECT bounds{};
        result = analysis->GetAlphaTextureBounds(DWRITE_TEXTURE_CLEARTYPE_3x1, &bounds);
        if (FAILED(result) || bounds.right <= bounds.left || bounds.bottom <= bounds.top) {
            return result;
        }

        const int width = bounds.right - bounds.left;
        const int height = bounds.bottom - bounds.top;
        const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
        if (pixelCount > std::numeric_limits<UINT32>::max() / 3U) {
            return E_OUTOFMEMORY;
        }

        std::vector<uint8_t> clearTypeMask(pixelCount * 3U);
        result = analysis->CreateAlphaTexture(DWRITE_TEXTURE_CLEARTYPE_3x1,
                                              &bounds,
                                              clearTypeMask.data(),
                                              static_cast<UINT32>(clearTypeMask.size()));
        if (FAILED(result)) {
            return result;
        }

        for (int localY = 0; localY < height; ++localY) {
            const int destinationY = bounds.top + localY;
            if (destinationY < 0 || destinationY >= _canvasHeight) {
                continue;
            }

            for (int localX = 0; localX < width; ++localX) {
                const int destinationX = bounds.left + localX;
                if (destinationX < 0 || destinationX >= _canvasWidth) {
                    continue;
                }

                const size_t sourceIndex = (static_cast<size_t>(localY) * static_cast<size_t>(width) + static_cast<size_t>(localX)) * 3U;
                const uint16_t coverage = static_cast<uint16_t>(clearTypeMask[sourceIndex]) + static_cast<uint16_t>(clearTypeMask[sourceIndex + 1U]) + static_cast<uint16_t>(clearTypeMask[sourceIndex + 2U]);
                const uint8_t grayscaleCoverage = static_cast<uint8_t>((coverage + 1U) / 3U);
                const size_t destinationIndex = static_cast<size_t>(destinationY) * static_cast<size_t>(_canvasWidth) + static_cast<size_t>(destinationX);
                (*_mask)[destinationIndex] = std::max((*_mask)[destinationIndex], grayscaleCoverage);
            }
        }

        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DrawUnderline(void *, FLOAT, FLOAT, const DWRITE_UNDERLINE *, IUnknown *) override {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void *, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH *, IUnknown *) override {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DrawInlineObject(void *, FLOAT, FLOAT, IDWriteInlineObject *, BOOL, BOOL, IUnknown *) override {
        return S_OK;
    }

private:
    ~GlyphMaskRenderer() {
        _factory->Release();
    }

    LONG _referenceCount{1};
    IDWriteFactory *_factory{nullptr};
    int _canvasWidth{0};
    int _canvasHeight{0};
    std::vector<uint8_t> *_mask{nullptr};
};

} // namespace

class DirectWriteTextRasterizer::Impl final {
public:
    Impl() {
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                            __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown **>(_factory.GetAddressOf()));
    }

    bool updateFont(const char *fontFamily, size_t fontFamilyLength, float fontSize, bool bold, bool italic) {
        _textFormat.Reset();
        if (!_factory || fontSize <= 0.0F) {
            return false;
        }

        std::wstring wideFontFamily;
        if (!utf8ToWide(fontFamily, fontFamilyLength, &wideFontFamily)) {
            return false;
        }

        const DWRITE_FONT_WEIGHT weight = bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL;
        const DWRITE_FONT_STYLE style = italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL;
        HRESULT result = _factory->CreateTextFormat(wideFontFamily.c_str(),
                                                    nullptr,
                                                    weight,
                                                    style,
                                                    DWRITE_FONT_STRETCH_NORMAL,
                                                    fontSize,
                                                    L"zh-cn",
                                                    &_textFormat);
        if (FAILED(result)) {
            return false;
        }

        _textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        _textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        _textFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        return true;
    }

    bool measureText(const char *text, size_t textLength, float *width, float *height) const {
        if (!width || !height) {
            return false;
        }

        ComPtr<IDWriteTextLayout> layout;
        if (!createLayout(text, textLength, 65535.0F, 65535.0F, layout.GetAddressOf())) {
            return false;
        }

        DWRITE_TEXT_METRICS metrics{};
        if (FAILED(layout->GetMetrics(&metrics))) {
            return false;
        }

        *width = std::ceil(metrics.widthIncludingTrailingWhitespace);
        *height = std::ceil(metrics.height);
        return true;
    }

    bool getLineMetrics(float *height, float *baseline) const {
        if (!height || !baseline) {
            return false;
        }

        ComPtr<IDWriteTextLayout> layout;
        constexpr char METRICS_SAMPLE[] = "Hg\xE5\x9B\xBD";
        if (!createLayout(METRICS_SAMPLE,
                          sizeof(METRICS_SAMPLE) - 1U,
                          65535.0F,
                          65535.0F,
                          layout.GetAddressOf())) {
            return false;
        }

        DWRITE_LINE_METRICS lineMetrics{};
        UINT32 actualLineCount = 0;
        if (FAILED(layout->GetLineMetrics(&lineMetrics, 1U, &actualLineCount)) || actualLineCount == 0U) {
            return false;
        }

        *height = lineMetrics.height;
        *baseline = lineMetrics.baseline;
        return true;
    }

    bool rasterize(const char *text,
                   size_t textLength,
                   float originX,
                   float originY,
                   int canvasWidth,
                   int canvasHeight,
                   std::vector<uint8_t> *coverageMask) const {
        if (!coverageMask || canvasWidth <= 0 || canvasHeight <= 0) {
            return false;
        }

        if (static_cast<size_t>(canvasWidth) > std::numeric_limits<size_t>::max() / static_cast<size_t>(canvasHeight)) {
            return false;
        }
        const size_t maskSize = static_cast<size_t>(canvasWidth) * static_cast<size_t>(canvasHeight);
        coverageMask->assign(maskSize, 0U);

        ComPtr<IDWriteTextLayout> layout;
        if (!createLayout(text,
                          textLength,
                          static_cast<float>(canvasWidth),
                          static_cast<float>(canvasHeight),
                          layout.GetAddressOf())) {
            return false;
        }

        auto *renderer = new GlyphMaskRenderer(_factory.Get(), canvasWidth, canvasHeight, coverageMask);
        const HRESULT result = layout->Draw(nullptr,
                                            renderer,
                                            std::round(originX),
                                            std::round(originY));
        renderer->Release();
        return SUCCEEDED(result);
    }

    bool isReady() const {
        return _factory && _textFormat;
    }

private:
    bool createLayout(const char *text,
                      size_t textLength,
                      float maxWidth,
                      float maxHeight,
                      IDWriteTextLayout **layout) const {
        if (!layout || !_factory || !_textFormat || !text || textLength == 0U) {
            return false;
        }

        std::wstring wideText;
        if (!utf8ToWide(text, textLength, &wideText)) {
            return false;
        }

        const HRESULT result = _factory->CreateGdiCompatibleTextLayout(wideText.data(),
                                                                       static_cast<UINT32>(wideText.size()),
                                                                       _textFormat.Get(),
                                                                       maxWidth,
                                                                       maxHeight,
                                                                       1.0F,
                                                                       nullptr,
                                                                       FALSE,
                                                                       layout);
        return SUCCEEDED(result);
    }

    ComPtr<IDWriteFactory> _factory;
    ComPtr<IDWriteTextFormat> _textFormat;
};

DirectWriteTextRasterizer::DirectWriteTextRasterizer()
: _impl(std::make_unique<Impl>()) {
}

DirectWriteTextRasterizer::~DirectWriteTextRasterizer() = default;

bool DirectWriteTextRasterizer::updateFont(const char *fontFamily,
                                           size_t fontFamilyLength,
                                           float fontSize,
                                           bool bold,
                                           bool italic) {
    return _impl->updateFont(fontFamily, fontFamilyLength, fontSize, bold, italic);
}

bool DirectWriteTextRasterizer::measureText(const char *text, size_t textLength, float *width, float *height) const {
    return _impl->measureText(text, textLength, width, height);
}

bool DirectWriteTextRasterizer::getLineMetrics(float *height, float *baseline) const {
    return _impl->getLineMetrics(height, baseline);
}

bool DirectWriteTextRasterizer::rasterize(const char *text,
                                          size_t textLength,
                                          float originX,
                                          float originY,
                                          int canvasWidth,
                                          int canvasHeight,
                                          std::vector<uint8_t> *coverageMask) const {
    return _impl->rasterize(text, textLength, originX, originY, canvasWidth, canvasHeight, coverageMask);
}

bool DirectWriteTextRasterizer::isReady() const {
    return _impl->isReady();
}

} // namespace cc
