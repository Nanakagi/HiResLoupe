# HiResLoupe / 高解像ルーペ

[English](#en) | [日本語](#ja)

<a id="en"></a>

## English

A small tray tool that magnifies the desktop around the mouse pointer in real time and enhances it to high resolution with the super resolution of the NVIDIA RTX Video SDK. Handy for small icons, diagrams and illustrations.

**No installer.** Unzip and run `HiResLoupe.exe`.

### Download

Go to [Releases](../../releases) and download **`HiResLoupe.zip`**.
(Do not download the automatic "Source code (zip)" / "Source code (tar.gz)" files. They contain the source only and no `.exe`.)

### Requirements

- Windows 11
- An NVIDIA RTX series GPU

### Quick start

1. Unzip `HiResLoupe.zip` and double-click `HiResLoupe.exe`. The loupe appears and the app stays in the system tray.
2. Show / hide the loupe: `Win` + `Alt` + `Z`. Quit: right-click the tray icon, then "Quit".
3. See [MANUAL.md](MANUAL.md) for all controls and settings.

### Build from source

Needs Visual Studio 2022 (C++ tools) and the NVIDIA RTX Video SDK. The SDK is not included in this repository; get it from NVIDIA and accept its license.

```powershell
.\build.ps1 -Sdk <path to the NVIDIA RTX Video SDK>
```

The path can also be given in the `NV_RTX_VIDEO_SDK` environment variable. The build puts `HiResLoupe.exe` and `nvngx_vsr.dll` in the project folder.

### License

The source code and the app are released under the [MIT License](LICENSE). `nvngx_vsr.dll` is part of the NVIDIA RTX Video SDK and follows NVIDIA's terms (see [NOTICE.txt](NOTICE.txt)).
The logo and icon were generated with ChatGPT. If you build something based on this software, we would appreciate a mention of "HiResLoupe" and "Nanakagi" (not required).
NVIDIA and NVIDIA RTX are trademarks or registered trademarks of NVIDIA Corporation. This app is an independent work and is not provided, endorsed or approved by NVIDIA.

### Author

Nanakagi

<!-- 作者リンク(Twitch など)と支援の窓口は、ここに追記する -->

---

<a id="ja"></a>

## 日本語

マウスの位置を中心にデスクトップ画面をリアルタイムで拡大し、NVIDIA RTX Video SDK の超解像で高解像度に補正して見る、タスクトレイ常駐の小さなツールです。小さなアイコンや図、イラストを見るときに便利です。

**インストールは要りません。** zip を展開して `HiResLoupe.exe` を起動するだけで使えます。

### ダウンロード

[Releases](../../releases) から **`HiResLoupe.zip`** をダウンロードしてください。
(自動で付く「Source code (zip)」「Source code (tar.gz)」は、ソースだけで `.exe` が入っていません。落とさないでください。)

### 必要なもの

- Windows 11
- NVIDIA RTX シリーズの GPU

### 使い始める

1. `HiResLoupe.zip` を展開して `HiResLoupe.exe` をダブルクリックします。ルーペが出て、タスクトレイに常駐します。
2. ルーペを出す / 消す: `Win` + `Alt` + `Z`。終了: トレイのアイコンを右クリック →「アプリを終了」。
3. 操作と設定は [MANUAL.md](MANUAL.md) を見てください。

### ソースからビルドする

Visual Studio 2022(C++ ツール)と NVIDIA RTX Video SDK が必要です。SDK はこのリポジトリに含まれていません。NVIDIA から入手し、その使用許諾に同意してください。

```powershell
.\build.ps1 -Sdk <NVIDIA RTX Video SDK のフォルダ>
```

SDK の場所は、環境変数 `NV_RTX_VIDEO_SDK` でも指定できます。ビルドすると、プロジェクトのフォルダに `HiResLoupe.exe` と `nvngx_vsr.dll` ができます。

### ライセンス

ソースコードとアプリ本体は [MIT License](LICENSE) で公開しています。`nvngx_vsr.dll` は NVIDIA RTX Video SDK の一部で、NVIDIA の条件に従います([NOTICE.txt](NOTICE.txt) を参照)。
ロゴとアイコンは ChatGPT で生成したものです。このソフトを元に何かを作ったときは、アプリ名「HiResLoupe」と作者名「Nanakagi」を載せてもらえると嬉しいです(必須ではありません)。
NVIDIA、NVIDIA RTX は NVIDIA Corporation の商標または登録商標です。本アプリは個人が作ったもので、NVIDIA が提供・推奨・承認したものではありません。

### 作者

Nanakagi
