# 引き継ぎ：ネイティブクライアントへの作り替え（2026-09-24）

クラウドのセッションからローカル（Mac上のClaude Code）へ引き継ぐためのメモです。tilefinch版の経緯は [HANDOFF.md](HANDOFF.md)、実機テストの手順は [FIELD-TEST.md](FIELD-TEST.md) を参照してください。

## 最初にやること

```sh
git fetch origin
git checkout claude/wonderful-cori-e21ml3   # 作業ブランチ（mainには未マージ）
./scripts/bootstrap.sh                       # tools/ が無ければ
./scripts/build_memprobe.sh                  # 調査EBOOTがビルドできることの確認
```

- ブランチは3コミットだけmainより先に進んでいる：調査EBOOT、その修正（v2）、実機結果の記録とMEMSIZE=1への変更。PRはまだ作っていない。
- `./scripts/build.sh`（tilefinch版のフルビルド）はローカルで通ることを確認済み（下の「未確認・注意点」参照）。

## ゴールと方針（ユーザーと合意済み）

tilefinch（PSP向けブラウザ）にYouTubeを表示させる今の構成から、ブラウザを使わないネイティブクライアントへ段階的に作り替える。対象はPSP-2000以降（64MB）。最初の山場は「ブラウザなしで動画を連続再生できるEBOOT」。

ユーザーの強い要望：**ユーザーの手間とPSPの持ち出し回数を最小にする**。実機で動かすEBOOTは、起動するだけで試験を最後まで実行し、ログをメモリースティックに残す作りにする。ボタン操作や画面の書き写しは求めない。

| 段階 | 内容 | 状態 | 実機 |
|---|---|---|---|
| 0前半 | メモリ配置の実測 | **完了** | 済 |
| 0後半 | 部品を`derived/`へ切り出し、`net/http.h`を定義 | 未着手 | 不要 |
| 1 | ブラウザなしの再生EBOOT（OSKで動画ID → `yt/resolver` → `media/Player`）、`profiles.cfg`の外部化 | 未着手 | 1回（10本連続再生） |
| 2 | HTTPの自前化（mbedTLS＋curlを直接、Range、gzip、リダイレクト、キャンセル） | 未着手 | 1回目か3回目に同乗 |
| 3 | ネイティブ画面（検索→結果（サムネイル）→再生、GU＋intraFont、JSONはストリーミングで読む） | 未着手 | 1回（操作感はユーザーが判断） |
| 4 | 関連動画、設定、字幕、履歴、スリープ復帰、PSP goの`ef0:` | 未着手 | 3回目に同乗できるかも |
| 5 | `vendor/tilefinch`を削除し、`derived/`とNOTICEだけ残す | 未着手 | 不要 |

### 段階0後半の具体的な作業

- tilefinchから次を`derived/`へ移す。ファイル先頭に元コミット`5a77720`（stjanovitz/tilefinch）とMITの表記を残す。
  - `media_mp4`、`media_backend_psp`、`psp_media_present_ge`、`psp_network`、`youtube_subtitles`
  - mbedTLSとcurlのビルド設定（`vendor/tilefinch/cmake/PspOwnedTransport.cmake`と`vendor/tilefinch/patches/`のmbedtls・curl分）
- `net/http.h`（抽象インターフェース）を定義する。最初の実装は既存の`fetch`を包むアダプタでよい。
- 既存のブラウザ版EBOOT（`./scripts/build.sh`）も並行してビルドできる状態を保つ。

### 段階1で必ず入れること（実機1回で済ませるため）

- 起動すると、テスト用の動画IDリスト（`data/`に置くテキスト）を自動で順に再生し、1本ごとに確保量、失敗、所要時間をログに残す。OSK入力は手動確認用の入口として残す。
- **H.264（mpeg_vsh / sceMpeg）が拡張領域（`0x0A000000`以上）のバッファを読めるか**の試験を同じEBOOTに入れる。AACは読めた（下記）が、H.264は未確認。
- `yt/resolver`は既存の`vendor/tilefinch/src/youtube_resolver.c`（クライアント設定、フォーマット選択、STSキャッシュ）から移植する。`yt/`とdemuxは、Mac上の実通信テストとfixtureテストの両方を用意する。
- 合格ライン：実機で10本連続再生して、確保失敗も断片化も起きないこと。

## 実機で確定したこと（PSP-3000、kuKernelGetModel=8、FW 6.61）

ログと集計は`docs/field-results/2026-09-24-memprobe-v1/`と`-v2/`。

- **使えるメモリはMEMSIZE=1のときだけ49.6MB**。MEMSIZE=0と2は22.6MB（このCFWでは2は拡張されない）。
- **今のkomi-tubeはMEMSIZE=2だった** → 実機では22.6MBで動いていた。`scripts/package.py`で1に書き換えるよう変更済み（実機未確認）。
- **MEは拡張領域も読み書きできる（AAC）**：ctrl・作業領域・入力・PCMを通常領域、揮発メモリ、入出力だけ高位、すべて高位の4通りに置き、24フレームとも成功、出力も一致した。tilefinchの`media_backend_psp_pool.h`冒頭のコメント（MEは`0x0A000000`以上を読めない）は、少なくともAACでは当てはまらない。
- AACの作業領域（`sceAudiocodecCheckNeedMem`の結果、ctrl[4]）は**100744バイト**。tilefinchのプールは64KBを想定しているため、実機ではヒープにフォールバックしているはず。
- AV・netモジュールの消費：avcodec 0、mpeg_vsh 約43KB、netモジュール 約432KB、sceNet初期化 約168KB。
- 揮発メモリ（`0x08400000`、4MB）はユーザーモードの`sceKernelVolatileMemTryLock`で取得でき、MEからも読める。
- デコーダーを10回開閉しても漏れは0。

確定したメモリ割り当て（MEMSIZE=1）：

| 領域 | 範囲 | 大きさ |
|---|---|---|
| プログラム・newlib・モジュール | `0x08800000-0x089C5B00` | 約1.8MB（調査EBOOTの場合。本番は大きくなる） |
| 空き（低位） | `0x089C5B00-0x08C00000` | 約2.2MB |
| ME専用領域 | `0x08C00000-0x09200000` | 6MB（4MB境界。先頭にDDR 2MB） |
| 予備 | `0x09200000-0x09300000` | 1MB |
| 汎用アリーナ | `0x09300000-0x0BA80000` | 39.5MB（うち26.5MBが拡張領域） |
| メインスレッドのスタックなど | `0x0BA80000-0x0BB00000` | 約0.5MB |

ME専用領域は保険として低位に固定したままにする（低位には余裕がある）。本番はプログラムが大きいので、4MB境界の候補が`0x09000000`へずれる可能性を考慮する（調査EBOOTと同じく、4MB境界ごとに確保を試す方式にすること）。

## 未確認・注意点

- `./scripts/build.sh`はローカル（Mac）で通ることを確認済み（2026-09-24）。その際、ランチャーが`sctrlKernelLoadExecVSHMs2`で起動するブラウザ本体`slot-a/EBOOT.PBP`がMEMSIZE=2のままだったため、`package.py`でこちらも1に書き換えるよう修正した（CFWは起動するEBOOTのSFOで区画の大きさを決める）。この版をメモリースティックに入れ済み、実機未確認。
- MEMSIZE=1にしたkomi-tubeで「検索→再生→関連→再生」が改善するか（HANDOFF.mdの未確認項目もまだ残っている）。
- PPSSPPの限界：MEMSIZE=2は24MB扱い、`sceAudiocodecCheckNeedMem`はサイズを書かない（調査EBOOTは16KBとみなして続行する）、`mpeg_vsh`が無い（kubridgeも無い）、MEのメモリ制約を再現しない。デコードの成否は実機でしか分からない。
- ビルドの注意：PSPのEBOOTで`-lpsputility -lpspnet_inet -lpspnet_resolver`をLIBSに書くと、ツールチェーンのspecsがlibcの後に同じライブラリを再度リンクし、importスタブが分断される（`psp-fixup-imports`が "stubs out of order" と警告する）。これらはspecsに任せ、LIBSに書かない（`native/memprobe/Makefile`参照）。

## 実機の自動化（次にやる準備）

ユーザーの手間を減らすため、PSPLinkで実機試験を自動化する予定。未検証なので、最初に一度動くか確かめる。

- Mac側で使う道具（`pspsh`、`usbhostfs_pc`）はpspdevのSDKに同梱されている（Linux版で確認。macOS版も同梱の見込み）。
- PSP側のPSPLink本体は [pspdev/psplinkusb](https://github.com/pspdev/psplinkusb) からビルドするか、配布物を使う。CFW 6.61で動くかは未確認。
- 使い方の見込み：`usbhostfs_pc <dist のフォルダ>`でMacのフォルダを`host0:`として見せ、`pspsh`からEBOOTを起動する。ログを受け取り、スクリーンショットは`scrshot`コマンドで撮る。これでメモリースティックへのコピーが要らなくなる。
- ボタンの遠隔操作と画面の転送が要るなら（段階3）RemoteJoyLite。自作のEBOOTは、試験の手順をスクリプト化して内蔵すれば、遠隔操作はほぼ要らない。
- ローカルで準備したこと（2026-09-24）：
  - Mac：`brew install libusb`済み。`tools/pspdev/bin/`の`pspsh`と`usbhostfs_pc`がそのまま動く。
  - PSP：PSPLink v3.2.1にtilefinchのパッチ2つ（HOMEでXMBへ戻る、再生中でも安全な`scrshot-user`）を当ててビルドし、`ms0:/PSP/GAME/PSPLINK/`に入れた。作り直すときは`git clone --branch v3.2.1 --depth 1 https://github.com/pspdev/psplinkusb.git <dir>`のあと`PSPDEV=$PWD/tools/pspdev vendor/tilefinch/scripts/build-psplink-home-exit.sh <dir> build/psplink`。
  - `scripts/psplink.sh ready`／`scripts/psplink.sh exec '<pspshのコマンド>'`：tilefinchの`psplink-shell.sh`を呼ぶ薄い入口。`host0:`は`dist/`。PSP無しで`ready`を実行し、`usbhostfs_pc`が起動してPSPを待つところまで確認済み。
  - CFWは**6.61 PRO-C**（ユーザー確認済み）。tilefinchはARK-4で使っていたので、PRO-CでPSPLinkが起動するか、EBOOTを`ld`できるか（ARK-4ではできず`tools/psplink-loop`のtfexec.prxでLoadExecしていた）はまだ試していない。
- tilefinch本体に実機自動化の一式がある：`vendor/tilefinch/docs/engineering/PSPLINK_DEV_LOOP.md`、`vendor/tilefinch/scripts/psplink-shell.sh`（`usbhostfs_pc`の起動と時間制限付きの`pspsh`実行）、`tools/psplink-loop`（EBOOTを直接`ld`できないCFW向けにLoadExecする小さなPRX）。PSP-3000＋ARK-4で使われていたもの。これを流用する。
- 注意（同文書より）：PSPLink v3.2.1の`scrshot`はMEのファームウェア領域と重なるバッファを使うため、再生中に撮るとAVC/AACがタイムアウトする。再生中の画面はEBOOT自身で撮ること。
- 目標は、ユーザーの作業を「USBでつなぐ → PSPでPSPLinkを起動する」だけにすること。以降の転送、起動、ログ回収、判定はClaude Codeが行う。

PSPLinkが使えない場合は、今までどおり`./scripts/field_sync.sh install`でコピー → ユーザーが起動 → `./scripts/field_sync.sh collect`で回収。

## このブランチで追加したもの

| パス | 内容 |
|---|---|
| `native/memprobe/` | 段階0の調査EBOOT（`main.c`、`Makefile`、`gen_aac_fixture.py`）。現行はv2 |
| `scripts/build_memprobe.sh` | MEMSIZE 0/1/2の3種類をビルドし、`dist/field-kit/PSP/GAME/KOMI_PROBE_M{0,1,2}`に置く。Linuxでは`PSPDEV=<pspdev-ubuntu展開先>/pspdev`を指定 |
| `scripts/field_sync.sh` | `install`（komi-tubeと調査EBOOTをメモリースティックへ。`data/`は上書きしない）／`collect`（ログを`field-logs/<日時>/`に集め、スティック上は`.old.txt`に改名し、集計を出す） |
| `scripts/memprobe_report.py` | memprobeのログをMarkdownの表にする |
| `scripts/package.py` | EBOOTのMEMSIZEを1に書き換える処理を追加 |
| `docs/FIELD-TEST.md` | 実機テスト1回分の手順と、全体の持ち出し予定 |
| `docs/field-results/` | 実機ログ（v1：全3種類、v2：m1） |
