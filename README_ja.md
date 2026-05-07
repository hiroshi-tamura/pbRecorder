<p align="center">
  <img src="resources/icon.png" alt="pbRecorder" width="128">
</p>

# pbRecorder

[English README](README.md)

Windows向けの高機能スクリーンレコーダーです。DXGI Desktop Duplicationを使用した高速なキャプチャと、複数のコーデック・コンテナ形式をサポートしています。GUIとCLIの両方に対応。

## 主な機能

### キャプチャモード
- **画面全体** — ディスプレイ全体を録画（マルチモニター対応）
- **ウィンドウ指定** — 特定のウィンドウだけを録画
- **範囲指定** — 画面の任意の矩形領域を録画
  - **オートアジャスト機能** — ウィンドウの境界線を自動検出し、選択範囲をスナップ
  - 選択後もエッジをドラッグして微調整可能

### 映像
- **コーデック**: H.264, WMV
- **ハードウェアエンコード**: Media Foundation経由のGPUエンコード対応
- **リアルタイム H.264 MP4**: Media Foundation の fragmented MP4 出力を使い、録画中にデータを確定しながら書き出して停止時の確定処理を軽くします
- **H.264オプション**: プロファイル (Baseline/Main/High), レベル (Auto/4.0〜5.1)
- **フレームレート**: 最大240fps
- **ビットレート**: 任意に設定可能（デフォルト8Mbps）
- **マウスカーソル**: 表示/非表示切り替え可能

### 音声
- **WASAPI**: システム音声（ループバック）またはマイク入力
- **ASIO**: 任意の低レイテンシASIOデバイス対応（ASIO SDKは別途入手）
- **コーデック**: AAC, MP3, Opus, Vorbis, PCM, WMA

### コンテナ形式
- **MP4** (.mp4) — H.264 + AAC/MP3
- **MKV** (.mkv) — H.264 + AAC/Opus/Vorbis/PCM（libmatroskaによるネイティブ実装）
- **WMV** (.wmv) — WMV + WMA

### その他
- **プリセット機能** — 録画設定の保存・読み込み
- **言語切り替え** — 英語 / 日本語 UI
- **ポータブル** — 設定はexe横のJSONファイルに保存（レジストリ不使用）
- **CLI対応** — フル機能のコマンドラインインターフェース (`pbRecorder-cli.exe`)

## 動作環境

- Windows 10 以降（64bit）
- DirectX 11対応GPU
- Qt 6ランタイム（同梱）

## インストール

1. [Releases](https://github.com/hiroshi-tamura/pbRecorder/releases)からZIPをダウンロード
2. 任意のフォルダに展開
3. `pbRecorder.exe`（GUI）または `pbRecorder-cli.exe`（CLI）を実行

## GUI の使い方

1. キャプチャソースを選択（画面全体/ウィンドウ/範囲指定）
2. 必要に応じて音声デバイスを選択
3. コンテナ形式・コーデックを設定
4. 出力先フォルダとファイル名を設定
5. 録画ボタン（または `Ctrl+R`）で録画開始/停止

### ショートカットキー
- `Ctrl+R` — 録画開始/停止
- 範囲選択時: `Enter` で確定、`Esc` でキャンセル

## CLI の使い方

`pbRecorder-cli.exe` はコマンドラインから全録画機能を利用できます。

### デバイス一覧

```bash
pbRecorder-cli --list-monitors
pbRecorder-cli --list-windows
pbRecorder-cli --list-audio-devices
```

### 基本的な録画

```bash
# 画面全体を録画、Ctrl+Cで停止
pbRecorder-cli --cli --auto-name -o ./Output/

# 60秒間録画
pbRecorder-cli --cli --duration 60 --auto-name -o ./Output/

# 出力ファイル指定
pbRecorder-cli --cli -o recording.mp4
```

### キャプチャモード

```bash
# 画面全体（モニター指定）
pbRecorder-cli --cli --mode screen --monitor 1 -o out.mp4

# ウィンドウ（タイトル部分一致）
pbRecorder-cli --cli --mode window --window "Chrome" -o out.mp4

# 範囲指定
pbRecorder-cli --cli --mode region --region 0,0,1920,1080 -o out.mp4
```

### 映像設定

```bash
# H.264, 60fps, 12Mbps, Highプロファイル
pbRecorder-cli --cli --vcodec h264 --container mp4 --fps 60 --vbitrate 12000 \
  --profile high --level 4.1 --hw-encoder -o out.mp4

# WMV, 30fps
pbRecorder-cli --cli --vcodec wmv --fps 30 --vbitrate 5000 -o out.wmv

# MKVコンテナ
pbRecorder-cli --cli --vcodec h264 --container mkv -o out.mkv
```

### 音声設定

```bash
# システム音声
pbRecorder-cli --cli --audio-out 0 --acodec aac --abitrate 192 -o out.mp4

# マイク
pbRecorder-cli --cli --audio-in 0 --acodec aac --abitrate 192 -o mic.mp4

# 音声なし
pbRecorder-cli --cli --no-audio -o out.mp4

# MKV + Opus
pbRecorder-cli --cli --container mkv --acodec opus --abitrate 128 -o out.mkv

# MKV + PCM (96kHz/24bit)
pbRecorder-cli --cli --container mkv --acodec pcm --sample-rate 96000 --bit-depth 24 -o out.mkv
```

### 全オプション一覧

`pbRecorder-cli --help` で全オプションを確認できます。

## ビルド方法

### 必要なもの
- CMake 3.24以上
- Qt 6.9以上
- MinGW-w64 または MSVC
- （任意）ASIO SDK — Steinberg から別途入手してください。このリポジトリには同梱しません

### ビルド手順

```bash
mkdir build && cd build
cmake .. -G "MinGW Makefiles" -DCMAKE_PREFIX_PATH="<Qt6のパス>"
cmake --build . --config Release -- -j4
```

ASIO を有効化する場合は、リポジトリ外の SDK パスを指定するか、git で無視される `third_party/asiosdk/` にローカル配置してください。

```bash
cmake .. -G "MinGW Makefiles" -DCMAKE_PREFIX_PATH="<Qt6のパス>" -DASIO_SDK_DIR="C:/SDKs/asiosdk"
```

`ASIO_SDK_DIR` に `common/asio.h` がない場合、pbRecorder は ASIO 無効でビルドされます。

以下の2つの実行ファイルが生成されます:
- `pbRecorder.exe` — GUIアプリケーション（WIN32サブシステム）
- `pbRecorder-cli.exe` — CLIアプリケーション（CONSOLEサブシステム）

サードパーティライブラリ（libebml, libmatroska, libogg, libvorbis, libopus）はCMakeのFetchContentで自動的にダウンロード・ビルドされます。

### スモークテスト

`BUILD_TESTING=ON` の場合、軽量な CTest スモークテストが登録されます。

```bash
ctest --test-dir build -C Release --output-on-failure
```

テストでは `pbRecorder-cli --help` によるCLI起動確認と、`pbRecorder.exe --ui-screenshot <path>` によるGUI初期画面のスクリーンショット生成を確認します。GUIテストは、アプリがハングした場合や画像が作成されない場合に失敗します。

GitHub Actions でも、push と pull request ごとに同じ Windows ビルドとスモークテストを実行します。

## 技術スタック

- **UI**: Qt 6 (Widgets)
- **キャプチャ**: DXGI Desktop Duplication API
- **エンコード**: Media Foundation (H.264/AAC/MP3/WMV/WMA)
- **MKVコンテナ**: libmatroska + libebml（ネイティブ実装）
- **音声キャプチャ**: WASAPI (ループバックまたはマイク), 任意のASIO
- **音声コーデック**: libopus, libvorbis（MKV用）

## エンコード・ライセンスについて

pbRecorderは**特許・ライセンス的にクリーンな構成**を採用しています。

### MP4コンテナ (H.264 + AAC/MP3)
- Windows標準の **Media Foundation** を使用してエンコード
- H.264/AACのコーデック実装はOS内蔵のものを利用するため、本アプリにコーデックライブラリは含まれていません
- **FFmpegやx264等のGPL/LGPLライブラリは一切使用していません**

### MKVコンテナ (H.264 + AAC/Opus/Vorbis/PCM)
- 映像: Media Foundation でraw H.264 NALUを生成し、**libmatroska/libebml** でMKVコンテナに直接書き込み
- 音声: AACはWindows標準AAC MFTを直接使用、Opusは**libopus** (BSD)、Vorbisは**libvorbis** (BSD)
- コンテナ: **libmatroska** (LGPL) + **libebml** (LGPL)
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
| Steinberg ASIO SDK | 任意・外部依存 | Steinbergライセンス | ASIOホスト対応。本リポジトリには同梱しません |

- **GPL汚染なし**: GPL/AGPLライセンスのライブラリは使用していません
- **FFmpeg不使用**: コーデック処理にFFmpegは使っていません
- Steinberg ASIO SDK のようなベンダーSDK本体は、公開リポジトリへコミットしない方針です

## ライセンス

MIT License
