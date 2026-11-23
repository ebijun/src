# NetBSD BCM2711 HDMI DDC / VC4 DRM2対応 設計書

- 文書バージョン: 0.6
- 作成日: 2026-11-22
- 最終更新日: 2026-11-23
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
