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

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace cc {

/**
 * Rasterizes Windows system fonts through DirectWrite's GDI-compatible
 * measuring and hinting path. The output is a single-channel coverage mask,
 * which is safe to tint and composite into a Cocos Label texture.
 */
class DirectWriteTextRasterizer final {
public:
    DirectWriteTextRasterizer();
    ~DirectWriteTextRasterizer();

    DirectWriteTextRasterizer(const DirectWriteTextRasterizer &) = delete;
    DirectWriteTextRasterizer &operator=(const DirectWriteTextRasterizer &) = delete;

    bool updateFont(const char *fontFamily, size_t fontFamilyLength, float fontSize, bool bold, bool italic);
    bool measureText(const char *text, size_t textLength, float *width, float *height) const;
    bool getLineMetrics(float *height, float *baseline) const;
    bool rasterize(const char *text,
                   size_t textLength,
                   float originX,
                   float originY,
                   int canvasWidth,
                   int canvasHeight,
                   std::vector<uint8_t> *coverageMask) const;
    bool isReady() const;

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace cc
