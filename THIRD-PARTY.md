# Third party

## xbdepends tables (Microsoft, MIT)

`src/analysis/xbdepends_data.hpp` is generated from the DLL and API name
tables of the `xbdepends` sample in
[microsoft/Xbox-GDK-Samples](https://github.com/microsoft/Xbox-GDK-Samples)
(commit `b1e77b6ac10f95b07296025fda171ca2ddc42dbe`). The file name patterns in
`src/analysis/modules.cpp` come from the same tool. That sample is MIT
licensed and the license asks for this notice to be kept:

    MIT License

    Copyright (c) Microsoft Corporation.

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE

## DXVK (zlib/libpng license)

With `-DXV_DXVK=ON`, [doitsujin/dxvk](https://github.com/doitsujin/dxvk) is
fetched at commit `2c94c5f25cf75c313ef2a55b2e0c9276d67c28d6` and altered by
`cmake/dxvk.patch` for console titles. The altered `d3d11.dll` and `dxgi.dll`
are shipped as `xv_d3d11.dll` and `xv_dxgi.dll`; they are not the original
DXVK. Its license:

    Copyright (c) 2017 Philip Rebohle
    Copyright (c) 2019 Joshua Ashton
    Copyright (c) 2019 Robin Kertels
    Copyright (c) 2023 Jeffrey Ellison

    zlib/libpng license

    This software is provided 'as-is', without any express or implied
    warranty. In no event will the authors be held liable for any damages
    arising from the use of this software.

    Permission is granted to anyone to use this software for any purpose,
    including commercial applications, and to alter it and redistribute it
    freely, subject to the following restrictions:

    - The origin of this software must not be misrepresented; you must not
      claim that you wrote the original software. If you use this software
      in a product, an acknowledgment in the product documentation would be
      appreciated but is not required.

    - Altered source versions must be plainly marked as such, and must not
      be misrepresented as being the original software.

    - This notice may not be removed or altered from any source distribution.

## Libraries

Fetched by CMake at configure time, each under its own license: GLFW,
Dear ImGui, Native File Dialog Extended, stb_image (logo decoding), spdlog, fmt,
nlohmann/json, pugixml and Catch2.
`stb_image` is fetched from [nothings/stb](https://github.com/nothings/stb)
at commit `5736b15f7ea0ffb08dd38af21067c314d6a3aae9` (MIT/public domain).

The interface font is Roboto Medium (Apache License 2.0, Google), taken from
`misc/fonts` of Dear ImGui and built into the executable. The logo uses
ProggyClean (MIT, Tristan Grimmer), Dear ImGui's built-in font.

## DirectX 12 Agility SDK (Microsoft, its own license)

The build fetches the NuGet package `Microsoft.Direct3D.D3D12` 1.620.2 and copies its
`D3D12Core.dll` to `D3D12\` next to `xvboot.exe`, which uses it in place of an older
Direct3D 12 runtime of the system when it is there. It is not part of the source and not
in the release archives; the package's `LICENSE.txt` has its terms.

## Ported code (GPL-3.0-or-later)

`src/runtime/modules/texture_layout.cpp` and `texture_layout.hpp` are a port to C++ of
`crates/victory-formats/src/texture.rs` of
[Marvisak/VIctory](https://github.com/Marvisak/VIctory), under the same license as this
project. That code follows AMD's addrlib (MIT), credited in `texture_layout.hpp`.

`src/runtime/modules/bcpack.cpp` and `bcpack.hpp` are a port to C++ of
`crates/victory-formats/src/bcpack.rs` of the same project (commit
`ffe310eb2098e52169f4c44db5ffdabf43a72622`). BCPack's constant tables (77,982 bytes) are not
part of it, nor of this repository. A user who has them puts them beside the executable as
`bcpack-tables.bin`; without them DirectStorage requests with BCPack fail.
