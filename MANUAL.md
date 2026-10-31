# HiResLoupe Manual / 高解像ルーペ 使い方

[English](#en) | [日本語](#ja)

---

<a id="en"></a>

## English

A tool that magnifies the desktop screen around the mouse pointer in real time, enhancing it to high resolution as it goes.
Magnification uses the super resolution of the NVIDIA RTX Video SDK.

No installer: unzip the folder and run `HiResLoupe.exe`.

### Requirements

- Windows 11 / an NVIDIA RTX series GPU
- `nvngx_vsr.dll` and `HiResLoupe.ini` in the same folder as `HiResLoupe.exe`

### Start and quit

- **Start**: Double-click `HiResLoupe.exe` (the loupe appears right away; the app stays in the system tray)
- **Show / hide the loupe**: `Win` + `Alt` + `Z`, or left-click the tray icon
- **Hide the loupe only**: `Esc` or right-click
- **Quit completely**: Right-click the tray icon, then "Quit" (menu names below are shown in English; they change with the selected language)
- **Start with Windows**: Right-click the tray icon, then check "Start with Windows"; select it again to remove. After you sign in, the app stays in the tray without showing the loupe; press `Win` + `Alt` + `Z` to show it. No installer or administrator rights are needed. If you move `HiResLoupe.exe`, run it once from the new location to update the registration.

### Removing the app

Quit the app from the tray menu, turn off "Start with Windows" if you had turned it on, then delete the folder.

If you delete the folder without turning it off, an entry named "HiResLoupe" stays in the startup list. It does no harm: if you use the app again, that entry is simply reused.

### Controls

- **Change magnification**: Wheel down: +25%, wheel up: -25% (100-400%, 200% at first)
- **Move the cursor freely**: Hold `Ctrl` (free mode)
- **Resize the loupe**: `Ctrl` + wheel (down: larger, up: smaller)
- **Resize by dragging the frame**: Hold `Ctrl` and drag the frame of the loupe
- **Shape presets**: `1` = square / `2` = wide 16:9 / `3` = tall 16:9

- Clicks pass through the loupe to the app underneath. The wheel and the number keys `1` to `3` are used for the loupe while it is shown.
- When the mouse moves to another monitor, the loupe moves to that monitor too.

### Settings

Open `HiResLoupe.ini` in Notepad (or use "Open settings file" in the tray menu).
After editing and saving, right-click the tray icon and choose "Reload settings". The loupe is closed and shown again with the new settings, so there is no need to restart the app.

You can change: the start key / the initial magnification and its upper limit / the loupe size / the frame (on/off and width)

**Menu language**: Right-click the tray icon, then "Language" to choose English, Japanese, Simplified Chinese or Korean. The first time, the language follows your Windows display language (English if it is none of these); your choice is saved in `HiResLoupe.ini`. To change individual texts, fill in `[text]` in the same file and choose "Reload settings". Empty entries keep the selected language. Save the file as UTF-8 or ANSI.

```
[text]
show_loupe=Show loupe
hide_loupe=Hide loupe
open_settings_file=Open settings file
reload_settings=Reload settings
open_log=Open log
start_with_windows=Start with Windows
language=Language
quit=Quit
tray_tooltip=HiResLoupe (Win+Alt+Z)
```

- Each key is named after the English menu item it changes. `show_loupe` is the menu text while the loupe is hidden, and `hide_loupe` while it is visible.
- `language` is the name of the "Language" item, and `tray_tooltip` is the text shown when you hover over the tray icon.
- Do not write notes after a value on the same line; everything to the end of the line is read as the text.

### Troubleshooting

- **The loupe does not appear with `Win` + `Alt` + `Z`**: Another app is using the same key. Change `[hotkey]` in `HiResLoupe.ini` to another key. You can also left-click the tray icon to show it.
- **Edges look blurry**: Super resolution does not work at low magnification. Use 175% or higher.
- **It runs strangely**: Set `log_timing` to `1` in `HiResLoupe.ini`, start the app, then open `HiResLoupe.log` from "Open log" in the tray menu. (The log is written in Japanese.)

### License and notices

The source code and the app are released under the MIT License. `nvngx_vsr.dll` follows NVIDIA's terms. Please read `LICENSE` and `NOTICE.txt`.

This app uses the NVIDIA RTX Video SDK. NVIDIA and NVIDIA RTX are trademarks or registered trademarks of NVIDIA Corporation.
This app is an independent work and is not provided, endorsed or approved by NVIDIA.

---

<a id="ja"></a>

## 日本語

マウスの位置を中心にデスクトップ画面をリアルタイムで高解像度補正しながら拡大して見るツールです。
拡大には NVIDIA RTX Video SDK の超解像を使います。

インストールは要りません。zip を展開して `HiResLoupe.exe` を起動するだけで使えます。

### 必要なもの

- Windows 11 / NVIDIA RTX シリーズの GPU
- `HiResLoupe.exe` と同じフォルダに `nvngx_vsr.dll` と `HiResLoupe.ini`

### 起動・終了

- **起動**: `HiResLoupe.exe` をダブルクリック(すぐルーペが出ます。タスクトレイに常駐します)
- **ルーペを出す / 消す**: `Win` + `Alt` + `Z`、またはトレイのアイコンを左クリック
- **ルーペだけ消す**: `Esc` または右クリック
- **完全に終了**: トレイのアイコンを右クリック →「アプリを終了」
- **Windows と同時に起動する**: トレイのアイコンを右クリック →「Windows と同時に起動」にチェックを入れる(もう一度押すと解除)。パソコンを起動すると、ルーペは出さずにトレイに常駐します。ルーペは `Win` + `Alt` + `Z` で出せます。インストールや管理者権限は要りません。`HiResLoupe.exe` を別の場所へ移したときは、新しい場所で一度起動すると登録が更新されます。

### アプリを削除するとき

トレイメニューからアプリを終了し、自動起動をオンにしていた場合はオフにしてから、フォルダごと削除してください。

オフにせずにフォルダを消した場合、スタートアップに「HiResLoupe」という登録が残りますが、害はありません。再びこのアプリを使ったときに、その設定が再利用されるだけです。

### 操作

- **倍率を変える**: ホイールを下へ回すと +25%、上へ回すと -25%(100〜400%、最初は 200%)
- **ルーペの大きさを変える**: `Ctrl` + ホイール(下で大きく、上で小さく)
- **カーソルを自由に動かす**: `Ctrl` を押しっぱなしにする(フリーモード)
- **枠をつかんで大きさを変える**: `Ctrl` を押しながら、ルーペの枠をドラッグ
- **形のテンプレート**: `1` = 正方形 / `2` = 横長 16:9 / `3` = 縦長 16:9

- ルーペが出ている間も、クリックは下のアプリに通ります。ただしホイールと数字キー `1`〜`3` はルーペの操作に使われます。
- マウスが別のモニターへ移ると、ルーペもそのモニターへ移ります。

### 設定

`HiResLoupe.ini` をメモ帳で開いて直します(トレイのメニュー「設定ファイルを開く」でも開けます)。
直して保存したら、トレイのアイコンを右クリック →「設定を再読み込み」を選びます。ルーペを一度閉じて新しい設定で出し直すので、アプリを起動し直す必要はありません。

変えられるもの: 起動キー / 最初の倍率と上限 / ルーペの大きさ / 枠の有無と太さ

**メニューの言語**: トレイのアイコンを右クリック →「言語」(Language)で、English / 日本語 / 中文(简体) / 한국어 を選べます。最初は Windows の表示言語に合わせます(この4つ以外は英語)。選んだ言語は `HiResLoupe.ini` に保存されます。文字を個別に変えたいときは、同じファイルの `[text]` に書いて、「設定を再読み込み」を選びます。空欄は選んだ言語のままです。文字コードは UTF-8 でも ANSI でも構いません。

```
[text]
show_loupe=ルーペを出す
hide_loupe=ルーペを消す
open_settings_file=設定ファイルを開く
reload_settings=設定を再読み込み
open_log=ログを開く
start_with_windows=Windows と同時に起動
language=言語
quit=アプリを終了
tray_tooltip=高解像ルーペ (Win+Alt+Z)
```

- キーの名前は、変えるメニュー項目の英語名です。`show_loupe` はルーペが消えているとき、`hide_loupe` はルーペが出ているときのメニューの文字です。
- `language` は「言語」の項目の名前、`tray_tooltip` はトレイのアイコンにマウスを重ねたときの文字です。
- 項目の右に説明を書かないでください(行の終わりまで文字として読まれます)。

### 困ったとき

- **`Win` + `Alt` + `Z` で出ない**: 他のアプリが同じキーを使っています。`HiResLoupe.ini` の `[hotkey]` を別のキーに変えてください。また、トレイのアイコンを左クリックすれば出せます。
- **輪郭がくっきりしない**: 倍率が低いと超解像が働きません。175% 以上にしてください。
- **動きがおかしい**: `HiResLoupe.ini` の `log_timing` を `1` にして起動し、トレイの「ログを開く」で `HiResLoupe.log` を見てください。

### ライセンス・表記

ソースコードとアプリ本体は MIT License で公開しています。`nvngx_vsr.dll` は NVIDIA の条件に従います。`LICENSE` と `NOTICE.txt` を読んでください。

本アプリは NVIDIA RTX Video SDK を使用しています。NVIDIA、NVIDIA RTX は NVIDIA Corporation の商標または登録商標です。
本アプリは個人が作ったもので、NVIDIA が提供・推奨・承認したものではありません。
