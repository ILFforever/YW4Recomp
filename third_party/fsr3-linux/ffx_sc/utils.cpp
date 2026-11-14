// This file is part of the FidelityFX SDK.
//
// Copyright (C) 2024 Advanced Micro Devices, Inc.
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

// Linux port: UTF-8 <-> wchar_t (UTF-32 on Linux) without WideCharToMultiByte.
#include "utils.h"

std::string WCharToUTF8(const std::wstring& wstr)
{
    std::string out;
    for (wchar_t wc : wstr)
    {
        uint32_t c = static_cast<uint32_t>(wc);
        if (c < 0x80)
            out += static_cast<char>(c);
        else if (c < 0x800)
        {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
        else if (c < 0x10000)
        {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
        else
        {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

std::wstring UTF8ToWChar(const std::string& str)
{
    std::wstring out;
    for (size_t i = 0; i < str.size();)
    {
        unsigned char b = static_cast<unsigned char>(str[i]);
        uint32_t c;
        size_t   n;
        if (b < 0x80)                { c = b;        n = 1; }
        else if ((b & 0xE0) == 0xC0) { c = b & 0x1F; n = 2; }
        else if ((b & 0xF0) == 0xE0) { c = b & 0x0F; n = 3; }
        else                         { c = b & 0x07; n = 4; }
        for (size_t k = 1; k < n && i + k < str.size(); ++k)
            c = (c << 6) | (static_cast<unsigned char>(str[i + k]) & 0x3F);
        out += static_cast<wchar_t>(c);
        i += n;
    }
    return out;
}
