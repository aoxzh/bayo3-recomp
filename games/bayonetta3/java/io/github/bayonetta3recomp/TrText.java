package io.github.bayonetta3recomp;

// The app's translations (Tr.java): {English as in the code, Simplified Chinese, Japanese}. A new Tr.t("...") or
// Tr.f("...") needs its row here (tests/test_launcher_i18n.py checks it, and that the %s placeholders match).
final class TrText {
    private TrText() {}

    static final String[][] ROWS = {
        // ---- home screen
        {"Static recompilation · Android", "静态重编译 · Android", "静的リコンパイル · Android"},
        {"Game data", "游戏数据", "ゲームデータ"},
        {"Shader cache", "着色器缓存", "シェーダーキャッシュ"},
        {"Controller", "手柄", "コントローラー"},
        {"Chip", "芯片", "チップ"},
        {"Temperature", "温度", "温度"},
        {"Report a bug", "提交 bug", "不具合を報告"},
        {"Settings", "设置", "設定"},
        {"PERFORMANCE", "性能", "パフォーマンス"},
        {"Battery saver", "省电", "省電力"},
        {"30 fps · coolest, longest on battery", "30 fps · 最凉快，续航最长", "30 fps · 発熱が最も少なく、電池が長持ち"},
        {"Balanced 40", "平衡 40", "バランス 40"},
        {"40 fps · steady on recent chips", "40 fps · 较新的芯片上很稳", "40 fps · 最近のチップなら安定"},
        {"Unlocked", "不限", "制限なし"},
        {"Up to 60 fps · falls as the phone warms", "最高 60 fps · 手机变热后会下降", "最大 60 fps · 端末が熱くなると下がる"},
        {"▶   START", "▶   开始", "▶   スタート"},
        {"SET UP", "安装", "セットアップ"},
        {"In the game: press Back 3 times (or Guide on a controller) for the menu",
                "游戏中：连按 3 次返回（或手柄的 Guide 键）打开菜单",
                "ゲーム中：戻るを 3 回押す（またはコントローラーのガイドボタン）とメニュー"},
        {"Ⓐ Select    Ⓑ Back    ☰ Start    L1 / R1  Profile    Ⓨ Settings",
                "Ⓐ 选择    Ⓑ 返回    ☰ 开始    L1 / R1  档位    Ⓨ 设置",
                "Ⓐ 決定    Ⓑ 戻る    ☰ スタート    L1 / R1  プロファイル    Ⓨ 設定"},
        {"Add this app to your phone's game mode first", "请先把本应用加入手机的游戏模式",
                "まずこのアプリを端末のゲームモードに追加してください"},
        {"Many phones (vivo, Xiaomi, OPPO, Honor, Samsung and others) slow down apps they do not know "
                + "as games. Add Bayonetta 3 Recomp in your phone's Game Center, Game Booster, Game Space or game mode "
                + "before you play, or it cannot run at full speed.",
                "很多手机（vivo、小米、OPPO、荣耀、三星等）会给不认识的游戏降频。玩之前请在手机的游戏中心、游戏助手、"
                + "游戏空间或游戏模式里添加 Bayonetta 3 Recomp，否则游戏无法全速运行。",
                "多くの端末（vivo、Xiaomi、OPPO、Honor、Samsung など）は、ゲームと認識していないアプリの速度を落とします。"
                + "遊ぶ前に端末のゲームセンター、Game Booster、ゲームスペース、またはゲームモードに Bayonetta 3 Recomp を"
                + "追加してください。追加しないと全速で動きません。"},
        {"Game mode now: %s", "当前游戏模式：%s", "現在のゲームモード：%s"},
        {"Standard", "标准", "標準"},
        {"Performance", "性能", "パフォーマンス"},
        {"Battery", "省电", "省電力"},
        {"Custom", "自定义", "カスタム"},
        {"Done, hide this", "已添加，隐藏", "追加した、閉じる"},
        {"Setting up · %s (tap SET UP to see it)", "正在安装 · %s（点“安装”查看）", "セットアップ中 · %s（セットアップをタップで表示）"},
        {"Not set up yet · SET UP builds the game from your dump here (30-60 min), or send it from a "
                + "computer over USB in minutes (desktop launcher: Send to my phone)",
                "尚未安装 · “安装”会在手机上用你的游戏转储构建游戏（30-60 分钟），也可以用电脑通过 USB 几分钟发过来"
                + "（电脑启动器：发送到我的手机）",
                "未セットアップ · セットアップはダンプからこの端末でゲームを作ります（30〜60 分）。PC から USB で数分で送る"
                + "こともできます（PC ランチャーの「スマホに送る」）"},
        {"None yet", "暂无", "まだなし"},
        {"%s not found.\nCreate it and copy the game data into it: analysis/, extract/romfs/ and spirv/.",
                "找不到 %s。\n请创建它并把游戏数据复制进去：analysis/、extract/romfs/ 和 spirv/。",
                "%s が見つかりません。\n作成して、ゲームデータ（analysis/、extract/romfs/、spirv/）をコピーしてください。"},
        {"Ready · %s", "就绪 · %s", "準備完了 · %s"},
        {"Missing in %s:", "%s 中缺少：", "%s に足りないもの："},
        {"made from the game's ExeFS by the setup tools", "由安装工具从游戏的 ExeFS 生成",
                "セットアップツールがゲームの ExeFS から作るもの"},
        {"the game's RomFS, extracted from your dump", "游戏的 RomFS，从你的转储中提取", "ゲームの RomFS（ダンプから抽出）"},
        {"the translated shaders: without them nothing is drawn", "转换后的着色器：没有它们画面什么都不显示",
                "変換済みシェーダー：これがないと何も描画されません"},
        {"%s · warm, fewer stutters", "%s · 已预热，卡顿更少", "%s · 準備済み、カクつき少なめ"},
        {"Empty · first scenes stutter while it fills", "空 · 刚开始的场景会在填充时卡顿", "空 · 最初の場面は貯まるまでカクつきます"},
        {"None · touch controls", "无 · 触屏按键", "なし · タッチ操作"},
        {"None · touch controls are off in Settings", "无 · 触屏按键已在设置中关闭", "なし · タッチ操作は設定でオフ"},
        {"Below the minimum: %s", "低于最低要求：%s", "最低要件未満：%s"},
        {"May struggle: %s", "可能吃力：%s", "動作が重いかも：%s"},
        {"Cool", "凉爽", "低温"},
        {"Light · fine", "微热 · 正常", "やや温かい · 問題なし"},
        {"Warm · clocks may drop", "温热 · 可能降频", "温かい · クロックが下がるかも"},
        {"Hot · expect a lower frame rate", "很热 · 帧率会下降", "熱い · フレームレートが下がります"},
        {"Critical · let it cool", "过热 · 请让它降温", "危険 · 冷ましてください"},
        {"Emergency", "紧急", "緊急"},
        {"Shutting down", "即将关机", "シャットダウン中"},
        {"Unknown", "未知", "不明"},
        {"This phone is below the minimum", "这台手机低于最低要求", "この端末は最低要件を満たしていません"},
        {"Close", "关闭", "閉じる"},
        {"Try anyway", "仍然尝试", "それでも試す"},
        {"The game data is incomplete: the list on the left says what is missing.", "游戏数据不完整：左边的列表写了缺什么。",
                "ゲームデータが不完全です。左の一覧に足りないものがあります。"},
        {"Could not write config.txt: %s", "无法写入 config.txt：%s", "config.txt を書き込めません：%s"},

        // ---- first run
        {"Allow access to the game folder", "允许访问游戏文件夹", "ゲームフォルダーへのアクセスを許可"},
        {"The game runs from Internal storage/%s: the files you made from your own copy of the game. Android lets "
                + "an app read a folder there only with “All files access”.\n\nThe app uses it for that folder alone: "
                + "the game data, your saves, the shader cache and the log. Turn it on for this app on the next page, "
                + "then come back.",
                "游戏从“内部存储/%s”运行：那里是用你自己的游戏副本制作的文件。Android 只允许拥有“所有文件访问权限”的应用"
                + "读取那里的文件夹。\n\n本应用只用它访问那一个文件夹：游戏数据、存档、着色器缓存和日志。请在下一页为本应用打开它，"
                + "然后回来。",
                "ゲームは「内部ストレージ/%s」から動きます。自分のゲームから作ったファイルです。Android では「すべての"
                + "ファイルへのアクセス」がないアプリはそこを読めません。\n\nこのアプリはそのフォルダーだけに使います：ゲームデータ、"
                + "セーブ、シェーダーキャッシュ、ログ。次のページでこのアプリに許可して、戻ってきてください。"},
        {"Check again", "重新检查", "もう一度確認"},
        {"Allow all files access", "允许访问所有文件", "すべてのファイルへのアクセスを許可"},

        // ---- settings panel
        {"Done", "完成", "完了"},
        {"Language", "语言", "言語"},
        {"The game's text and voices", "游戏的文字和语音", "ゲームの文字と音声"},
        {"System language", "跟随系统", "システムの言語"},
        {"Xbox button layout", "Xbox 按键布局", "Xbox ボタン配置"},
        {"Menu button", "菜单按键", "メニューボタン"},
        {"Follow @aoxzh", "关注 @aoxzh", "@aoxzh をフォロー"},
        {"Follow the author", "关注作者", "作者をフォロー"},
        {"@aoxzh on X: news, previews and releases", "X 上的 @aoxzh：动态、预览和新版本", "X の @aoxzh：お知らせ、プレビュー、リリース"},
        {"Check for updates", "检查更新", "更新を確認"},
        {"When the launcher opens, ask GitHub whether a newer version is out. Nothing is downloaded by itself",
                "启动器打开时向 GitHub 查询是否有新版本。不会自动下载任何东西",
                "ランチャーを開いたときに GitHub で新しいバージョンがあるか確認します。自動でダウンロードはしません"},
        {"Update available: %s", "有新版本：%s", "新しいバージョンがあります：%s"},
        {"Download the new APK and install it over this app, then tap SET UP again. Your game files and saves stay.",
                "下载新的 APK 覆盖安装到这个应用上，然后再点一次 SET UP。游戏文件和存档都会保留。",
                "新しい APK をダウンロードしてこのアプリに上書きインストールし、もう一度 SET UP をタップしてください。ゲームファイルとセーブはそのまま残ります。"},
        {"Later", "稍后", "あとで"},
        {"Open the release page", "打开发布页面", "リリースページを開く"},
        {"Opens the in-game menu. Guide and Back x3 always work too", "打开游戏内菜单。Guide 键和连按三次返回始终可用",
                "ゲーム内メニューを開きます。Guide と戻る 3 回はいつでも使えます"},
        {"Guide or Back x3", "Guide 或返回 ×3", "Guide か戻る ×3"},
        {"Select + Start", "Select + Start", "Select + Start"},
        {"L3 + R3", "L3 + R3", "L3 + R3"},
        {"On: the button marked A is the game's A. Off: by position, as on Switch",
                "开：标着 A 的键就是游戏里的 A。关：按位置，和 Switch 一样",
                "オン：A と書かれたボタンがゲームの A。オフ：Switch と同じく位置で対応"},
        {"On", "开", "オン"},
        {"Off", "关", "オフ"},
        {"Mute", "静音", "ミュート"},
        {"No sound at all", "完全没有声音", "音を一切出さない"},
        {"TOUCH CONTROLS", "触屏按键", "タッチ操作"},
        {"Touch controls", "触屏按键", "タッチ操作"},
        {"Drawn over the game", "显示在游戏画面上", "ゲーム画面の上に表示"},
        {"Hide with a pad", "有手柄时隐藏", "コントローラー使用中は隠す"},
        {"Always", "始终显示", "常に表示"},
        {"Opacity", "不透明度", "不透明度"},
        {"Size", "大小", "サイズ"},
        {"STORAGE", "存储", "ストレージ"},
        {"Game folder", "游戏文件夹", "ゲームフォルダー"},
        {"Where the game data, your saves and the shader cache are", "游戏数据、存档和着色器缓存所在的位置",
                "ゲームデータ、セーブ、シェーダーキャッシュの場所"},
        {"Where the game data, your saves and the shader cache are: Internal storage or %s",
                "游戏数据、存档和着色器缓存所在的位置：内部存储或 %s",
                "ゲームデータ、セーブ、シェーダーキャッシュの場所：内部ストレージまたは %s"},
        {"Where the game data, your saves and the shader cache are. An SD card shows up here once inserted",
                "游戏数据、存档和着色器缓存所在的位置。插入 SD 卡后会显示在这里",
                "ゲームデータ、セーブ、シェーダーキャッシュの場所。SD カードを入れるとここに表示されます"},
        {"App folder", "应用文件夹", "アプリのフォルダー"},
        {"SUPPORT", "支持", "サポート"},
        {"Share a bug report", "分享 bug 报告", "不具合レポートを共有"},
        {"A zip of nx.log, config.txt and the chip info, through the share sheet",
                "把 nx.log、config.txt 和芯片信息打包成 zip，通过分享菜单发送",
                "nx.log、config.txt、チップ情報の zip を共有メニューで送る"},
        {"Advanced", "高级", "詳細設定"},
        {"NX_KEY=value, one per line", "NX_KEY=值，每行一个", "NX_KEY=値、1 行に 1 つ"},
        {"Data folder: %s\nOther lines of config.txt, kept as they are. A line for a setting above is replaced by "
                + "that setting.",
                "数据文件夹：%s\nconfig.txt 的其他行，原样保留。属于上面某个设置的行会被那个设置替换。",
                "データフォルダー：%s\nconfig.txt のその他の行（そのまま残ります）。上の設定に当たる行はその設定で置き換えられます。"},

        // ---- game folder (Storage.java)
        {"Internal storage", "内部存储", "内部ストレージ"},
        {"SD card", "SD 卡", "SD カード"},
        {"The setup is running: change the game folder after it.", "安装正在进行：请在完成后再更改游戏文件夹。",
                "セットアップ中です。終わってからゲームフォルダーを変更してください。"},
        {"No SD card found: insert one (exFAT) and come back.", "没有找到 SD 卡：请插入一张（exFAT）后再回来。",
                "SD カードが見つかりません。exFAT のカードを入れてから戻ってください。"},
        {"(now)", "（当前）", "（現在）"},
        {"%s free", "可用 %s", "空き %s"},
        {"holds the game", "已有游戏", "ゲームあり"},
        {"Cancel", "取消", "キャンセル"},
        {"Use the game on %s?", "使用 %s 上的游戏？", "%s のゲームを使いますか？"},
        {"%s/%s already holds the game data: the game runs from there, with the saves and settings in it. %s stays "
                + "as it is.",
                "%s/%s 里已经有游戏数据：游戏会从那里运行，使用那里的存档和设置。%s 保持不变。",
                "%s/%s にはすでにゲームデータがあります。ゲームはそこから、そこにあるセーブと設定で動きます。%s はそのままです。"},
        {"Use it", "使用", "使う"},
        {"Move the game to %s?", "把游戏移到 %s？", "ゲームを %s に移動しますか？"},
        {"The game folder is %s and %s has %s free. Free some space there first.",
                "游戏文件夹有 %s，而 %s 只剩 %s。请先在那里腾出空间。",
                "ゲームフォルダーは %s、%s の空きは %s です。先に空きを作ってください。"},
        {"This copies the game folder (%s: the game data, your saves, settings and shader cache) to %s/%s, then "
                + "removes it from %s. About %d minutes on a fast card; if it stops half way, the game stays where it "
                + "is. Keep the phone charging.",
                "这会把游戏文件夹（%s：游戏数据、存档、设置和着色器缓存）复制到 %s/%s，然后从 %s 删除。高速卡上大约 %d 分钟；"
                + "如果中途停止，游戏仍留在原处。请保持手机充电。",
                "ゲームフォルダー（%s：ゲームデータ、セーブ、設定、シェーダーキャッシュ）を %s/%s にコピーしてから、%s から"
                + "削除します。速いカードで約 %d 分。途中で止まってもゲームは元の場所に残ります。充電しながら行ってください。"},
        {"Move", "移动", "移動"},
        {"SET UP puts the game on %s.", "“安装”会把游戏放到 %s。", "セットアップでゲームを %s に入れます。"},
        {"The game folder is now on %s.", "游戏文件夹现在在 %s。", "ゲームフォルダーは %s になりました。"},
        {"Could not change the game folder: %s", "无法更改游戏文件夹：%s", "ゲームフォルダーを変更できません：%s"},
        {"Starting", "正在开始", "開始中"},
        {"Moving the game to %s", "正在把游戏移到 %s", "ゲームを %s に移動中"},
        {"Stop", "停止", "停止"},
        {"%s of %s", "%s / %s", "%s / %s"},
        {"The game is now on %s.", "游戏现在在 %s。", "ゲームは %s に移りました。"},
        {"Not moved: %s. The game is still in %s.", "没有移动：%s。游戏仍在 %s。", "移動しませんでした：%s。ゲームは %s のままです。"},

        // ---- device check
        {"Minimum: Vulkan 1.3, a CPU core at 2.4 GHz or faster, 6 GB of memory and an Adreno 640 / Mali-G68 class "
                + "GPU or newer. Recommended: Snapdragon 8 Gen 1 or newer with 8 GB.",
                "最低要求：Vulkan 1.3、至少一个 2.4 GHz 以上的 CPU 核心、6 GB 内存、Adreno 640 / Mali-G68 级别或更新的 GPU。"
                + "推荐：骁龙 8 Gen 1 或更新，8 GB 内存。",
                "最低要件：Vulkan 1.3、2.4 GHz 以上の CPU コア、メモリ 6 GB、Adreno 640 / Mali-G68 クラス以上の GPU。"
                + "推奨：Snapdragon 8 Gen 1 以降、メモリ 8 GB。"},
        {"No Vulkan 1.3 driver (the game draws with Vulkan 1.3)", "没有 Vulkan 1.3 驱动（游戏用 Vulkan 1.3 绘制）",
                "Vulkan 1.3 ドライバーがありません（ゲームは Vulkan 1.3 で描画します）"},
        {"%.1f GB of memory (6 GB at least)", "%.1f GB 内存（至少需要 6 GB）", "メモリ %.1f GB（6 GB 以上必要）"},
        {"%.1f GB of memory: the game may swap and stutter (8 GB recommended)",
                "%.1f GB 内存：游戏可能会用到交换内存而卡顿（推荐 8 GB）",
                "メモリ %.1f GB：スワップしてカクつくかも（8 GB 推奨）"},
        {"Fastest CPU core %.1f GHz (2.4 GHz at least)", "最快的 CPU 核心 %.1f GHz（至少需要 2.4 GHz）",
                "最速の CPU コア %.1f GHz（2.4 GHz 以上必要）"},
        {"Fastest CPU core %.1f GHz: expect under 30 fps in fights", "最快的 CPU 核心 %.1f GHz：战斗中可能低于 30 fps",
                "最速の CPU コア %.1f GHz：戦闘で 30 fps を下回りそう"},
        {"%.1f GB free in Internal storage (the setup needs about %.0f GB)",
                "内部存储剩余 %.1f GB（安装大约需要 %.0f GB）", "内部ストレージの空き %.1f GB（セットアップに約 %.0f GB 必要）"},
        {"%.1f GB free in Internal storage (the setup needs about %.0f GB, with the dump copied in from the other "
                + "storage)",
                "内部存储剩余 %.1f GB（安装大约需要 %.0f GB，包括从另一个存储复制过来的转储）",
                "内部ストレージの空き %.1f GB（セットアップに約 %.0f GB 必要。もう一方のストレージからダンプをコピーする分を含む）"},
        {"%.1f GB free on %s (the game folder there needs about %.0f GB)",
                "%2$s 剩余 %1$.1f GB（那里的游戏文件夹大约需要 %3$.0f GB）",
                "%2$s の空き %1$.1f GB（そこのゲームフォルダーに約 %3$.0f GB 必要）"},
        {"%.1f GB free on %s (the game folder there needs about %.0f GB, with the dump copied in from Internal "
                + "storage)",
                "%2$s 剩余 %1$.1f GB（那里的游戏文件夹大约需要 %3$.0f GB，包括从内部存储复制过来的转储）",
                "%2$s の空き %1$.1f GB（そこのゲームフォルダーに約 %3$.0f GB 必要。内部ストレージからダンプをコピーする分を含む）"},
        {"(Adreno 640 or newer)", "（需要 Adreno 640 或更新）", "（Adreno 640 以降が必要）"},
        {": a mid-range GPU, expect a low frame rate", "：中端 GPU，帧率会偏低", "：ミドルレンジの GPU、フレームレートは低め"},
        {"(its driver could not build the game's shaders on a Mali-G57)", "（它的驱动在 Mali-G57 上无法构建游戏的着色器）",
                "（Mali-G57 ではドライバーがゲームのシェーダーを作れませんでした）"},
        {": not tested yet", "：尚未测试", "：未テスト"},
        {"(too old for the game's shaders)", "（太旧，无法运行游戏的着色器）", "（古すぎてゲームのシェーダーを扱えません）"},
        {"The game is very unlikely to run on this phone:", "这台手机很可能无法运行游戏：", "この端末ではゲームはほぼ動きません："},
        {"A setup takes 30 to 60 minutes and about 6 GB, so it is better not to start one here. You can still try: "
                + "the game may not start, or may show only a black screen.",
                "安装需要 30 到 60 分钟和大约 6 GB 空间，所以最好不要在这台手机上开始。你仍然可以尝试：游戏可能无法启动，"
                + "或者只显示黑屏。",
                "セットアップには 30〜60 分と約 6 GB が必要なので、この端末では始めないほうがよいでしょう。試すこともできますが、"
                + "ゲームが起動しないか、黒い画面だけになるかもしれません。"},

        // ---- setup (SetupActivity, SetupService, Setup)
        {"Set up the game on this phone", "在这台手机上安装游戏", "この端末でゲームをセットアップ"},
        {"Back", "返回", "戻る"},
        {"Start setup", "开始安装", "セットアップ開始"},
        {"Copy the exefs and romfs folders you extracted from your own Bayonetta 3 (update 1.2.0) with your emulator "
                + "to the phone: %1$s/dump, or anywhere in Download or Documents, on %2$s (the setup finds them; "
                + "Settings > Game folder picks Internal storage or an SD card). The setup moves them into the game's "
                + "folder on %2$s (no copy when they are on the same storage), translates the game's code and shaders "
                + "and compiles them here: about 30 to 60 minutes and 6 GB of free space in Internal storage. Keep the "
                + "phone charging. You can leave this screen: the setup goes on in the background, its progress in the "
                + "notification; if it stops, Start setup carries on where it left off. One small file is downloaded "
                + "(the Kraken decoder's source, which may not ship in the APK); nothing leaves the phone.\n\nFaster: a "
                + "computer that has set the game up builds it for the phone in a few minutes and copies it over USB "
                + "(the desktop launcher's Send to my phone).",
                "把你用模拟器从自己的 Bayonetta 3（1.2.0 更新）提取出的 exefs 和 romfs 文件夹复制到手机：%2$s 上的 "
                + "%1$s/dump，或 Download、Documents 里的任意位置（安装会自己找到；“设置 > 游戏文件夹”可以选择内部存储或 "
                + "SD 卡）。安装会把它们移到 %2$s 上的游戏文件夹（在同一存储上不会复制），转换游戏的代码和着色器并在手机上编译："
                + "大约 30 到 60 分钟，内部存储需要 6 GB 可用空间。请保持手机充电。你可以离开这个页面：安装会在后台继续，"
                + "进度显示在通知里；如果停止了，再点“开始安装”会从中断处继续。只会下载一个小文件（Kraken 解码器的源码，"
                + "它不能放进 APK）；不会上传任何东西。\n\n更快的方法：已经装好游戏的电脑几分钟就能为手机构建好，并通过 USB "
                + "复制过来（电脑启动器的“发送到我的手机”）。",
                "自分の Bayonetta 3（更新 1.2.0）からエミュレーターで抽出した exefs と romfs のフォルダーを端末にコピーしてください："
                + "%2$s の %1$s/dump、または Download や Documents のどこか（セットアップが見つけます。「設定 > ゲームフォルダー」"
                + "で内部ストレージか SD カードを選べます）。セットアップはそれを %2$s のゲームフォルダーに移し（同じストレージなら"
                + "コピーなし）、ゲームのコードとシェーダーを変換してこの端末でコンパイルします。約 30〜60 分、内部ストレージに "
                + "6 GB の空きが必要です。充電しながら行ってください。この画面を離れてもセットアップはバックグラウンドで続き、"
                + "進み具合は通知に出ます。止まった場合は「セットアップ開始」で続きから再開します。ダウンロードするのは小さな"
                + "ファイル 1 つだけ（APK に入れられない Kraken デコーダーのソース）で、端末から外には何も送りません。\n\n"
                + "もっと速く：ゲームをセットアップ済みの PC なら数分で端末用に作って USB でコピーできます（PC ランチャーの"
                + "「スマホに送る」）。"},
        {"Ready to start · game files found in %s", "可以开始 · 在 %s 找到了游戏文件", "開始できます · ゲームファイルを %s で発見"},
        {"Not yet: %s", "还不行：%s", "まだです：%s"},
        {"Done: the game is ready", "完成：游戏已就绪", "完了：ゲームの準備ができました"},
        {"Back to the launcher", "回到启动器", "ランチャーに戻る"},
        {"Stopped", "已停止", "停止しました"},
        {"Game setup", "游戏安装", "ゲームのセットアップ"},
        {"The game is ready", "游戏已就绪", "ゲームの準備ができました"},
        {"Setup stopped", "安装已停止", "セットアップが止まりました"},
        {"Open Bayonetta 3 Recomp and tap START.", "打开 Bayonetta 3 Recomp 并点“开始”。",
                "Bayonetta 3 Recomp を開いてスタートをタップしてください。"},
        {"Open the setup to carry on.", "打开安装页面继续。", "セットアップを開いて続けてください。"},
        {"Setting up the game", "正在安装游戏", "ゲームをセットアップ中"},
        {"no extracted game found. Extract the ExeFS and the RomFS of your Bayonetta 3 (update 1.2.0 installed) "
                + "with your emulator (Ryujinx: right-click > Extract Data > ExeFS, then RomFS; Eden / yuzu: Dump "
                + "RomFS > Full, and Dump ExeFS) and copy the folder with exefs and romfs to the phone, for example "
                + "into %s/dump or Download on the storage the game goes to (%s).",
                "没有找到提取好的游戏。请用模拟器提取你的 Bayonetta 3（已装 1.2.0 更新）的 ExeFS 和 RomFS（Ryujinx：右键 > "
                + "Extract Data > ExeFS，然后 RomFS；Eden / yuzu：Dump RomFS > Full，以及 Dump ExeFS），再把包含 exefs 和 "
                + "romfs 的文件夹复制到手机，例如游戏所在存储（%2$s）上的 %1$s/dump 或 Download。",
                "抽出済みのゲームが見つかりません。エミュレーターで Bayonetta 3（更新 1.2.0 適用済み）の ExeFS と RomFS を"
                + "抽出し（Ryujinx：右クリック > Extract Data > ExeFS、次に RomFS。Eden / yuzu：Dump RomFS > Full と "
                + "Dump ExeFS）、exefs と romfs の入ったフォルダーを端末にコピーしてください。例：ゲームを入れるストレージ"
                + "（%2$s）の %1$s/dump や Download。"},

        // ---- in-game menu (GameActivity)
        {"Paused", "已暂停", "一時停止中"},
        {"Ⓐ Select    Ⓑ Resume    ‹ › Change", "Ⓐ 选择    Ⓑ 继续    ‹ › 更改", "Ⓐ 決定    Ⓑ 再開    ‹ › 変更"},
        {"Resume", "继续游戏", "ゲームに戻る"},
        {"Frame rate", "帧率", "フレームレート"},
        {"Battery saver 30", "省电 30", "省電力 30"},
        {"Unlocked 60", "不限 60", "制限なし 60"},
        {"Restart to apply: quit to the launcher and start again", "重启后生效：退回启动器后重新开始",
                "再起動後に反映：ランチャーに戻ってもう一度スタート"},
        {"Quit to launcher", "退回启动器", "ランチャーに戻る"},
        {"Quit to the launcher?", "退回启动器？", "ランチャーに戻りますか？"},
        {"The game closes. Progress since your last save is lost.", "游戏会关闭。上次存档之后的进度会丢失。",
                "ゲームを終了します。最後のセーブ以降の進行は失われます。"},
        {"Quit", "退出", "終了"},
        {"Not saved: %s", "未保存：%s", "保存できません：%s"},

        // ---- bug report (Report.java)
        {"Preparing the report…", "正在准备报告…", "レポートを準備中…"},
        {"Could not make the report: %s", "无法生成报告：%s", "レポートを作れません：%s"},
        {"Saved %s in Downloads: attach it on the page that opens", "已把 %s 保存到“下载”：请在打开的页面上附上它",
                "%s を Download に保存しました。開くページで添付してください"},
        {"Send the bug report", "发送 bug 报告", "不具合レポートを送る"},
        {"No browser found: open github.com/aoxzh/bayo3-recomp/issues", "没有找到浏览器：请打开 github.com/aoxzh/bayo3-recomp/issues",
                "ブラウザーが見つかりません：github.com/aoxzh/bayo3-recomp/issues を開いてください"},
    };
}
