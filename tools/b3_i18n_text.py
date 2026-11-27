"""The desktop launcher's translations (b3_i18n.py): {English as in the code: (Simplified Chinese, Japanese)}.
A new _('...'), setting label, help or choice needs its line here (tests/test_launcher_i18n.py checks it)."""

TEXT = {
    # ---- home
    'Native build of the Switch game · bring your own game files':
        ('Switch 游戏的原生移植 · 请自备游戏文件', 'Switch 版ゲームのネイティブ移植 · ゲームファイルはご自身で用意'),
    'Start game': ('开始游戏', 'ゲーム開始'),
    'Stop game': ('停止游戏', 'ゲームを止める'),
    'Settings': ('设置', '設定'),
    'Game log': ('游戏日志', 'ゲームログ'),
    'Hide game log': ('隐藏游戏日志', 'ゲームログを隠す'),
    'Report a bug': ('提交 bug', '不具合を報告'),
    'Quit': ('退出', '終了'),
    'Set up from my dump': ('用我的转储安装', '自分のダンプからセットアップ'),
    'Setting up…': ('正在安装…', 'セットアップ中…'),
    'Rebuild': ('重新构建', '再ビルド'),
    'Rebuilding…': ('正在重新构建…', '再ビルド中…'),
    'Send to my phone': ('发送到我的手机', 'スマホに送る'),
    'Sending to the phone…': ('正在发送到手机…', 'スマホに送信中…'),
    '↑ ↓ choose   ·   Enter / A confirm   ·   Esc quit': ('↑ ↓ 选择   ·   Enter / A 确认   ·   Esc 退出',
                                                          '↑ ↓ 選択   ·   Enter / A 決定   ·   Esc 終了'),
    'Running': ('运行中', '実行中'),
    'Stopping…': ('正在停止…', '停止中…'),
    'Not built yet · {hint}': ('尚未构建 · {hint}', '未ビルド · {hint}'),
    'Ready to play': ('可以开始游戏', 'プレイできます'),
    'Unsaved changes': ('有未保存的更改', '未保存の変更があります'),
    'All changes saved': ('所有更改已保存', 'すべての変更を保存しました'),
    'No controller yet · XInput ready': ('还没有手柄 · XInput 已就绪', 'コントローラーなし · XInput 準備完了'),
    'No controller · keyboard works': ('没有手柄 · 可以用键盘', 'コントローラーなし · キーボードで操作できます'),
    'Controller: {error}': ('手柄：{error}', 'コントローラー：{error}'),
    '(no log yet: it appears once the game has run)': ('（还没有日志：游戏运行过之后才会出现）', '（ログはまだありません。ゲームを一度動かすと出ます）'),
    '(empty log)': ('（日志为空）', '（ログは空です）'),

    # ---- settings panel
    'Back': ('返回', '戻る'),
    'Save': ('保存', '保存'),
    'Reset to defaults': ('恢复默认', '初期設定に戻す'),
    'Default': ('默认', '既定'),
    'Default {hint}': ('默认 {hint}', '既定 {hint}'),
    'Desktop shortcut': ('桌面快捷方式', 'デスクトップのショートカット'),
    'Follow the author on X: @aoxzh': ('在 X 上关注作者：@aoxzh', 'X で作者をフォロー：@aoxzh'),
    'Update available: {tag}': ('有新版本：{tag}', '新しいバージョンがあります：{tag}'),
    'Your settings, saves, game data and mods stay.': ('你的设置、存档、游戏数据和 mod 都会保留。', '設定、セーブ、ゲームデータ、MOD はそのまま残ります。'),
    'Check for updates': ('检查更新', '更新を確認'),
    'Update now': ('立即更新', '今すぐ更新'),
    "What's new": ('更新内容', '変更内容'),
    'Update to {tag}': ('更新到 {tag}', '{tag} に更新'),
    'This downloads the new release ({size}), checks it, and puts it in place of the program files. Your settings, '
    'saves, game data and mods stay.\n\nThe launcher then restarts and rebuilds the game with the new version (a few '
    'minutes).':
        ('将下载新版本（{size}），校验后替换程序文件。你的设置、存档、游戏数据和 Mod 都会保留。\n\n'
         '随后启动器会重启，并用新版本重新构建游戏（几分钟）。',
         '新しいリリース（{size}）をダウンロードして確認し、プログラムのファイルを置き換えます。設定、セーブ、ゲームデータ、Mod は'
         'そのまま残ります。\n\nその後ランチャーが再起動し、新しいバージョンでゲームを再ビルドします（数分）。'),
    'Close the game first, then update.': ('请先关闭游戏再更新。', '先にゲームを閉じてから更新してください。'),
    'Update: starting the download': ('更新：开始下载', '更新：ダウンロードを開始'),
    'Update: downloading {done} / {total} MB': ('更新：正在下载 {done} / {total} MB', '更新：ダウンロード中 {done} / {total} MB'),
    'Update: restarting…': ('更新：正在重启…', '更新：再起動中…'),
    'Update stopped: {why}': ('更新已停止：{why}', '更新を中止しました：{why}'),
    'Updated to {version}': ('已更新到 {version}', '{version} に更新しました'),
    'Updated to {version}: rebuilding the game': ('已更新到 {version}：正在重新构建游戏', '{version} に更新しました：ゲームを再ビルド中'),
    'The update could not copy its files: see private/update/apply.log':
        ('更新未能复制文件：请查看 private/update/apply.log', '更新でファイルをコピーできませんでした：private/update/apply.log を確認してください'),
    'At start, ask GitHub whether a newer release is out and say so on the home screen. Nothing is downloaded or '
    'installed by itself.':
        ('启动时向 GitHub 查询是否有新版本，有的话在首页提示。不会自动下载或安装任何东西。',
         '起動時に GitHub で新しいリリースがあるか確認し、あればホーム画面に表示します。自動でダウンロードやインストールはしません。'),
    'Shortcut: {paths}': ('快捷方式：{paths}', 'ショートカット：{paths}'),
    'Could not make the shortcut: {e}': ('无法创建快捷方式：{e}', 'ショートカットを作れません：{e}'),
    'Could not open {folder}: {e}': ('无法打开 {folder}：{e}', '{folder} を開けません：{e}'),
    'Saved to {path}': ('已保存到 {path}', '{path} に保存しました'),
    'Reset settings': ('恢复设置', '設定のリセット'),
    'Drop every saved setting and go back to the defaults?': ('丢弃所有已保存的设置，恢复为默认值？', '保存した設定をすべて消して、初期設定に戻しますか？'),
    'Defaults restored': ('已恢复默认', '初期設定に戻しました'),
    'On': ('开', 'オン'),
    'Off': ('关', 'オフ'),
    # groups
    'Picture': ('画面', '画面'),
    'Controller': ('手柄', 'コントローラー'),
    'Audio': ('声音', 'サウンド'),
    'System': ('系统', 'システム'),
    'Mods': ('Mod', 'Mod'),
    'Advanced': ('高级', '詳細'),
    # mods
    'Add mod (.zip)': ('添加 Mod（.zip）', 'Mod を追加（.zip）'),
    'Add folder': ('添加文件夹', 'フォルダーを追加'),
    'Open mods folder': ('打开 Mod 文件夹', 'Mod フォルダーを開く'),
    'No mods yet.': ('还没有 Mod。', 'Mod はまだありません。'),
    'Add a mod from its .zip or folder below (a romfs/ folder for Atmosphere, yuzu or Ryujinx works as it is), or put '
    'each mod in its own folder in {folder}.':
        ('用下面的按钮从 .zip 或文件夹添加 Mod（Atmosphere、yuzu 或 Ryujinx 用的 romfs/ 文件夹可以直接用），'
         '或者把每个 Mod 放进 {folder} 里各自的文件夹。',
         '下のボタンで .zip かフォルダーから Mod を追加するか（Atmosphere、yuzu、Ryujinx 用の romfs/ フォルダーは'
         'そのまま使えます）、各 Mod を {folder} の中にそれぞれのフォルダーで置いてください。'),
    'Each folder in {folder} replaces game files by their RomFS path (its romfs/ folder, also one inside an Atmosphere '
    'or emulator layout, or the folder itself). Mods apply top to bottom and a lower one wins; ▲ ▼ reorder. Changes '
    'apply at the next start. Code patches (.ips, .pchtxt for version 1.2.0) work too: when they change, Play makes '
    'the game code again first (a few minutes).':
        ('{folder} 里的每个文件夹按 RomFS 路径替换游戏文件（它的 romfs/ 文件夹，Atmosphere 或模拟器目录结构里的也行，'
         '或文件夹本身）。Mod 从上到下应用，下面的优先；用 ▲ ▼ 调整顺序。更改在下次启动时生效。代码补丁（1.2.0 版的 '
         '.ips、.pchtxt）也可以用：补丁有变化时，开始游戏前会先重新生成游戏代码（几分钟）。',
         '{folder} の各フォルダーは RomFS のパスでゲームファイルを置き換えます（その romfs/ フォルダー、Atmosphere や'
         'エミュレーターの構成の中にあるものも可、またはフォルダー自体）。Mod は上から順に適用され、下のものが優先です。'
         '▲ ▼ で並べ替え。変更は次の起動から反映されます。コードパッチ（バージョン 1.2.0 用の .ips、.pchtxt）も使えます。'
         '変わったときは、プレイ前にゲームコードを作り直します（数分）。'),
    'Mod {name} is on from the next start.': ('Mod {name} 下次启动时开启。', 'Mod {name} は次の起動からオンです。'),
    'Mod {name} is off from the next start.': ('Mod {name} 下次启动时关闭。', 'Mod {name} は次の起動からオフです。'),
    'Mod {name} moved; the order applies from the next start.': ('Mod {name} 已移动；顺序在下次启动时生效。',
                                                                 'Mod {name} を移動しました。順番は次の起動から反映されます。'),
    'Mod {name} added; it is on from the next start.': ('Mod {name} 已添加；下次启动时开启。', 'Mod {name} を追加しました。次の起動からオンです。'),
    'Could not add the mod: {e}': ('无法添加 Mod：{e}', 'Mod を追加できません：{e}'),
    'A mod archive': ('Mod 压缩包', 'Mod のアーカイブ'),
    'Zip archives': ('Zip 压缩包', 'Zip アーカイブ'),
    'All files': ('所有文件', 'すべてのファイル'),
    "A mod's folder (its romfs/ or the folder above it)": ('Mod 的文件夹（它的 romfs/ 或上一级文件夹）',
                                                           'Mod のフォルダー（その romfs/ か、その一つ上のフォルダー）'),

    # ---- settings (b3_launcher_config.py)
    'Window size': ('窗口大小', 'ウィンドウサイズ'),
    'The window. The game renders at 1600 x 900 (1280 x 720 in handheld mode) and is scaled to it.':
        ('游戏窗口。游戏以 1600 x 900（掌机模式 1280 x 720）渲染，再缩放到窗口大小。',
         'ウィンドウ。ゲームは 1600 x 900（携帯モードでは 1280 x 720）で描画され、ウィンドウに合わせて拡大縮小されます。'),
    'Largest 16:9 that fits the display (auto)': ('适合屏幕的最大 16:9（自动）', '画面に収まる最大の 16:9（自動）'),
    '1280 x 720 (720p, fastest)': ('1280 x 720（720p，最快）', '1280 x 720（720p、最速）'),
    '1920 x 1080 (1080p)': ('1920 x 1080（1080p）', '1920 x 1080（1080p）'),
    '2560 x 1440 (1440p)': ('2560 x 1440（1440p）', '2560 x 1440（1440p）'),
    '3840 x 2160 (4K, slowest)': ('3840 x 2160（4K，最慢）', '3840 x 2160（4K、最も重い）'),
    'Fullscreen': ('全屏', 'フルスクリーン'),
    'Start fullscreen; F11 or Alt+Enter toggles it.': ('以全屏启动；F11 或 Alt+Enter 切换。', 'フルスクリーンで起動します。F11 か Alt+Enter で切り替え。'),
    'FSR 1 upscale': ('FSR 1 放大', 'FSR 1 アップスケール'),
    'Upscale and sharpen when the picture is smaller than the window. This game renders at the window size, so it has '
    'nothing to upscale.':
        ('画面比窗口小时放大并锐化。本游戏按窗口大小渲染，所以没有需要放大的。',
         '画像がウィンドウより小さいときに拡大してシャープにします。このゲームはウィンドウサイズで描画するので、拡大するものはありません。'),
    'FSR sharpness': ('FSR 锐度', 'FSR シャープネス'),
    'Only with FSR 1 on.': ('仅在开启 FSR 1 时有效。', 'FSR 1 がオンのときだけ。'),
    '0.2 (default)': ('0.2（默认）', '0.2（既定）'),
    'Show frame rate': ('显示帧率', 'フレームレートを表示'),
    'The frame rate in the top left corner of the game.': ('在游戏画面左上角显示帧率。', 'ゲーム画面の左上にフレームレートを表示します。'),
    'Frame rate cap': ('帧率上限', 'フレームレート上限'),
    "The game's own rule (60, 30 when heavy)": ('游戏自己的规则（60，负载重时 30）', 'ゲーム自身の規則（60、重いときは 30）'),
    '30 fps': ('30 fps', '30 fps'),
    'Hold the game at 30 fps: a flat 30 instead of an uneven 45-60 swing. Measured on a Snapdragon 8 Gen 2: 163 of 174 windows at '
    'exactly 30.0 fps even heat-soaked, replay 14.2 ms of the 33.3 ms budget. It helps most on a phone that is already '
    'warm. The game speed at 30 is NOT verified, so try both. On PC, start from unset.':
        ('把游戏锁在 30 fps：稳定的 30，而不是在 45-60 之间忽高忽低。在骁龙 8 Gen 2 上实测：即使持续发热，174 个统计窗口中有 163 个'
         '正好是 30.0 fps，回放只用了 33.3 ms 预算中的 14.2 ms。对已经发热的手机帮助最大。30 fps 下的游戏速度尚未验证，'
         '两种都试试。电脑上请先保持不设置。',
         'ゲームを 30 fps に固定します：45〜60 の不安定な揺れではなく、平らな 30。Snapdragon 8 Gen 2 での実測：熱がこもっても 174 区間中 '
         '163 区間がちょうど 30.0 fps、再生は 33.3 ms の予算のうち 14.2 ms。すでに熱くなった端末で最も効果があります。'
         '30 での速度は未検証なので両方試してください。PC では未設定から始めてください。'),
    'Optionally cap at 30 fps; used in the documented Snapdragon 8 Gen 2 / Adreno 740 combat tests. See Performance '
    'for heat-related limits.':
        ('可选锁定 30 fps；文档中骁龙 8 Gen 2 / Adreno 740 的战斗测试就是这样跑的。发热相关的限制见“性能”一节。',
         '必要なら 30 fps に制限します。資料にある Snapdragon 8 Gen 2 / Adreno 740 の戦闘テストはこの設定です。'
         '発熱による制限は「パフォーマンス」を参照してください。'),
    'Frame rate': ('帧率', 'フレームレート'),
    "The display's own rate": ('显示器自身的刷新率', 'ディスプレイ自身のリフレッシュレート'),
    '60 Hz': ('60 Hz', '60 Hz'),
    '90 Hz': ('90 Hz', '90 Hz'),
    '120 Hz': ('120 Hz', '120 Hz'),
    '144 Hz': ('144 Hz', '144 Hz'),
    'The rate the game paces itself by. Above 60 the gameplay and its timing windows are kept at real time (hit-stops '
    'are a little shorter); fights that cannot keep up settle where they can.':
        ('游戏按这个频率控制节奏。高于 60 时，玩法和判定时间窗按真实时间保持（打击停顿会稍短）；跟不上的战斗会稳定在能达到的帧率。',
         'ゲームが進行の基準にするレート。60 を超えると、ゲームプレイと判定の時間枠は実時間に保たれます（ヒットストップは少し'
         '短め）。追いつかない戦闘は出せるところで落ち着きます。'),
    'V-sync': ('垂直同步', '垂直同期'),
    'Wait for the display refresh (no tearing, a little more latency). Off shows the newest frame at each refresh '
    'without waiting.':
        ('等待显示器刷新（没有画面撕裂，延迟稍高）。关闭时每次刷新都显示最新的一帧，不等待。',
         'ディスプレイの更新を待ちます（ティアリングなし、遅延が少し増える）。オフでは待たずに各更新で最新のフレームを表示します。'),
    'Anisotropic filtering': ('各向异性过滤', '異方性フィルタリング'),
    'Sharper ground and walls seen at a slant, for a small GPU cost.': ('斜看的地面和墙壁更清晰，GPU 开销很小。',
                                                                        '斜めに見える地面や壁がくっきりします。GPU の負担は少しです。'),
    "The game's own": ('游戏默认', 'ゲーム既定'),
    'Model detail': ('模型细节', 'モデルの詳細度'),
    'Highest': ('最高', '最高'),
    'Highest: models keep their finest detail at any distance, for some GPU time.': ('最高：模型在任何距离都保持最精细的细节，会多用一些 GPU。',
                                                                                     '最高：どの距離でもモデルを最も細かく保ちます。GPU を少し使います。'),
    'Film grain': ('胶片颗粒', 'フィルムグレイン'),
    "Off: a clean picture without the game's moving noise (from the next scene on); the vignette stays.":
        ('关闭：画面干净，没有游戏的动态噪点（从下一个场景起）；暗角保留。', 'オフ：ゲームの動くノイズのないきれいな画面（次の場面から）。周辺減光は残ります。'),
    'Xbox button layout': ('Xbox 按键布局', 'Xbox ボタン配置'),
    'Swap A<->B and X<->Y, for players used to Xbox labels.': ('交换 A<->B 和 X<->Y，适合习惯 Xbox 按键标识的玩家。',
                                                               'A<->B と X<->Y を入れ替えます。Xbox の表記に慣れた人向け。'),
    'Stick deadzone': ('摇杆死区', 'スティックのデッドゾーン'),
    'Default 7849 of 32767. 0: no deadzone.': ('默认 7849（共 32767）。0：没有死区。', '既定は 32767 中 7849。0：デッドゾーンなし。'),
    'Trigger threshold': ('扳机阈值', 'トリガーのしきい値'),
    'Default 12000 of 32767.': ('默认 12000（共 32767）。', '既定は 32767 中 12000。'),
    'Player 1 controller': ('1P 手柄', '1P コントローラー'),
    'Which connected controller plays (0 = the first). Default 0.': ('用哪个已连接的手柄玩（0 = 第一个）。默认 0。',
                                                                     'どの接続中のコントローラーで遊ぶか（0 = 最初の 1 台）。既定は 0。'),
    'Invert camera X': ('反转镜头 X 轴', 'カメラ X 軸を反転'),
    'Invert camera Y': ('反转镜头 Y 轴', 'カメラ Y 軸を反転'),
    'Mouse camera': ('鼠标控制镜头', 'マウスでカメラ操作'),
    'Windows: click into the game window to turn the camera with the mouse; F10 releases it. Keys are rebound with '
    'key_<input>= lines in the settings file.':
        ('Windows：点击游戏窗口后用鼠标转动镜头；F10 释放鼠标。按键可以在设置文件里用 key_<input>= 行重新绑定。',
         'Windows：ゲームのウィンドウをクリックするとマウスでカメラを回せます。F10 で解放。キーは設定ファイルの '
         'key_<input>= の行で割り当て直せます。'),
    'Mouse speed': ('鼠标速度', 'マウス速度'),
    '1.0 (default)': ('1.0（默认）', '1.0（既定）'),
    'Invert mouse Y': ('反转鼠标 Y 轴', 'マウス Y 軸を反転'),
    'Language': ('语言', '言語'),
    'The game text language. Same as the system: the language Windows or Linux is set to.':
        ('游戏文字的语言。跟随系统：Windows 或 Linux 设置的语言。', 'ゲームの文字の言語。システムと同じ：Windows や Linux で設定している言語。'),
    'Same as the system': ('跟随系统', 'システムと同じ'),
    'English (US)': ('英语（美国）', '英語（米国）'),
    'English (UK)': ('英语（英国）', '英語（英国）'),
    'Japanese': ('日语', '日本語'),
    'French': ('法语', 'フランス語'),
    'French (Canada)': ('法语（加拿大）', 'フランス語（カナダ）'),
    'German': ('德语', 'ドイツ語'),
    'Italian': ('意大利语', 'イタリア語'),
    'Spanish': ('西班牙语', 'スペイン語'),
    'Spanish (Latin America)': ('西班牙语（拉丁美洲）', 'スペイン語（中南米）'),
    'Korean': ('韩语', '韓国語'),
    'Russian': ('俄语', 'ロシア語'),
    'Chinese (Simplified)': ('简体中文', '中国語（簡体字）'),
    'Chinese (Traditional)': ('繁体中文', '中国語（繁体字）'),
    'Handheld mode': ('掌机模式', '携帯モード'),
    'The game renders 1280 x 720 instead of docked 1600 x 900 (a lighter GPU load).':
        ('游戏以 1280 x 720 渲染，而不是主机模式的 1600 x 900（GPU 负担更轻）。', 'ゲームは TV モードの 1600 x 900 ではなく 1280 x 720 で描画します（GPU の負担が軽い）。'),
    'Log file': ('日志文件', 'ログファイル'),
    'Default: <repo>/private/b3.log.': ('默认：<repo>/private/b3.log。', '既定：<repo>/private/b3.log。'),
    'Mute': ('静音', 'ミュート'),
    'No audio output.': ('不输出声音。', '音を出しません。'),
    'Display resolution answer': ('报告给游戏的分辨率', 'ゲームに伝える解像度'),
    'NX_RENDER_RES: the resolution reported to the game. Other ports render at it; Bayonetta 3 renders at its own '
    'console size, so this changes nothing here.':
        ('NX_RENDER_RES：报告给游戏的分辨率。其他移植会按它渲染；Bayonetta 3 按自己的主机分辨率渲染，所以这里没有影响。',
         'NX_RENDER_RES：ゲームに伝える解像度。ほかの移植ではこれで描画しますが、Bayonetta 3 は本体の解像度で描画するので、'
         'ここでは何も変わりません。'),
    'Console size (1920 x 1080, 1280 x 720 handheld)': ('主机分辨率（1920 x 1080，掌机 1280 x 720）', '本体のサイズ（1920 x 1080、携帯 1280 x 720）'),
    'Resolution scale': ('分辨率缩放', '解像度スケール'),
    'NX_RENDER_SCALE: a factor on the reported resolution. Same caveat as above.': ('NX_RENDER_SCALE：报告分辨率的缩放系数。注意事项同上。',
                                                                                     'NX_RENDER_SCALE：伝える解像度にかける倍率。注意点は上と同じ。'),
    '1.0 (native)': ('1.0（原生）', '1.0（ネイティブ）'),
    'Ultrawide': ('超宽屏', 'ウルトラワイド'),
    'For windows wider than 16:9 (21:9, 32:9): the world shows more to the sides (Hor+) and menus and text keep their '
    'shape, centred. Some menu backgrounds still end at the 16:9 edges.':
        ('用于比 16:9 更宽的窗口（21:9、32:9）：两侧能看到更多场景（Hor+），菜单和文字保持比例并居中。部分菜单背景仍停在 16:9 的边缘。',
         '16:9 より横長のウィンドウ（21:9、32:9）向け：左右により多く見え（Hor+）、メニューと文字は形を保って中央に。'
         '一部のメニュー背景は 16:9 の端で切れます。'),
    'Memory limit (GB)': ('内存上限（GB）', 'メモリ上限（GB）'),
    # The public tree's own wording of two settings (public/overlay/tools/b3_launcher_config.py).
    'The guard stops the process above this footprint. Empty uses the platform default; Windows uses 60% of RAM.':
        ('超过这个占用时保护机制会结束进程。留空使用平台默认值；Windows 上为内存的 60%。',
         'この使用量を超えるとガードがプロセスを止めます。空欄ならプラットフォームの既定値で、Windows では RAM の 60% です。'),
    'Upscale and sharpen when the rendered picture is smaller than the output window.':
        ('渲染画面比输出窗口小时放大并锐化。', 'レンダリング画像が出力ウィンドウより小さいとき、拡大してシャープにします。'),
    'Default 8: the guard ends the process past this footprint.': ('默认 8：超过这个占用时保护机制会结束进程。',
                                                                   '既定は 8：この使用量を超えると保護機能がプロセスを終了します。'),
    'GPU submits in flight': ('同时进行的 GPU 提交', '同時に進行する GPU 送信'),
    'Experimental; absent is synchronous.': ('实验性；不设置时为同步。', '実験的。未設定なら同期。'),
    'Off (synchronous)': ('关（同步）', 'オフ（同期）'),
    'Pipeline cache': ('管线缓存', 'パイプラインキャッシュ'),
    'On-disk shader pipeline cache; off disables it.': ('硬盘上的着色器管线缓存；关闭则不使用。', 'ディスク上のシェーダーパイプラインキャッシュ。オフで無効。'),
    'Headless': ('无窗口', 'ヘッドレス'),
    'No window and no sound device (benchmarks only).': ('没有窗口也没有声音设备（仅用于测试）。', 'ウィンドウも音声デバイスもなし（ベンチマーク専用）。'),

    # ---- setup wizard
    'Set it up': ('安装', 'セットアップする'),
    'Set up from your game files': ('用你的游戏文件安装', 'ゲームファイルからセットアップ'),
    'Your game files': ('你的游戏文件', 'あなたのゲームファイル'),
    'Looking on this computer…': ('正在这台电脑上查找…', 'このコンピューターを探しています…'),
    'Start setup': ('开始安装', 'セットアップ開始'),
    'Choose a folder…': ('选择文件夹…', 'フォルダーを選ぶ…'),
    'Cancel': ('取消', 'キャンセル'),
    'Ready: {folder}\nThe setup builds the game from these files on this PC (about half an hour to two hours the first '
    'time; closing the launcher pauses it, Set up carries on).':
        ('就绪：{folder}\n安装会在这台电脑上用这些文件构建游戏（第一次大约半小时到两小时；关闭启动器会暂停，再点“安装”会继续）。',
         '準備完了：{folder}\nセットアップはこれらのファイルからこの PC でゲームを作ります（初回は約 30 分〜2 時間。ランチャーを'
         '閉じると一時停止し、セットアップで続きから再開）。'),
    'The folder with your extracted exefs and romfs': ('包含你提取出的 exefs 和 romfs 的文件夹', '抽出した exefs と romfs のあるフォルダー'),
    'Checking…': ('正在检查…', '確認中…'),
    'Nothing found in the usual places. Choose the folder where you extracted the game.':
        ('在常用位置没有找到。请选择你提取游戏的文件夹。', 'いつもの場所には見つかりませんでした。ゲームを抽出したフォルダーを選んでください。'),
    'Extract the game files with your emulator, after installing update 1.2.0 there:\n  Ryujinx: right-click Bayonetta 3 '
    '> Extract Data > ExeFS, pick a folder; then Extract Data > RomFS, same folder.\n  Eden / yuzu / Citron: right-click '
    'Bayonetta 3 > Dump RomFS > Full; for the ExeFS turn on Emulation > Configure > Debug > "Dump ExeFS" and start the '
    'game once.\nYou need a folder with exefs (main, sdk, subsdk0, main.npdm) and romfs (about 15 GB of .pkz files).':
        ('先在模拟器里装好 1.2.0 更新，再用模拟器提取游戏文件：\n  Ryujinx：右键 Bayonetta 3 > Extract Data > ExeFS，选一个文件夹；'
         '然后 Extract Data > RomFS，选同一个文件夹。\n  Eden / yuzu / Citron：右键 Bayonetta 3 > Dump RomFS > Full；ExeFS 需要打开 '
         'Emulation > Configure > Debug > "Dump ExeFS"，然后启动一次游戏。\n你需要一个包含 exefs（main、sdk、subsdk0、main.npdm）'
         '和 romfs（大约 15 GB 的 .pkz 文件）的文件夹。',
         'エミュレーターに更新 1.2.0 を入れてから、ゲームファイルを抽出してください：\n  Ryujinx：Bayonetta 3 を右クリック > '
         'Extract Data > ExeFS でフォルダーを選び、次に Extract Data > RomFS で同じフォルダーへ。\n  Eden / yuzu / Citron：'
         'Bayonetta 3 を右クリック > Dump RomFS > Full。ExeFS は Emulation > Configure > Debug > "Dump ExeFS" をオンにして'
         'ゲームを一度起動。\nexefs（main、sdk、subsdk0、main.npdm）と romfs（約 15 GB の .pkz ファイル）のあるフォルダーが必要です。'),

    # ---- tasks, phone, play
    'Setup': ('安装', 'セットアップ'),
    'Update': ('更新', '更新'),
    'Phone': ('手机', 'スマホ'),
    'starting': ('正在开始', '開始中'),
    '{kind}: starting': ('{kind}：正在开始', '{kind}：開始中'),
    '{kind} finished: ready to play': ('{kind}完成：可以开始游戏', '{kind}完了：プレイできます'),
    '{kind} stopped: {last}': ('{kind}已停止：{last}', '{kind}が止まりました：{last}'),
    'Could not start {kind}: {exc}': ('无法开始{kind}：{exc}', '{kind}を開始できません：{exc}'),
    'Phone: {last}': ('手机：{last}', 'スマホ：{last}'),
    'Phone ready: open Bayonetta 3 Recomp on the phone and tap START': ('手机已就绪：在手机上打开 Bayonetta 3 Recomp 并点“开始”',
                                                                        'スマホの準備完了：スマホで Bayonetta 3 Recomp を開いてスタートをタップ'),
    'Not sent: the phone is below the minimum': ('未发送：手机低于最低要求', '送っていません：スマホが最低要件を満たしていません'),
    'Builds the game for an Android phone on this computer (a few minutes), then installs the app and copies the game to '
    'the phone over USB (about 15 GB the first time; later only what changed).\n\nBefore you start: on the phone, turn on '
    'USB debugging (Settings > About phone > tap Build number 7 times, then Developer options > USB debugging), connect '
    'it by USB, allow this computer, and keep the phone unlocked.\n\nOn the phone, add Bayonetta 3 Recomp to its Game '
    'Center, Game Booster or game mode before you play: many phones slow down apps they do not know as games.':
        ('在这台电脑上为安卓手机构建游戏（几分钟），然后通过 USB 安装应用并把游戏复制到手机（第一次大约 15 GB；之后只复制有变化的部分）。'
         '\n\n开始之前：在手机上打开 USB 调试（设置 > 关于手机 > 连点版本号 7 次，然后开发者选项 > USB 调试），用 USB 连接，'
         '允许这台电脑，并保持手机解锁。\n\n玩之前请在手机的游戏中心、游戏助手或游戏模式里添加 Bayonetta 3 Recomp：很多手机会给'
         '不认识的游戏降频。',
         'この PC で Android 向けにゲームを作り（数分）、アプリをインストールして USB でゲームをスマホにコピーします（初回は約 '
         '15 GB、以降は変わった分だけ）。\n\n始める前に：スマホで USB デバッグをオンにし（設定 > 端末情報 > ビルド番号を 7 回'
         'タップ、次に開発者向けオプション > USB デバッグ）、USB で接続して、この PC を許可し、スマホのロックを解除したままに'
         'してください。\n\n遊ぶ前に、スマホのゲームセンター、Game Booster、またはゲームモードに Bayonetta 3 Recomp を追加して'
         'ください。多くの端末はゲームと認識していないアプリの速度を落とします。'),
    'The game is very unlikely to run on this phone:': ('这台手机很可能无法运行游戏：', 'このスマホではゲームはほぼ動きません：'),
    'Sending it copies about 15 GB, and the game may not start or may show only a black screen. Send it anyway?':
        ('发送会复制大约 15 GB，而且游戏可能无法启动或只显示黑屏。仍然发送吗？', '送ると約 15 GB をコピーしますが、ゲームが起動しないか黒い画面だけになるかもしれません。それでも送りますか？'),
    'This phone is below the minimum': ('这台手机低于最低要求', 'このスマホは最低要件を満たしていません'),
    'The game is very unlikely to run well on this computer:': ('这台电脑很可能无法流畅运行游戏：', 'このコンピューターではゲームはほぼまともに動きません：'),
    '{what} anyway?': ('仍然{what}？', 'それでも{what}？'),
    'This computer is below the minimum': ('这台电脑低于最低要求', 'このコンピューターは最低要件を満たしていません'),
    'Not started: this computer is below the minimum': ('未启动：这台电脑低于最低要求', '起動していません：このコンピューターは最低要件を満たしていません'),
    'This computer may struggle': ('这台电脑可能会吃力', 'このコンピューターでは重いかもしれません'),
    'The game runs here, but:': ('游戏能在这里运行，但是：', 'ゲームは動きますが：'),
    'Minimum: 12 GB of memory, a CPU like an 8th-gen Core i5 or Ryzen 5 3600, and a Vulkan 1.3 GPU like a GTX 1050, '
    'RX 560 or Intel UHD. Recommended: 16 GB, an 11th-gen Core i5 / Ryzen 5 5600 or newer, and a GTX 1060 / RX 570 / '
    'Iris Xe / Radeon 680M or newer (the Steam Deck is fine).':
        ('最低要求：12 GB 内存，第 8 代 Core i5 或 Ryzen 5 3600 级别的 CPU，以及 GTX 1050、RX 560 或 Intel UHD 级别的 Vulkan 1.3 '
         'GPU。推荐：16 GB 内存，第 11 代 Core i5 / Ryzen 5 5600 或更新的 CPU，以及 GTX 1060 / RX 570 / Iris Xe / Radeon 680M 或'
         '更新的 GPU（Steam Deck 没问题）。',
         '最低要件：メモリ 12 GB、第 8 世代 Core i5 や Ryzen 5 3600 程度の CPU、GTX 1050、RX 560、Intel UHD 程度の Vulkan 1.3 '
         'GPU。推奨：16 GB、第 11 世代 Core i5 / Ryzen 5 5600 以降、GTX 1060 / RX 570 / Iris Xe / Radeon 680M 以降'
         '（Steam Deck は問題なし）。'),
    'Start the game': ('开始游戏', 'ゲームを開始'),
    'Not built yet: {path} ({hint})': ('尚未构建：{path}（{hint}）', '未ビルド：{path}（{hint}）'),
    'Entry script missing: {path}': ('缺少启动脚本：{path}', '起動スクリプトがありません：{path}'),
    'Headless is on: the game runs without a window (turn it off to play)': ('“无窗口”已开启：游戏会在没有窗口的情况下运行（玩的话请关闭它）',
                                                                              'ヘッドレスがオン：ゲームはウィンドウなしで動きます（遊ぶならオフに）'),
    'Could not start the game: {exc}': ('无法启动游戏：{exc}', 'ゲームを起動できません：{exc}'),
    'Running · log {path}': ('运行中 · 日志 {path}', '実行中 · ログ {path}'),
    'Could not make the bug report: {e}': ('无法生成 bug 报告：{e}', '不具合レポートを作れません：{e}'),
    'Bug report: {path}: drag it into the issue page that opened (no game files)': ('bug 报告：{path}：把它拖进刚打开的 issue 页面（不含游戏文件）',
                                                                                  '不具合レポート：{path}：開いた issue ページにドラッグしてください（ゲームファイルは含みません）'),
    'The game exited': ('游戏已退出', 'ゲームが終了しました'),
    'The game handed off to the next program (code 86)': ('游戏已交给下一个程序（代码 86）', 'ゲームは次のプログラムに引き継ぎました（コード 86）'),
    'Stopped': ('已停止', '停止しました'),
    'The game was killed (signal {signal})': ('游戏被强制结束（信号 {signal}）', 'ゲームが強制終了されました（シグナル {signal}）'),
    'The game exited (code {code})': ('游戏已退出（代码 {code}）', 'ゲームが終了しました（コード {code}）'),
}
