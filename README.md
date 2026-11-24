# RawDeveloper

Windows 用の RAW 現像ソフト。C++20 ＋ Qt 6 ＋ LibRaw。

- `src/engine/` … 現像エンジン（画面に依存しない部分）
- `src/ui/` … 画面（Qt 6 Widgets）
- `tests/` … エンジンの動作確認
- `prototype/python/` … Python 版の初期版（機能の仕様・見た目の基準）

## exe の入手
GitHub の「Actions」タブ → 最新の「Build Windows」→ 下の「Artifacts」から `RawDeveloper-windows` をダウンロードして展開し、`RawDeveloper.exe` を起動します。

## 開発状況
C++ 版は移植中です。いまは RAW を開いて表示し、露出を変えられるところまでです。
次は Python 版の現像処理（WB・トーン・カーブ・HSL・カラーホイール・ディテール・レンズ補正）を順に移します。

## 手元でビルドする場合
Visual Studio 2022、CMake、vcpkg、Qt 6 を入れて：

```
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake -DCMAKE_PREFIX_PATH=<Qt>/msvc2022_64
cmake --build build --config Release
```
