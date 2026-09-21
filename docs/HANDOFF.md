# 引き継ぎメモ（2026-09-21）

目的：**PSP単体でYouTubeを検索・ブラウズ・再生できること**だけを最優先。Wikipedia等のブラウザ機能は不要。ユーザーはPSP-3000（CFW、seplugins使用）で実機テストしている。

## 公開・名前
- 名前は **komi-tube**。公開リポジトリ https://github.com/kouminiku-9900/komi-tube （MIT）。エンジンは上流 stjanovitz/tilefinch（MIT、基点 5a77720）を `vendor/tilefinch/` に直接取り込んで変更している（submodule・フォークは廃止）。
- PSPフォルダは `PSP/GAME/KOMI_TUBE`、XMBタイトル komi-tube、アイコンは `assets/ICON0.PNG`（`scripts/make_icon.py` で生成、`package.py` が EBOOT に埋め込む）。
- 組み込みの YouTube Web APIキーは削除済み（youtubei search/next はキー無しで応答）。

## 構成
- 本体：`vendor/tilefinch`（上流Tilefinch＝PSP向けWebブラウザ）。YouTubeは「lite」経路（`src/youtube_lite.c`）が軽量HTMLを生成し、ネイティブプレーヤー（`src/psp_media_*.c`, `src/media_backend_psp.c`）で再生。動画URLの解決は `src/youtube_resolver.c`（現状 VISIONOS クライアント、360p=itag134+140）。
- 変更は GitHub の main に push して管理する。

## ビルド・検証・実機インストール
```sh
./scripts/build.sh      # PSPビルド＋dist/YouTube-for-PSP.zip（前回パッケージは dist/previous-package-日時 に退避）
./scripts/test.sh       # ホスト全テスト（fetch-redirect-tests は既知の環境依存失敗。trace-acquisition は並列時のみタイムアウト）
python vendor/tilefinch/tests/test_psp_sdk_contracts.py vendor/tilefinch   # ソース契約テスト
```
- PPSSPP検証（ログ付きビルド `build-preset-psp-validation7`）：
  `PSP_BROWSER_PSP_BUILD_DIR=$PWD/build-preset-psp-validation7 PPSSPP=<repo>/tools/ppsspp/PPSSPPSDL.app/Contents/MacOS/PPSSPPSDL scripts/run-ppsspp-network.sh --url <URL>`（vendor/tilefinch で、`scripts/env.sh` 読込後）。`--play-media`＋`TILEFINCH_YOUTUBE_TEST_URL` で解決まで確認（PPSSPPは mpeg_vsh が無く再生不可）。
- ホスト実通信プローブ：`build-preset-release/tilefinch-youtube-resolver-probe <watchURL> 360 [--demux-open|--demux-full]`。lite経路はscratchの liteprobe.c 方式（libtilefinch_core.dylib をリンク）で確認していた。
- 実機インストール（PSPは `/Volumes/NO NAME`）：`dist/PSP/GAME/KOMI_TUBE` から `EBOOT.PBP BUILD-INFO.json README-ja.md SHA256SUMS slot-a NOTICES OPTIONAL` を `COPYFILE_DISABLE=1 rsync -r --inplace` で上書き、`find ... -name '._*' -delete`、`cmp` で確認。**`data/` は上書きしない**。
- 実機ログ：`data/tilefinch-error-history.txt`（今回追加、全エラーを追記、heap-largest付き）、`data/tilefinch-last-error.txt`。読んだら `.N.old.txt` に改名して区別している（最新の退避は `.4.old`）。

## これまでの変更（すべて実機の症状から）
1. **Wi‑Fi**：起動直後（ブラウザ起動前、`psp_script_main.c` main冒頭・メディアプール確保直後）にPSP公式接続ダイアログ（sceUtilityNetconf）を表示。失敗時は公式メッセージで再試行/終了。ネットワーク層はブート接続を引き継ぎ再初期化しない（`psp_network.c` `psp_network_boot_services`）。旧原因：warmupが固定プロファイル1で `sceNetApctlConnect`→apctl=0のまま45秒タイムアウト。
2. **検索**：START入力はYouTube検索へ（`browser_omnibox_resolve_youtube`）。ホームのWikipediaタイル削除。
3. **漢字入力**：OSKを本体言語が日本語なら漢字/かな等を明示して起動（`psp_text_input.c`）。
4. **2本目で `aligned codec allocation failed missing=ddr pool=none`**：メディア専用プール（約5.3MB、4MB境界）確保条件を heap 24MB→3MB＋プールへ緩和、失敗時は初回再生時に遅延確保して保持（`media_backend_psp_pool.h`, `psp_media_pool_ensure`）。
5. **`player: response exceeded shared memory budget`**：動画選択時にキャッシュ一括解放・ページ通信停止（`psp_app_focus_memory_on_video`）、ページ移動前にも一括解放（`psp_reclaim_before_navigation`）。解決器の受信バッファは384KB一括＋256KB刻み。
6. **検索ページ取得失敗 `stream consumer rejected ...`**：検索HTMLが展開後約580KBで倍々バッファ1MB化に失敗。全取得をJSON APIへ：ホーム＝通信なし、検索＝`youtubei/v1/search`（約180KB）、詳細/関連＝`youtubei/v1/next`（160〜215KB）。組み込み公開キー＋clientVersion `2.20260918.00.00`（`lite_default_identity`）。identityキャッシュはCookie指紋に縛らない（`browser_session_site_adapter_state_get_unbound`）。API拒否時はHTMLにフォールバック。
7. **再生開始直後に音がゆっくり**（推定：TLSハンドシェイクのCPU競合）：開始前バッファ3秒→5秒（窓が満杯なら2秒で開始）。**実機効果は未確認**。
9. **2本目以降の再生失敗の修正（resolver）**：失敗は budget でも heap量でもなく、watchページ（1.3〜1.7MB、STSは約680KB地点）受信中の 620KB→896KB realloc。キャッシュ識別子に STS が無く enriched が失敗→毎回 watch HTML を丸ごと溜めていた。修正：2KB窓で流し読み、visitor/APIキー/STS をプロセス内キャッシュ（2本目以降は watch 取得なし、失敗時は無効化）、take_chunk 後の終端1バイトはみ出し修正、失敗時メッセージに budget 内訳。PPSSPPで解決成功（watch 約725KBで打ち切り）。**実機の2本目は未確認**。
10. **起動**：一度「既定URLを www.youtube.com にして engine-first 起動」にしたら実機で検索・動画が止まった（エラー記録なし）。**engine-first 起動経路は使わない**。現在は native HOME 起動のまま、boot queue に `https://m.youtube.com/` を積み、HOMEのYouTubeタイルと同じ順序（leave_native_surface→begin_page_load）で自動遷移（`psp_script_main.c`）。PPSSPPで deferred-navigation started=1・http=200 を確認（`--startup-test` は「起動時に通信しない」前提なので fail 表示になるが想定内）。
11. **Wi‑Fi**：netconf action 3（前回AP）→失敗時のみ選択画面（`psp_network.c`）。実機で動作確認済み。
12. かな→漢字候補ページは不要（削除済み）。
8. 診断：エラー履歴、メディア/HLSエラー詳細、確保失敗の必要バイト数、長尺（2時間）の分割ストリーミングはホストで完走確認。

## 未確認・既知の懸念
- 最新ビルド（6）はPSPにインストール済みだが**実機未確認**（関連動画→再生の繰り返し）。
- 詳細ページのAPI経路では説明文抜粋が出ない場合あり。説明全文/コメント表示はHTML経路のまま（大きい）。
- 組み込みclientVersionが古くなるとAPI拒否→HTML経路（メモリ大）に戻る。
- HLS（ライブ）で `HLS master has no compatible 240p AVC rendition` が一度出たが原因未特定（ホストのパーサーは実マスターを正しく処理）。今は失敗時に変種一覧をエラーに残す。
- 通常heapは約9〜11MB（PPSSPP実測、拡張メモリ未使用と推定）で、プール確保後の残りは少ない。

## 次の方針候補
- 実機で「検索→再生→関連→再生」の反復を確認し、エラー履歴の heap-largest を見る。
- なお不足なら：HTML/CSSエンジンを使わない**ネイティブ画面**（検索結果・詳細をJSONから直接描画）への作り直しを提案済み（大工事、実機反復が必要）。
- clientVersion をAPI応答（responseContext）から更新・保存する仕組み。
