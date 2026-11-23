# NetBSD BCM2711 HDMI DDC / VC4 DRM2対応 設計書

- 文書バージョン: 0.1
- 作成日: 2026-11-22
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


