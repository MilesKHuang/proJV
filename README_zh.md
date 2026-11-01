# proJV

<p align="center">
  <img src="assets/social-preview.png" width="780" alt="proJV锛氬師鐢?C++ 缁堢 AI agent"/>
</p>

**绠€浣撲腑鏂?* | [English](README.md)

`proJV = Project Just Vibing` 鈥斺€?鐢ㄥ師鐢?C++ 鍐欑殑缁堢 AI agent锛屽熀浜?DeepSeek銆?

![CI](https://img.shields.io/github/actions/workflow/status/MilesKHuang/proJV/ci.yml?branch=main&label=CI)
![Release](https://img.shields.io/github/v/release/MilesKHuang/proJV)
![License](https://img.shields.io/github/license/MilesKHuang/proJV)
![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-lightgrey)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)
![Binary](https://img.shields.io/badge/binary-~3.4%20MB-green)

![proJV 婕旂ず](assets/demo.gif)

**proJV 鐨勬瘡涓€琛屼唬鐮侀兘鏄?proJV 鑷繁鍐欑殑銆?*

---

## 涓変釜涓嶄竴鏍风殑鍦版柟

- **鍘熺敓 C++锛岀害 3.4 MB锛岄浂鍖呯鐞嗗櫒銆?* FTXUI + libcurl + SQLite 鍏ㄩ儴鍐呯疆锛?
| **终端状态** | 通过 **OSC 7501**（Program Status Protocol）向宿主终端上报 `working` / `blocked` / `done`，Windows Terminal、Ghostty、Ostlr 都能在标签上显示；输出被重定向时不多写一个字节，`osc_status = false` 或 `PROJV_NO_OSC=1` 可关闭 |
  涓€鏉?`cmake --build` 灏卞緱鍒颁竴涓彲浠?scp 璧扮殑鍗曟枃浠躲€?
- **澶?Agent 鎶€鑳芥槸鏁版嵁锛屼笉鏄唬鐮併€?* 涓€涓妧鑳藉氨鏄竴涓枃浠跺す锛歚config.json` +
  姣忎釜瑙掕壊涓€浠?`.md` + 鍔ㄤ綔宸ュ叿鐨?schema銆傛嫹杩涘幓灏辫兘璺戙€備富绠″彲浠ュ湪涓€鏉″洖澶嶉噷鎶?
  娲绘淳缁欏崄涓苟鍙戞墽琛岀殑 worker銆?
- **姣忎竴灞傞兘鑳芥敼銆?* 瑙掕壊銆佸伐鍏枫€佹彁绀鸿瘝銆佷富棰樸€佸帇缂╃瓥鐣ャ€丳ython 宸ュ叿 鈥斺€?鍏ㄦ槸鍙?
  鎵ц鏂囦欢鏃佽竟鐨勭函鏂囦欢锛屾病鏈夊皝闂敓鎬併€?

---

## 涓夋鐢ㄨ捣鏉?

1. **涓嬭浇**瀵瑰簲骞冲彴鐨勫彂甯冨寘骞惰В鍘?
   锛圼Releases](https://github.com/MilesKHuang/proJV/releases)锛沗proJV_tui` 涓?
   `projv_files/` 蹇呴』鏀惧湪涓€璧凤級銆?
2. **濉?DeepSeek API Key**锛氶娆″惎鍔ㄧ殑閰嶇疆绐楅噷绮樿创
   锛圼鍦ㄨ繖閲岀敵璇穄(https://platform.deepseek.com/api_keys)锛夈€?
3. **鍙戜竴鏉℃秷鎭?*锛氳瀹冭鏂囦欢銆佹敼 bug锛屾垨鑰呰窇 `/monica D:\path\to\project`銆?

Windows 鍙兘鎻愮ず浜岃繘鍒舵湭绛惧悕锛圫martScreen ->銆屾洿澶氫俊鎭€?>銆屼粛瑕佽繍琛屻€嶏級銆?
Linux 鑷缂栬瘧鏃堕渶瑕?`libcurl4-openssl-dev`銆?

鎯宠嚜宸辩紪璇戯紵-> [瀹夎鎸囧崡](docs/zh/user-guide/01-install.md)

---

## 装成一条命令，还能向终端汇报状态

| 什么 | 为什么值得在意 |
|---|---|
| **把 `projv` 装进 PATH** | `proJV_tui --install` 只写一个四行转发器，不需要管理员权限；之后在 cmd、PowerShell 或任意 shell、任意目录敲 `projv` 就能起来。`--uninstall` 只删转发器，`--purge` 删安装目录；你的配置和会话永远不动。设计文档：[`docs/archive/projv-install.md`](docs/archive/projv-install.md) |
| **OSC 7501 状态** | proJV 把 `working` / `blocked` / `done` 报给宿主终端，Windows Terminal、Ghostty、cmux 可以直接画在标签上。输出被重定向时一个字节都不多写；`osc_status = false` 或 `PROJV_NO_OSC=1` 可关闭 |
| **配套 Ostlr** | 多窗口 agent 管理器，读的就是这份状态：proJV 会话在 Ostlr 里是原生可见的 agent，两边都不需要胶水代码 |

---

## 鑳藉仛浠€涔?

| 鑳藉姏 | 璇存槑 |
|------|------|
| **宸ュ叿璋冨害** | 璇诲啓鏀规枃浠躲€佹寜琛屽尯闂磋鍙栥€佺洰褰曟爲銆佹鍒欐悳绱€乻hell銆乼odo銆佹妧鑳?鈥斺€?鐢辨ā鍨嬪喅瀹?|
| **鎶€鑳?* | `/monica <鐩綍>`锛堝崄涓?worker 骞跺彂鏁寸悊椤圭洰锛夈€乣/bigbang`锛堜笁鏂瑰渾妗岃京璁?-> 涓€浠芥墽琛屾枃妗ｏ級銆乣skill_maker` 鑷姩鐢熸垚鏂版妧鑳?|
| **鍥剧墖杈撳叆** | `@璺緞` 闄勫姞鎴浘鎴栧浘鐗囷紱缁欑洰褰曚細鑷姩灞曞紑鍏朵腑鐨勫浘鐗?|
| **鎬濊€冮摼** | 瀹炴椂娴佸紡鏄剧ず銆佸彲鎶樺彔锛宍F8` 鐪嬪畬鏁存€濊€冿紱`reasoning_effort` 鎺у埗娣卞害锛坣one / low / high / max锛?|
| **1M 涓婁笅鏂?* | DeepSeek V4 瀹屾暣 1M token 绐楀彛锛屽帇鍔涢珮鏃惰嚜鍔ㄥ帇缂╅暱浼氳瘽 |
| **缃戦〉** | DeepSeek 瀹樻柟 Search / Fetch API锛堢粨鏋勫寲缁撴灉銆丮arkdown 椤甸潰锛?|
| **浼氳瘽** | 姣忔瀵硅瘽涓€涓?SQLite 搴擄紝宕╂簝涓嶄涪锛宍/compress` 鍘嬬缉闀垮璇?|
| **涓婚** | 2 涓唴缃?+ 6 濂楃簿閫夛紝鍙鍖栫紪杈戝櫒瀹炴椂棰勮锛孞SON 瀵煎叆瀵煎嚭 |
| **Python 宸ュ叿** | 涓竴涓惈 `tool.json` + `main.py` 鐨勭洰褰曡繘鍘伙紝agent 灏辫兘璋?|
| **鎴愭湰鍙** | 瀹炴椂 token 鏁颁笌鎸?`config.toml` 浠锋牸琛ㄧ殑璐圭敤浼扮畻 |

缁嗚妭 -> [鐢ㄦ埛鎸囧崡](docs/zh/user-guide/README.md) 路 [鎶€鑳絔(docs/zh/user-guide/06-skills.md) 路 [pytool](docs/zh/user-guide/07-pytool.md)

---

## 妯悜瀵规瘮

浜嬪疄鏍稿疄鏃ユ湡 2026-10-25锛沗鈥擿 琛ㄧず鏈枃鏈牳瀹炪€俻roJV 涓€鍒椾互鏈粨搴撲唬鐮佷负鍑嗐€?

| | proJV | Claude Code | OpenAI Codex CLI | Aider |
|---|---|---|---|---|
| 瀹炵幇璇█ | C++20 | TypeScript锛圢ode锛?| Rust | Python |
| 瀹夎鏂瑰紡 | 鍗曟枃浠朵簩杩涘埗锛屼緷璧栧唴缃?| npm | npm / brew / Release | pip |
| 杩愯鐜 | 鏃?| 闇€瑕?Node.js | 鏃狅紙npm 鍙槸澶栧３锛?| 闇€瑕?Python |
| 澶?Agent | 鏁版嵁椹卞姩鐨勬妧鑳芥枃浠跺す锛寃orker 骞跺彂鎵ц | subagents | TOML subagents | 鈥?|
| 鎵╁睍鏂瑰紡 | 涓㈡枃浠讹細鎶€鑳姐€佽鑹层€佷富棰樸€丳ython 宸ュ叿 | MCP銆乭ooks | MCP銆佹彃浠?| 鈥?|
| 妯″瀷閫夋嫨 | 浠?DeepSeek | Anthropic | OpenAI | 澶歱rovider |
| 璁¤垂 | DeepSeek 鎸?token | 璁㈤槄鎴?API | ChatGPT 濂楅鎴?API | 鍙栧喅浜?provider |

鍒绘剰鍋氱獎锛歱roJV 鍙妸涓€涓ā鍨嬫湇鍔″晢鍋氬ソ锛屽苟鎶婃墿灞曢潰鏀惧湪绾枃浠堕噷锛岃€屼笉鏄彃浠舵敞鍐岃〃銆?

---

## 涓嬩竴姝ュ幓鍝?

| 浣犳兂 | 鍘?|
|------|-----|
| 鏃ュ父浣跨敤 | [鐢ㄦ埛鎸囧崡](docs/zh/user-guide/README.md) |
| 璺?/ 鍐欐妧鑳?| [鎶€鑳絔(docs/zh/user-guide/06-skills.md) |
| 鑷畾涔夎嚜宸辩殑娴佺▼ | [缂栧啓 skill](docs/zh/user-guide/09-skills-authoring.md) |
| 鏀逛唬鐮?| [寮€鍙戞寚鍗梋(docs/zh/dev-guide/README.md) 路 [妯″潡鍦板浘](docs/zh/dev-guide/module-map.md) 路 [鏀瑰姩閰嶆柟](docs/zh/dev-guide/recipes.md) |
| 鍙備笌璐＄尞 | [CONTRIBUTING_zh](CONTRIBUTING_zh.md) |

---

## 涓轰粈涔堣繖涔堥€?

**鍘熺敓 C++ + 缁堢鐣岄潰銆?* 浣撶Н灏忋€佹病鏈夎繍琛屾椂銆丼SH 鍜?tmux 閲岄兘鑳界敤锛岃繕鑳界洿鎺ヨ皟
绯荤粺 API銆傜湡闇€瑕佹煇涓ぇ搴撴椂锛岀敤 Python 宸ュ叿琛ヤ笂銆?

**涓嶅仛 MCP銆?* 杩欓噷鐨勫伐鍏峰氨鏄竴涓甫 JSON schema 鐨勭粨鏋勪綋鍔犱竴涓嚱鏁般€備换浣?MCP 鑳藉姏
閮借兘鐢ㄥ嚑鍗佽鍐欐垚涓€涓?pytool 鎴栧唴缃伐鍏凤紝涓棿涓嶉渶瑕佸崗璁眰銆?

**鐩墠鍙帴 DeepSeek銆?* 瀵逛竴涓暣澶╄鍐欐枃浠剁殑 agent 鏉ヨ瀹冭冻澶熶究瀹滐紝澶?Agent 鎵嶈窇
寰楄捣銆?

---

## 鑷磋阿

[DeepSeek](https://deepseek.com) 路 [FTXUI](https://github.com/ArthurSonzogni/FTXUI)
路 [SQLiteCpp](https://github.com/SRombauts/SQLiteCpp) 路 [libcurl](https://curl.se/)
路 [loguru](https://github.com/emilk/loguru) 路 浠ュ強鎵€鏈夎杩欎竴鍒囨垚涓哄彲鑳界殑寮€婧愪綔鑰呫€?

## 鏀寔

濡傛灉 proJV 甯綘鐪佷簡鏃堕棿锛屾杩庤鎴戝枬鏉挅鍟★紙寰俊锛夛細

<p align="left">
  <img src="assets/wechat_donate.jpg" width="150" alt="寰俊璧炶祻"/>
</p>
