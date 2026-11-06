# AquaSynth v0.2

上物用のシンセ。Logic Pro で AU 音源として使う。Intel Mac と Apple Silicon のどちらでも動くようにビルドする（macOS 10.13 以降）。

## Mac 用のプラグインを作る（GitHub が自動でビルド）
1. GitHub で新しいリポジトリを作る（例：`aquasynth`）。v0.1 のリポジトリがあれば、中身を全部入れ替えてもいい。
2. この zip の中身を全部、リポジトリの一番上にアップロードする（フォルダごと上げない）。
3. `.github/workflows/build-mac.yml` が上がっているか確認する。
   Finder では `.github` が隠しフォルダなので、上がっていないことがある。そのときは
   GitHub の「Add file → Create new file」で、名前に `.github/workflows/build-mac.yml` と打ち、
   `mac-build.yml` の中身をそのまま貼って保存する。
4. 「Actions」タブ →「Build Mac plugin」が緑のチェックになるまで待つ（10〜15分）。
5. その実行を開いて、下の「Artifacts」から `AquaSynth-mac` をダウンロードする。

## Logic に入れる
1. ダウンロードした zip を解凍 → 中の `AquaSynth-AU.zip` も解凍 → `AquaSynth.component` ができる。
2. Finder の「移動 → フォルダへ移動」に `~/Library/Audio/Plug-Ins/Components/` と入れて開き、そこに `AquaSynth.component` を入れる（古いものは上書き）。
3. ターミナルで次の1行を実行する（ネットから落としたファイルの「開けない」ロックを外す）：
   `xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/Components/AquaSynth.component`
4. Logic を開く → ソフトウェア音源トラック → 音源スロット → AU 音源 → Shohei → AquaSynth。
   出てこないときは Logic Pro → 設定 → プラグインマネージャ →「選択したものをリセットして再スキャン」。
5. 画面左上のシールに「V0.2」と出ていれば新しい版。

## ページ
画面上の `MAIN` `FX` `PRESETS` で切り替える。

### MAIN
- つまみ：上下にドラッグ。ダブルクリックで初期値に戻る。
- OSCILLATORS の 1〜4：使うオシレーターの数。
- 波形の上をタップ：波形の種類を切り替え（SINE / TRIANGLE / JUNO SAW / JUNO PULSE / WAVETABLE / FM / FORMANT / NOISE / SAMPLE）。
- `OSC1` の文字（＋）をタップ：細かい設定（OCT・FINE・UNISON・DETUNE・POS、WAVETABLE なら TABLE、SAMPLE なら KEY）を開く。もう一度タップで閉じる。
- MOD ROUTING：点をタップすると、その LFO（L1〜L3）がその行き先につながる（1つの LFO に2か所まで）。
- LFO：波形の部分をタップすると、上の RATE / DEPTH / SMOOTH がその LFO を操作する。波形名をタップで形が変わる。右の行き先の文字をタップでも行き先を変えられる。
- SYNC：Logic のテンポに合わせて揺れる（再生中は小節の頭に揺れの位置がそろう）。HZ：秒で指定。
- エンベロープ：FILTER / AMP / PITCH を切り替え。
- RANDOM：TARGET で対象（全部／OSC／LFO／ENV／FILTER／FX）を選ぶ。「A LITTLE」は少しだけ崩す、「FULL」は大きく変える。「UNDO」で1つ前に戻す。
- 右上の丸3つ：白／グレー／黒。
- spread：鳴っている音を周波数ごとに点で表示。横が左右、縦が高さ、小さく薄い点ほど残響っぽい。

### FX
左上から右下の順に通る。DISTORTION / CHORUS / FLANGER / DELAY（Logic のテンポに合う）/ REVERB / EQ / COMP / STEREO。`ON` `OFF` をタップで切り替え。
全部 ON でも軽い（試験では4オシレーター×7ボイスのユニゾン×8音で、実時間の8%ほど）。

### PRESETS
- STARTER：最初から入っている5つの音。
- MY PRESETS：自分で保存した音。タップで読み込む。
- 保存：下の NAME に名前を入れて SAVE。保存場所は `~/Music/AquaSynth/Presets`（`.aqpreset`）。
- IMPORT SERUM 2 PRESET：`.SerumPreset` をここにドラッグするか、タップして選ぶ。近い設定になったら、そのまま SAVE で自分の音として残せる。
  - 結果は3列で出る。APPLIED＝そのまま再現、CLOSE MATCH＝近い波形などに置き換え、NOT AVAILABLE＝再現できなかったもの。
  - Serum 本体に入っている波形やサンプルは使えないので、このシンセの近い波形に置き換える。
  - プリセットの中に録音そのものが入っている場合（鳥の声など）は、その録音をそのまま使う。その場合、保存した `.aqpreset` は数MB〜十数MBになる。

## 中身（参考にした論文）
- 波形：PolyBLEP による折り返しの少ない波形（Välimäki & Huovilainen 2007）
- WAVETABLE：波形を位置でめくっていく方式（Horner, Beauchamp & Haken 1993）
- FM：2オペレーター FM（Chowning 1973）
- FORMANT：母音のフォルマント（Rodet 1984 の考え方）
- フィルター：4極ラダー、ゼロ遅延フィードバック（Huovilainen 2004 / Zavalishin 2012）
- コーラス・フランジャー：Juno 風の BBD（Raffel & Smith 2010）

## 開発用
Linux / Mac で `cmake -S . -B build -DAQUASYNTH_TESTS=ON && cmake --build build` → `build/RenderTest out <SerumPresetのフォルダ>` で試し音と画面の画像を書き出す。

使っているもの：JUCE 8、Zstandard の展開部分（BSD、`ThirdParty/zstd/LICENSE`）、フォント Doto（SIL Open Font License、`Resources/Doto-OFL.txt`）。
