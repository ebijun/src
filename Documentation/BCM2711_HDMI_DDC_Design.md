# NetBSD BCM2711 HDMI DDC / VC4 DRM2対応 設計書

- 文書バージョン: 0.9
- 作成日: 2026-11-22
- 最終更新日: 2026-11-24
- 対象リポジトリ: [https://github.com/ebijun/src](https://github.com/ebijun/src)
- 対象ブランチ: `vc4-drmkms`
- 対象ハードウェア: Raspberry Pi 4 / BCM2711
- 対象ソフトウェア: NetBSD ARM / DRM2 / LinuxKPI / VC4 DRM

---

## 1. 目的

Raspberry Pi 4のBCM2711 HDMI出力を、NetBSD上のVC4 DRM/KMSドライバから利用できるようにする。

特に、HDMIコネクタのEDID取得に必要なDDC通信を実現する。

Linux版VC4 DRMドライバはLinux I2C adapterを使用する。一方、NetBSDではI2Cコントローラが`i2c_controller`として管理されるため、両者の接続が必要になる。

本設計では、次の機能を段階的に実装する。

1. BCM2711 HDMI DDCコントローラの初期化
2. NetBSD I2Cコントローラとしての登録
3. LinuxKPI I2C adapterとの接続
4. VC4 HDMIドライバからのEDID取得
5. HDMIモード検出・設定の動作確認

## 2. 現在のソース構成

作業ブランチは`vc4-drmkms`。

確認時点では、ローカル作業ツリーはoriginと同期しており、未コミット変更はなかった。

主な関連ファイルは以下のとおり。

| ファイル                                                                 | 役割                     |
| -------------------------------------------------------------------- | ---------------------- |
| `sys/external/bsd/drm2/dist/drm/vc4/vc4_hdmi.c`                      | VC4 HDMIコネクタ・EDID処理    |
| `sys/external/bsd/drm2/include/linux/i2c.h`                          | LinuxKPI I2C adapter定義 |
| `sys/arch/arm/broadcom/bcm2835_bsc.c`                                | BCM2835 BSC転送処理        |
| `sys/arch/arm/broadcom/bcm2835_bsc_fdt.c`                            | BCM2835 BSCのFDT attach |
| `sys/arch/arm/broadcom/bcm2835_bscreg.h`                             | BCM2835 BSCレジスタ定義      |
| `sys/arch/arm/broadcom/bcm2835_bscvar.h`                             | BCM2835 BSC softc定義    |
| `sys/arch/arm/broadcom/files.bcm2835`                                | Broadcom ARMドライバ登録     |
| `sys/external/gpl2/dts/dist/arch/arm/boot/dts/broadcom/bcm2711.dtsi` | BCM2711 Device Tree    |

## 3. 確認済み事項

### 3.1 VC4 HDMIドライバ

`vc4_hdmi.c`では、EDIDを次のように取得している。

```
edid = drm_get_edid(connector, vc4->hdmi->ddc);
```

DDC adapterの取得には次のLinux APIが使用されている。

```
hdmi->ddc = of_find_i2c_adapter_by_node(ddc_node);
```

終了時には次の処理がある。

```
put_device(&hdmi->ddc->dev);
```

これらはLinuxのI2C adapter/device管理を前提としている。

NetBSD LinuxKPIでは`struct i2c_adapter`の`dev`がLinuxの`struct device`と同一ではないため、そのままの移植はできない。

### 3.2 NetBSD既存BCM2835 BSC

既存ドライバは以下のファイルで構成される。

- `bcm2835_bsc.c`
- `bcm2835_bsc_fdt.c`
- `bcm2835_bscreg.h`
- `bcm2835_bscvar.h`

FDT compatibleは以下のみ。

```
{ .compat = "brcm,bcm2835-i2c" },
```

このドライバはBCM2835 BSCレジスタを直接操作する。

代表的なレジスタは以下のとおり。

| レジスタ     | オフセット |
| -------- | ----- |
| BSC_C    | 0x00  |
| BSC_S    | 0x04  |
| BSC_DLEN | 0x08  |
| BSC_A    | 0x0c  |
| BSC_FIFO | 0x10  |
| BSC_DIV  | 0x14  |

BCM2711 HDMI DDCは異なるレジスタ構造を使用するため、既存ドライバにcompatibleを追加するだけでは対応できない。

### 3.3 BCM2711 Device Tree

BCM2711 DTSには、HDMI DDC用のI2Cノードが2つ存在する。

compatible:

```
compatible = "brcm,bcm2711-hdmi-i2c";
```

レジスタ領域は2つ。

```
reg = <...>, <...>;
reg-names = "bsc", "auto-i2c";
```

クロック設定:

```
clock-frequency = <97500>;
```

NetBSDで確認したDTSノードには割り込みプロパティが存在しない。

そのため、初期実装ではポーリング方式を使用する。

### 3.4 FDT API

NetBSDには名前によるレジスタ取得APIが存在する。

```
int fdtbus_get_reg_byname(int, const char *,
    bus_addr_t *, bus_size_t *);
```

以下のように使用できる。

```
fdtbus_get_reg_byname(phandle, "bsc",
    &bsc_addr, &bsc_size);

fdtbus_get_reg_byname(phandle, "auto-i2c",
    &auto_addr, &auto_size);
```

### 3.5 Linux BCM2711 DDC実装

Linux側の実装は次のファイル。

`drivers/i2c/busses/i2c-brcmstb.c`

対応compatible:

```
"brcm,bcm2711-hdmi-i2c"
```

BCM2711では`bsc`と`auto-i2c`の2領域を使用する。

初期化時には`auto-i2c`の以下のレジスタを操作する。

```
#define AUTOI2C_CTRL0              0x26c
#define AUTOI2C_CTRL0_RELEASE_BSC  BIT(1)
```

処理内容:

```
writel(AUTOI2C_CTRL0_RELEASE_BSC,
    autoi2c + AUTOI2C_CTRL0);
```

続いてBSCの`iic_enable`を0にする。

Linux実装は割り込みが利用できない場合、ポーリング方式にフォールバックする。

### 3.6 Linux BCM2711 BSCレジスタ構造

Linuxドライバの構造体から確認できるレジスタ配置。

| レジスタ           | オフセット       |
| -------------- | ----------- |
| chip_address   | 0x00        |
| data_in[0..7]  | 0x04 - 0x20 |
| cnt_reg        | 0x24        |
| ctl_reg        | 0x28        |
| iic_enable     | 0x2c        |
| data_out[0..7] | 0x30 - 0x4c |
| ctlhi_reg      | 0x50        |
| scl_param      | 0x54        |

この配置はBCM2835 BSCとは異なる。

Linux実装ではデータレジスタ群を使用し、転送方向・START/STOP条件を制御する。

実際の転送単位や分割条件については、Linuxドライバの転送処理を詳細に確認してからNetBSD側に反映する。


### 3.7 Linux upstream転送処理の詳細確認（2026-11-23）

Linux upstreamの `drivers/i2c/busses/i2c-brcmstb.c` を確認し、BCM2711 HDMI DDCに関係する初期化・転送処理を整理した。確認対象はGitHubのLinux `master` ブランチ上のファイル（確認時blob SHA: `52762249d46aafda0310772c688ae6cdf9a3b2f9`）。

#### 3.7.1 データレジスタ幅と1回の転送量

BCM2711では `data_regsz = sizeof(u32)`、すなわち1データレジスタあたり4バイトとして扱われる。データレジスタは8個なので、1回のBSCデータ転送で扱う上限は32バイトとなる。

- 送信データ: `data_in[0..7]`（0x04～0x20）
- 受信データ: `data_out[0..7]`（0x30～0x4c）
- 1回の転送長: 最大32バイト
- 32バイトを超えるメッセージ: Linux実装では32バイト以下の複数転送に分割

`ctlhi_reg` の `BSC_CTLHI_REG_DATAREG_SIZE_MASK`（bit 6）をセットし、4バイト幅を選択する。NetBSD実装でもこの設定と、データレジスタへのバイト詰め・取り出し順序を再現する必要がある。

#### 3.7.2 BSC制御ビット

Linuxドライバで使用される主なビットは以下のとおり。

| レジスタ | ビット／マスク | 意味 |
| --- | ---: | --- |
| `ctl_reg` | `0x03` | DTF転送方向フィールド |
| `ctl_reg` | `0x30` | SCL選択 |
| `ctl_reg` | `0x40` | 割り込み許可 |
| `ctl_reg` | `0x80` | クロック分周 |
| `iic_enable` | `0x40` | RESTART |
| `iic_enable` | `0x20` | NOSTART |
| `iic_enable` | `0x10` | NOSTOP |
| `iic_enable` | `0x04` | NOACK制御 |
| `iic_enable` | `0x02` | INTRP（転送完了状態） |
| `iic_enable` | `0x01` | ENABLE（転送開始） |
| `ctlhi_reg` | `0x40` | 4バイトデータレジスタ幅 |
| `ctlhi_reg` | `0x02` | IGNORE_ACK |

DTFはwriteで0、readで1を設定する。combined transfer用の値としてread/write方向の組み合わせも定義されているが、通常のメッセージ転送では各メッセージの方向に応じて設定する。

#### 3.7.3 転送開始・完了・後処理

Linux実装の1コマンドの概略は次のとおり。

1. `iic_enable.INTRP` が立っていないことを確認し、前回転送中でないことを確認する（最大100ms）。
2. 割り込みモードではcompletionを初期化する。ポーリングモードではINTRPビットを監視する。
3. `ctl_reg.INT_EN` を有効にする。
4. `iic_enable.ENABLE` をセットして転送を開始する。
5. 完了を待つ。タイムアウトは100ms。
6. 通常のread/writeコマンドでは `iic_enable.NOACK` を確認し、NACKならエラーとする。
7. `cnt_reg` と `iic_enable` に0を書き、コマンド状態をクリアする。

ポーリング時もLinux実装は `iic_enable.INTRP` を完了判定に使用する。NetBSD側では割り込みを初期段階で実装せず、同じ状態ビットをタイムアウト付きで監視する方針とする。NetBSDの遅延・待機APIは実装時に適切なものを選ぶ。

#### 3.7.4 START / RESTART / STOPの扱い

Linuxドライバは各 `i2c_msg` の前に `iic_enable` のSTART/STOP関連ビットを設定し、メッセージ間の条件を制御する。

- 次のメッセージが通常のメッセージなら、現在のメッセージをNOSTOPで終え、次のメッセージでRESTARTを発行する。
- 最終メッセージではRESTART/NOSTOPを解除し、転送終端でSTOPを発行する。
- `I2C_M_NOSTART` が指定された場合は、アドレス再送とSTART条件を抑制する。
- 1メッセージが32バイトを超える場合、途中チャンクではSTOPを発行せず、次チャンクをNOSTART | NOSTOPで継続する。

したがって、LinuxKPI bridgeは複数の `i2c_msg` を個別の `iic_exec()` に単純分割してはならない。メッセージ列全体のSTART/RESTART/STOP意味を保てる転送経路が必要である。

#### 3.7.5 アドレス処理

7-bitアドレスの場合、Linux実装は `i2c_8bit_addr_from_msg()` でread/writeビットを含むアドレス値を生成し、`chip_address` に設定する。

10-bitアドレスでは、上位アドレスバイトと下位アドレスバイトを別々に送信し、readの場合はRESTART後にreadビット付き上位バイトを再送する処理がある。EDID/DDCの初期用途は7-bitアドレス（0x50、必要に応じて0x30）だが、NetBSD側が対応するI2C機能の範囲はbridgeの実装時に明示する。

#### 3.7.6 クロック設定

Linuxドライバは対応する離散的な周波数テーブルを持つ。DTSの `clock-frequency = <97500>` に対しては、SCL選択値 `SPD_390K`（選択フィールド値1）とDIV_CLKを設定する。

このため、NetBSD初期実装ではDTS値97500Hzを読み取り、対応する `ctl_reg` 設定を行う。任意の周波数を計算して設定する実装ではなく、Linuxと同様にサポートする値を明確に扱う。

#### 3.7.7 エラーと未確定事項

Linux実装では、busy待ち・転送完了待ちの両方に100msのタイムアウトがある。通常のread/writeでNACKを検出するとリモートI/Oエラーを返す。NetBSD側ではNetBSDのエラー値へ適切に対応付ける。

Linuxコードの確認で転送チャンク上限32バイト、4バイトレジスタ幅、完了判定、NACK判定、メッセージ間条件の実装方針は確認できた。一方、実機での信号波形・タイミング、DDC拡張ブロックを含むEDID取得、実際の起動DTBでのノード有効状態は引き続き実機検証事項である。

---

## 4. 設計方針

### 4.1 BCM2711専用ドライバを新設する

既存のBCM2835 BSCドライバは変更せず、BCM2711 HDMI DDC専用ドライバを新設する。

予定ファイル:

```
sys/arch/arm/broadcom/
    bcm2711_hdmi_i2c.c
    bcm2711_hdmi_i2c_reg.h
```

デバイス名:

```
bcm2711ddc
```

FDT compatible:

```
brcm,bcm2711-hdmi-i2c
```

### 4.2 FDT attach

FDTから次の領域を取得する。

- `bsc`
- `auto-i2c`

それぞれ`bus_space_map()`でマッピングする。

クロックについては、BCM2711 DDCノードに`clocks`プロパティがない構成を確認しているため、既存BCM2835 BSCのようにFDT clock取得を必須にしない。

### 4.3 BSC解放処理

Linux実装に合わせ、以下を行う。

1. `auto-i2c + 0x26c`へ`BIT(1)`を書き込む。
2. `bsc + 0x2c`へ0を書き込む。

この処理はBCM2711専用ドライバに実装する。

### 4.4 転送方式

初期実装はポーリング方式とする。

Linux実装の動作を参考にし、以下を実装する。

- I2Cアドレス設定
- 書き込みデータの設定
- 読み込みデータの取得
- 転送長設定
- 転送方向設定
- START/STOP/RESTART制御
- NACK検出
- タイムアウト処理

データレジスタの数と幅から転送単位を確認し、必要に応じて転送を分割する。

転送単位、FIFO相当の扱い、最大転送長はLinux実装とハードウェア仕様を照合して確定する。

### 4.5 NetBSD I2C接続

最終的にはNetBSDの`struct i2c_controller`として登録する。

必要な関数:

```
ic_acquire_bus
ic_release_bus
ic_exec
```

`ic_exec`からBCM2711専用転送関数を呼び出す。

I2Cバスの排他制御はNetBSDのI2C frameworkに従う。

### 4.6 LinuxKPI adapter bridge

VC4側は`struct i2c_adapter`を必要とする。

NetBSDの`i2c_controller`とLinuxKPIの`i2c_adapter`を接続するbridgeを用意する。

bridgeの責務:

- `i2c_adapter`の生成
- `i2c_algorithm`の登録
- `i2c_transfer()`からNetBSD I2C転送への変換
- ロック処理の変換
- adapterとFDT phandleの対応付け
- adapterの寿命管理

LinuxKPIの既存`i2c_add_adapter()`等はstubであるため、必要な管理機構を調査して実装する。

### 4.7 VC4 HDMI接続

最終的には`vc4_hdmi.c`のDDC取得部分をNetBSD向けに接続する。

Linuxの以下のAPIをそのまま使うことはできない可能性が高い。

```
of_find_i2c_adapter_by_node()
put_device()
```

NetBSD向けのadapter検索・解放方法を設計する。

---

## 5. 実装段階

| 段階 | 内容                          | 状態     |
| -- | --------------------------- | ------ |
| 1  | BCM2711専用ドライバ骨格・FDT attach  | 作業開始前  |
| 2  | 2領域のmapとBSC解放処理             | 段階1に含む |
| 3  | レジスタ定義の専用ヘッダー化              | 未着手    |
| 4  | ポーリングによるI2C転送               | 未着手    |
| 5  | NetBSD I2C controller登録     | 未着手    |
| 6  | LinuxKPI I2C adapter bridge | 未着手    |
| 7  | VC4 HDMI DDC接続              | 未着手    |
| 8  | EDID取得・HDMI表示確認             | 未着手    |

各段階でビルドを確認し、問題を小さく切り分ける。

---

## 6. 未確認事項・リスク

### 6.1 実機Device Tree

DTSの`status = "disabled"`は確認できているが、実機起動時のDTBやoverlay適用後にDDCノードが有効になっているかは未確認。

実機のDevice Treeで以下を確認する必要がある。

- DDC0/1のstatus
- reg / reg-names
- clock-frequency
- interruptsの有無
- HDMIノードのDDC phandle

### 6.2 BCM2711転送タイミング

Linux実装のレジスタ制御をNetBSDに移植する際、次を実機で検証する。

- busy状態の解除
- NOACK検出
- 転送完了判定
- タイムアウト
- RESTART条件
- 長い転送の分割
- EDIDのsegment pointer対応

### 6.3 EDID拡張ブロック

EDIDの先頭ブロックだけでなく、CTA拡張ブロック等の読み出しも確認する。

DDCでは通常、EDIDのsegment pointerアドレス0x30とEDIDアドレス0x50を使用する場合がある。

NetBSDの既存EDID読み出し処理が拡張ブロックを正しく扱えるか、実装を確認する必要がある。

### 6.4 I2C API変換

Linuxの`i2c_transfer()`は複数メッセージを一連のトランザクションとして処理できる。

NetBSDの`iic_exec()`は1回の呼び出しで1つのI2Cアドレスを扱うため、異なるアドレスを含む複数メッセージをどう変換するかが課題となる。

特にDDCの0x30と0x50を連続して扱うケースを確認する。

### 6.5 adapterの寿命管理

Linuxの`put_device()`とNetBSD LinuxKPIのデバイス構造は同一ではない。

参照カウントやdetach時の処理を含め、NetBSD側の管理方法を明確にする必要がある。

---

## 7. 現時点の結論

BCM2711 HDMI DDCは既存BCM2835 BSCとは別のハードウェアインターフェースであり、専用ドライバを新設する方針とする。

まずFDTから2つのレジスタ領域を取得し、BSC解放処理を行うところまで実装する。

その後、ポーリングによる転送、NetBSD I2C controller登録、LinuxKPI adapter bridge、VC4 HDMI接続の順に進める。

既存BCM2835 BSCの動作を壊さないことを重視し、段階ごとにビルド・実機確認を行う。

---

## 8. 参考ソース

- NetBSD作業ブランチ: [https://github.com/ebijun/src/tree/vc4-drmkms](https://github.com/ebijun/src/tree/vc4-drmkms)
- Linux BCM2711/BCMSTB I2C driver: [https://github.com/torvalds/linux/blob/master/drivers/i2c/busses/i2c-brcmstb.c](https://github.com/torvalds/linux/blob/master/drivers/i2c/busses/i2c-brcmstb.c)
- NetBSD BCM2835 BSC driver: `sys/arch/arm/broadcom/bcm2835_bsc.c`
- NetBSD BCM2835 BSC FDT attach: `sys/arch/arm/broadcom/bcm2835_bsc_fdt.c`
- NetBSD LinuxKPI I2C: `sys/external/bsd/drm2/include/linux/i2c.h`
- NetBSD VC4 HDMI: `sys/external/bsd/drm2/dist/drm/vc4/vc4_hdmi.c`

## 9. NetBSD `-current`への追従方針

VC4 DRM/KMS移植はNetBSD `-current`のカーネルAPI、LinuxKPI、DRM2、FDT、ARM Broadcom関連コードに依存する。そのため、長期間ベースコードから乖離しないよう、定期的に作業ブランチへ `trunk` の変更を取り込む。

### 9.1 追従頻度

基本方針は次のとおり。

- 通常の開発中は週1回を目安に追従を検討する。
- 大きな実装段階（DDC FDT attach、I2C転送、LinuxKPI bridge、VC4 HDMI接続など）が完了した時点でも追従する。
- DRM2、LinuxKPI、FDT、bus_space、ARM Broadcom関連に大きな変更があった場合は、週次を待たずに内容を確認する。
- 開発を長期間停止していた場合は、再開時にまず差分を確認する。

これは機械的に毎週mergeするという意味ではない。変更内容を確認し、作業の区切りとビルド可能な状態を確保してから取り込む。

### 9.2 追従前の確認

作業ブランチに未コミット変更がないことを確認する。未コミット変更がある場合は、先に内容を整理してコミットするか、安全な方法で退避する。

```sh
git switch vc4-drmkms
git status --short
git fetch origin
```

取り込まれていない `trunk` のコミットと変更規模を確認する。

```sh
git log --oneline --decorate HEAD..origin/trunk
git diff --stat HEAD...origin/trunk
```

特に次の領域に関係する変更を確認する。

- `sys/external/bsd/drm2/`
- `sys/dev/i2c/` および LinuxKPI I2C関連
- `sys/dev/fdt/`
- `sys/arch/arm/broadcom/`
- カーネル構成・ビルドシステム関連

### 9.3 取り込みと検証

差分を確認したうえで、作業ブランチへmergeする。

```sh
git merge --no-ff origin/trunk
```

コンフリクトが発生した場合は、NetBSD本体側の変更を無条件に採用せず、VC4移植側の意図と照合して解決する。解決後は差分を確認し、対象カーネルをビルドする。

ビルドが成功したら作業ブランチをpushする。

```sh
git status
git log --oneline -5
git push origin vc4-drmkms
```

共有済みの作業ブランチでは、履歴を書き換えるrebaseよりmergeを基本とする。追従mergeとVC4移植の機能変更は、原因を追跡しやすくするため別々のコミットにする。

### 9.4 ビルド失敗時

追従merge後にビルドが失敗した場合は、まずエラーがNetBSD `trunk` のAPI変更によるものか、既存のVC4移植コードに起因するものかを切り分ける。merge直後の状態とビルドログを保存し、修正を小さな単位で行う。原因が分からないまま追加の機能変更を重ねない。



---

## 10. GitHub Actionsによるクロスビルド確認

### 10.1 ワークフロー構成

作業ブランチにはGitHub Actionsのワークフローが設定されている。

ファイル:

```
.github/workflows/kernel-build.yml
```

ワークフロー名:

```
NetBSD Kernel Cross-Compile CI
```

トリガーは次のとおり。

- `vc4-drmkms` ブランチへのpush
- `main` ブランチへのpush
- `main` を対象とするpull request

現在の作業ブランチ `vc4-drmkms` へのpushで自動実行される。

実行環境とビルド対象:

| 項目 | 設定 |
| --- | --- |
| 実行環境 | `ubuntu-latest` |
| NetBSD machine | `evbarm` |
| ABI | `earmv7hf` |
| カーネル構成 | `GENERIC` |
| 並列度 | `-j4` |
| ビルド | build.shでtoolsおよびkernel=GENERIC |
| 成果物 | `obj/sys/arch/evbarm/compile/GENERIC/netbsd` |

成功時は `netbsd-kernel-earmv7hf` という名前でカーネルをArtifactとして保存する。

このCIはクロスコンパイル確認であり、Raspberry Pi 4実機での起動・HDMI表示・DDC通信を保証するものではない。また、現行設定はearmv7hfのみであり、AArch64（aarch64/earmv8）ビルドは対象外である。

### 10.2 2026-11-23時点の実行結果

確認対象:

- Run #67
  - commit: `f53ee9b37f3f12e412d4c42bbfbcb56143e67eee`
  - URL: https://github.com/ebijun/src/actions/runs/45153182432
  - 結果: **failure**
- Run #66
  - commit: `4e8518db1f025a53aa031ef93dda837f9c6d3e7b`
  - URL: https://github.com/ebijun/src/actions/runs/45152966915
  - 結果: **failure**

Run #67は当初確認時には実行中だったが、その後GitHub Actions上で完了し、failureとなった。Run #66とRun #67のログでは、いずれも `vc4_hdmi.o` のコンパイルでビルドが停止している。

### 10.3 主なコンパイルエラー

ログから確認できた主な不足・不整合は次のとおり。

**CEC関連**

- `struct cec_msg` が不完全型
- `struct cec_adap_ops` の定義・メンバーが不足
- `cec_received_msg()`、`cec_transmit_done()`、`cec_allocate_adapter()`、`cec_register_adapter()`、`cec_unregister_adapter()` 等の宣言がない
- `CEC_TX_STATUS_OK`、`CEC_TX_STATUS_NACK` 等の定義がない

**ASoC / HDMI audio関連**

- `snd_soc_card_set_drvdata()`、`devm_snd_soc_register_card()` 等の宣言がない
- ASoC component、DAI、PCM設定に関する構造体定義が不完全
- 関連するstatic定義が未使用となる警告（`-Werror`でエラー扱い）

**Device Tree / I2C関連**

- `of_parse_phandle()`、`of_node_put()`、`of_find_i2c_adapter_by_node()` の宣言がない
- Linux `struct device` に期待される `of_node` メンバーがNetBSD LinuxKPI側にない
- `EPROBE_DEFER` が未定義
- `put_device()` の宣言がない

**Clock / GPIO / IRQ / PM関連**

- `devm_clk_get()`、`of_get_named_gpio_flags()`、`devm_request_threaded_irq()` 等の宣言・互換実装が不足
- `struct of_phandle_args` 相当のGPIO関連型が不完全
- `pm_runtime_enable()` が未定義

**ポインタ演算**

- `vc4_hdmi.c` の先頭付近（97～100行）およびCEC処理付近で、`void *`に対する算術演算がエラーとなる
- NetBSDのコンパイラ設定では `-Werror=pointer-arith` によりエラーになるため、適切な型付きポインタまたは整数アドレス型への変換が必要

これらは単にヘッダーを追加すればすべて解決するとは限らない。NetBSD LinuxKPIにAPI自体が存在しないケース、構造体の互換定義が不足しているケース、Linux側の機能をNetBSDで無効化・代替する設計が必要なケースを分けて調査する。

### 10.4 CIを使った開発手順

今後は次のサイクルを基本とする。

1. ローカルで変更を小さく実施する。
2. ローカルで対象カーネルのビルドを行う。
3. ビルドが通った変更をコミットして `vc4-drmkms` にpushする。
4. GitHub Actionsの該当commitの実行結果を確認する。
5. 失敗した場合は最初のコンパイルエラーを起点に原因を分類する。
6. 修正後、再度ローカルビルドとActionsの両方で確認する。

CIの失敗ログは、NetBSD側に不足しているLinuxKPI APIや型定義を発見するための継続的な調査資料として利用する。

GitHub Actionsの結果を確認するときは、ブランチ全体の最新実行だけでなく、対象commit SHAに紐づくrunを確認する。これにより、別のpushによる実行結果との取り違えを防ぐ。

### 10.5 現時点での評価と次の課題

現行CIはNetBSD kernel cross-compileの自動確認手段として有効である。一方、現在はVC4 HDMIソースのCEC、ASoC、Device Tree、I2C、clock、GPIO、IRQ、PMなど複数のLinux API互換性が不足しており、`vc4_hdmi.o` で停止している。

今後はエラーを機能領域ごとに整理し、次の順で扱う。

1. VC4 HDMIの必須表示経路と、初期段階では不要なCEC/ASoC機能を切り分ける。
2. NetBSD LinuxKPIに既存実装があるAPIは、その実装・ヘッダーを調査して不足を補う。
3. Linux専用APIでNetBSDに対応する仕組みがないものは、単純なスタブ追加を避け、NetBSDのFDT、I2C、clock、GPIO、IRQ等の仕組みに置き換える。
4. `void *`ポインタ演算は、対象のレジスタ領域・データ領域に応じた型を明示して修正する。
5. 各修正後にローカルビルドとGitHub Actionsの結果を照合する。

CIが成功しても、実機でのDDC/EDID取得およびHDMI表示確認は別途必要である。


### 10.6 `vc4_hdmi.c` コンパイルエラーの機能別切り分け

Run #67のコンパイルログと作業ブランチ上の `vc4_hdmi.c` を照合し、エラーを機能単位に分類した。ここでの分類は原因と移植上の依存関係を整理したものであり、修正が適用済みであることや、各APIの実機動作を確認したことを意味しない。

#### 10.6.1 レジスタアクセス

対象は `HDMI_READ` / `HDMI_WRITE` および関連するレジスタアクセスマクロ。レジスタベースの型が `void *` のままバイトオフセット演算されると、NetBSDの `-Werror=pointer-arith` で失敗する。

修正時は、レジスタ幅・アクセスAPIに合わせた型付きポインタ（例: `uint8_t *`）を使用する。HDMI core側だけでなく、HD側など同種のマクロも確認する。これは機能依存が少なく、最初に対処できる局所的な型問題である。

#### 10.6.2 HDMI CEC

CEC部分では、次のようなLinux CEC frameworkの型・関数が不足している。

- `struct cec_msg`、`struct cec_adap_ops`
- `cec_get_drvdata()`
- `cec_allocate_adapter()`、`cec_register_adapter()`、`cec_unregister_adapter()`
- `cec_transmit_done()`、`cec_received_msg()`
- `CEC_CAP_*`、`CEC_TX_STATUS_*` 等の定数

関数宣言だけを追加してもCEC framework自体の互換性は得られない。CECは初期のHDMI映像出力・DDC/EDID確認には必須ではないため、初期ビルドではCEC機能を無効化し、CEC framework移植を後段に分離する方針が適切である。

ソースには `CONFIG_DRM_VC4_HDMI_CEC` による条件コンパイルが存在する。NetBSD側では `sys/external/bsd/drm2/drm/files.drmkms` のVC4設定にある `makeoptions vc4drm CPPFLAGS+="-DCONFIG_DRM_VC4_HDMI_CEC"` が定義元である（10.7節参照）。

#### 10.6.3 HDMI Audio / ASoC

Audio部分ではLinux ASoCに属する以下の型・関数が不足している。

- `struct snd_soc_card`、`struct snd_soc_dai_link`、`struct snd_soc_dai_link_component`
- `struct snd_soc_component_driver`、`struct snd_soc_dai_ops`、`struct snd_soc_dai_driver`
- `struct snd_pcm_hardware`
- `snd_soc_card_set_drvdata()`、`devm_snd_soc_register_card()`、`devm_snd_soc_register_component()` 等

これは単なるヘッダー不足ではなく、ASoCサブシステムの移植範囲に関係する。CECとは独立した機能であり、CECを無効化してもAudio部分は残る。現状の `vc4_hdmi.c` ではAudioコードはCECとは独立して無条件にコンパイルされる。NetBSD側のAudio専用opt-inマクロを新設して分離する方針を10.7節に記録する。初期の映像/DDC確認ではAudioを後段に回す。

#### 10.6.4 Device Tree / DDC / I2C

HDMI DDC adapter取得処理はLinuxのOF・I2C device管理を前提としている。

- `of_parse_phandle()`、`of_node_put()`
- `of_find_i2c_adapter_by_node()`
- `put_device()`
- `EPROBE_DEFER`
- Linux `struct device` の `of_node` メンバー

NetBSD LinuxKPIの `struct i2c_adapter.dev` はLinuxの `struct device` と同じではない。前方宣言や関数プロトタイプだけを追加しても、ノードとadapterの関連付け・寿命管理は解決しない。

BCM2711 HDMI DDC専用のNetBSD I2C controllerを用意し、LinuxKPI `i2c_adapter` bridgeを介してVC4に接続する設計を基本とする。既存BCM2835 BSCにcompatibleを追加するだけの対応は行わない。

#### 10.6.5 Clock / Runtime PM

不足APIとして `devm_clk_get()` および `pm_runtime_enable()` が確認されている。ClockはHDMIハードウェア初期化に関係するため、NetBSD側の既存clock frameworkと接続する方法を調べる。Runtime PMは空のスタブで済ませず、NetBSDの電源管理との整合を確認する。初期段階で固定電源状態を利用できるかは、初期化・停止経路を確認して判断する。

#### 10.6.6 HPD GPIO

HPD取得部分では `enum of_gpio_flags`、`of_get_named_gpio_flags()`、`OF_GPIO_ACTIVE_LOW` 等が不足している。Linux OF GPIO APIをそのまま宣言するのではなく、NetBSD FDT GPIO APIおよびGPIO frameworkに置き換える。HPDは接続検出に関係するため、固定接続を仮定した試験で代替できるかは検出経路と実機条件を確認して判断する。

#### 10.6.7 IRQ

`devm_request_threaded_irq()` のエラーはCEC IRQ登録経路に属する。通常のHDMI表示経路で必要となるIRQと混同しない。初期段階でCECを無効化する場合は、CEC用threaded IRQ登録も同じ条件で除外する。NetBSDの通常IRQ登録とLinuxKPI threaded IRQ互換層は別の課題として扱う。

#### 10.6.8 段階的な対応順序

| 段階 | 対応内容 | 目的 |
| --- | --- | --- |
| Phase 1 | レジスタポインタ演算の修正。CECとAudioを独立してビルド対象から分離 | `vc4_hdmi.o` の不要な依存を減らし、基本コードのコンパイルを進める |
| Phase 2 | Clock、FDT、HPD GPIO、Runtime PMのNetBSD側接続方針を確定 | HDMI初期化・接続検出に必要な基盤を整える |
| Phase 3 | BCM2711 HDMI DDC専用I2C controllerとLinuxKPI adapter bridgeを実装 | VC4からDDC通信を利用可能にする |
| Phase 4 | EDID取得、モード検出、HDMI表示を実機確認 | 基本映像経路を検証する |
| Phase 5 | CEC frameworkおよびHDMI Audio / ASoCを個別に移植 | 追加機能を段階的に有効化する |

Phase 1では、CECを無効化する設定がどこで定義されているかを先に確認する。またAudioはCECとは別の条件で囲む必要がある。条件コンパイルの変更前に、NetBSD側のVC4 Makefile、Kconfig相当の設定、LinuxKPIのconfig headerおよびコンパイルフラグを調査する。

各段階で最初のエラーから順に修正し、ローカルビルドとGitHub Actionsの両方で確認する。コンパイルが通ったことと、DDC/EDID・HDMI表示が実機で動作することは別々に判定する。


### 10.7 CEC設定マクロとAudio条件コンパイルの調査

2026-11-23時点の `vc4-drmkms` ブランチを調査し、CECマクロの定義元とAudioコードの条件コンパイル範囲を確認した。

#### 10.7.1 CECマクロの定義元

定義元はLinux Kconfigではなく、NetBSD側の次のファイルである。

```
sys/external/bsd/drm2/drm/files.drmkms
```

同ファイルの「Broadcom VC4 DRMKMS」節に、次の設定がある。

```make
makeoptions	vc4drm	CPPFLAGS+="-DCONFIG_DRM_VC4_HDMI_CEC"
```

VC4の各ソースファイルは同節で `vc4drm` 条件によりカーネルへ組み込まれる。このため、上記makeoptionsがVC4コンパイル時の `CONFIG_DRM_VC4_HDMI_CEC` を定義し、CECコードを有効化している。CECの有効化はカーネルconfigの `options` ではなく、`files.drmkms` のコンパイルフラグによる。

調査した `sys/external/bsd/drm2/dist/drm/vc4/` には `Kconfig` と `Makefile` は存在しない。したがって、Linux側KconfigでCECを選択解除する方法ではなく、NetBSD側の `files.drmkms` からこのdefineを外すのが最小の設定変更となる。

#### 10.7.2 CECを無効化する際のソース側注意点

`vc4_hdmi.c` にはCEC関連の条件コンパイルが既に存在するが、CEC依存がすべて囲まれているわけではない。現状確認できた箇所は次のとおり。

- `#include "media/cec.h"` が無条件。
- `struct vc4_hdmi` 内の `struct cec_adapter *`、`struct cec_msg` およびCEC状態フィールドが無条件。
- CEC IRQ handler、CEC adapter ops、bind時のadapter登録・IRQ登録・エラー処理は `CONFIG_DRM_VC4_HDMI_CEC` で条件コンパイルされている。
- `vc4_hdmi_unbind()` 内の `cec_unregister_adapter()` は無条件。

したがって、`files.drmkms` からdefineを削除するだけでは、CECヘッダーや型が不足している環境でコンパイルが通るとは限らない。CECを完全に無効化するには、CECヘッダー、構造体メンバー、unbind時の解除処理も同じ `#ifdef CONFIG_DRM_VC4_HDMI_CEC` で保護する必要がある。既存のCEC handler・bind処理のガードは維持する。

#### 10.7.3 Audioコードの現状

現行 `vc4_hdmi.c` ではAudio/ASoC関連が無条件にコンパイルされる。主な範囲は次のとおり。

- ファイル先頭の `sound/dmaengine_pcm.h`、`sound/pcm_drm_eld.h`、`sound/pcm_params.h`、`sound/soc.h` のinclude。
- `struct vc4_hdmi_audio` と `struct vc4_hdmi` 内の `audio` メンバー。
- HDMI Audio infoframeを作る `vc4_hdmi_set_audio_infoframe()`。
- DAI、PCM、ASoC component、controls、DAPM、DMA設定などのAudio callback群。
- `vc4_hdmi_audio_init()`。
- bind末尾の `vc4_hdmi_audio_init(hdmi)` 呼び出し。

CECの条件マクロはAudioコードを保護しない。CECを無効化してもASoC型・関数のコンパイルエラーは残る。

#### 10.7.4 Audioを分離する条件コンパイル案

NetBSD側のVC4 HDMI Audio専用opt-inマクロとして `CONFIG_DRM_VC4_HDMI_AUDIO` を設ける案を推奨する。初期段階では `files.drmkms` にこのマクロを定義せず、ASoC互換層が整った段階で明示的に有効化する。

条件コンパイルは以下の依存範囲を一体として扱う。

1. 4つのsoundヘッダーinclude。
2. `struct vc4_hdmi_audio` と `struct vc4_hdmi` の `audio` メンバー。
3. `vc4_hdmi_set_audio_infoframe()`。
4. Audio callback・ASoC component・DAI・PCM・DMA定義一式（`vc4_hdmi_audio_set_mai_clock()` から `vc4_hdmi_audio_init()` まで）。
5. bind関数内の `vc4_hdmi_audio_init()` 呼び出しと、その戻り値によるエラー分岐。

概念的なガード配置は次の形とする。

```c
#ifdef CONFIG_DRM_VC4_HDMI_AUDIO
#include <sound/dmaengine_pcm.h>
#include <sound/pcm_drm_eld.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#endif

#ifdef CONFIG_DRM_VC4_HDMI_AUDIO
struct vc4_hdmi_audio {
        /* existing ASoC / PCM state */
};
#endif

struct vc4_hdmi {
        /* common HDMI state */
#ifdef CONFIG_DRM_VC4_HDMI_AUDIO
        struct vc4_hdmi_audio audio;
#endif
};

#ifdef CONFIG_DRM_VC4_HDMI_AUDIO
static void vc4_hdmi_set_audio_infoframe(struct drm_encoder *encoder)
{
        /* existing implementation */
}

/* All ASoC, PCM, DAI and DMA callbacks and definitions. */

static int vc4_hdmi_audio_init(struct vc4_hdmi *hdmi)
{
        /* existing implementation */
}
#endif

/* In vc4_hdmi_bind(): */
#ifdef CONFIG_DRM_VC4_HDMI_AUDIO
        ret = vc4_hdmi_audio_init(hdmi);
        if (ret)
                goto err_destroy_encoder;
#endif
```

上記はガード範囲を示す設計例であり、そのまま適用する完成パッチではない。実際の編集では既存関数の開始・終了位置を確認し、Audio専用関数の未使用警告やAudio初期化のエラー経路も含めて整合させる。Linux upstreamのKconfig設定を変更するのではなく、NetBSD側でAudioをopt-inにするための移植用スイッチとして扱う。

#### 10.7.5 推奨する最小変更順序

| 順序 | 対象 | 変更 |
| --- | --- | --- |
| 1 | `sys/external/bsd/drm2/drm/files.drmkms` | `-DCONFIG_DRM_VC4_HDMI_CEC` を一旦削除 |
| 2 | `vc4_hdmi.c` | CEC include、構造体メンバー、unbindの解除処理を既存CECマクロで保護 |
| 3 | `vc4_hdmi.c` | Audio関連一式を `CONFIG_DRM_VC4_HDMI_AUDIO` で保護 |
| 4 | `files.drmkms` | ASoC移植が完了するまでAudioマクロは定義しない |
| 5 | ビルド | `vc4_hdmi.o` を再ビルドし、次の未解決機能を確認 |

CECとAudioを別々に制御することで、CEC frameworkやASoCを移植する前でも、HDMI映像・DDC/EDID経路のコンパイルを先に進められる。

なお、この節は調査と設計方針の記録であり、上記のソース変更はまだ実施していない。次段階ではまずCECのdefine削除とCEC依存箇所のガードを小さな変更として行い、ビルド結果を確認してからAudioガードを追加する。


### 10.8 CECのみを無効化する変更計画（ソース編集前の調査）

本節は、Audioを変更せずCECだけを無効化するための差分範囲を、現在の `vc4_hdmi.c` と `files.drmkms` に基づいて整理したもの。ここでは設計と変更対象を記録し、ソースコード自体はまだ変更しない。

#### 10.8.1 ビルド設定

対象ファイル:

```
sys/external/bsd/drm2/drm/files.drmkms
```

VC4設定内の次の1行を削除する。

```make
makeoptions	vc4drm	CPPFLAGS+="-DCONFIG_DRM_VC4_HDMI_CEC"
```

include pathの指定、VC4ソースファイルの `file ... vc4drm` 登録、Audio関連の設定は変更しない。これによりCEC用defineだけがコンパイルコマンドから外れる。カーネル構成を再生成・再ビルドした後、実際のコンパイルコマンドにdefineが残っていないことも確認する。

#### 10.8.2 vc4_hdmi.cで追加保護が必要な箇所

現状のソース調査では、CEC関連コードの一部は既に `#ifdef CONFIG_DRM_VC4_HDMI_CEC` で保護されているが、次の参照は無条件のままである。defineを削除する前に、これらを同じマクロで保護する必要がある。

| 箇所 | 現状 | 変更方針 |
| --- | --- | --- |
| `#include "media/cec.h"` | 無条件include | CECマクロで囲む |
| `CEC_CLOCK_FREQ` / `CEC_CLOCK_DIV` | CEC用定数 | CECマクロで囲むか、未使用なら削除 |
| `struct vc4_hdmi` のCECメンバー4個 | 無条件メンバー | CECマクロで囲む |
| `vc4_hdmi_connector_detect()` 内2か所 | `cec_phys_addr_invalidate()` が無条件 | 各呼び出しをCECマクロで囲む |
| `vc4_hdmi_connector_get_modes()` 内 | `cec_s_phys_addr_from_edid()` が無条件 | 呼び出しをCECマクロで囲む |
| `vc4_hdmi_unbind()` | `cec_unregister_adapter()` が無条件 | 呼び出しをCECマクロで囲む |

一方、次の箇所は現状既にCECマクロで保護されているため、ガードを維持する。

- CEC IRQ handler、CECメッセージ処理、CEC adapter ops（約1133行以降）
- `vc4_hdmi_bind()` の `struct cec_connector_info` ローカル変数
- CEC adapter生成、CECハードウェア設定、threaded IRQ登録、adapter登録（bind内）
- `err_delete_cec_adap` および `err_destroy_conn` のエラーラベルと処理

#### 10.8.3 追加するガードの形

includeと定数は次のように保護する。

```c
#ifdef CONFIG_DRM_VC4_HDMI_CEC
#include "media/cec.h"

#define CEC_CLOCK_FREQ 40000
#define CEC_CLOCK_DIV  (HSM_CLOCK_FREQ / CEC_CLOCK_FREQ)
#endif
```

CECメンバーは共通HDMI状態構造体の中でまとめて保護する。

```c
struct vc4_hdmi {
        /* common HDMI state */

#ifdef CONFIG_DRM_VC4_HDMI_CEC
        struct cec_adapter *cec_adap;
        struct cec_msg cec_rx_msg;
        bool cec_tx_ok;
        bool cec_irq_was_rx;
#endif
};
```

connector detect内の2か所とEDID取得後の呼び出しも個別に保護する。

```c
#ifdef CONFIG_DRM_VC4_HDMI_CEC
        cec_phys_addr_invalidate(vc4->hdmi->cec_adap);
#endif
```

```c
#ifdef CONFIG_DRM_VC4_HDMI_CEC
        cec_s_phys_addr_from_edid(vc4->hdmi->cec_adap, edid);
#endif
```

unbind時も同様にする。

```c
#ifdef CONFIG_DRM_VC4_HDMI_CEC
        cec_unregister_adapter(hdmi->cec_adap);
#endif
        vc4_hdmi_connector_destroy(hdmi->connector);
```

これらはガード配置を示す抜粋であり、関数全体の置換コードではない。実際の編集では既存インデントと周辺エラー経路を維持する。

#### 10.8.4 変更しない範囲

今回の「CECだけを無効化」では、次の項目を変更しない。

- Audio / ASoC関連include、構造体、callback、初期化関数と `vc4_hdmi_audio_init()` 呼び出し
- HDMI DDC/I2C、EDID取得、HPD、clock、Runtime PM
- HDMI本体のIRQや通常の表示経路
- CECレジスタ定義そのもの（CECコードが無効化されてもレジスタヘッダー全体を削除しない）

したがって、この変更でCEC関連のコンパイル依存は減るが、Audio/ASoCおよびOF/I2C/clock/GPIO/PM関連のエラーは残る見込みである。CEC無効化の成否とAudioの問題を混同しない。

#### 10.8.5 適用後の確認手順

1. `files.drmkms` からCEC defineだけを削除する。
2. 上表の無条件CEC参照を `#ifdef CONFIG_DRM_VC4_HDMI_CEC` で保護する。
3. `vc4_hdmi.c` の差分を確認し、Audio関連行に変更がないことを確認する。
4. カーネル構成・ビルド環境に応じて必要な再生成を行い、古いオブジェクトを使わず `vc4_hdmi.o` を再コンパイルする。
5. コンパイルコマンドに `-DCONFIG_DRM_VC4_HDMI_CEC` が残っていないことを確認する。
6. CEC関連の不完全型・未定義関数エラーが消えたか確認する。
7. 次に残るエラーを別機能（特にAudio/ASoC）として記録し、CEC変更と同じコミットに混在させない。

この変更はCEC機能を無効にするもので、HDMI CEC端子の動作確認ではない。また、CECコードを条件コンパイルで除外しても、CEC以外のエラーが原因で `vc4_hdmi.o` 全体のビルドが失敗する可能性はある。


### 10.9 GitHub Actions Run #72 の失敗分析（2026-11-23）

対象は Run #72、commit `5e6b1b11f04f3a25a40cc39377db5e5d45bc29cd`（CEC無効化）、job `build-kernel`、`earmv7hf / evbarm / GENERIC`。Checkoutと依存導入は成功し、`vc4_hdmi.o` のコンパイルで停止した。ログ上、CEC frameworkの型・関数に関するエラーは見当たらず、CECを無効化した目的に対しては前進した。ただしオブジェクト全体は以下の別機能の問題で失敗している。

#### 10.9.1 Audio / ASoC

ログでは `struct snd_soc_component_driver`、`snd_soc_dai_ops`、`snd_soc_dai_driver`、`snd_soc_dai_link`、`snd_soc_card`、`snd_dmaengine_pcm_config` が不完全、または必要なメンバーを持たない。さらに `snd_soc_card_set_drvdata()`、`devm_snd_soc_register_card()`、`devm_snd_soc_register_component()`、`devm_snd_dmaengine_pcm_register()`、`snd_soc_dai_init_dma_data()` 等が未宣言で、PCM rate/format/streamやDMA bus-width定数も不足している。Audio controls/widgets/routesが未使用となる警告も発生し、`-Werror` で停止する。

これは単純なinclude不足ではなく、NetBSD DRM2 sound互換層にLinux ASoCの構造体定義・メンバー・関数群が不足していることを示す。前方宣言では構造体メンバー参照や実体サイズを解決できない。初期の映像/DDC移植からAudioを分離するため、10.7.4節の方針どおり `CONFIG_DRM_VC4_HDMI_AUDIO` をNetBSD側opt-inスイッチとして導入し、Audio用include、構造体メンバー、callback/DAI/PCM/DMA定義、bind時のAudio初期化とエラー経路を一括してガードする。Audio専用static変数もガード内に含め、unused警告を残さない。ASoC移植完了後に再度有効化する。

#### 10.9.2 Clock

`devm_clk_get()` が暗黙宣言となり、戻り値がintと扱われて `struct clk *` への変換警告が出ている。前方宣言だけでは解決しない。DRM2 LinuxKPIに既存の宣言・実装があるか調べ、なければNetBSD clock frameworkおよびFDT clock取得との接続を設計する。仮のプロトタイプだけを追加してリンクを通す対応は避ける。

#### 10.9.3 OF / Device Tree / DDC I2C

`of_parse_phandle()`、`of_node_put()`、`of_find_property()`、`of_get_address()`、`of_find_i2c_adapter_by_node()` が未宣言。`struct device` に `of_node` がなく、`EPROBE_DEFER` と `put_device()` も不足する。これはLinux OF/device modelとNetBSD LinuxKPIの構造差であり、宣言を足すだけではnodeの取得・参照寿命やI2C adapterとの関連付けを解決しない。NetBSD FDT nodeの取得方法、BCM2711 HDMI DDC専用I2C controller、LinuxKPI `i2c_adapter` bridgeを一体で設計する。`EPROBE_DEFER` も単なる定数追加ではなく、DDC controller未attach時のprobe/bind延期方針が必要。`put_device()` はNetBSD側オブジェクト寿命との対応を確認してから扱う。

#### 10.9.4 HPD GPIO

`enum of_gpio_flags` のサイズ不明、`of_get_named_gpio_flags()` 未宣言、`OF_GPIO_ACTIVE_LOW` 未定義、および `dev->of_node` 不在が報告された。`hpd_gpio_flags` のunusedはGPIO API不成立に伴う二次エラー。NetBSD FDT GPIO APIとGPIO controllerの取得方法に置き換える。

#### 10.9.5 HD register access

次のHD側マクロが `void *` に対してオフセット演算しており、`-Werror=pointer-arith` で失敗する。

```c
#define HD_READ(offset) readl(vc4->hdmi->hd_regs + offset)
#define HD_WRITE(offset, val) writel(val, vc4->hdmi->hd_regs + offset)
```

レジスタ領域の宣言型とMMIO helperの期待型を確認した上で、バイトオフセットなら例えば次のように型を明示する。

```c
#define HD_READ(offset) \\
        readl((uint8_t *)vc4->hdmi->hd_regs + (offset))
#define HD_WRITE(offset, val) \\
        writel((val), (uint8_t *)vc4->hdmi->hd_regs + (offset))
```

HDMI core側の `HDMI_READ/HDMI_WRITE` にも同種の問題がないか確認する。

#### 10.9.6 Runtime PM

`pm_runtime_enable()` が未宣言で、コンパイラは `pm_runtime_disable()` を候補として示す。LinuxKPI Runtime PM互換層にenable操作が存在するか、宣言だけ不足しているかを調べる。空のスタブは避け、bind/unbindの対称性とHDMI blockの電源・clock状態を保証する方法を確認する。

#### 10.9.7 対応順序

| 順序 | 領域 | 対応 |
| --- | --- | --- |
| 1 | Audio / ASoC | 専用opt-inマクロで全体を分離 |
| 2 | HD register access | MMIO型を確認しvoid pointer演算を修正 |
| 3 | Clock | 既存LinuxKPI実装とNetBSD clock接続を調査 |
| 4 | OF / DDC I2C | NetBSD FDTとI2C bridgeへ置換 |
| 5 | HPD GPIO | NetBSD FDT GPIO APIへ置換 |
| 6 | Runtime PM | 既存PM互換層と電源管理方針を確認 |

Run #72は `vc4_hdmi.o` で停止しているため、他のVC4 objectやカーネルリンクの成否はこのRunから判断しない。次の独立変更はAudio分離とし、CEC無効化の差分とは分ける。各runはbranch最新状態だけでなく対象commit SHAと対応づけて記録する。CIコンパイル成功と実機のDDC/EDID・HDMI表示・Audio動作は別々に判定する。


### 10.10 Audio / ASoC分離およびHD register型修正の実施計画

本節はRun #72で残ったAudio/ASoCとHD register accessのコンパイル問題について、次のソース変更の境界・確認項目を定義する。ここでは設計方針を記録し、`vc4_hdmi.c` と `files.drmkms` の実変更はまだ行わない。変更はAudio分離とMMIO型修正の2件に分け、差分とCI結果を個別に確認できるようにする。

#### 10.10.1 Audio分離の目的とビルド方針

NetBSD側のVC4 HDMI初期移植では、まずHDMI映像・connector・DDC/EDID経路をコンパイル可能にする。Linux ASoC互換層が不足しているAudioは、CECと同様にNetBSD側のopt-inマクロ `CONFIG_DRM_VC4_HDMI_AUDIO` で独立して無効化する。

- Audio無効が既定状態であり、`files.drmkms` では `-DCONFIG_DRM_VC4_HDMI_AUDIO` を定義しない。
- CEC用 `CONFIG_DRM_VC4_HDMI_CEC` とAudio用マクロを混同しない。
- Linux upstreamのKconfigやLinux本来の機能構成は変更しない。これはNetBSD移植ビルド用の制御である。
- Audio機能を削除するのではなく、ASoC互換層の移植後に再有効化できる状態で保持する。
- Audio無効時にもHDMI映像・DDC/EDIDに必要なコード、共通HDMI register access、connector処理はコンパイル対象に残す。

#### 10.10.2 Audioガードの対象範囲

対象ファイルは `sys/external/bsd/drm2/dist/drm/vc4/vc4_hdmi.c`。現行ソースではsoundヘッダーが無条件includeであり、`struct vc4_hdmi_audio` と `struct vc4_hdmi.audio` メンバーも無条件で定義されている。Audio callback/DAI/PCM/DMA定義は連続した領域にまとまっている。

次の依存範囲をすべて同じ `#ifdef CONFIG_DRM_VC4_HDMI_AUDIO` で保護する。

1. `<sound/dmaengine_pcm.h>`、`<sound/pcm_drm_eld.h>`、`<sound/pcm_params.h>`、`<sound/soc.h>` の4つのinclude。
2. `struct vc4_hdmi_audio` 定義全体。
3. `struct vc4_hdmi` 内の `struct vc4_hdmi_audio audio;` メンバー。
4. Audio専用の `vc4_hdmi_set_audio_infoframe()` と、そのほかAudioだけから参照される関数・データ。
5. `vc4_hdmi_audio_set_mai_clock()` から `vc4_hdmi_audio_init()` までのASoC component、DAI、PCM、DMA、callback、control/widget/route定義一式。
6. `vc4_hdmi_bind()` 内の `vc4_hdmi_audio_init(hdmi)` 呼び出しと、その戻り値を受けるAudio初期化専用のエラー分岐。

概念的な配置:

```c
#ifdef CONFIG_DRM_VC4_HDMI_AUDIO
#include <sound/dmaengine_pcm.h>
#include <sound/pcm_drm_eld.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#endif

#ifdef CONFIG_DRM_VC4_HDMI_AUDIO
struct vc4_hdmi_audio {
        /* existing Audio/ASoC state */
};
#endif

struct vc4_hdmi {
        /* common HDMI state */
#ifdef CONFIG_DRM_VC4_HDMI_AUDIO
        struct vc4_hdmi_audio audio;
#endif
        /* remaining common HDMI state */
};

#ifdef CONFIG_DRM_VC4_HDMI_AUDIO
/* Audio infoframe helper, ASoC callbacks, DAI/PCM/DMA data,
 * controls/widgets/routes and vc4_hdmi_audio_init().
 */
#endif

/* In vc4_hdmi_bind(): */
#ifdef CONFIG_DRM_VC4_HDMI_AUDIO
        ret = vc4_hdmi_audio_init(hdmi);
        if (ret)
                goto err_destroy_encoder;
#endif
```

このコードはガード範囲の説明であり、そのまま貼り付ける完成パッチではない。実編集時には `vc4_hdmi_set_audio_infoframe()` の全参照元を確認し、Audio以外から呼ばれている場合は共通側に残すか、呼び出し側を適切にガードする。Audio専用static変数・配列・関数がガード外に残ると `-Werror=unused-*` が再発するため、参照関係を確認してまとめて保護する。

bind関数のエラーラベルはCECの `err_delete_cec_adap` / `err_destroy_conn` と独立に扱う。Audio初期化呼び出しをガードしただけで、CEC側の既存ラベルやconnector/encoderのcleanup順序を変更しない。Audio初期化を無効にした場合、Audio登録失敗にしか使われないラベルや変数があれば、それらも依存関係に従って整理する。ただし共通の `err_destroy_encoder` など他機能が使用するラベルは残す。

#### 10.10.3 Audio分離の確認項目

- `files.drmkms` に `CONFIG_DRM_VC4_HDMI_AUDIO` のdefineを追加しない。
- soundヘッダーとASoC型への参照がAudioガード外に残っていない。
- `struct vc4_hdmi` のAudioメンバー参照がAudioガード外に残っていない。
- Audio専用関数・配列・staticデータが無効ビルド時に未使用警告を発生させない。
- `vc4_hdmi_audio_init()` が無効ビルドから呼ばれない。
- CECガードとAudioガードが独立しており、CEC無効化の動作を変えない。
- HDMI connector、EDID、DDC、HPD、映像モード設定、共通HDMI register accessをAudio分離のためにガードしない。

#### 10.10.4 HD register accessの型問題

Run #72で指摘されたHD register accessのマクロは、現行 `vc4_hdmi.c` の次の定義である。

```c
#define HD_READ(offset) readl(vc4->hdmi->hd_regs + offset)
#define HD_WRITE(offset, val) writel(val, vc4->hdmi->hd_regs + offset)
```

`hd_regs` は `void __iomem *` として保持されており、NetBSDのビルド設定では `void *` のポインタ演算が `-Werror=pointer-arith` で拒否される。レジスタオフセットはバイト単位であるため、byte pointerへ明示的にcastしてから加算する。

修正案:

```c
#define HD_READ(offset) \\
        readl((char __iomem *)vc4->hdmi->hd_regs + (offset))
#define HD_WRITE(offset, val) \\
        writel((val), (char __iomem *)vc4->hdmi->hd_regs + (offset))
```

`char` pointerを使うことで追加の整数型ヘッダーに依存せず、オフセットがバイト単位であることを表現する。実際の適用前にLinuxKPIの `readl()` / `writel()` 宣言が `void __iomem *` 相当を受け取ること、NetBSD側で `__iomem` がどのように定義されるかを確認する。必要なら既存DRM2コードのMMIO accessorの書き方に合わせる。

現行ファイルには同様の形をした `HDMI_READ/HDMI_WRITE` も存在する。

```c
#define HDMI_READ(offset) readl(vc4->hdmi->hdmicore_regs + offset)
#define HDMI_WRITE(offset, val) writel(val, vc4->hdmi->hdmicore_regs + offset)
```

Run #72で報告されたHD側エラーを直す際、HDMI core側も同じ `void __iomem *` 加算になっていることを見落とさない。コンパイラが指摘している範囲を確認し、同じ型問題が発生する場合は同じbyte-pointer方針で修正する。ただし今回の変更差分では、実際にエラーとなったマクロを起点に必要最小限の変更とし、MMIO accessor全体の設計変更やレジスタ幅・順序の変更は行わない。

#### 10.10.5 HD register修正の検証

- `hd_regs` と `hdmicore_regs` の宣言型を確認する。
- offsetがbyte単位であることを確認する。
- `readl/writel` に渡るアドレスが元のMMIO base + 同一offsetとなり、アクセス幅やレジスタ値を変更しないことを確認する。
- `-Werror=pointer-arith` が解消したことをActionsログで確認する。
- Audio分離と同時に修正する場合でも、Git diffではAudioガードとHD register macro変更を別々にレビューできるようにする。

#### 10.10.6 変更・CIの進め方

1. まず `vc4_hdmi.c` のAudio依存範囲を再確認し、`CONFIG_DRM_VC4_HDMI_AUDIO` のガードを追加する。CEC関連行は変更しない。
2. 差分でAudio依存だけがガードされ、映像/DDC/EDID経路が残っていることを確認する。
3. HD register macroのbyte-pointer castを別の小さな差分として追加する。HDMI core側macroもコンパイルエラーの有無を確認する。
4. `git diff --check` と対象ファイルの差分確認後にcommit/pushする。
5. GitHub Actionsの対象commit SHAとRun番号を記録し、まず `vc4_hdmi.o` のAudio/ASoCおよびpointer-arithエラーが消えたか確認する。
6. その後に残るClock、OF/DDC、HPD GPIO、Runtime PMを個別課題として扱う。

Audio分離はASoC互換層を完成させる変更ではなく、Audioを使わずに映像/DDC移植を進めるための一時的なNetBSD opt-in制御である。HD register castもポインタ演算の型を明示する局所修正であり、実機でのHDMI動作確認の代わりにはならない。


### 10.11 Run #75結果とVC4 DRM coreのNetBSD/Linux-KPI境界

本節は、Audio分離およびHD register pointer arithmetic修正後に実行したGitHub Actions Run #75の結果を記録し、次の作業対象である `vc4_drv.c` とNetBSD DRM2/LinuxKPIの境界を整理する。

対象:
- ブランチ: `vc4-drmkms`
- 対象ソースコミット: `3b51dee5b19c128c16052bcb1eadf049e53b068f`
- GitHub Actions Run #75: [45262628653](https://github.com/ebijun/src/actions/runs/45262628653)
- 主な対象: `sys/external/bsd/drm2/dist/drm/vc4/vc4_drv.c`
- NetBSD側互換層: `sys/external/bsd/drm2/include/linux/` および `sys/external/bsd/drm2/dist/include/drm/`

#### 10.11.1 Run #75で確認できたこと

Run #75では、前段の変更により次のエラー群が解消した。

- HDMI Audio / ASoCの型・関数不足はエラー一覧から消えた。Audioを初期移植の対象外にした分離方針は有効だった。
- `HD_READ` / `HD_WRITE` の `void *` pointer arithmeticエラーは消えた。MMIO baseをbyte pointerへcastする局所修正は有効だった。

一方、ビルドは成功していない。少なくとも `vc4_drv.o`、`vc4_hdmi.o`、`vc4_hvs.o` に未解決項目が残る。Run #75は「AudioとHD registerの問題を切り分けた段階」であり、VC4全体のNetBSD対応完了を意味しない。

#### 10.11.2 `vc4_drm_fops` とNetBSD DRM device file operation

Run #75の `vc4_drv.c` では、次のLinux用file operations定義がNetBSDビルドに露出している。

```c
static const struct file_operations vc4_drm_fops = {
        .owner = THIS_MODULE,
        .open = drm_open,
        .release = drm_release,
        .unlocked_ioctl = drm_ioctl,
        .mmap = vc4_mmap,
        .poll = drm_poll,
        .read = drm_read,
        .compat_ioctl = drm_compat_ioctl,
        .llseek = noop_llseek,
};
```

Run #75のエラーは `struct file_operations` が不完全、`owner` メンバーがない、`drm_open` / `drm_release` / `noop_llseek` が宣言されていない、というものだった。

NetBSD側の `sys/external/bsd/drm2/include/linux/fs.h` は `sys/file.h` 等をincludeし、`file_inode()` を提供する最小互換ヘッダーであり、Linuxの `struct file_operations` を定義していない。これは単なるinclude不足ではない。LinuxのDRM character device file operationsをNetBSDのfileopsへそのまま移植することはできない。

一方、NetBSD側の `sys/external/bsd/drm2/dist/include/drm/drm_drv.h` には `struct drm_driver` の `fops` メンバーに加え、`__NetBSD__` 専用の `ioctl_override` が存在する。また、同ファイルの `gem_prime_mmap` もNetBSD用に `off_t *`、`uvm_object **` 等を受け取る別シグネチャへ分岐している。したがってDRM coreにはすでにOS固有境界が一部設けられている。

次の調査では、NetBSD DRM2のDRM device node生成・open/ioctl/mmap処理を追い、VC4が必要とする操作がDRM core側で提供される範囲を確定する。Linux用 `vc4_drm_fops` をNetBSD向けに模造したり、空のfileopsを追加したりしない。

検討する方向:
1. Linux専用の `vc4_drm_fops` 定義と `.fops = &vc4_drm_fops` 初期化を `#ifndef __NetBSD__` で隔離できるかを調べる。
2. NetBSD DRM coreが `.fops` を参照せず、DRM2固有のdevice node/fileopsおよび `.ioctl_override` を使用する経路を確認する。
3. VC4固有ioctlがNetBSD DRM coreのioctl dispatchへ正しく登録されるか確認する。
4. `vc4_mmap`、GEM mmap、PRIME mmapの責務を分け、Linux VMA callbackをNetBSD UVM callbackへ誤って流用しない。

`#ifndef __NetBSD__` による隔離は有力な候補だが、DRM coreの実際の登録経路を確認する前に確定修正として適用しない。

#### 10.11.3 DRM component masterとNetBSD device attach

`vc4_drv.c` は単独のplatform deviceだけで完結せず、VC4の複数ブロックをcomponentとして束ねるLinux component frameworkを前提としている。

現行VC4 master側の処理:
- `vc4_match_add_drivers()` が `component_drivers[]` の各platform driverを検索する。
- `platform_find_device_by_driver()` と `component_match_add()` でmasterに参加するdeviceを登録する。
- `component_master_add_with_match()` がmasterを登録する。
- masterのbind callbackで `drm_dev_alloc()`、GEM初期化、`component_bind_all()`、KMS初期化、`drm_dev_register()`、fbdev初期化を行う。
- unbind callbackではcomponent unbindとDRM/KMS/GEM資源の解放を行う。

NetBSD側 `sys/external/bsd/drm2/include/linux/component.h` の現状は、`component_ops` と `component_add()` / `component_del()` の最小stubに限られる。master側の `component_master_ops`、`component_match`、`component_master_add_with_match()`、`component_bind_all()`、`component_unbind_all()`、`component_match_add()` は提供されていない。また `platform_find_device_by_driver()` も今回のNetBSD側ソースでは利用できない。

これは関数宣言を足すだけの問題ではない。VC4ではHDMI/HVS/CRTC/V3D等の子deviceのattach完了を待ち、master bind時にそれぞれのbind callbackを実行し、失敗時には部分bindを逆順に解除する必要がある。no-op stubではDRM deviceが登録されても表示ハードウェアが初期化されず、動作上の誤りを隠す。

次の調査で確定すること:
- NetBSD FDT/platform attachで各VC4ブロックがどのdevice_t階層として生成されるか。
- 各VC4 component driverのprobe/attachとbind/unbind相当処理の対応関係。
- HDMI/HVS/CRTC等の依存関係とbind順序。
- 子deviceが未attach、attach失敗、detachされる場合のmaster側の状態遷移。
- NetBSDのdevice frameworkに合わせたmaster orchestrationをVC4側に実装するか、LinuxKPI component frameworkを意味のある形で実装するか。

初期表示を優先する場合でも、VC4の全componentを無条件にbind済みとみなすstubは採用しない。まず表示に必須のHDMI/HVS/CRTC経路を特定し、V3D等のoptional componentを欠く構成での成立条件も確認する。

#### 10.11.4 IRQ callbackとGEM PRIME mmapのOS別シグネチャ

Run #75では、VC4 IRQ callbackの型がDRM coreの期待する型と一致しないエラーも発生した。

VC4側ヘッダー `vc4_drv.h` の宣言はLinux形式の `irqreturn_t vc4_irq(int irq, void *arg)` である。一方、NetBSDの `sys/external/bsd/drm2/include/drm/drm_irq_netbsd.h` は `DRM_IRQ_ARGS` を `void *arg` と定義し、`struct drm_driver.irq_handler` は `irqreturn_t (*)(void *)` になる。NetBSD DRM2のIRQ登録モデルはLinuxのIRQ番号引数を含むhandler ABIとは異なる。

対応方針:
- Linux callback本体を安易に変更せず、NetBSD IRQ登録側が渡す引数とVC4 softc/DRM deviceの取得方法を確認する。
- 必要であればNetBSD専用wrapperを用意し、正しいdevice contextを受け取って共通IRQ処理へ橋渡しする。
- IRQ番号を使わないからといって引数を単純に捨てるのではなく、request_irq/free_irqとhandlerの引数契約を一体で確認する。

同じく `gem_prime_mmap` は、Linux側の `struct vm_area_struct *` を受け取るcallbackと、NetBSD DRM2側の `off_t *`、`size_t`、`uvm_object **` 等を受け取るcallbackでシグネチャが異なる。NetBSDの `drm_drv.h` にはOS別メンバー宣言が既に存在するため、VC4側の `vc4_prime_mmap` を確認し、NetBSD用のUVM mapping処理を実装するか、既存のDRM CMA helperへ委譲できるかを判断する。関数ポインタのcastで警告を隠すことは禁止する。

#### 10.11.5 `vc4_drv.c` の次の調査・実装順序

次の作業は次の順で行う。各段階の調査結果と差分を分け、GitHub Actionsで確認する。

1. **DRM device node境界の調査**: NetBSD DRM coreのdevice node登録、fileops、ioctl dispatch、mmap経路を読む。VC4のLinux `.fops` がNetBSDで不要か、NetBSD固有経路が何を要求するかを確定する。
2. **component orchestrationの調査**: NetBSD FDT/platform device階層とVC4 component driverのattach/bind関係を一覧化する。必要なcomponentとoptional componentを分ける。
3. **IRQ ABIの調査**: `drm_irq_netbsd.h`、DRM IRQ登録関数、VC4 IRQ handlerの呼び出し元・引数を照合し、NetBSD wrapperの設計を決める。
4. **GEM/PRIME mmap ABIの調査**: `vc4_prime_mmap` とNetBSD UVM helperのシグネチャ・所有権・offset処理を照合する。
5. **小さな修正を個別に適用**: 調査で境界が明らかになったものから一項目ずつ変更する。component frameworkは特に、stub追加でビルドだけ通すことを避ける。
6. **CIで再確認**: 変更commitとActions Runを対応づけ、最初に `vc4_drv.o` のエラーがどう変化したか、その後のobjectで新たなエラーが出たかを記録する。

#### 10.11.6 Run #75で残った他ファイルの課題

`vc4_drv.c` の境界調査と並行して、次の局所問題も別差分として扱う。

- `vc4_hdmi.c`: CEC無効化後にも `vc4_hdmi_connector_detect()` の切断経路に `cec_phys_addr_invalidate()` が無条件で残っている。CECを使わない構成では、この呼び出しも `#ifdef CONFIG_DRM_VC4_HDMI_CEC` で保護する。先行変更でCEC guardの重複と漏れが生じたため、同関数の両方の切断経路を確認してから修正する。
- `vc4_hdmi.c` / `vc4_hvs.c`: `struct seq_file` はNetBSD LinuxKPIの `linux/seq_file.h` で前方宣言されているだけで、Linuxのseq_file実装は提供されていない。メンバーアクセスを行うdebugfs callbackは、単にheaderを追加しても解決しない。NetBSD debugfs実装との対応を確認し、初期移植で不要なdebugfs機能を適切に隔離できるか、実装が必要かを決める。
- `vc4_hvs.c`: `hvs->regs + SCALER_DLIST_START` のような `void *` 加算が報告されている。レジスタoffsetの単位と `regs` の宣言を確認し、byte pointerへの明示castで修正する。HDMI側と同じく、アクセス幅やoffsetの意味を変えない。

#### 10.11.7 作業原則

- ビルドエラーを「ヘッダー不足」と即断せず、LinuxとNetBSDで型・callback・device lifecycleの契約が異なる場合はOS境界の問題として扱う。
- NetBSD DRM2に存在する `__NetBSD__` 専用実装、`pdev->pd_dev`、`device_t`、FDT attach、UVM、bus_spaceの既存設計を優先して調べる。
- 不足APIをno-op stubや関数ポインタcastで埋めてCIだけ通すことはしない。実際のbind/unbind、資源所有権、IRQ、mmapの意味を保つ。
- 変更前に対象関数の全参照元とcleanup経路を確認し、変更後は `git diff --check` と対象差分をレビューする。
- GitHub Actionsはコンパイル検証であり、実機のHDMI表示、EDID取得、割り込み動作を証明するものではない。
- まず表示に必要な最小構成を明確にし、Audio/CEC/debugfs/V3D等のoptional機能とHDMI映像経路を分離して段階的に有効化する。


### 10.12 NetBSD DRM cdevsw / ioctl / mmap経路の追加調査

10.11で次の調査項目としたDRM device node/file operationsについて、NetBSD DRM2の実装を追跡した結果を記録する。今回の調査で、Linuxの `struct file_operations` をNetBSD側に追加する必要はなく、VC4 driver側ではLinux専用 `.fops` をNetBSDビルドから除外する方向が妥当であることが確認できた。一方、NetBSD DRMのmmap callbackはVC4側に別途実装・接続する必要がある。

調査した主なファイル:
- `sys/external/bsd/drm2/drm/drm_cdevsw.c`
- `sys/external/bsd/drm2/dist/drm/drm_drv.c`
- `sys/external/bsd/drm2/dist/drm/drm_ioctl.c`
- `sys/external/bsd/drm2/dist/drm/drm_file.c`
- `sys/external/bsd/drm2/drm/drm_gem_vm.c`
- `sys/external/bsd/drm2/dist/drm/drm_prime.c`
- `sys/external/bsd/drm2/dist/include/drm/drm_drv.h`
- `sys/external/bsd/drm2/include/linux/fs.h`

#### 10.12.1 NetBSDのDRM file operationsは共通cdevswが担当する

NetBSD DRM2には `sys/external/bsd/drm2/drm/drm_cdevsw.c` があり、ここでDRM共通の `drm_cdevsw` と `drm_fileops` が定義されている。

- `drm_cdevsw` はデバイス番号からDRM minorを取得するopen entry pointを提供する。
- open成功時に `drm_file_alloc(dminor)` で `struct drm_file` を作成し、NetBSDの `fd_clone()` で `drm_fileops` を使うfile descriptorを構成する。
- `drm_fileops` はNetBSDの `fo_read`、`fo_ioctl`、`fo_poll`、`fo_close`、`fo_kqfilter`、`fo_mmap` 等を持つ。
- closeはNetBSD側の `drm_close()` がfile contextを解放し、open countとlast-close処理を管理する。

この経路はLinuxの `drm_stub_open()` がdriverの `file_operations` に切り替える方式とは異なる。NetBSDではDRM core共通のcdevsw/fileopsがDRM file descriptorの入口となり、個々のdriverがLinux `struct file_operations` を供給する設計ではない。

したがって、Run #75で問題となった `vc4_drm_fops` はNetBSD用に代替定義するのではなく、Linux専用定義とdriver初期化子を次のように隔離する方針が妥当である。

```c
#ifndef __NetBSD__
static const struct file_operations vc4_drm_fops = {
        .owner = THIS_MODULE,
        .open = drm_open,
        .release = drm_release,
        .unlocked_ioctl = drm_ioctl,
        .mmap = vc4_mmap,
        .poll = drm_poll,
        .read = drm_read,
        .compat_ioctl = drm_compat_ioctl,
        .llseek = noop_llseek,
};
#endif

static struct drm_driver vc4_drm_driver = {
        /* common DRM callbacks */
#ifndef __NetBSD__
        .fops = &vc4_drm_fops,
#endif
};
```

これはLinuxのfileops ABIをNetBSD側へ移植する提案ではない。NetBSD DRM coreが独自fileopsを持つため、Linux専用メンバー参照をコンパイル対象から外す提案である。適用時には `vc4_mmap` を含むLinux VMA専用関数がNetBSD側で未使用static function警告を出さないよう、定義範囲も確認する。

#### 10.12.2 NetBSD open/closeとVC4 driver callbackの接続

NetBSDの `drm_cdevsw.c:drm_open()` は `drm_file_alloc()` を呼ぶ。`drm_file_alloc()` は `drm_file.c` にあり、共通GEM/syncobj/PRIME file-private stateを初期化した後、次のdriver callbackを実行する。

```c
if (dev->driver->open) {
        ret = dev->driver->open(dev, file);
        if (ret < 0)
                goto out_prime_destroy;
}
```

したがってVC4の `.open = vc4_open` は、Linuxの `drm_open()` を経由しなくてもNetBSD共通open処理から呼び出される。VC4の `vc4_open()` が確保する `struct vc4_file` と `vc4_close()` 相当の後処理は、NetBSD共通file close pathでどのdriver callbackが呼ばれるかを含めて整合を確認する。Linux `drm_release()` 自体をNetBSD用に定義する必要はない。

#### 10.12.3 ioctl dispatchと `ioctl_override` の意味

NetBSD `drm_cdevsw.c` の `drm_ioctl_shim()` は、driverの `ioctl_override` が設定されていればそれを呼び、未設定なら通常の `drm_ioctl()` を呼ぶ。

通常の `drm_ioctl()` は `drm_ioctl.c` のNetBSD実装であり、ioctl番号が `DRM_COMMAND_BASE` 以上のdriver-private ioctlなら、`drm_driver.ioctls` と `num_ioctls` からdescriptorを選択してdispatchする。これはVC4の次の定義と直接接続する。

```c
.ioctls = vc4_drm_ioctls,
.num_ioctls = ARRAY_SIZE(vc4_drm_ioctls),
```

よって、VC4は通常の `drm_ioctl()` dispatchを利用するため、初期移植で `.ioctl_override` を設定する必要はない。`ioctl_override` はdriver-private ioctlを追加するためのフックではなく、NetBSD cdevswから通常のDRM ioctl処理全体を置き換えるoverrideである。安易に設定すると共通DRM ioctl、permission check、suspend/unplug保護、引数zero-padding等を迂回する危険がある。

#### 10.12.4 NetBSD mmapは `mmap_object` とUVMで実装される

NetBSDの `drm_fileops.fo_mmap` は `drm_cdevsw.c:drm_fop_mmap()` へ入り、次のdriver callbackを呼ぶ。

```c
error = -(*dev->driver->mmap_object)(dev, *offp, len, prot, uobjp,
    offp, file->filp);
```

このcallbackの型は `sys/external/bsd/drm2/dist/include/drm/drm_drv.h` の `__NetBSD__` 分岐で定義されている。

```c
int (*mmap_object)(struct drm_device *, off_t, size_t, int,
    struct uvm_object **, voff_t *, struct file *);
const struct uvm_pagerops *gem_uvm_ops;
```

NetBSDのGEM mmap実装は `sys/external/bsd/drm2/drm/drm_gem_vm.c` にあり、`drm_gem_mmap_object()` はGEM VMA offsetを検索し、許可されたGEM objectについて `gemo_uvmobj` とoffsetを返す。pager側ではdriverの `gem_uvm_ops` が必要になる。従って `mmap_object` と `gem_uvm_ops` は役割が異なる。

- `mmap_object`: DRM file descriptorのmmap入口から呼ばれる、byte offset/lengthをUVM objectへ変換するdriver/core callback。
- `gem_uvm_ops`: GEM objectのUVM pager operation。GEM objectのfault/reference/detach等を処理する。
- `gem_prime_mmap`: PRIME dma-buf mmap用callback。NetBSDではUVM引数を受け取る別ABIである。

現在のVC4 driver initializerはNetBSD用 `.gem_uvm_ops = &drm_gem_cma_uvm_ops` を設定しているが、確認した `vc4_drm_driver` 初期化子には `.mmap_object` の指定がない。NetBSDの `drm_fop_mmap()` は `mmap_object` を直接呼ぶため、これを別途接続しないままではmmap時に成立しない。次の実装前に、VC4 GEM objectがCMA pagerを利用する条件と既存の `drm_gem_mmap_object()` / `drm_gem_or_legacy_mmap_object()` のどちらが適切かを確定する。単純に関数ポインタをcastしてはならない。

#### 10.12.5 PRIME mmap callbackの型差と既存NetBSD helper

`drm_driver.gem_prime_mmap` はNetBSDとLinuxで異なる型を持つ。NetBSD側は `off_t *`、`size_t`、`prot`、`uvm_object **` 等を受け取り、Linux側は `struct vm_area_struct *` を受け取る。

NetBSDの `drm_prime.c:drm_gem_prime_mmap()` はNetBSD ABIで実装され、GEM objectの `obj->funcs->mmap` があればそれを呼ぶ。なければ `driver->mmap_object` に処理を委譲する。Linux側の `fops->mmap` へは進まない。

Run #75で `vc4_prime_mmap` と `drm_driver.gem_prime_mmap` の関数ポインタ型不一致が出たため、VC4側のLinux VMA callbackをそのまま登録することはできない。対応方針は以下のとおり。

1. VC4の `vc4_prime_mmap` の実装内容とGEM object function tableを確認する。
2. NetBSD UVM ABIで同等の処理を実装できるか確認する。
3. 標準 `drm_gem_prime_mmap()` へ委譲できるなら、そのNetBSDシグネチャを使う。
4. object-specific mappingが必要ならNetBSD専用wrapperを作り、offset、length、protection、UVM object参照の契約を明示する。

関数ポインタcastやダミーcallbackでコンパイルエラーを隠すことは禁止する。

#### 10.12.6 `vc4_drv.c` に対する修正候補と未確定事項

今回の調査から、`vc4_drv.c` のfile operations部分は次の小さな変更候補として確定できた。

- `vc4_drm_fops` 定義を `#ifndef __NetBSD__` で囲む。
- `drm_driver` 初期化子の `.fops = &vc4_drm_fops` も `#ifndef __NetBSD__` で囲む。
- NetBSD用の `struct file_operations`、`drm_open`、`drm_release`、`noop_llseek` を追加しない。
- `.ioctls` / `.num_ioctls` は残す。NetBSD DRM cdevswが通常の `drm_ioctl()` を経由してdriver-private ioctlをdispatchするためである。
- `.open = vc4_open` / `.postclose = vc4_close` はDRM coreのdriver callbackであり、Linux fileopsのopen/releaseとは別物なので残す。

ただし、以下はまだ実装判断を確定していない。

- VC4の `mmap_object` に適切なNetBSD GEM/legacy mapping helper。
- `vc4_prime_mmap` のNetBSD UVM callback実装または標準helperへの置換。
- `vc4_mmap` とLinux `vm_operations_struct` 依存コードをどこまでNetBSDビルドから除外するか。
- NetBSDの `drm_file_alloc()` / `drm_file_free()` とVC4 `open/postclose` callbackの対応関係を含むclose lifecycleの最終確認。

#### 10.12.7 次の作業

次の変更は `vc4_drv.c` のLinux-only fops隔離だけに限定する。mmap callbackやcomponent frameworkを同じcommitで変更しない。

1. `vc4_drm_fops` 定義と `.fops` 初期化子をNetBSDビルドから隔離する。
2. `vc4_mmap` 等Linux VMA専用関数のunused warningを確認する。
3. `git diff --check` と差分確認を行う。
4. GitHub Actionsで `vc4_drv.o` のfile_operations関連エラーが消えることを確認する。
5. 次の独立課題として `mmap_object` と `gem_prime_mmap` のNetBSD UVM接続を設計する。
6. その後にcomponent master orchestrationへ進む。

この段階のゴールはVC4 DRMのNetBSD fileopsを実装することではなく、NetBSD DRM2が既に提供するcdevsw/fileops経路を正しく利用し、Linux専用 `.fops` ABIへの不要な依存を取り除くことである。


### 10.13 `vc4_drv.c` Linux file operationsのNetBSD除外

10.12の調査結果に基づき、VC4 driverのLinux専用file operationsをNetBSDビルド対象から除外した。

対象:
- `sys/external/bsd/drm2/dist/drm/vc4/vc4_drv.c`
- commit: `2a2f7f02194853335450bf06ead53d56ebc98418`
- commit message: `drm/vc4: exclude Linux file operations on NetBSD`

#### 10.13.1 変更内容

Linux `struct file_operations` に依存する `vc4_drm_fops` 定義を `__NetBSD__` で除外した。

```c
#ifndef __NetBSD__
static const struct file_operations vc4_drm_fops = {
        .owner = THIS_MODULE,
        .open = drm_open,
        .release = drm_release,
        .unlocked_ioctl = drm_ioctl,
        .mmap = vc4_mmap,
        .poll = drm_poll,
        .read = drm_read,
        .compat_ioctl = drm_compat_ioctl,
        .llseek = noop_llseek,
};
#endif /* !__NetBSD__ */
```

同時に `struct drm_driver vc4_drm_driver` の `.fops` 初期化も同じ条件で囲んだ。

```c
        .ioctls = vc4_drm_ioctls,
        .num_ioctls = ARRAY_SIZE(vc4_drm_ioctls),
#ifndef __NetBSD__
        .fops = &vc4_drm_fops,
#endif /* !__NetBSD__ */
```

`vc4_vm_ops` は既に `#ifndef __NetBSD__` で囲まれていたため変更していない。`.ioctls` / `.num_ioctls`、`.open = vc4_open`、`.postclose = vc4_close` も維持した。これらはNetBSD DRM cdevsw/fileopsから呼ばれるDRM driver callbackであり、Linux file_operationsの `.open` / `.release` と同一のものではない。

#### 10.13.2 期待する効果と未確認事項

この変更により、NetBSDコンパイル時に以下のLinux file operations型・関数を参照しなくなる。

- `struct file_operations` および `.owner`
- Linux DRM file operationの `drm_open` / `drm_release` / `drm_ioctl`
- `drm_poll` / `drm_read` / `drm_compat_ioctl`
- `noop_llseek`
- Linux VMA mmap entryである `vc4_mmap`

NetBSDでは共通 `drm_cdevsw` / `drm_fileops` がopen/ioctl/read/poll/close/mmapを処理するため、上記Linux fileopsをdriver側で登録しないことが今回の変更意図である。

ただし、この変更はNetBSD GEM mmapを完成させるものではない。NetBSD `drm_fop_mmap()` は `drm_driver.mmap_object` を呼ぶため、VC4の `.mmap_object` は別途設計・実装が必要である。`gem_uvm_ops` や `gem_prime_mmap` の型・動作も今回変更していない。

#### 10.13.3 CI確認

この変更commitをpushすると `vc4-drmkms` branchのGitHub Actionsが自動実行される。現時点ではソース差分をGitHub branch上で確認した段階であり、Actionsの結果は未確認として扱う。

次の確認項目:
1. `vc4_drv.o` で `file_operations`、`owner`、`drm_open`、`drm_release`、`noop_llseek` のエラーが消えること。
2. `vc4_mmap` 等のLinux専用static関数がNetBSDビルドでunused warning/errorにならないこと。
3. `vc4_drm_driver` initializerで `.fops` に関するエラーが出ないこと。
4. 新たに現れた最初のエラーを記録し、fileops問題とは別の課題として切り分けること。

CI成功前に「vc4_drv.cがビルド可能になった」とは判断しない。


### 10.14 GitHub Actions #78/#79 によるVC4 file operations変更後の検証

#### 10.14.1 確認対象

2026-11-24にGitHub branch `vc4-drmkms` の状態を確認した。

- Repository: `ebijun/src`
- branch HEAD: `cd0f5173ab2728c0fe46e41528dd21edff86a1e2`
- file operations変更commit: `2a2f7f02194853335450bf06ead53d56ebc98418`
- 記録更新commit: `cd0f5173ab2728c0fe46e41528dd21edff86a1e2`
- CI #78: run ID `45304017094`、failure
- CI #79: run ID `45304333435`、failure
- 対象job: `build-kernel` / `Build Tools and Kernel (earmv7hf)`

参照:
- [Actions #78](https://github.com/ebijun/src/actions/runs/45304017094)
- [Actions #79](https://github.com/ebijun/src/actions/runs/45304333435)
- [file operations変更commit](https://github.com/ebijun/src/commit/2a2f7f02194853335450bf06ead53d56ebc98418)

#### 10.14.2 file operations変更の評価

GitHub上の `sys/external/bsd/drm2/dist/drm/vc4/vc4_drv.c` を確認し、次の2箇所が `#ifndef __NetBSD__` で囲まれていることを確認した。

1. Linux `struct file_operations vc4_drm_fops` の定義。
2. `struct drm_driver vc4_drm_driver` の `.fops = &vc4_drm_fops` 初期化。

Actions #79の `vc4_drv.o` ログでは、以前のLinux file operationsに関する `file_operations`、`.owner`、`drm_open`、`drm_release`、`noop_llseek`、`.fops` のエラーは確認できなかった。

従って、Linux file operationsをNetBSDビルドから除外する変更はソースに反映され、当該エラー群を先へ進めたと評価する。ただし、これは `vc4_drv.o` のコンパイル成功を意味しない。

#### 10.14.3 Actions #79で残った主なエラー

ログから確認したエラーを機能別に整理する。

| 対象 | エラー・問題 | 次の調査方針 |
|---|---|---|
| `vc4_drv.c:192` | `irq_handler` がNetBSD側の `irqreturn_t (*)(void *)` とLinux側の `vc4_irq(int, void *)` で不一致 | NetBSD DRM driver IRQ callbackの契約を確認し、適切なwrapperを設計 |
| `vc4_drv.c:220` | `gem_prime_mmap` がNetBSD UVM ABIとLinux `vm_area_struct *` ABIで不一致 | NetBSD用標準GEM/PRIME mmap helperへの委譲可否を調査 |
| `vc4_drv.c:244-256` | `component_match`、`component_match_add`、`platform_find_device_by_driver`、`put_device` 等が未定義または型不一致 | Linux component frameworkをそのまま移植せず、NetBSD側のcomponent対応状況と代替構成を調査 |
| `vc4_drv.c:271` | `struct device` に `coherent_dma_mask` がない | NetBSD Linux-KPIのDMA mask APIと `pdev->pd_dev` の対応を確認 |
| `vc4_drv.c:278-281` | OF node検索・参照関数が未定義 | NetBSD FDT APIとの接続、既存KPI実装を調査 |
| `vc4_drv.c:291` | `mutex_init` の型・引数が不一致 | Linux mutex wrapperを使うべき箇所か、NetBSD mutex APIとの境界を確認 |
| `vc4_drv.c:301,320,346-375` | component master bind/unbind APIと構造体が未実装 | VC4 component orchestrationのNetBSD設計を独立して行う |
| `vc4_drv.c:396-418` | platform driver登録API、module alias、未使用register関数 | NetBSDのdriver attach/module構成に合わせて初期化経路を整理 |
| `vc4_hdmi.c` | `seq_file`、GPIO、CEC、clock、OF、I2C adapter、runtime PM関連の未定義API・型不一致 | HDMIの各Linux-KPI依存を個別に分類。CEC無効化方針との整合も確認 |
| `vc4_hvs.c` | `seq_file` の不完全型、`void *` pointer arithmetic | debugfs/seq_file互換とMMIO register型を調査 |

このほか、`vc4_hdmi.c` では `EPROBE_DEFER`、`struct device.of_node`、`enum of_gpio_flags` 等もエラーとなった。これらは単純なヘッダー追加だけで解決するとは限らず、NetBSD側の構造体・APIの有無を先に確認する。

#### 10.14.4 CI結果の解釈

#78と#79はいずれもfailureであり、kernel artifact uploadはskipされた。#79の失敗箇所は `Build Tools and Kernel (earmv7hf)` である。

#79のログは `vc4_drv.o` だけでなく `vc4_hdmi.o`、`vc4_hvs.o` にも到達して複数のコンパイルエラーを報告している。したがって、今回のfile operations隔離は部分的な前進だが、VC4全体のNetBSDビルドは未達である。

#### 10.14.5 次の作業順序

一度に多数の互換層を変更せず、次の順に小さく切り分ける。

1. `vc4_drv.c` のIRQ callback ABIを確認する。
2. `gem_prime_mmap` と `mmap_object` のNetBSD UVM経路を整理する。関数ポインタcastで回避しない。
3. component master / platform driver orchestrationを、NetBSD DRM2の実装範囲と照合して設計する。
4. `struct device` / DMA / OF / mutexのLinux-KPI境界を個別に確認する。
5. `vc4_hdmi.c` と `vc4_hvs.c` のdebugfs、CEC、clock、GPIO、OF依存を別課題として扱う。
6. 各変更後にGitHub Actionsを実行し、最初に残るエラーが解消したか確認する。

次の着手点は `vc4_drv.c:192` のIRQ callback ABIとする。component framework全体の変更を同じパッチへ混在させない。


### 10.15 VC4 HDMI Audio / CEC のNetBSDビルド設定実体確認

本節では、2026-11-25時点の `vc4-drmkms` ブランチをGitHub上で確認し、`CONFIG_DRM_VC4_HDMI_AUDIO` と `CONFIG_DRM_VC4_HDMI_CEC` の実際の定義箇所を確定する。従来の調査メモにはCECのdefineが `files.drmkms` に存在するとの記録があるが、現在のブランチのファイル内容とは一致しないため、過去の状態と現状を区別する。

#### 10.15.1 確認したファイルと状態

確認対象:

- `sys/external/bsd/drm2/drm/files.drmkms`
- `sys/external/bsd/drm2/dist/drm/vc4/vc4_hdmi.c`
- 本ドキュメント

現行 `sys/external/bsd/drm2/drm/files.drmkms` の `# Broadcom VC4 DRMKMS` セクションでは、`vc4drm` 用のinclude pathを追加し、次のソースを `vc4drm` として登録している。

```make
# Broadcom VC4 DRMKMS
makeoptions	vc4drm	CPPFLAGS+="-I$S/external/bsd/drm2/include"
makeoptions	vc4drm	CPPFLAGS+="-I$S/external/bsd/drm2/include/drm"
makeoptions	vc4drm	CPPFLAGS+="-I$S/external/bsd/drm2/include/linux"
makeoptions	vc4drm	CPPFLAGS+="-I$S/external/bsd/drm2/include/sound"
makeoptions	vc4drm	CPPFLAGS+="-I$S/external/bsd/drm2/include/media"
makeoptions	vc4drm	CPPFLAGS+="-I$S/external/bsd/drm2/dist/include"
makeoptions	vc4drm	CPPFLAGS+="-I$S/external/bsd/drm2/dist/include/drm"
makeoptions	vc4drm	CPPFLAGS+="-I$S/external/bsd/drm2/dist/include/uapi"
makeoptions	vc4drm	CPPFLAGS+="-I$S/external/bsd/drm2/dist/drm/vc4"
makeoptions	vc4drm	CPPFLAGS+="-I$S/external/bsd/common/include"

file	external/bsd/drm2/dist/drm/vc4/vc4_drv.c		vc4drm
file	external/bsd/drm2/dist/drm/vc4/vc4_kms.c		vc4drm
file	external/bsd/drm2/dist/drm/vc4/vc4_crtc.c		vc4drm
file	external/bsd/drm2/dist/drm/vc4/vc4_hdmi.c		vc4drm
file	external/bsd/drm2/dist/drm/vc4/vc4_hvs.c		vc4drm
file	external/bsd/drm2/dist/drm/vc4/vc4_plane.c		vc4drm
file	external/bsd/drm2/dist/drm/vc4/vc4_v3d.c		vc4drm
```

このセクションには、次の定義は存在しない。

```make
-DCONFIG_DRM_VC4_HDMI_AUDIO
-DCONFIG_DRM_VC4_HDMI_CEC
```

従って、少なくともこの `files.drmkms` の `vc4drm` 用CPPFLAGS設定は、Audio/CECのどちらも明示的に有効化していない。sound/include pathが存在することは、Audio機能の有効化を意味しない。また、CECのdefineをこのファイルから削除したという過去の記録は、現行ファイルの状態を説明するものではない。

#### 10.15.2 現行 `vc4_hdmi.c` の条件コンパイル

現行 `sys/external/bsd/drm2/dist/drm/vc4/vc4_hdmi.c` では、次の条件コンパイルが確認できる。

| 対象 | 条件 |
|---|---|
| Sound関連4ヘッダー | `CONFIG_DRM_VC4_HDMI_AUDIO` |
| `struct vc4_hdmi_audio` | `CONFIG_DRM_VC4_HDMI_AUDIO` |
| `struct vc4_hdmi` のaudioメンバー | `CONFIG_DRM_VC4_HDMI_AUDIO` |
| Audio処理関数群 | `CONFIG_DRM_VC4_HDMI_AUDIO` |
| `vc4_hdmi_audio_init()` 呼び出し | `CONFIG_DRM_VC4_HDMI_AUDIO` |
| CECヘッダー | `CONFIG_DRM_VC4_HDMI_CEC` |
| `struct vc4_hdmi` のCECメンバー | `CONFIG_DRM_VC4_HDMI_CEC` |
| CEC処理・初期化・終了処理 | `CONFIG_DRM_VC4_HDMI_CEC` |

ソース上の代表的な位置は、Audio includeが45行付近、Audio構造体が63行付近、Audio処理本体が720行から1153行付近、bind時のAudio初期化が1463行付近である。CECについても51行付近のinclude、95行付近の状態メンバー、1155行以降の処理本体などに条件コンパイルがある（行番号は確認時点のもの）。

このため、現行のソースでは両マクロが未定義なら、それぞれの条件ブロックは通常のCプリプロセッサ動作によりコンパイル対象から除外される。現行 `vc4_hdmi.c` はAudio/CECの独立したガードを既に持っている。

#### 10.15.3 ビルド設定について確定したこと・未確定のこと

確定したこと:

1. `sys/external/bsd/drm2/drm/files.drmkms` はVC4ソースの登録と `vc4drm` 用include pathを設定している。
2. 同ファイルの現行VC4セクションにはAudio/CECいずれの `-D` defineもない。
3. `vc4_hdmi.c` はAudioとCECを別々のマクロで条件コンパイルしている。
4. したがって、このファイルの設定だけを根拠に「CECは有効」「Audioは無効」と判定することはできない。両方とも明示的な有効化は見当たらない。

未確定のこと:

- カーネル構成、生成されたmakefile、他のmakeoptions、コマンドライン等から同名マクロが追加定義されていないか。
- GitHub Actionsが実際に実行したコンパイルコマンドに、どちらかのdefineが含まれていないか。
- CIが参照したcommitと今回確認したブランチ先端が一致するか。

従って「カーネル全体で未定義」と断言するには、Actionsログの実際のコンパイルコマンドまたは生成済みビルド設定を確認する必要がある。本節の結論は、GitHub上の現行 `files.drmkms` における明示的な定義の有無である。

#### 10.15.4 過去の記録との整合

10.8節には `files.drmkms` に `makeoptions vc4drm CPPFLAGS+="-DCONFIG_DRM_VC4_HDMI_CEC"` が存在するとの記載がある。この記述は、今回取得した現行ファイルには該当行がないため、過去の調査時点の状態を示す記録として扱う。現行設定の説明として引用してはならない。

10.10節の「Audio defineを追加しない」という方針は現行ファイルと整合する。一方、Audio/CECの実際の有効・無効を確定するには、最終的にCIコンパイルコマンドを確認する必要がある。

#### 10.15.5 次の確認手順

作業端末では、対象ブランチをcheckoutしたうえで次を実行する。

```sh
git switch vc4-drmkms

git grep -n 'CONFIG_DRM_VC4_HDMI_AUDIO'
git grep -n 'CONFIG_DRM_VC4_HDMI_CEC'

grep -n -A40 '^# Broadcom VC4 DRMKMS' \
    sys/external/bsd/drm2/drm/files.drmkms

git diff --check
```

GitHub Actionsでは、`vc4_hdmi.c` のコンパイルコマンドを確認し、`-DCONFIG_DRM_VC4_HDMI_AUDIO` と `-DCONFIG_DRM_VC4_HDMI_CEC` の有無を記録する。必要ならコンパイル時の `-dM -E` 相当のプリプロセッサマクロ一覧も確認する。

現時点では、ビルド設定ファイルへの追加変更は行わない。Audio/CECのどちらも `files.drmkms` から有効化されていないことを確認できたため、まずActionsの実コンパイル条件を確定する。映像/DDC経路を維持したままAudio/CECをopt-inにする既存方針は維持する。


### 10.16 GitHub Actions #79の実コンパイルコマンドによるAudio / CEC条件の確定

10.15節で未確定としていた「Actionsが実際にどのマクロ条件で `vc4_hdmi.c` をコンパイルしたか」について、GitHub Actions #79のjob logを取得して確認した。

#### 10.16.1 調査対象

- Repository: `ebijun/src`
- Branch: `vc4-drmkms`
- Workflow run: [Actions #79](https://github.com/ebijun/src/actions/runs/45304333435)
- Run ID: `45304333435`
- Job: `build-kernel`
- Job ID: `138144332177`
- 対象step: `Build Tools and Kernel (earmv7hf)`
- コンパイル対象: `sys/external/bsd/drm2/dist/drm/vc4/vc4_hdmi.c`
- コンパイラ: `armv7--netbsdelf-eabihf-gcc`

#### 10.16.2 実際のコンパイルコマンド

job logの `vc4_hdmi.o` compile行を確認した。主要な部分は次の通り。

```sh
/home/runner/work/src/src/./tools/bin/armv7--netbsdelf-eabihf-gcc \
    -mfloat-abi=soft -mapcs-frame -ffreestanding \
    -std=gnu11 -Werror -Wall -Wpointer-arith \
    -nostdinc \
    -DCOMPAT_UTILS -DARM_GENERIC_TODR \
    -DDIAGNOSTIC -DDRM2 -D_KERNEL -D_KERNEL_OPT \
    -I.../sys/external/bsd/drm2/include \
    -I.../sys/external/bsd/drm2/include/sound \
    -I.../sys/external/bsd/drm2/include/media \
    -I.../sys/external/bsd/drm2/dist/include \
    -I.../sys/external/bsd/drm2/dist/drm/vc4 \
    -c .../sys/external/bsd/drm2/dist/drm/vc4/vc4_hdmi.c \
    -o vc4_hdmi.o
```

ログの実コマンド全体を検査した結果:

| マクロ | compile command内の `-D` | 判定 |
|---|---|---|
| `CONFIG_DRM_VC4_HDMI_AUDIO` | なし | 未定義 |
| `CONFIG_DRM_VC4_HDMI_CEC` | なし | 未定義 |

job log全体にもこれらのマクロ名は現れなかった。したがって、少なくともRun #79の `vc4_hdmi.c` コンパイルでは、AudioとCECの両マクロは定義されていない。通常のCプリプロセッサ動作により、両方の `#ifdef` ブロックは除外された状態でコンパイルされたと判断できる。

#### 10.16.3 この確認から確定したビルド条件

Run #79の実ビルド条件は次の通り。

- HDMI Audio: 無効（`CONFIG_DRM_VC4_HDMI_AUDIO` 未定義）。
- HDMI CEC: 無効（`CONFIG_DRM_VC4_HDMI_CEC` 未定義）。
- HDMI本体、connector、DDC/EDID等の共通コード: `vc4_hdmi.c` のコンパイル対象。
- sound/media include path: コンパイルコマンドには存在するが、これらのinclude pathは機能を有効化するdefineではない。
- コンパイルは `-Werror` と `-Wpointer-arith` を有効にして実行されている。

これにより、10.15節で残していた「Actionsの実コンパイルコマンドにdefineが含まれているか」という未確定事項は、Run #79について解消した。Audio/CECの無効化は、単に `files.drmkms` の記述から推測したものではなく、CIの実コマンドから確認できた。

#### 10.16.4 ビルド失敗との関係

Run #79は成功ではない。job logでは `vc4_hdmi.o` の失敗として、少なくとも次のNetBSD/LinuxKPI境界の不足が報告されている。

- `of_find_i2c_adapter_by_node()`、`of_node_put()`、`of_find_property()`、`of_get_named_gpio_flags()` の宣言不足。
- `EPROBE_DEFER)、`OF_GPIO_ACTIVE_LOW`、`enum of_gpio_flags` の不足。
- NetBSD LinuxKPIの `struct device` に `of_node` がない。
- `pm_runtime_enable()`、`put_device()` の宣言不足。

これらはAudio/CECを無効にした状態でも残る問題であり、Audio/CECの条件設定が原因ではない。次の作業ではOF/FDT、I2C adapter参照、GPIO、Runtime PM、device参照寿命の境界を個別に扱う。

#### 10.16.5 適用範囲と注意

この結論は、Run #79の対象commitに対する `earmv7hf / evbarm / GENERIC` ビルドの実測結果である。異なるcommit、別のkernel config、別アーキテクチャの全ビルド条件まで自動的に保証するものではない。今後のCIで同じ条件を維持する場合は、各runの `vc4_hdmi.c` compile commandを確認する。

現時点では、Audio/CECを有効化する変更も、無効化のための追加define変更も不要である。現行CIの実条件は既に両機能無効であり、次の修正対象は `vc4_hdmi.c` のOF/FDT・I2C・GPIO・Runtime PM依存とする。


### 10.17 VC4 HDMIのOF/FDT依存とNetBSD実装の照合

10.16節で確認したRun #79の失敗項目について、現行 `vc4-drmkms` ブランチのLinux-KPIヘッダー、NetBSD FDT/I2C/GPIO実装、および `vc4_hdmi.c` の呼び出し側を照合した。本節はGitHub上の現行ソースを対象とする。ビルド・実機動作の検証を行ったものではない。

#### 10.17.1 Linux-KPI側のdeviceとNetBSDのFDT nodeの違い

確認した定義:

- `sys/external/bsd/drm2/include/linux/device.h`
- `sys/external/bsd/drm2/include/linux/platform_device.h`
- `sys/external/bsd/drm2/include/linux/of_device.h`
- `sys/external/bsd/drm2/include/linux/of_platform.h`
- `sys/external/bsd/drm2/include/linux/of_address.h`
- `sys/dev/fdt/fdtvar.h`
- `sys/dev/ofw/openfirm.h`

現行Linux-KPIの `struct device` は次の最小構造であり、Linuxの `dev->of_node` は存在しない。

```c
struct device {
        device_t bsdev;
};
```

一方、NetBSDの `struct platform_device` は `pd_dev` とLinux-KPI用の `dev` を持つ。

```c
struct platform_device {
        device_t        pd_dev;
        struct device   dev;
        /* ... */
};
```

NetBSDではデバイスのFDT nodeはLinuxの `struct device_node *` として保持されるのではなく、device handleからOF node番号へ変換して扱う仕組みがある。

```c
devhandle_to_of(device_handle(dev))
```

この変換関数は `sys/dev/ofw/openfirm.h` で宣言されている。FDT側のAPIは主に `int` のnode/phandle値を受け取る。したがってLinuxの `dev->of_node` を単にメンバー追加して既存のLinux APIを実装するだけでは不十分であり、NetBSD device handleから得られるnode値との対応をLinux-KPI境界で明示する必要がある。

#### 10.17.2 OF APIとNetBSD FDT APIの対応

| VC4/Linux側のAPI | NetBSD側で確認したAPI・実装 | 差異・対応方針 |
|---|---|---|
| `dev->of_node` | `device_handle()`、`devhandle_to_of()`、platform deviceの `pd_dev` | Linuxのnode pointerは存在しない。device handleからnode番号を得る境界が必要 |
| `of_parse_phandle(node, prop, index)` | `fdtbus_get_phandle(node, prop)`、必要に応じ `fdtbus_get_phandle_from_native()` | NetBSD APIは整数node値を扱う。Linuxのnode pointer/refcountモデルを持ち込まない |
| `of_node_put(node)` | 対応するnode参照解放APIは今回確認したFDT API群には見当たらない | 整数node番号を保持する設計なら通常はput操作を必要としない。Linux互換関数を安易にpointer用no-opとして追加しない |
| `of_find_property(node, name, len)` | `fdtbus_get_prop(node, name, &len)`、`of_hasprop(node, name)` | propertyの存在確認とデータ取得は分離して利用できる |
| `of_get_address(node, index, ...)` | `fdtbus_get_reg(node, index, &addr, &size)` | NetBSD側はbus address/sizeを出力引数で返す。Linuxの `const __be32 *` を返すAPIとは型・変換が異なる |
| `of_device_is_available(node)` | `fdtbus_status_okay(node)` | FDT nodeのstatus判定に対応する既存APIがある |
| `of_device_is_compatible(node, ...)` | `of_compatible()`、`of_compatible_match()` 等 | compatible照合APIはNetBSD側に存在する |

`sys/dev/fdt/fdtvar.h` には `fdtbus_get_phandle()`、`fdtbus_get_prop()`、`fdtbus_get_reg()`、`fdtbus_status_okay()` 等が宣言されている。`sys/dev/ofw/openfirm.h` には `devhandle_to_of()`、`of_getprop_uint32()`、`of_hasprop()` 等が宣言されている。

現行の `sys/external/bsd/drm2/include/linux/of_device.h` は `of_match_device()` の簡易実装だけで、nodeアクセスAPIを提供していない。`linux/of.h` は今回確認したブランチ上のパスには存在しない。`of_address.h` と `of_platform.h` も現状はincludeを束ねる薄いヘッダーであり、OF API本体の実装ではない。

#### 10.17.3 HDMI DDC I2C adapterの取得

VC4側の `vc4_hdmi_bind()` は次の順序でDDC nodeを取得し、I2C adapterを探す。

```c
ddc_node = of_parse_phandle(dev->of_node, "ddc", 0);
if (!ddc_node)
        return -ENODEV;

hdmi->ddc = of_find_i2c_adapter_by_node(ddc_node);
of_node_put(ddc_node);
if (!hdmi->ddc)
        return -EPROBE_DEFER;
```

NetBSD側では次の実装が確認できた。

- `sys/dev/fdt/fdt_i2c.h`: `fdtbus_i2c_get_tag(int)`、`fdtbus_i2c_acquire(int, const char *)`
- `sys/dev/fdt/fdt_i2c.c`: I2C controllerを `i2c_tag_t` とFDT phandleの組で登録・検索する
- controller登録時のnode取得: `devhandle_to_of(device_handle(dev))`
- `fdtbus_i2c_acquire(node, "ddc")` はpropertyから参照先nodeを得て、登録済みcontrollerの `i2c_tag_t` を返す

ただし、これはLinuxの `struct i2c_adapter *` を返す `of_find_i2c_adapter_by_node()` と同じAPIではない。Linux-KPIの `struct i2c_adapter` は `sys/external/bsd/drm2/include/linux/i2c.h` に定義され、`linux_i2c.c` はそのadapter経由の転送をNetBSD `i2c_tag_t` に橋渡しする。一方、今回確認した `fdtbus_i2c_get_tag()` / `fdtbus_i2c_acquire()` は `i2c_tag_t` を直接返し、Linux adapterの取得・参照寿命管理までは行わない。

したがって、VC4から `fdtbus_i2c_acquire()` の戻り値を `struct i2c_adapter *` として扱うことはできない。必要なのは、FDT nodeに対応するNetBSD `i2c_tag_t` とLinux-KPI `struct i2c_adapter` を結び付ける明示的なbridge/registryである。また、現行VC4の `put_device(&hdmi->ddc->dev)` と同じ寿命管理をNetBSD側でどう表現するかも別途決める必要がある。

#### 10.17.4 HDMI HPD GPIOの取得

VC4側は `hpd-gpios` propertyの存在を調べ、Linux GPIO番号とactive-low flagを取得する。

```c
if (of_find_property(dev->of_node, "hpd-gpios", &value)) {
        enum of_gpio_flags hpd_gpio_flags;

        hdmi->hpd_gpio = of_get_named_gpio_flags(dev->of_node,
            "hpd-gpios", 0, &hpd_gpio_flags);
        if (hdmi->hpd_gpio < 0)
                return hdmi->hpd_gpio;

        hdmi->hpd_active_low =
            hpd_gpio_flags & OF_GPIO_ACTIVE_LOW;
}
```

その後のVC4共通コードは `hdmi->hpd_gpio` を整数GPIO番号として `gpio_get_value_cansleep()` に渡す。

NetBSD側で確認したGPIO APIは次の通り。

- `sys/dev/fdt/fdt_gpio.h`: `struct fdtbus_gpio_pin` とacquire/release/read/write API
- `sys/dev/fdt/fdt_gpio.c`: FDT propertyのphandle/cellを解析し、GPIO controllerのcallbackを呼び出す
- `fdtbus_gpio_acquire(phandle, prop, flags)`: opaqueな `struct fdtbus_gpio_pin *` を返す
- `fdtbus_gpio_read()` / `fdtbus_gpio_read_raw()`: 論理値またはraw値を読み取る
- `fdtbus_gpio_release()`: acquireしたpinを解放する

NetBSDのGPIOは整数番号を返すLinux legacy GPIO APIとは異なり、pin objectを通じてcontrollerにアクセスする。従って `fdtbus_gpio_acquire()` の結果を `int hpd_gpio` に格納したり、`gpio_get_value_cansleep()` に渡したりすることはできない。active-lowの扱いもLinux `OF_GPIO_ACTIVE_LOW` とNetBSDのGPIO controller/pin APIの論理値・raw値の契約を確認してから決める必要がある。

現行 `sys/external/bsd/drm2/include/linux/of_gpio.h` は `linux/gpio/consumer.h` と `linux/of_device.h` をincludeするだけで、`of_get_named_gpio_flags()`、`enum of_gpio_flags`、`OF_GPIO_ACTIVE_LOW` の定義を提供していない。さらに今回確認したパスには `linux/gpio/consumer.h` 自体も見当たらない。ヘッダーをincludeするだけでは解決しない。

#### 10.17.5 VC4での呼び出し位置と影響範囲

現行 `sys/external/bsd/drm2/dist/drm/vc4/vc4_hdmi.c` の主なOF/FDT呼び出しは次の通り。

| 位置（確認時点） | 呼び出し | 条件・用途 |
|---|---|---|
| 1079 | `of_find_property(dev->of_node, "dmas", NULL)` | HDMI Audio初期化内。Audio macroが未定義のCIでは除外 |
| 1092 | `of_get_address(dev->of_node, 1, ...)` | HDMI Audio初期化内。Audio macroが未定義のCIでは除外 |
| 1360 | `of_parse_phandle(dev->of_node, "ddc", 0)` | HDMI bindの共通経路。Audio/CEC無効でも必要 |
| 1366 | `of_find_i2c_adapter_by_node(ddc_node)` | HDMI bindの共通経路。Audio/CEC無効でも必要 |
| 1367 | `of_node_put(ddc_node)` | HDMI bindの共通経路 |
| 1393 | `of_find_property(dev->of_node, "hpd-gpios", ...)` | HDMI HPD GPIOの共通経路 |
| 1396 | `of_get_named_gpio_flags(...)` | HDMI HPD GPIOの共通経路 |

従って、CIでAudio/CECが無効であっても、DDC phandle、I2C adapter lookup、HPD GPIOのOF/FDT依存は残る。Audio内の `of_find_property("dmas")` と `of_get_address()` は現行CIでの最初の修正対象ではない。

#### 10.17.6 修正設計の結論

今回の照合から、必要な作業を次のように分ける。

1. **device nodeの取得境界**: Linux `struct device.of_node` を前提とせず、NetBSD `device_t` / `platform_device.pd_dev` から `device_handle()` と `devhandle_to_of()` でnode番号を得る。どのdevice wrapperがその `device_t` を保持するかを明示してから実装する。
2. **phandle/property helper**: `of_parse_phandle` 相当を整数node番号で表現するのか、Linux互換 `struct device_node` wrapperを作るのかを決める。現在のFDT APIとの整合性からは整数node番号を使う方が自然だが、VC4側の型変更範囲を見て決定する。
3. **DDC I2C bridge**: `fdtbus_i2c_acquire()` の `i2c_tag_t` とLinux-KPI `struct i2c_adapter` の対応付けを実装する。戻り値型をキャストするだけの対応は禁止。参照取得・解放の契約も定義する。
4. **HPD GPIO bridge**: Linux整数GPIO番号を無理に再現するより、VC4側のHPD保持型とread/release経路をNetBSD `struct fdtbus_gpio_pin *` に合わせる案を優先検討する。active-lowの論理値はNetBSD GPIO APIの契約に従って扱う。
5. **Audio専用OF API**: Audioを有効化する段階で `dmas` propertyとbus address取得を改めて対応する。現行のAudio無効ビルドに対して先行実装しない。

現時点で `linux/of.h` にLinux互換のnode構造体・関数群を一括追加する変更は行わない。NetBSDのFDT nodeは整数handle/offsetを中心に構成されており、Linuxのnode pointerと参照カウントをそのまま模倣すると、不要な寿命管理や誤った型変換を導入するおそれがある。

#### 10.17.7 次の実装順序

- 第1段階: `struct device` と `platform_device.pd_dev` の対応を確認し、VC4 bindからNetBSD FDT node番号を取得する単一の内部helperを決める。
- 第2段階: `ddc` propertyの解決と、NetBSD `i2c_tag_t` からLinux-KPI adapterへのbridgeを設計する。
- 第3段階: HPD GPIOを `fdtbus_gpio_pin` ベースに変更し、acquire/read/releaseとactive-lowを実装する。
- 第4段階: Audioを有効化する段階でのみ `dmas` と `reg` の取得helperを実装する。

本節は現行ソースの照合結果と設計判断であり、まだCソースの修正やカーネルビルドを実施したものではない。次のパッチでは、まず第1段階のnode番号取得境界を最小変更で実装し、コンパイル結果を確認してからI2C/GPIOへ進む。


### 10.18 FDT node取得helperの設計

10.17節の照合結果を踏まえ、VC4 Linux-KPI側からNetBSD FDT nodeを取得するhelperを設計する。本節ではAPI契約と配置、呼び出し側の変更案を定義する。実装パッチやビルド検証はまだ行わない。

#### 10.18.1 設計方針

FDT nodeはLinuxの `struct device_node *` として保持せず、NetBSDのOF phandle整数値（`int`）として扱う。理由は次の通り。

- NetBSD `device_t` は `device_handle()` から `devhandle_t` を取得できる。
- FDT/OFデバイスのhandle種別は `DEVHANDLE_TYPE_OF` で識別できる。
- `devhandle_to_of()` はOF型handleを整数node値に変換する。
- NetBSD `fdtbus_*` / `of_*` APIは整数node値を受け取る。
- NetBSD側にはLinux `device_node` のget/put参照カウントをそのまま適用する根拠がない。

特に現行 `struct device` は `device_t bsdev` のみを持つ。現行 `platform_device` は `pd_dev` と `dev` を別メンバーとして持つが、`pd_dev` から `dev.bsdev` への初期化・同期を保証する実装は今回確認できていない。そのため、`dev->bsdev` を前提にする汎用helperや、`struct device` に `of_node` を追加する案は採用しない。

#### 10.18.2 helperの配置とAPI契約

helperは `sys/external/bsd/drm2/include/linux/platform_device.h` に置く。現状のVC4呼び出し元は `to_platform_device(dev)` で `struct platform_device *` を得ており、NetBSD固有の実deviceは `pdev->pd_dev` に明示的に存在するためである。

提案するAPI:

```c
static inline int
platform_device_of_node(const struct platform_device *pdev)
{
        devhandle_t handle;

        if (pdev == NULL || pdev->pd_dev == NULL)
                return -1;

        handle = device_handle(pdev->pd_dev);
        if (devhandle_type(handle) != DEVHANDLE_TYPE_OF)
                return -1;

        return devhandle_to_of(handle);
}
```

必要な宣言・include:

```c
#include <sys/device.h>
#include <dev/ofw/openfirm.h>
```

`sys/device.h` は `devhandle_t`、`device_handle()`、`devhandle_type()`、`DEVHANDLE_TYPE_OF` を提供する。`<dev/ofw/openfirm.h>` は `devhandle_to_of()` を宣言する。

戻り値契約:

- 0以上: NetBSD OF/FDT node値。呼び出し元はそのまま `fdtbus_*` / `of_*` APIへ渡す。
- -1: platform deviceがNULL、`pd_dev` がNULL、またはdevice handleがOF型ではない。
- node値は借用した整数値であり、helperは参照取得・解放を行わない。
- 呼び出し側は `of_node_put()` 相当を呼ばない。

`devhandle_to_of()` 自体は内部で `KASSERT(devhandle_type(handle) == DEVHANDLE_TYPE_OF)` を行うため、helper内で種別を確認してから呼ぶ。非OF handleを直接渡してはならない。

#### 10.18.3 VC4 HDMIでの使用方法

現行 `vc4_hdmi_bind()` は既に `struct platform_device *pdev` を取得している。したがって、nodeを必要とする共通処理では次のように置き換える。

```c
int node;

node = platform_device_of_node(pdev);
if (node < 0) {
        DRM_ERROR("device has no OF/FDT node\\n");
        return -ENXIO;
}

ddc_node = fdtbus_get_phandle(node, "ddc");
if (ddc_node < 0) {
        DRM_ERROR("Failed to find ddc node in device tree\\n");
        return -ENODEV;
}
```

この後のDDC I2C adapter bridgeは別段階であり、`ddc_node` を取得できたことだけでLinux `struct i2c_adapter *` が得られるわけではない。

HPD GPIOについても同じ `node` をproperty検索の起点として使用する。ただしGPIO取得は `fdtbus_gpio_acquire(node, "hpd-gpios", ...)` とopaque pin objectの保持に変更する必要があり、このhelper設計だけではGPIO部分は完成しない。

Audio専用 `vc4_hdmi_audio_init()` は `struct device *dev` しか直接受け取らないため、実装時には `to_platform_device(dev)` から `pdev` を得て同じhelperを使うか、Audio初期化関数の引数を `struct platform_device *pdev` に変更する。現行CIではAudioが無効なので、Audio対応は後段とする。

#### 10.18.4 このhelperをplatform_device限定にする理由

現行 `struct device` は `bsdev` を持つが、platform deviceの実deviceは `pd_dev` である。現時点で両者の同一性・初期化順序がコード上で保証されていることを確認できていない。したがって、次のような実装は避ける。

```c
/* 採用しない: bsdevの初期化を確認できていない */
node = devhandle_to_of(device_handle(dev->bsdev));
```

また、`to_platform_device()` は現在 `device_private(self)` を返すNetBSD固有の実装である。helperに渡す `pdev` は、この既存変換が正しくplatform deviceを返す呼び出し経路に限定する。通常のLinux `struct device *` 全般に適用できるAPIとして公開しない。

#### 10.18.5 エラー処理とprobe延期の扱い

node取得失敗と、DDC controller未登録は別のエラーである。

| 状態 | helper/API結果 | 呼び出し側の扱い |
|---|---|---|
| platform device / pd_devなし | `-1` | device構成異常として `-ENXIO` 等で失敗 |
| handleがOF型ではない | `-1` | FDT deviceではないため失敗 |
| nodeは取得できるが `ddc` propertyなし | `fdtbus_get_phandle()` が負値 | device tree設定不備として `-ENODEV` |
| ddc phandleはあるがI2C controller未登録 | `fdtbus_i2c_get_tag()` 等がNULL | probe/bind延期の仕組みが別途必要 |
| DDC controllerが登録済み | NetBSD `i2c_tag_t` | Linux-KPI adapter bridgeへ渡す |

Linuxの `-EPROBE_DEFER` を返すだけでNetBSD component frameworkが自動的にprobeを再実行するとは限らない。controller未登録時の再試行・attach順序は、FDT attachとcomponent bindの実際の呼び出し順を確認して別途設計する。

#### 10.18.6 実装前チェックリスト

- [ ] `platform_device.h` に `<dev/ofw/openfirm.h>` を追加し、上記helperを実装する。
- [ ] `device_handle(pdev->pd_dev)` がVC4 HDMI deviceについてOF型handleを返すことを確認する。
- [ ] `platform_device_of_node()` の戻り値が有効なFDT nodeであることを確認する。
- [ ] `vc4_hdmi_bind()` のDDC property取得を `fdtbus_get_phandle()` に置き換える。
- [ ] `of_node_put()` を削除する（node整数値に参照解放は不要）。
- [ ] `struct device` に `of_node` や未初期化の `bsdev` 依存を追加しない。
- [ ] I2C adapter bridgeとHPD GPIO変換は別パッチで実装・検証する。

このhelperはFDT node番号を得るための最小境界であり、Linux OF API全体の互換層ではない。実装後はまずコンパイルし、次に実機または診断ログで `pdev->pd_dev` のhandle種別とnode値を確認する。現段階ではドキュメント上の設計提案であり、ソースコードは未変更である。


### 10.19 `platform_device_of_node()` 仕様書

10.18節で設計したFDT node取得helperについて、実装者・呼び出し側が共有する仕様を定義する。本節をhelperのAPI契約とし、実装時に契約を変更する場合は本節も同時に更新する。

#### 10.19.1 目的と適用範囲

`platform_device_of_node()` は、NetBSD Linux-KPIの `struct platform_device` から、そのplatform deviceに対応するNetBSD OF/FDT node値を取得するための内部helperである。

- 対象はNetBSDのplatform deviceに限定する。
- Linuxの `struct device_node *` は生成・返却しない。
- 返却値はNetBSD `devhandle_to_of()` が返す整数node値であり、NetBSDの `fdtbus_*` / `of_*` APIに渡す。
- propertyの検索、phandleの解決、node参照カウント、I2C/GPIO資源の取得は行わない。
- Linux-KPI全般に公開する汎用OF helperではない。

#### 10.19.2 宣言・配置

実装ファイル:

`sys/external/bsd/drm2/include/linux/platform_device.h`

関数宣言:

```c
static inline int platform_device_of_node(
    const struct platform_device *pdev);
```

ヘッダーは、少なくとも以下の宣言が可視となるようにする。

```c
#include <sys/device.h>
#include <dev/ofw/openfirm.h>
```

既存のinclude順序や重複include guardに従い、同じヘッダーを重複して追加しない。実装本体は10.18.2節の提案コードを基準とする。

#### 10.19.3 入力契約

| 項目 | 仕様 |
|---|---|
| 引数 | `const struct platform_device *pdev` |
| NULL | 許容する。失敗値を返す |
| `pdev->pd_dev` | NetBSDの実device (`device_t`) であること。NULLは失敗 |
| device handle | `device_handle(pdev->pd_dev)` で取得 |
| handle種別 | `DEVHANDLE_TYPE_OF` であること |
| handle検査 | `devhandle_to_of()` を呼ぶ前に種別を検査する |

このhelperは `pdev->dev.bsdev`、`struct device.of_node`、`device_private()` を直接参照してはならない。`pd_dev` がNetBSD側の実deviceであるというplatform deviceの既存契約を利用する。

#### 10.19.4 戻り値契約

| 戻り値 | 意味 | 呼び出し側の処理 |
|---|---|---|
| `>= 0` | OF/FDT node値を取得できた | FDT/OF APIへ渡す |
| `-1` | 引数または実deviceがNULL、またはhandleがOF型でない | 呼び出し側で適切なerrnoへ変換する |

`-1` はこのhelper固有の失敗値であり、Linux errnoを直接返すものではない。呼び出し側はこの値を `-ENXIO` 等へ変換し、`-1` をそのまま上位のprobe/bind戻り値として返さない。

成功時のnode値はNetBSDのOF node識別子であり、Linux `struct device_node *` ではない。node値をポインタへcastしたり、Linux OF APIへそのまま渡してはならない。

#### 10.19.5 所有権・寿命・並行性

- node値はdevice handleから得た識別値であり、helperはnodeやFDT blobの参照カウントを増減しない。
- 呼び出し側はこの戻り値に対して `of_node_put()`、`of_node_get()` を呼ばない。
- 戻り値はFDT nodeの存在を表す識別値であり、DDC controllerやGPIO pin等の資源所有権を表さない。
- DDC/GPIO等の資源は、それぞれのNetBSD APIで個別に取得・解放する。
- helperは状態を保持せず、global cacheやstatic mutable dataを導入しない。
- `pdev` と `pdev->pd_dev` の寿命・attach順序は呼び出し側が保証する。helper自体はdevice detachとの同期を行わない。

#### 10.19.6 エラー分類

helperが返す `-1` は、NULL入力または非OF handleをまとめた結果である。詳細な原因を区別する必要がある場合は、helperの責務を拡張せず、呼び出し側で入力を検査して診断メッセージを出す。

node取得成功後のproperty・phandle・資源取得失敗はhelperのエラーではない。例えば:

- `ddc` propertyがない: FDT property解決側のエラー。
- phandle先のI2C controllerがまだ登録されていない: I2C attach順序・再試行側の問題。
- `hpd-gpios` がない、またはGPIO controllerが利用できない: GPIO property/acquire側の問題。

これらを `platform_device_of_node()` の戻り値に混在させない。

#### 10.19.7 呼び出し側の標準パターン

```c
int node;

node = platform_device_of_node(pdev);
if (node < 0) {
        device_printf(pdev->pd_dev,
            "device has no OF/FDT node\\n");
        return -ENXIO;
}

/* node is a NetBSD OF/FDT node identifier. */
ddc_node = fdtbus_get_phandle(node, "ddc");
if (ddc_node < 0)
        return -ENODEV;
```

上記はAPIの使用例であり、`fdtbus_get_phandle()` の戻り値やDDC adapter取得の実装は別途NetBSD側APIの契約に従う。node取得helperの成功をもってDDC controllerの存在・登録完了を仮定してはならない。

#### 10.19.8 実装時の受け入れ条件

実装パッチは次の条件を満たすこと。

1. `platform_device.h` に宣言・定義があり、必要な型・関数宣言を正しいヘッダーから取得する。
2. `pdev == NULL` と `pdev->pd_dev == NULL` を安全に拒否する。
3. `device_handle()` の結果が `DEVHANDLE_TYPE_OF` であることを確認してから `devhandle_to_of()` を呼ぶ。
4. 非OF handleに対して `devhandle_to_of()` を呼ばず、KASSERTを発生させない。
5. 成功時は `devhandle_to_of()` の結果をそのまま返し、ポインタ変換や独自node番号変換を行わない。
6. `struct device` に `of_node` を追加せず、`pdev->dev.bsdev` に依存しない。
7. 呼び出し側で失敗値をerrnoに変換し、成功nodeに対する `of_node_put()` を削除する。
8. VC4 HDMIの実際の `pdev->pd_dev` がOF型handleを持つことを、ビルドに加えて診断ログまたは実機で確認する。

#### 10.19.9 仕様上の未確定事項

本仕様はhelperの入力・出力・所有権を定義するものであり、VC4 HDMIのplatform device生成側が `pd_dev` にどのdevice handleを設定するかまでは保証しない。実装時にFDT node取得が失敗した場合は、helperを緩めて別メンバーを推測参照するのではなく、platform device生成・登録経路を調査する。

また、本helperの戻り値は現時点で `int` / `-1` とする設計である。NetBSDのOF node値に負値が使われないことを対象ブランチのAPI契約で再確認する。将来、より明確なエラー伝達が必要になった場合は、戻り値とnode出力引数を分離するAPIへの変更を検討し、その際は呼び出し側を含めて仕様を更新する。

この仕様書の追加時点では、helper本体のC実装・コンパイル・実機動作確認は未実施である。
