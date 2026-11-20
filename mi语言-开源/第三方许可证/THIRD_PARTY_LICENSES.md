# 第三方许可证清单 / Third-party license inventory

`发行/mi.exe` 与 `发行/mi-browser.exe` 是 PyInstaller onefile 打包产物。exe 内
包含以下第三方组件；MI 项目自身代码以 MIT 许可证发布（见仓库根目录，若尚无
LICENSE 文件请由维护者补充）。

| 组件 | 版本 | 许可证 | 在 exe 中的角色 |
|---|---|---|---|
| Python | 3.11.4 | PSF License 2.0 | 解释器运行时 + 标准库 |
| Tcl/Tk | 8.6 | BSD-style (Tcl/Tk license) | 桌面 Mi Browser 的图形界面（仅 `mi-browser.exe`） |
| Pillow | 10.2.0 | MIT-CMU (HPND) | `view.image` 渲染（仅 `mi-browser.exe`） |
| PyInstaller | 6.22.2 | GPL-2.0-or-later **with bootloader exception** | 构建工具；其 C 启动器（bootloader）被嵌入两个 exe |

完整许可证文本在 `打包/licenses/`：

* `打包/licenses/LICENSE-Python.txt` — Python 3.11 PSF 许可证
* `打包/licenses/LICENSE-Tcl-Tk.txt` — Tcl/Tk 许可证（BSD 风格）
* `打包/licenses/LICENSE-Pillow.txt` — Pillow MIT-CMU / HPND 许可证

`打包/licenses/` 中的文件来自本机 Python 3.11.4 安装（`LICENSE.txt`、
`tcl/tk8.6/license.terms`、`pillow-10.2.0.dist-info/LICENSE`）。PyInstaller
安装包本身不带独立许可证文件，其条款见下方链接。

## PyInstaller bootloader exception

PyInstaller 本身以 GPL-2.0-or-later 发布，但带有明确的例外条款：允许使用
PyInstaller 打包的应用程序以任意许可证（包括闭源/商业许可证）分发，因为被嵌入
到产物中的只有 PyInstaller 的 bootloader（C 启动器），而不是整个 PyInstaller
程序。原文要点（摘自 PyInstaller `COPYING.txt`）：

> As a special exception to the GNU General Public License, if you distribute
> this software as part of a program that is distributed under a license other
> than the GNU General Public License, the copyright holders of this software
> give you permission to link this software with the other program and to
> distribute the resulting executable…

因此：**MI 的两个 exe 可以按 MIT 发布**；只要你修改了 PyInstaller 的
bootloader 源码再分发，就需要按 GPL 处理该修改。PyInstaller 的完整许可证与例外
条款见 <https://pyinstaller.org/en/stable/license.html>（安装包 `site-packages`
中可能不含独立 LICENSE 文件，故此处以链接形式引用）。

## 未随 exe 分发的构建期工具

以下工具只在**构建机器**上使用，不会进入 `发行/` 产物，因此不分发其许可证文本：

* PyInstaller（构建工具本身；仅 bootloader 例外地进入产物）
* pytest / Playwright（测试用）
* 本仓库的 `工具/build_exe.py`（MIT，随仓库分发）

## 标准库

exe 中的 Python 标准库模块（`http.server`、`urllib`、`tkinter`、`json`、`ssl`
等）属于 Python 发行版，按 `LICENSE-Python.txt`（PSF-2.0）分发。

## 复核方式

```bat
python -m PyInstaller.utils.cliutils.archive_viewer -l dist\mi-browser.exe
```

会列出嵌入的模块与数据；其中 `PIL\*`、`_tkinter.pyd`、`_tcl_data`、`_tk_data`
分别对应上表中的 Pillow 与 Tcl/Tk。
