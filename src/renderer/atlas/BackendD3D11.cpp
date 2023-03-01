#include "pch.h"
#include "BackendD3D11.h"

#include <custom_shader_ps.h>
#include <custom_shader_vs.h>
#include <shader_ps.h>
#include <shader_vs.h>

#include "dwrite.h"

#pragma warning(disable : 4100) // '...': unreferenced formal parameter
#pragma warning(disable : 4127)
// Disable a bunch of warnings which get in the way of writing performant code.
#pragma warning(disable : 26429) // Symbol 'data' is never tested for nullness, it can be marked as not_null (f.23).
#pragma warning(disable : 26446) // Prefer to use gsl::at() instead of unchecked subscript operator (bounds.4).
#pragma warning(disable : 26459) // You called an STL function '...' with a raw pointer parameter at position '...' that may be unsafe [...].
#pragma warning(disable : 26481) // Don't use pointer arithmetic. Use span instead (bounds.1).
#pragma warning(disable : 26482) // Only index into arrays using constant expressions (bounds.2).

using namespace Microsoft::Console::Render::Atlas;

BackendD3D11::BackendD3D11(wil::com_ptr<ID3D11Device2> device, wil::com_ptr<ID3D11DeviceContext2> deviceContext) :
    _device{ std::move(device) },
    _deviceContext{ std::move(deviceContext) }
{
    // Our constant buffer will never get resized
    {
        static constexpr D3D11_BUFFER_DESC desc{
            .ByteWidth = sizeof(ConstBuffer),
            .Usage = D3D11_USAGE_DEFAULT,
            .BindFlags = D3D11_BIND_CONSTANT_BUFFER,
        };
        THROW_IF_FAILED(_device->CreateBuffer(&desc, nullptr, _constantBuffer.put()));
    }

    THROW_IF_FAILED(_device->CreateVertexShader(&shader_vs[0], sizeof(shader_vs), nullptr, _vertexShader.put()));
    THROW_IF_FAILED(_device->CreatePixelShader(&shader_ps[0], sizeof(shader_ps), nullptr, _textPixelShader.put()));

    {
        // The final step of the ClearType blending algorithm is a lerp() between the premultiplied alpha
        // background color and straight alpha foreground color given the 3 RGB weights in alphaCorrected:
        //   lerp(background, foreground, weights)
        // Which is equivalent to:
        //   background * (1 - weights) + foreground * weights
        //
        // This COULD be implemented using dual source color blending like so:
        //   .SrcBlend = D3D11_BLEND_SRC1_COLOR
        //   .DestBlend = D3D11_BLEND_INV_SRC1_COLOR
        //   .BlendOp = D3D11_BLEND_OP_ADD
        // Because:
        //   background * (1 - weights) + foreground * weights
        //       ^             ^        ^     ^           ^
        //      Dest     INV_SRC1_COLOR |    Src      SRC1_COLOR
        //                            OP_ADD
        //
        // BUT we need simultaneous support for regular "source over" alpha blending
        // (SHADING_TYPE_PASSTHROUGH)  like this:
        //   background * (1 - alpha) + foreground
        //
        // This is why we set:
        //   .SrcBlend = D3D11_BLEND_ONE
        //
        // --> We need to multiply the foreground with the weights ourselves.
        static constexpr D3D11_BLEND_DESC1 desc{
            .RenderTarget = { {
                .BlendEnable = TRUE,
                .SrcBlend = D3D11_BLEND_ONE,
                .DestBlend = D3D11_BLEND_INV_SRC1_COLOR,
                .BlendOp = D3D11_BLEND_OP_ADD,
                .SrcBlendAlpha = D3D11_BLEND_ONE,
                .DestBlendAlpha = D3D11_BLEND_ZERO,
                .BlendOpAlpha = D3D11_BLEND_OP_ADD,
                .RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL,
            } },
        };
        THROW_IF_FAILED(_device->CreateBlendState1(&desc, _textBlendState.put()));
    }

    {
        static constexpr D3D11_INPUT_ELEMENT_DESC layout[]{
            { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(QuadInstance, position), D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(QuadInstance, texcoord), D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, offsetof(QuadInstance, color), D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "ShadingType", 0, DXGI_FORMAT_R32_UINT, 0, offsetof(QuadInstance, shadingType), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        THROW_IF_FAILED(_device->CreateInputLayout(&layout[0], gsl::narrow_cast<UINT>(std::size(layout)), &shader_vs[0], sizeof(shader_vs), _textInputLayout.put()));
    }

#ifndef NDEBUG
    _sourceDirectory = std::filesystem::path{ __FILE__ }.parent_path();
    _sourceCodeWatcher = wil::make_folder_change_reader_nothrow(_sourceDirectory.c_str(), false, wil::FolderChangeEvents::FileName | wil::FolderChangeEvents::LastWriteTime, [this](wil::FolderChangeEvent, PCWSTR path) {
        if (til::ends_with(path, L".hlsl"))
        {
            auto expected = INT64_MAX;
            const auto invalidationTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
            _sourceCodeInvalidationTime.compare_exchange_strong(expected, invalidationTime.time_since_epoch().count(), std::memory_order_relaxed);
        }
    });
#endif
}

void BackendD3D11::_recreateBackgroundBitmapSamplerState(const RenderingPayload& p)
{
    const auto color = colorFromU32Premultiply<DXGI_RGBA>(p.s->misc->backgroundColor);
    const D3D11_SAMPLER_DESC desc{
        .Filter = D3D11_FILTER_MIN_MAG_MIP_POINT,
        .AddressU = D3D11_TEXTURE_ADDRESS_BORDER,
        .AddressV = D3D11_TEXTURE_ADDRESS_BORDER,
        .AddressW = D3D11_TEXTURE_ADDRESS_BORDER,
        .MipLODBias = 0.0f,
        .MaxAnisotropy = 1,
        .ComparisonFunc = D3D11_COMPARISON_NEVER,
        .BorderColor = { color.r, color.g, color.b, color.a },
        .MinLOD = -FLT_MAX,
        .MaxLOD = FLT_MAX,
    };
    THROW_IF_FAILED(_device->CreateSamplerState(&desc, _backgroundBitmapSamplerState.put()));
}

void BackendD3D11::Render(const RenderingPayload& p)
{
    _debugUpdateShaders();

    if (_generation != p.s.generation())
    {
        _swapChainManager.UpdateSwapChainSettings(
            p,
            _device.get(),
            [this]() {
                _renderTargetView.reset();
                _deviceContext->ClearState();
            },
            [this]() {
                _renderTargetView.reset();
                _deviceContext->ClearState();
                _deviceContext->Flush();
            });

        if (!_renderTargetView)
        {
            const auto buffer = _swapChainManager.GetBuffer();
            THROW_IF_FAILED(_device->CreateRenderTargetView(buffer.get(), nullptr, _renderTargetView.put()));
        }

        const auto fontChanged = _fontGeneration != p.s->font.generation();
        const auto miscChanged = _miscGeneration != p.s->misc.generation();
        const auto targetSizeChanged = _targetSize != p.s->targetSize;
        const auto cellCountChanged = _cellCount != p.s->cellCount;

        if (fontChanged)
        {
            DWrite_GetRenderParams(p.dwriteFactory.get(), &_gamma, &_cleartypeEnhancedContrast, &_grayscaleEnhancedContrast, _textRenderingParams.put());

            if (_d2dRenderTarget)
            {
                _d2dRenderTargetUpdateFontSettings(p);
            }
        }

        if (miscChanged)
        {
            _recreateBackgroundBitmapSamplerState(p);
            _recreateCustomShader(p);
        }

        if (cellCountChanged)
        {
            _recreateBackgroundColorBitmap(p);
        }

        if (targetSizeChanged || miscChanged)
        {
            _recreateCustomOffscreenTexture(p);
        }

        if (targetSizeChanged || fontChanged)
        {
            _recreateConstBuffer(p);
        }

        _generation = p.s.generation();
        _fontGeneration = p.s->font.generation();
        _miscGeneration = p.s->misc.generation();
        _cellCount = p.s->cellCount;
    }

    _instancesSize = 0;
    _indicesSize = 0;

    {
        // IA: Input Assembler
        _deviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        _deviceContext->IASetIndexBuffer(_indexBuffer.get(), DXGI_FORMAT_R32_UINT, 0);

        // VS: Vertex Shader
        _deviceContext->VSSetShader(_vertexShader.get(), nullptr, 0);
        _deviceContext->VSSetConstantBuffers(0, 1, _constantBuffer.addressof());
        _deviceContext->VSSetShaderResources(0, 1, _instanceBufferView.addressof());

        // RS: Rasterizer Stage
        D3D11_VIEWPORT viewport{};
        viewport.Width = static_cast<f32>(p.s->targetSize.x);
        viewport.Height = static_cast<f32>(p.s->targetSize.y);
        _deviceContext->RSSetViewports(1, &viewport);

        // PS: Pixel Shader
        ID3D11ShaderResourceView* const resources[]{ _backgroundBitmapView.get(), _glyphAtlasView.get() };
        _deviceContext->PSSetShader(_textPixelShader.get(), nullptr, 0);
        _deviceContext->PSSetConstantBuffers(0, 1, _constantBuffer.addressof());
        _deviceContext->PSSetSamplers(0, 1, _backgroundBitmapSamplerState.addressof());
        _deviceContext->PSSetShaderResources(0, 2, &resources[0]);

        // OM: Output Merger
        _deviceContext->OMSetBlendState(_textBlendState.get(), nullptr, 0xffffffff);
        _deviceContext->OMSetRenderTargets(1, _renderTargetView.addressof(), nullptr);
    }

    // Background
    {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        THROW_IF_FAILED(_deviceContext->Map(_backgroundBitmap.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
        for (size_t i = 0; i < p.s->cellCount.y; ++i)
        {
            memcpy(mapped.pData, p.backgroundBitmap.data() + i * p.s->cellCount.x, p.s->cellCount.x * sizeof(u32));
            mapped.pData = static_cast<void*>(static_cast<std::byte*>(mapped.pData) + mapped.RowPitch);
        }
        _deviceContext->Unmap(_backgroundBitmap.get(), 0);
    }
    {
        const auto targetWidth = static_cast<f32>(p.s->targetSize.x);
        const auto targetHeight = static_cast<f32>(p.s->targetSize.y);
        const auto contentWidth = static_cast<f32>(p.s->cellCount.x * p.s->font->cellSize.x);
        const auto contentHeight = static_cast<f32>(p.s->cellCount.y * p.s->font->cellSize.y);
        _appendRect(
            { 0.0f, 0.0f, targetWidth, targetHeight },
            { 0.0f, 0.0f, targetWidth / contentWidth, targetHeight / contentHeight },
            0,
            ShadingType::Background);
    }

    {
        // Text
        {
            {
                if (!_glyphAtlas)
                {
                    _resetAtlasAndBeginDraw(p);
                }

                size_t y = 0;
                for (const auto& row : p.rows)
                {
                    const auto baselineY = p.d.font.cellSizeDIP.y * y + p.s->font->baselineInDIP;
                    f32 cumulativeAdvance = 0;

                    for (const auto& m : row.mappings)
                    {
                        for (auto i = m.glyphsFrom; i < m.glyphsTo; ++i)
                        {
                            bool inserted = false;
                            auto& entry = _glyphCache.FindOrInsert(m.fontFace.get(), row.glyphIndices[i], inserted);
                            if (inserted)
                            {
                                _beginDrawing();

                                if (!_drawGlyph(p, entry, m.fontEmSize))
                                {
                                    _endDrawing();
                                    _flushRects(p);
                                    _resetAtlasAndBeginDraw(p);
                                    --i;
                                    continue;
                                }
                            }

                            if (entry.shadingType)
                            {
                                _appendRect(
                                    {
                                        (cumulativeAdvance + row.glyphOffsets[i].advanceOffset) * p.d.font.pixelPerDIP + entry.offset.x,
                                        (baselineY - row.glyphOffsets[i].ascenderOffset) * p.d.font.pixelPerDIP + entry.offset.y,
                                        entry.texcoord.z,
                                        entry.texcoord.w,
                                    },
                                    entry.texcoord,
                                    row.colors[i],
                                    static_cast<ShadingType>(entry.shadingType));
                            }

                            cumulativeAdvance += row.glyphAdvances[i];
                        }
                    }

                    y++;
                }

                if (beganDrawing)
                {
                    THROW_IF_FAILED(_d2dRenderTarget->EndDraw());
                }
            }

            {
                size_t y = 0;
                for (const auto& row : p.rows)
                {
                    for (const auto& r : row.gridLineRanges)
                    {
                        assert(r.lines.any());

                        const auto top = p.s->font->cellSize.y * y;
                        const auto left = p.s->font->cellSize.x * r.from;
                        const auto width = p.s->font->cellSize.x * (r.to - r.from);

                        if (r.lines.test(GridLines::Left))
                        {
                            _appendRect(
                                {
                                    static_cast<f32>(left),
                                    static_cast<f32>(top),
                                    static_cast<f32>(p.s->font->thinLineWidth),
                                    static_cast<f32>(p.s->font->cellSize.y),
                                },
                                r.color,
                                ShadingType::SolidFill);
                        }
                        if (r.lines.test(GridLines::Top))
                        {
                            _appendRect(
                                {
                                    static_cast<f32>(left),
                                    static_cast<f32>(top),
                                    static_cast<f32>(p.s->font->cellSize.x),
                                    static_cast<f32>(p.s->font->thinLineWidth),
                                },
                                r.color,
                                ShadingType::SolidFill);
                        }
                        if (r.lines.test(GridLines::Right))
                        {
                            _appendRect(
                                {
                                    static_cast<f32>(left + p.s->font->cellSize.x - p.s->font->thinLineWidth),
                                    static_cast<f32>(top),
                                    static_cast<f32>(p.s->font->thinLineWidth),
                                    static_cast<f32>(p.s->font->cellSize.y),
                                },
                                r.color,
                                ShadingType::SolidFill);
                        }
                        if (r.lines.test(GridLines::Bottom))
                        {
                            _appendRect(
                                {
                                    static_cast<f32>(left),
                                    static_cast<f32>(top + p.s->font->cellSize.y - p.s->font->thinLineWidth),
                                    static_cast<f32>(p.s->font->cellSize.x),
                                    static_cast<f32>(p.s->font->thinLineWidth),
                                },
                                r.color,
                                ShadingType::SolidFill);
                        }
                        if (r.lines.test(GridLines::Underline))
                        {
                            _appendRect(
                                {
                                    static_cast<f32>(left),
                                    static_cast<f32>(top + p.s->font->underlinePos),
                                    static_cast<f32>(width),
                                    static_cast<f32>(p.s->font->underlineWidth),
                                },
                                r.color,
                                ShadingType::SolidFill);
                        }
                        if (r.lines.test(GridLines::HyperlinkUnderline))
                        {
                            _appendRect(
                                {
                                    static_cast<f32>(left),
                                    static_cast<f32>(top + p.s->font->underlinePos),
                                    static_cast<f32>(width),
                                    static_cast<f32>(p.s->font->underlineWidth),
                                },
                                r.color,
                                ShadingType::DashedLine);
                        }
                        if (r.lines.test(GridLines::DoubleUnderline))
                        {
                            _appendRect(
                                {
                                    static_cast<f32>(left),
                                    static_cast<f32>(top + p.s->font->doubleUnderlinePos.x),
                                    static_cast<f32>(width),
                                    static_cast<f32>(p.s->font->thinLineWidth),
                                },
                                r.color,
                                ShadingType::SolidFill);

                            _appendRect(
                                {
                                    static_cast<f32>(left),
                                    static_cast<f32>(top + p.s->font->doubleUnderlinePos.y),
                                    static_cast<f32>(width),
                                    static_cast<f32>(p.s->font->thinLineWidth),
                                },
                                r.color,
                                ShadingType::SolidFill);
                        }
                        if (r.lines.test(GridLines::Strikethrough))
                        {
                            _appendRect(
                                {
                                    static_cast<f32>(left),
                                    static_cast<f32>(top + p.s->font->strikethroughPos),
                                    static_cast<f32>(width),
                                    static_cast<f32>(p.s->font->strikethroughWidth),
                                },
                                r.color,
                                ShadingType::SolidFill);
                        }
                    }

                    y++;
                }
            }
        }

        if (p.cursorRect.non_empty())
        {
            f32x4 rect{
                static_cast<f32>(p.s->font->cellSize.x * p.cursorRect.left),
                static_cast<f32>(p.s->font->cellSize.y * p.cursorRect.top),
                static_cast<f32>(p.s->font->cellSize.x * (p.cursorRect.right - p.cursorRect.left)),
                static_cast<f32>(p.s->font->cellSize.y * (p.cursorRect.bottom - p.cursorRect.top)),
            };
            f32r rect2{ rect.x, rect.y, rect.x + rect.z, rect.y + rect.w };

            // Cursors that are 0xffffffff invert the color they're on. The problem is that the inversion
            // of a pure gray background color (0x7f) is also gray and so the cursor would appear invisible.
            // An imperfect but simple solution is to instead XOR the color with 0xc0, flipping the top two bits.
            // This preserves the lower 6 bits and so (0x7f) gray gets inverted to light gray (0xbf) instead.
            // Normally this would be super trivial to do using D3D11_LOGIC_OP_XOR, but this would break
            // the lightness adjustment that the ClearType/Grayscale AA algorithms use. Additionally,
            // in case of ClearType specifically, this would break the red/blue shift on the edges.
            if (p.s->cursor->cursorColor == INVALID_COLOR)
            {
                static constexpr auto invertColor = [](u32 color) -> u32 {
                    return color ^ 0xc0c0c0;
                };
                static constexpr auto intersect = [](const f32r& clip, f32r& r) {
                    r.left = std::max(clip.left, r.left);
                    r.right = std::min(clip.right, r.right);
                    r.top = std::max(clip.top, r.top);
                    r.bottom = std::min(clip.bottom, r.bottom);
                    return r.left < r.right && r.top < r.bottom;
                };

                // TODO: when inverting wide glyphs we should look up the color of each cell from .left to .right
                const auto idx = p.cursorRect.top * p.s->cellCount.y + p.cursorRect.left;
                const auto backgroundColor = p.backgroundBitmap[idx];
                const auto backgroundColorInv = invertColor(backgroundColor);
                _appendRect(rect, backgroundColorInv, ShadingType::SolidFill);

                for (size_t i = 0, l = _vertexInstanceData.size() - 1; i < l; ++i)
                {
                    const auto& ref = _vertexInstanceData[i];
                    const auto& refrect = ref.rect;
                    f32r refrect2{ refrect.x, refrect.y, refrect.x + refrect.z, refrect.y + refrect.w };

                    if (intersect(rect2, refrect2))
                    {
                        auto copy = ref;
                        copy.rect.x = refrect2.left;
                        copy.rect.y = refrect2.top;
                        copy.rect.z = refrect2.right - refrect2.left;
                        copy.rect.w = refrect2.bottom - refrect2.top;
                        copy.tex.x += copy.rect.x - ref.rect.x;
                        copy.tex.y += copy.rect.y - ref.rect.y;
                        copy.tex.z = copy.rect.z;
                        copy.tex.w = copy.rect.w;
                        copy.color = invertColor(copy.color);
                        copy.shadingType = copy.shadingType == ShadingType::Passthrough ? ShadingType::PassthroughInvert : copy.shadingType;
                        _vertexInstanceData.emplace_back(copy);
                    }
                }
            }
            else
            {
                _appendRect(rect, p.s->cursor->cursorColor, ShadingType::SolidFill);
            }

            // TODO hole punching if 0x00000000
        }

        // Selection
        {
            size_t y = 0;
            for (const auto& row : p.rows)
            {
                if (row.selectionTo > row.selectionFrom)
                {
                    _appendRect(
                        {
                            static_cast<f32>(p.s->font->cellSize.x * row.selectionFrom),
                            static_cast<f32>(p.s->font->cellSize.y * y),
                            static_cast<f32>(p.s->font->cellSize.x * (row.selectionTo - row.selectionFrom)),
                            static_cast<f32>(p.s->font->cellSize.y),
                        },
                        p.s->misc->selectionColor,
                        ShadingType::SolidFill);
                }

                y++;
            }
        }

        _flushRects(p);
    }

    _swapChainManager.Present(p);
}

bool BackendD3D11::RequiresContinuousRedraw() noexcept
{
    return _requiresContinuousRedraw;
}

void BackendD3D11::WaitUntilCanRender() noexcept
{
    _swapChainManager.WaitUntilCanRender();
}

void BackendD3D11::_debugUpdateShaders()
try
{
#ifndef NDEBUG
    const auto invalidationTime = _sourceCodeInvalidationTime.load(std::memory_order_relaxed);

    if (invalidationTime == INT64_MAX || invalidationTime > std::chrono::steady_clock::now().time_since_epoch().count())
    {
        return;
    }

    _sourceCodeInvalidationTime.store(INT64_MAX, std::memory_order_relaxed);

    static const auto compile = [](const std::filesystem::path& path, const char* target) {
        wil::com_ptr<ID3DBlob> error;
        wil::com_ptr<ID3DBlob> blob;
        const auto hr = D3DCompileFromFile(
            /* pFileName   */ path.c_str(),
            /* pDefines    */ nullptr,
            /* pInclude    */ D3D_COMPILE_STANDARD_FILE_INCLUDE,
            /* pEntrypoint */ "main",
            /* pTarget     */ target,
            /* Flags1      */ D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR | D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS,
            /* Flags2      */ 0,
            /* ppCode      */ blob.addressof(),
            /* ppErrorMsgs */ error.addressof());

        if (error)
        {
            std::thread t{ [error = std::move(error)]() noexcept {
                MessageBoxA(nullptr, static_cast<const char*>(error->GetBufferPointer()), "Compilation error", MB_ICONERROR | MB_OK);
            } };
            t.detach();
        }

        THROW_IF_FAILED(hr);
        return blob;
    };

    struct FileVS
    {
        std::wstring_view filename;
        wil::com_ptr<ID3D11VertexShader> BackendD3D11::*target;
    };
    struct FilePS
    {
        std::wstring_view filename;
        wil::com_ptr<ID3D11PixelShader> BackendD3D11::*target;
    };

    static std::array filesVS{
        FileVS{ L"shader_vs.hlsl", &BackendD3D11::_vertexShader },
    };
    static std::array filesPS{
        FilePS{ L"shader_text_cleartype_ps.hlsl", &BackendD3D11::_textPixelShader },
        FilePS{ L"shader_text_grayscale_ps.hlsl", &BackendD3D11::_textPixelShader },
    };

    std::array<wil::com_ptr<ID3D11VertexShader>, filesVS.size()> compiledVS;
    std::array<wil::com_ptr<ID3D11PixelShader>, filesPS.size()> compiledPS;

    // Compile our files before moving them into `this` below to ensure we're
    // always in a consistent state where all shaders are seemingly valid.
    for (size_t i = 0; i < filesVS.size(); ++i)
    {
        const auto blob = compile(_sourceDirectory / filesVS[i].filename, "vs_4_0");
        THROW_IF_FAILED(_device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, compiledVS[i].addressof()));
    }
    for (size_t i = 0; i < filesPS.size(); ++i)
    {
        const auto blob = compile(_sourceDirectory / filesPS[i].filename, "ps_4_0");
        THROW_IF_FAILED(_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, compiledPS[i].addressof()));
    }

    for (size_t i = 0; i < filesVS.size(); ++i)
    {
        this->*filesVS[i].target = std::move(compiledVS[i]);
    }
    for (size_t i = 0; i < filesPS.size(); ++i)
    {
        this->*filesPS[i].target = std::move(compiledPS[i]);
    }
#endif
}
CATCH_LOG()

void BackendD3D11::_recreateCustomShader(const RenderingPayload& p)
{
    _customOffscreenTexture.reset();
    _customOffscreenTextureView.reset();
    _customOffscreenTextureTargetView.reset();
    _customVertexShader.reset();
    _customPixelShader.reset();
    _customShaderConstantBuffer.reset();
    _customShaderSamplerState.reset();
    _requiresContinuousRedraw = false;

    if (!p.s->misc->customPixelShaderPath.empty())
    {
        const char* target = nullptr;
        switch (_device->GetFeatureLevel())
        {
        case D3D_FEATURE_LEVEL_10_0:
            target = "ps_4_0";
            break;
        case D3D_FEATURE_LEVEL_10_1:
            target = "ps_4_1";
            break;
        default:
            target = "ps_5_0";
            break;
        }

        static constexpr auto flags =
            D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR
#ifdef NDEBUG
            | D3DCOMPILE_OPTIMIZATION_LEVEL3;
#else
            // Only enable strictness and warnings in DEBUG mode
            //  as these settings makes it very difficult to develop
            //  shaders as windows terminal is not telling the user
            //  what's wrong, windows terminal just fails.
            //  Keep it in DEBUG mode to catch errors in shaders
            //  shipped with windows terminal
            | D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS | D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

        wil::com_ptr<ID3DBlob> error;
        wil::com_ptr<ID3DBlob> blob;
        const auto hr = D3DCompileFromFile(
            /* pFileName   */ p.s->misc->customPixelShaderPath.c_str(),
            /* pDefines    */ nullptr,
            /* pInclude    */ D3D_COMPILE_STANDARD_FILE_INCLUDE,
            /* pEntrypoint */ "main",
            /* pTarget     */ target,
            /* Flags1      */ flags,
            /* Flags2      */ 0,
            /* ppCode      */ blob.addressof(),
            /* ppErrorMsgs */ error.addressof());

        // Unless we can determine otherwise, assume this shader requires evaluation every frame
        _requiresContinuousRedraw = true;

        if (SUCCEEDED(hr))
        {
            THROW_IF_FAILED(_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, _customPixelShader.put()));

            // Try to determine whether the shader uses the Time variable
            wil::com_ptr<ID3D11ShaderReflection> reflector;
            if (SUCCEEDED_LOG(D3DReflect(blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(reflector.put()))))
            {
                if (ID3D11ShaderReflectionConstantBuffer* constantBufferReflector = reflector->GetConstantBufferByIndex(0)) // shader buffer
                {
                    if (ID3D11ShaderReflectionVariable* variableReflector = constantBufferReflector->GetVariableByIndex(0)) // time
                    {
                        D3D11_SHADER_VARIABLE_DESC variableDescriptor;
                        if (SUCCEEDED_LOG(variableReflector->GetDesc(&variableDescriptor)))
                        {
                            // only if time is used
                            _requiresContinuousRedraw = WI_IsFlagSet(variableDescriptor.uFlags, D3D_SVF_USED);
                        }
                    }
                }
            }
        }
        else
        {
            if (error)
            {
                LOG_HR_MSG(hr, "%*hs", error->GetBufferSize(), error->GetBufferPointer());
            }
            else
            {
                LOG_HR(hr);
            }
            if (p.warningCallback)
            {
                p.warningCallback(D2DERR_SHADER_COMPILE_FAILED);
            }
        }
    }
    else if (p.s->misc->useRetroTerminalEffect)
    {
        THROW_IF_FAILED(_device->CreatePixelShader(&custom_shader_ps[0], sizeof(custom_shader_ps), nullptr, _customPixelShader.put()));
        // We know the built-in retro shader doesn't require continuous redraw.
        _requiresContinuousRedraw = false;
    }

    if (_customPixelShader)
    {
        THROW_IF_FAILED(_device->CreateVertexShader(&custom_shader_vs[0], sizeof(custom_shader_vs), nullptr, _customVertexShader.put()));

        {
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = sizeof(CustomConstBuffer);
            desc.Usage = D3D11_USAGE_DYNAMIC;
            desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            THROW_IF_FAILED(_device->CreateBuffer(&desc, nullptr, _customShaderConstantBuffer.put()));
        }

        {
            D3D11_SAMPLER_DESC desc{};
            desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            desc.AddressU = D3D11_TEXTURE_ADDRESS_BORDER;
            desc.AddressV = D3D11_TEXTURE_ADDRESS_BORDER;
            desc.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
            desc.MaxAnisotropy = 1;
            desc.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
            desc.MaxLOD = D3D11_FLOAT32_MAX;
            THROW_IF_FAILED(_device->CreateSamplerState(&desc, _customShaderSamplerState.put()));
        }

        _customShaderStartTime = std::chrono::steady_clock::now();
    }
}

void BackendD3D11::_recreateCustomOffscreenTexture(const RenderingPayload& p)
{
    if (!p.s->misc->customPixelShaderPath.empty())
    {
        // Avoid memory usage spikes by releasing memory first.
        _customOffscreenTexture.reset();
        _customOffscreenTextureView.reset();
        _customOffscreenTextureTargetView.reset();

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = p.s->targetSize.x;
        desc.Height = p.s->targetSize.y;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc = { 1, 0 };
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        THROW_IF_FAILED(_device->CreateTexture2D(&desc, nullptr, _customOffscreenTexture.addressof()));
        THROW_IF_FAILED(_device->CreateShaderResourceView(_customOffscreenTexture.get(), nullptr, _customOffscreenTextureView.addressof()));
        THROW_IF_FAILED(_device->CreateRenderTargetView(_customOffscreenTexture.get(), nullptr, _customOffscreenTextureTargetView.addressof()));
    }
}

void BackendD3D11::_recreateBackgroundColorBitmap(const RenderingPayload& p)
{
    // Avoid memory usage spikes by releasing memory first.
    _backgroundBitmap.reset();
    _backgroundBitmapView.reset();

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = p.s->cellCount.x;
    desc.Height = p.s->cellCount.y;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc = { 1, 0 };
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    THROW_IF_FAILED(_device->CreateTexture2D(&desc, nullptr, _backgroundBitmap.addressof()));
    THROW_IF_FAILED(_device->CreateShaderResourceView(_backgroundBitmap.get(), nullptr, _backgroundBitmapView.addressof()));
}

void BackendD3D11::_recreateConstBuffer(const RenderingPayload& p)
{
    ConstBuffer data;
    data.positionScale = { 2.0f / p.s->targetSize.x, -2.0f / p.s->targetSize.y };
    data.grayscaleEnhancedContrast = _grayscaleEnhancedContrast;
    data.cleartypeEnhancedContrast = _cleartypeEnhancedContrast;
    DWrite_GetGammaRatios(_gamma, data.gammaRatios);
    data.dashedLineLength = p.s->font->underlineWidth * 3.0f;
    _deviceContext->UpdateSubresource(_constantBuffer.get(), 0, nullptr, &data, 0, 0);
}

void BackendD3D11::_d2dRenderTargetUpdateFontSettings(const RenderingPayload& p) const
{
    _d2dRenderTarget->SetDpi(p.s->font->dpi, p.s->font->dpi);
    _d2dRenderTarget->SetTextAntialiasMode(static_cast<D2D1_TEXT_ANTIALIAS_MODE>(p.s->font->antialiasingMode));
}

void BackendD3D11::_beginDrawing()
{
    if (!beganDrawing)
    {
        _d2dRenderTarget->BeginDraw();
        beganDrawing = true;
    }
}

void BackendD3D11::_endDrawing()
{
    if (beganDrawing)
    {
        THROW_IF_FAILED(_d2dRenderTarget->EndDraw());
        beganDrawing = false;
    }
}

void BackendD3D11::_resetAtlasAndBeginDraw(const RenderingPayload& p)
{
    // This block of code calculates the size of a power-of-2 texture that has an area larger than the targetSize
    // of the swap chain. In other words for a 985x1946 pixel swap chain (area = 1916810) it would result in a u/v
    // of 2048x1024 (area = 2097152). This has 2 benefits: GPUs like power-of-2 textures and it ensures that we don't
    // resize the texture every time you resize the window by a pixel. Instead it only grows/shrinks by a factor of 2.
    auto area = static_cast<u32>(p.s->targetSize.x) * static_cast<u32>(p.s->targetSize.y);
    // The index returned by _BitScanReverse is undefined when the input is 0. We can simultaneously
    // guard against this and avoid unreasonably small textures, by clamping the min. texture size.
    area = std::max(uint32_t{ 256 * 256 }, area);
    unsigned long index;
    _BitScanReverse(&index, area - 1);
    const auto u = ::base::saturated_cast<u16>(1u << ((index + 2) / 2));
    const auto v = ::base::saturated_cast<u16>(1u << ((index + 1) / 2));

    if (u != _rectPacker.width || v != _rectPacker.height)
    {
        _d2dRenderTarget.reset();
        _d2dRenderTarget4.reset();
        _glyphAtlas.reset();
        _glyphAtlasView.reset();

        {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = u;
            desc.Height = v;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc = { 1, 0 };
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            THROW_IF_FAILED(_device->CreateTexture2D(&desc, nullptr, _glyphAtlas.addressof()));
            THROW_IF_FAILED(_device->CreateShaderResourceView(_glyphAtlas.get(), nullptr, _glyphAtlasView.addressof()));
        }

        {
            const auto surface = _glyphAtlas.query<IDXGISurface>();

            D2D1_RENDER_TARGET_PROPERTIES props{};
            props.type = D2D1_RENDER_TARGET_TYPE_DEFAULT;
            props.pixelFormat = { DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED };
            wil::com_ptr<ID2D1RenderTarget> renderTarget;
            THROW_IF_FAILED(p.d2dFactory->CreateDxgiSurfaceRenderTarget(surface.get(), &props, renderTarget.addressof()));
            _d2dRenderTarget = renderTarget.query<ID2D1DeviceContext>();
            _d2dRenderTarget4 = renderTarget.query<ID2D1DeviceContext4>();

            // We don't really use D2D for anything except DWrite, but it
            // can't hurt to ensure that everything it does is pixel aligned.
            _d2dRenderTarget->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
            // Ensure that D2D uses the exact same gamma as our shader uses.
            _d2dRenderTarget->SetTextRenderingParams(_textRenderingParams.get());

            _d2dRenderTargetUpdateFontSettings(p);
        }

        {
            static constexpr D2D1_COLOR_F color{ 1, 1, 1, 1 };
            THROW_IF_FAILED(_d2dRenderTarget->CreateSolidColorBrush(&color, nullptr, _brush.put()));
            _brushColor = 0xffffffff;
        }

        ID3D11ShaderResourceView* const resources[]{ _backgroundBitmapView.get(), _glyphAtlasView.get() };
        _deviceContext->PSSetShaderResources(0, 2, &resources[0]);
    }

    _glyphCache.Clear();
    _rectPackerData = Buffer<stbrp_node>{ u };
    stbrp_init_target(&_rectPacker, u, v, _rectPackerData.data(), gsl::narrow_cast<int>(_rectPackerData.size()));

    _beginDrawing();
    _d2dRenderTarget->Clear();
}

void BackendD3D11::_appendRect(f32x4 position, u32 color, ShadingType shadingType)
{
    _appendRect(position, {}, color, shadingType);
}

void BackendD3D11::_appendRect(f32x4 position, f32x4 texcoord, u32 color, ShadingType shadingType)
{
    const auto off = gsl::narrow_cast<u32>(_instancesSize * 4);

    if (_instancesSize >= _instances.size())
    {
        _bumpInstancesSize();
    }

    _instances[_instancesSize++] = QuadInstance{ position, texcoord, color, static_cast<u32>(shadingType) };
    _indices[_indicesSize++] = off + 0;
    _indices[_indicesSize++] = off + 1;
    _indices[_indicesSize++] = off + 2;
    _indices[_indicesSize++] = off + 3;
    _indices[_indicesSize++] = off + 2;
    _indices[_indicesSize++] = off + 1;
}

void BackendD3D11::_bumpInstancesSize()
{
    _instances = Buffer<QuadInstance>{ std::max<size_t>(1024, _instances.size() << 1) };
    _indices = Buffer<u32>{ _instances.size() * 6 };
}

void BackendD3D11::_flushRects(const RenderingPayload& p)
{
    if (!_instancesSize)
    {
        return;
    }

    if (_instancesSize > _instanceBufferSize)
    {
        const auto estimatedMinimum = static_cast<size_t>(p.s->cellCount.x) * static_cast<size_t>(p.s->cellCount.y);
        const auto minSize = _instancesSize + _instancesSize / 2;
        const auto newSize = std::max(estimatedMinimum, minSize);

        _instanceBuffer.reset();
        _instanceBufferView.reset();

        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = gsl::narrow<UINT>(sizeof(QuadInstance) * newSize);
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.StructureByteStride = sizeof(QuadInstance);
        THROW_IF_FAILED(_device->CreateBuffer(&desc, nullptr, _instanceBuffer.put()));
        THROW_IF_FAILED(_device->CreateShaderResourceView(_instanceBuffer.get(), nullptr, _instanceBufferView.addressof()));

        _deviceContext->VSSetShaderResources(0, 1, _instanceBufferView.addressof());

        _instanceBufferSize = newSize;
    }

    if (_indicesSize > _indexBufferSize)
    {
        const auto estimatedMinimum = static_cast<size_t>(p.s->cellCount.x) * static_cast<size_t>(p.s->cellCount.y);
        const auto minSize = _indicesSize + _indicesSize / 2;
        const auto newSize = std::max(estimatedMinimum, minSize);

        _indexBuffer.reset();

        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = gsl::narrow<UINT>(sizeof(u32) * newSize);
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        THROW_IF_FAILED(_device->CreateBuffer(&desc, nullptr, _indexBuffer.put()));

        _deviceContext->IASetIndexBuffer(_indexBuffer.get(), DXGI_FORMAT_R32_UINT, 0);

        _indexBufferSize = newSize;
    }

    {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        THROW_IF_FAILED(_deviceContext->Map(_instanceBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
        memcpy(mapped.pData, _instances.data(), _instancesSize * sizeof(QuadInstance));
        _deviceContext->Unmap(_instanceBuffer.get(), 0);
    }

    {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        THROW_IF_FAILED(_deviceContext->Map(_indexBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
        memcpy(mapped.pData, _indices.data(), _indicesSize * sizeof(u32));
        _deviceContext->Unmap(_indexBuffer.get(), 0);
    }

    // I found 4 approaches to drawing lots of quads quickly.
    // They can often be found in discussions about "particle" rendering in game development.
    // * Compute Shader: My understanding is that at the time of writing games are moving over to bucketing
    //   particles into "tiles" on the screen and drawing them with a compute shader. While this improves
    //   performance, it doesn't mix well with our goal of allowing arbitrary overlaps between glyphs.
    //   Additionally none of the next 3 approaches use any significant amount of GPU time in the first place.
    // * Geometry Shader: Geometry shaders can generate vertices on the fly, which would neatly replace
    //   our need for an index buffer. The reason this wasn't chosen is the same as for the next point.
    // * DrawInstanced: On my own hardware (Nvidia RTX 4090) this seems to perform ~50% better than the final point,
    //   but with no significant difference in power draw. However the popular "Vertex Shader Tricks" talk from
    //   Bill Bilodeau at GDC 2014 suggests that this at least doesn't apply to 2014ish hardware, which supposedly
    //   performs poorly with very small, instanced meshes. Furthermore, public feedback suggests that we still
    //   have a lot of users with older hardware, so I've chosen the following approach, suggested in the talk.
    // * DrawIndexed: This works about the same as DrawInstanced, but instead of using D3D11_INPUT_PER_INSTANCE_DATA,
    //   it uses a SRV (shader resource view) for instance data and maps each SV_VertexID to a SRV slot.
    _deviceContext->DrawIndexed(gsl::narrow_cast<UINT>(_indicesSize), 0, 0);

    _instancesSize = 0;
    _indicesSize = 0;
}

bool BackendD3D11::_drawGlyph(const RenderingPayload& p, GlyphCacheEntry& entry, f32 fontEmSize)
{
    DWRITE_GLYPH_RUN glyphRun{};
    glyphRun.fontFace = entry.fontFace;
    glyphRun.fontEmSize = fontEmSize;
    glyphRun.glyphCount = 1;
    glyphRun.glyphIndices = &entry.glyphIndex;

    auto box = getGlyphRunBlackBox(glyphRun, 0, 0);
    if (box.left >= box.right || box.top >= box.bottom)
    {
        return true;
    }

    box.left = floorf(box.left * p.d.font.pixelPerDIP) - 1.0f;
    box.top = floorf(box.top * p.d.font.pixelPerDIP) - 1.0f;
    box.right = ceilf(box.right * p.d.font.pixelPerDIP) + 1.0f;
    box.bottom = ceilf(box.bottom * p.d.font.pixelPerDIP) + 1.0f;

    stbrp_rect rect{};
    rect.w = gsl::narrow_cast<int>(box.right - box.left);
    rect.h = gsl::narrow_cast<int>(box.bottom - box.top);
    if (!stbrp_pack_rects(&_rectPacker, &rect, 1))
    {
        return false;
    }

    const D2D1_POINT_2F baseline{
        (rect.x - box.left) * p.d.font.dipPerPixel,
        (rect.y - box.top) * p.d.font.dipPerPixel,
    };
    const auto colorGlyph = _drawGlyphRun(p.dwriteFactory4.get(), _d2dRenderTarget.get(), _d2dRenderTarget4.get(), baseline, &glyphRun, _brush.get());

    entry.shadingType = static_cast<u16>(colorGlyph ? ShadingType::Passthrough : p.s->font->antialiasingMode == D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE ? ShadingType::TextClearType :
                                                                                                                                                     ShadingType::TextGrayscale);
    entry.offset.x = gsl::narrow_cast<i16>(box.left);
    entry.offset.y = gsl::narrow_cast<i16>(box.top);
    entry.texcoord.x = static_cast<f32>(rect.x);
    entry.texcoord.y = static_cast<f32>(rect.y);
    entry.texcoord.z = static_cast<f32>(rect.w);
    entry.texcoord.w = static_cast<f32>(rect.h);
    return true;
}
