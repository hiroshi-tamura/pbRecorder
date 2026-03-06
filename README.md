# pbRecorder

Windows向けの高機能スクリーンレコーダーです。DXGI Desktop Duplicationを使用した高速なキャプチャと、複数のコーデック・コンテナ形式をサポートしています。

## 主な機能

### キャプチャモード
- **画面全体** - ディスプレイ全体を録画（マルチモニター対応）
- **ウィンドウ指定** - 特定のウィンドウだけを録画
- **範囲指定** - 画面の任意の矩形領域を録画
  - **オートアジャスト機能** - ウィンドウの境界線を自動検出し、選択範囲をスナップ
  - 選択後もエッジをドラッグして微調整可能

### 映像
- **コーデック**: H.264, WMV
- **ハードウェアエンコード**: Media Foundation経由のGPUエンコード対応
- **フレームレート**: 最大60fps
- **ビットレート**: 任意に設定可能（デフォルト8Mbps）
- **マウスカーソル**: 表示/非表示切り替え可能

### 音声
- **WASAPI**: システム音声（ループバック）とマイク入力の同時録音
- **ASIO**: 低レイテンシのASIOデバイス対応（ASIO SDKが必要）
- **コーデック**: AAC, MP3, Opus, Vorbis, PCM, WMA

### コンテナ形式
- **MP4** (.mp4) - H.264 + AAC/MP3
- **MKV** (.mkv) - H.264 + Opus/Vorbis/PCM（libmatroskaによるネイティブ実装）
- **WMV** (.wmv) - WMV + WMA

## 動作環境

- Windows 10 以降（64bit）
- DirectX 11対応GPU
- Qt 6ランタイム（同梱）

## インストール

1. [Releases](https://github.com/hiroshi-tamura/pbRecorder/releases)からZIPをダウンロード
2. 任意のフォルダに展開
3. `pbRecorder.exe` を実行

## ビルド方法

### 必要なもの
- CMake 3.24以上
- Qt 6.9以上
- MinGW-w64 または MSVC
- （任意）ASIO SDK - `third_party/asiosdk/` に配置

### ビルド手順

```bash
mkdir build && cd build
cmake .. -G "MinGW Makefiles" -DCMAKE_PREFIX_PATH="<Qt6のパス>"
cmake --build . --config Release -- -j4
```

サードパーティライブラリ（libebml, libmatroska, libogg, libvorbis, libopus）はCMakeのFetchContentで自動的にダウンロード・ビルドされます。

## 使い方

1. キャプチャソースを選択（画面全体/ウィンドウ/範囲指定）
2. 必要に応じて音声デバイスを選択
3. コンテナ形式・コーデックを設定
4. 出力先ファイルパスを設定
5. 録画ボタン（または `Ctrl+R`）で録画開始/停止

### ショートカットキー
- `Ctrl+R` - 録画開始/停止
- 範囲選択時: `Enter` で確定、`Esc` でキャンセル

## 技術スタック

- **UI**: Qt 6 (Widgets)
- **キャプチャ**: DXGI Desktop Duplication API
- **エンコード**: Media Foundation (H.264/AAC/MP3/WMV/WMA)
- **MKVコンテナ**: libmatroska + libebml（ネイティブ実装）
- **音声キャプチャ**: WASAPI (ループバック/マイク), ASIO
- **音声コーデック**: libopus, libvorbis（MKV用）

## エンコード・デコードについて

pbRecorderは**特許・ライセンス的にクリーンな構成**を採用しています。

### MP4コンテナ (H.264 + AAC/MP3)
- Windows標準の **Media Foundation** を使用してエンコード
- H.264/AACのコーデック実装はOS内蔵のものを利用するため、本アプリにコーデックライブラリは含まれていません
- FFmpegやx264等のGPL/LGPLライブラリは**一切使用していません**

### MKVコンテナ (H.264 + Opus/Vorbis/PCM)
- 映像エンコード: Media Foundation (OS内蔵H.264エンコーダ) でraw H.264 NALUを生成し、**libmatroska/libebml** でMKVコンテナに直接書き込み
- 音声エンコード: **libopus** (BSD) または **libvorbis** (BSD) を使用
- MKVコンテナ: **libmatroska** (LGPL) + **libebml** (LGPL) によるネイティブ実装
- PCM（無圧縮）での録音も可能

### WMVコンテナ (WMV + WMA)
- Media Foundation によるエンコード（OS内蔵）

### 使用ライブラリとライセンス

| ライブラリ | バージョン | ライセンス | 用途 |
|-----------|-----------|-----------|------|
| Qt 6 | 6.9+ | LGPL v3 | UIフレームワーク |
| libmatroska | 1.7.1 | LGPL v2.1 | MKVコンテナ書き込み |
| libebml | 1.4.5 | LGPL v2.1 | EBML (MKV基盤) |
| libopus | 1.4 | BSD 3-Clause | Opus音声エンコード |
| libvorbis | 1.3.7 | BSD 3-Clause | Vorbis音声エンコード |
| libogg | 1.3.5 | BSD 3-Clause | Ogg基盤ライブラリ |
| Media Foundation | OS内蔵 | Windows標準 | H.264/AAC/MP3/WMV/WMAエンコード |
| DXGI | OS内蔵 | Windows標準 | 画面キャプチャ |
| WASAPI | OS内蔵 | Windows標準 | 音声キャプチャ |

- **GPL汚染なし**: GPL/AGPLライセンスのライブラリは使用していません
- **FFmpeg不使用**: コーデック処理にFFmpegは使っていません
- すべてのサードパーティライブラリはBSDまたはLGPLであり、商用利用可能です

## ライセンス

MIT License
