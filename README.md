<p align="center">
  <img src="docs/images/logo.png" alt="CTR Nitro-Fueled PC" width="320">
</p>

<h1 align="center">CTR Nitro-Fueled PC — Online Edition</h1>

<p align="center">
  <b>Crash Team Racing Nitro-Fueled (PS4) running natively on Windows, with online play and no central server.</b><br>
  <a href="#english">English</a> · <a href="#العربية">العربية</a> ·
  <a href="../../releases/latest"><b>Download / تحميل</b></a>
</p>

<p align="center">
  <img src="docs/images/launcher_en_home.jpg" alt="Launcher, Home" width="49%">
  <img src="docs/images/launcher_ar_friends.jpg" alt="الواجهة بالعربية، الأصدقاء" width="49%">
</p>

> **No game files are included.** You need your own legally obtained PS4 dump of the game and its
> 1.21 update. Not affiliated with or endorsed by Sony Interactive Entertainment, Activision or
> Beenox. *Crash Team Racing*, *Nitro-Fueled* and *PlayStation* are trademarks of their owners.

---

<a id="english"></a>
## English

### What this is

This project joins two existing open-source ports of the game, giving full credit to both:

| Project | Author | What it brings here |
|---|---|---|
| [**CTR-NitroFueled-PC**](https://github.com/GetLostIn/CTR-NitroFueled-PC) | [**GetLostIn**](https://github.com/GetLostIn) | The base: the native runtime, the Vulkan renderer, the performance, DLSS / FSR 1 / FSR 3 / FSR 4, frame generation, ultrawide, controllers, patches, saves, DLC, trophies and every launcher feature. |
| [**CTRNativePorted**](https://github.com/guzuold/CTRNativePorted) | [**guzuold**](https://github.com/guzuold) | Online play without a central server: the local online service, friends and friend codes, invitations and JOIN, public matchmaking, the Pit Stop economy, and the "Nitro Cinema" launcher design this launcher follows. |

The online code runs on CTR-NitroFueled-PC's runtime and renderer, which are unchanged, so the
game keeps that port's performance. With online play off, nothing of it is even loaded.

### Features

**Performance and graphics** (from CTR-NitroFueled-PC)
- The game's own code runs natively on the CPU; the GPU work goes to Vulkan (shadPS4's renderer).
- Internal resolution from 480p to native 4K; 60 FPS patch, FPS limit up to 240.
- Upscaling: AMD FSR 1, AMD FSR 3.1, AMD FSR 4 (experimental) and NVIDIA DLSS (RTX).
- AMD FSR 3 frame generation (experimental); 21:9 and 32:9 ultrawide with centered HUD.
- Xbox / PlayStation / SDL controllers and keyboard, live controller test, button remapping, split screen.
- Community patches, save backups and imports, optional DLC, the 48 trophies.

**Online** (from CTRNativePorted)
- Each player's launcher runs its own online service; services find each other on the BitTorrent DHT.
- Add friends by code, see their presence, invite them or JOIN them from the game's own friends list.
- Public matchmaking (*Online > Matchmaking*); the hub is elected automatically.
- Race traffic goes directly between the players' games.
- **Compatible with CTRNativePorted players**: same friend codes, friends and matchmaking.
- Sony and Activision servers are never contacted.

**This edition**
- A launcher in the "Nitro Cinema" style: tabs, the Home page with artwork and the big PLAY button,
  the friends drawer, cards for the settings.
- **Arabic and English** interface (technical terms stay in English); Arabic is shaped and laid out
  right to left, with the Lemonada font for titles.
- Replaceable artwork in `assets\launcher`.

### Download and play

1. Download `CTR-NitroFueled-PC-Online-v1.0.2-windows.zip` from
   [Releases](../../releases/latest) and unzip it anywhere.
2. Open **`CTR-NitroFueled-PC.exe`**. In **ADVANCED**, choose your base game folder
   (CUSA14876 Europe or CUSA13795 USA) and your 1.21 update folder, then **Prepare the game**.
3. Press **PLAY**.

**Online:** SETTINGS → *ONLINE & MULTI* → turn on **Multiplayer Online** and choose a player name.
Open **FRIENDS** (top right), **COPY MY CODE**, send it to a friend and add theirs: you must add each
other. Then invite or join from the game. Details: [ONLINE.md](packaging/windows/portable/ONLINE.md).

Requirements: Windows 10/11 64-bit, a Vulkan 1.3 GPU, your own decrypted dump with the 1.21 update.
Only one online launcher per PC at a time (CTRNativePorted uses the same ports).

### Building from source

MSYS2 CLANG64, as in [packaging/windows/README.md](packaging/windows/README.md):

```bash
git clone --recursive <this repository>
bash build.sh
python packaging/windows/make_portable.py --version v1.0.2 --name CTR-NitroFueled-PC-Online
```

What this edition adds to the CTR-NitroFueled-PC tree:
- `online/` — CTRNativePorted's online service (`online_server/`), guest functions (`hle/`, built as
  `ctr_online.dll`) and DHT (`dht/`), with the changes needed to build them with clang/MinGW.
- `src/runtime_online.c` — binds those functions, only when online is on, and installs the online
  service's traffic key in memory.
- `launcher/windows/` — the new launcher (`online.cpp`, `arabic.cpp`, the Nitro look in `launcher.cpp`).
- `gpu/patches/imgui/` — an optional text-shaping hook for Arabic (unused by the in-game overlay).
- `tools/arabic_presentation_forms.py` — prepares the Arabic font.

The original CTR-NitroFueled-PC README is kept in [docs/UPSTREAM_README.md](docs/UPSTREAM_README.md).

---

<a id="العربية"></a>
<div dir="rtl">

## العربية

### ما هذا المشروع؟

هذا المشروع يجمع بين مشروعين مفتوحَي المصدر للعبة، مع حفظ حقوق صاحبَيهما بالكامل:

| المشروع | صاحبه | ما أُخذ منه |
|---|---|---|
| [**CTR-NitroFueled-PC**](https://github.com/GetLostIn/CTR-NitroFueled-PC) | [**GetLostIn**](https://github.com/GetLostIn) | الأساس كله: تشغيل اللعبة بشكل أصلي، والرندر بـ Vulkan، والأداء الممتاز، و DLSS و FSR 1 و FSR 3 و FSR 4، وتوليد الإطارات، و UltraWide، وأيادي التحكم، والباتشات، والحفظ، و DLC، والكؤوس، وكل ميزات المشغل. |
| [**CTRNativePorted**](https://github.com/guzuold/CTRNativePorted) | [**guzuold**](https://github.com/guzuold) | اللعب الأونلاين بدون سيرفر مركزي: خدمة الأونلاين المحلية، والأصدقاء وأكوادهم، والدعوات والانضمام (JOIN)، والـ Matchmaking العام، ومتجر Pit Stop، وتصميم واجهة "Nitro Cinema" التي تتبعها هذه الواجهة. |

كود الأونلاين يعمل فوق محرك CTR-NitroFueled-PC دون تغيير المحرك، لذلك تحتفظ اللعبة بنفس الأداء.
وعند إيقاف الأونلاين لا يُحمَّل كود الأونلاين أصلاً.

### المزايا

**الأداء والرسوميات** (من CTR-NitroFueled-PC)
- كود اللعبة الأصلي يعمل مباشرة على المعالج، والرسوميات عبر Vulkan (رندر shadPS4).
- دقة داخلية من 480p حتى 4K، وباتش 60 FPS، وحد FPS حتى 240.
- رفع الدقة: AMD FSR 1 و AMD FSR 3.1 و AMD FSR 4 (تجريبي) و NVIDIA DLSS (لكروت RTX).
- توليد الإطارات AMD FSR 3 (تجريبي)، و UltraWide بنسبة 21:9 و 32:9 مع HUD في المنتصف.
- أيادي Xbox و PlayStation و SDL ولوحة المفاتيح، مع تجربة اليد مباشرة وتغيير الأزرار والشاشة المقسومة.
- باتشات المجتمع، ونسخ الحفظ احتياطياً واستيراده، و DLC اختياري، والكؤوس الـ 48.

**الأونلاين** (من CTRNativePorted)
- كل لاعب يشغّل خدمة أونلاين خاصة به داخل المشغل، والخدمات تجد بعضها عبر شبكة BitTorrent DHT.
- أضف أصدقاءك بالكود، وشاهد حالتهم، وادعُهم أو انضم إليهم من قائمة الأصدقاء داخل اللعبة.
- Matchmaking عام (*Online > Matchmaking*)، والمضيف يُختار تلقائياً.
- بيانات السباق تنتقل مباشرة بين أجهزة اللاعبين.
- **متوافق مع لاعبي CTRNativePorted**: نفس الأكواد والأصدقاء والـ Matchmaking.
- لا يتم الاتصال أبداً بسيرفرات Sony أو Activision.

**هذه النسخة**
- واجهة بأسلوب "Nitro Cinema": تبويبات، وصفحة رئيسية بخلفية وزر "العب" كبير، ودرج الأصدقاء، وبطاقات للإعدادات.
- **واجهة بالعربية والإنجليزية** (المصطلحات التقنية تبقى بالإنجليزية)، مع تشكيل الحروف والكتابة من اليمين لليسار وخط Lemonada للعناوين.
- الصور قابلة للتغيير من مجلد `assets\launcher`.

### التحميل والتشغيل

1. حمّل الملف `CTR-NitroFueled-PC-Online-v1.0.2-windows.zip` من صفحة [Releases](../../releases/latest) وفك ضغطه في أي مكان.
2. شغّل **`CTR-NitroFueled-PC.exe`**، ومن تبويب **متقدم** اختر مجلد اللعبة الأساسية (CUSA14876 الأوروبية أو CUSA13795 الأمريكية) ومجلد تحديث 1.21، ثم اضغط **تجهيز اللعبة**.
3. اضغط **العب**.

**الأونلاين:** الإعدادات ← بطاقة *الأونلاين واللعب الجماعي* ← فعّل **اللعب الأونلاين** واكتب اسم اللاعب.
افتح **الأصدقاء** (أعلى اليمين) ← **نسخ كودي** وأرسله لصديقك وأضف كوده: يجب أن يضيف كل منكما الآخر.
بعدها ادعُه أو انضم إليه من داخل اللعبة. التفاصيل في [README-AR.txt](packaging/windows/portable/README-AR.txt).

المتطلبات: ويندوز 10 أو 11 (64 بت)، وكرت شاشة يدعم Vulkan 1.3، ونسختك الخاصة من اللعبة مع تحديث 1.21.
لا تشغّل مشغّلَين أونلاين في نفس الوقت على نفس الجهاز (CTRNativePorted يستخدم نفس المنافذ).

</div>

---

## Credits / الشكر والحقوق

This project exists thanks to / هذا المشروع موجود بفضل:

- **[GetLostIn](https://github.com/GetLostIn)** — [CTR-NitroFueled-PC](https://github.com/GetLostIn/CTR-NitroFueled-PC), the Windows port this project is built on (made with Claude, Anthropic).
- **[guzuold](https://github.com/guzuold)** — [CTRNativePorted](https://github.com/guzuold/CTRNativePorted): the serverless online play and the Nitro Cinema launcher design (implemented and reviewed with Claude, OpenAI Codex and Gemini).
- **[T3CHNOLOG1C](https://github.com/T3CHNOLOG1C)** — [SacredFire](https://github.com/T3CHNOLOG1C/SacredFire), the native CTR Nitro-Fueled port CTR-NitroFueled-PC forks.
- **[deadinside28](https://github.com/deadinside28)** — [bbport](https://github.com/deadinside28/bloodborne_pc); **[Supermedo](https://github.com/Supermedo)** — its Windows port.
- **[shadPS4](https://github.com/shadps4-emu/shadPS4)** — the video core and shader recompiler.
- **[IFreemz](https://github.com/IFreemz)** — the DLSS bridge; **[FireBurn/FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan)** — AMD FSR 3.1 / 4 on Vulkan.
- **illusion** and **TroyWarez** — the game patches; **tcosi** — ultrawide; **martijnhe** — the European edition; **MetaHeart**, **Naif Alqurashi**.
- **[jech/dht](https://github.com/jech/dht)** (MIT) — the BitTorrent DHT.
- Fonts: **Luckiest Guy** by Astigmatic (Apache-2.0), **Lemonada** by the Lemonada Project Authors (OFL-1.1, modified as "Lemonada CTR").
- NVIDIA DLSS SDK (`nvngx_dlss.dll`, redistributed under its license). NVIDIA, DLSS and GeForce RTX are trademarks of NVIDIA Corporation.
- The launcher's background and logo are official *Crash Team Racing Nitro-Fueled* artwork © Activision Publishing, Inc., used only to identify the game; they are not covered by this project's license.

## License / الرخصة

GNU General Public License **version 2** ([LICENSE](LICENSE)). CTR-NitroFueled-PC and its sources are
GPL-2.0-or-later; CTRNativePorted's code in `online/` is GPL-2.0-only
([online/LICENSE.CTRNativePorted](online/LICENSE.CTRNativePorted)), so the combined program is
distributed under GPL-2.0. Third-party licenses are in the package's `licenses\` folder and
its `THIRD-PARTY.txt`.

الرخصة: GPL الإصدار 2. الكود الأصلي لكل مشروع يبقى تحت رخصته، وحقوق كل جزء محفوظة لأصحابه المذكورين أعلاه.
