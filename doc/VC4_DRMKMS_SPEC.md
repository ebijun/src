# NetBSD Broadcom VC4 DRMKMS ドライバ移植・実装 全体仕様書

## 1. ドキュメント概要・目的

### 1.1 目的
本仕様書は、NetBSD/evbarm（Raspberry Pi シリーズ等）環境において、Broadcom VC4 (VideoCore IV / VI) GPU 向けの **DRMKMS (Direct Rendering Manager / Kernel Mode Setting) ドライバ** を NetBSD カーネルソースツリー（`sys/external/bsd/drm2`）へ移植・実装するための全体アーキテクチャ、ファイル構成、コンポーネント設計、およびビルド/検証仕様を定義するものである。

### 1.2 達成目標
1. **DRM デバイスノードの生成**: NetBSD 上で `/dev/dri/card0` および `/dev/dri/renderD128` を認識・生成する。
2. **FDT バインディングの確立**: Device Tree (FDT) から `/soc/gpu`、`/soc/v3d`、`/soc/hdmi` 等のノードを検出し、自動アタッチする。
3. **ハードウェア加速描画の開放**: `mpv --vo=drm` や OpenGL ES (Mesa/GBM) によるハードウェア描画およびコンソールダイレクト出力を可能にする。
4. **NetBSD 本家への統合**: KNF (Kernel Normal Form) および NetBSD 開発規約に準拠し、本家ソースツリー（`src`）への Pull Request / マージが可能な品質を確保する。

### 1.3 適用対象ハードウェア
- **プライマリターゲット (Phase 1)**:
  - Raspberry Pi 2 Model B (BCM2836)
  - Raspberry Pi 3 Model B / B+ / A+ / Zero 2 W (BCM2837)
- **セカンダリターゲット (Phase 2)**:
  - Raspberry Pi 4 Model B / Compute Module 4 (BCM2711)
- **フューチャーターゲット (Phase 3)**:
  - Raspberry Pi 5 (BCM2712)

---

## 2. システムアーキテクチャ

NetBSD カーネルにおける DRMKMS は、Linux カーネルの DRM サブシステムを NetBSD の `sys/external/bsd/drm2` 互換レイヤー（Linux KPI 抽象化層）を介して動作させる構造となっている。

```

\+-----------------------------------------------------------------------+
|                       User Space Applications                         |
|             (mpv --vo=drm, Mesa3D / GBM / EGL, wsfb-player)           |
\+-----------------------------------------------------------------------+
| (libdrm / ioctl)
v
\+-----------------------------------------------------------------------+
| NetBSD Kernel (/dev/dri/card0)                                        |
|                                                                       |
|  +-----------------------------------------------------------------+  |
|  | sys/external/bsd/drm2/dist/drivers/gpu/drm/vc4/               |  |
|  |  - vc4\_drv.c / vc4\_kms.c / vc4\_crtc.c / vc4\_hdmi.c / vc4\_v3d.c   |  |
|  |  (Linux DRM 移植コア・レンダリング/表示パイプライン制御)        |  |
|  +-----------------------------------------------------------------+  |
|                                  ^                                    |
|                                  | (Linux KPI / DRM2 Bridge)          |
|  +-----------------------------------------------------------------+  |
|  | sys/external/bsd/drm2/vc4/vc4\_fdt.c                             |  |
|  |  (NetBSD 独自 FDT アタッチメント・バスバインディング)             |  |
|  +-----------------------------------------------------------------+  |
|                                  ^                                    |
|                                  | (armfdt / simplebus)               |
|  +-----------------------------------------------------------------+  |
|  | NetBSD Device Tree (FDT) Subsystem                             |  |
|  |  - /soc/gpu, /soc/v3d, /soc/hdmi, /soc/hvs, /soc/pixelvalve      |  |
|  +-----------------------------------------------------------------+  |
\+-----------------------------------------------------------------------+
| (MMIO / DMA / Register Access)
v
\+-----------------------------------------------------------------------+
| Hardware: Broadcom BCM2835/2836/2837/2711 VideoCore IV GPU            |
\+-----------------------------------------------------------------------+

``` 

---

## 3. ディレクトリ・追加ファイル構成

本プロジェクトで追加および変更する NetBSD カーネルツリー内の主要ファイルおよびディレクトリ構造を以下に示す。

```

src/
├── .github/
│   └── workflows/
│       └── kernel-build.yml           \# GitHub Actions Cross-Compile CI
├── AGENTS.md                          \# AI / 開発者共通ガイドライン (KNF規約等)
├── doc/
│   └── VC4\_DRMKMS\_SPEC.md             \# 本仕様書
└── sys/
├── arch/arm/broadcom/
│   └── files.bcm2835              \# SoC デバイス定義ファイル (vc4drm 定義の追加)
├── arch/evbarm/conf/
│   └── GENERIC                    \# カーネルコンフィグ (vc4drm\* at fdt? の追加)
└── external/bsd/drm2/
├── conf/
│   └── files.drmkms           \# DRM2 ソースファイル・ビルド定義を追加
└── vc4/
├── README.md              \# VC4 移植開発用ドキュメント
├── vc4\_fdt.c              \# NetBSD 独自: FDT アタッチメント・バインディング
└── dist/                  \# Linux 由来コアコード (GPL/Dual License 保持)
└── drivers/gpu/drm/vc4/
├── vc4\_drv.c
├── vc4\_kms.c
├── vc4\_crtc.c
├── vc4\_hdmi.c
├── vc4\_hvs.c
├── vc4\_plane.c
└── vc4\_v3d.c

```` 

---

## 4. 各コンポーネント詳細設計

### 4.1 FDT アタッチメントドライバ (`sys/external/bsd/drm2/vc4/vc4_fdt.c`)
FDT (Flattened Device Tree) 上の `/soc/gpu`（または `brcm,bcm2835-vc4` / `brcm,bcm2837-vc4` / `brcm,bcm2711-vc4` コムパチブル文字列）を検出し、NetBSD のデバイスアタッチ機構（`cfattach`）を通じて DRMKMS のメイン構造体（`struct drm_device`）を生成・バインドするコアモジュール。

- **主な割り当て関数**:
  - `vc4_fdt_match()`: FDT ノードの `compatible` 文字列をマッチング。
  - `vc4_fdt_attach()`:
    1. MMIO レジスタ範囲（`bus_space_tag_t`, `bus_space_handle_t`）の確保。
    2. IRQ 割り込みハンドラ（`fdtbus_intr_establish`）の登録。
    3. `drm_dev_alloc()` および `vc4_drm_init()` を呼び出し、DRM2 サブシステムへデバイスを登録。
  - `vc4_fdt_detach()`: デバイスの安全な切り離し・リソース解放。

### 4.2 ビルド定義の設定 (`sys/arch/arm/broadcom/files.bcm2835` / `files.drmkms`)
カーネルビルドシステムに対して `vc4drm` デバイスの依存関係とソースコードのコンパイル条件を定義する。

- **`files.bcm2835` の追記仕様**:
  ```config
  # Broadcom VC4 DRMKMS Graphics
  attach vc4drm at fdt with vc4_fdt
  file   external/bsd/drm2/vc4/vc4_fdt.c             vc4drm

````

  - **`files.drmkms` の追記仕様**:
    ``` config
    # VC4 DRM Driver source files
    makeoptions vc4drm CPPFLAGS+="-I$S/external/bsd/drm2/dist/drivers/gpu/drm/vc4"
    
    file external/bsd/drm2/dist/drivers/gpu/drm/vc4/vc4_drv.c    vc4drm
    file external/bsd/drm2/dist/drivers/gpu/drm/vc4/vc4_kms.c    vc4drm
    file external/bsd/drm2/dist/drivers/gpu/drm/vc4/vc4_crtc.c   vc4drm
    file external/bsd/drm2/dist/drivers/gpu/drm/vc4/vc4_hdmi.c   vc4drm
    file external/bsd/drm2/dist/drivers/gpu/drm/vc4/vc4_hvs.c    vc4drm
    file external/bsd/drm2/dist/drivers/gpu/drm/vc4/vc4_plane.c  vc4drm
    file external/bsd/drm2/dist/drivers/gpu/drm/vc4/vc4_v3d.c    vc4drm
    
    ```

### 4.3 カーネルコンフィグの有効化 (`sys/arch/evbarm/conf/GENERIC`)

`GENERIC` コンフィグファイルに `vc4drm` デバイスのエントリを有効化する。

  - **設定仕様**:
    ``` config
    # DRM2 / DRMKMS Graphics
    options     DRM2
    vc4drm*     at fdt? pass 5      # Broadcom VC4 DRMKMS
    
    ```

-----

## 5\. 品質基準・コーディング規約 (NetBSD KNF)

NetBSD 本家（`src`）へのマージを達成するため、追加・修正するすべてのコードは以下の基準を100%遵守しなければならない。

1.  **インデント・レイアウト (KNF / `style(9)`)**:
      - インデントにはハードタブ（Tab幅=8）を厳格に使用する。
      - コメントは C スタイル `/* ... */` のみを使用し、C++ スタイル `//` コメントは一切排除する。
      - 1行長は 80 文字以内とする。
2.  **変数宣言**:
      - 変数はすべてブロック/関数の最頭部で宣言する（C99 スタイルのコード途中宣言は不可）。
3.  **ライセンス規定**:
      - 新規作成ファイル（`vc4_fdt.c` 等）には **2-Clause BSD License** ヘッダーを付与する。
      - Linux 由来のコード（`dist/` 配下）の著作権表示およびライセンス表示（GPL/Dual）は改変せず保持する。

-----

## 6\. ビルドおよび CI パイプライン仕様 (GitHub Actions)

開発中のコードの整合性を保つため、`git push` および Pull Request 発行時に GitHub Actions（`kernel-build.yml`）上で全自動クロスコンパイルを実施する。

  - **ビルドターゲット**:
      - アーキテクチャ: `evbarm` (`earmv7hf` / `aarch64`)
      - コマンド: `./build.sh -m evbarm -a earmv7hf -O ../obj -T ../tools kernel=GENERIC`
  - **検証合否基準**:
      - ツールチェーン構築およびカーネルイメージ（`netbsd`）のコンパイルが警告/エラーなしで完了すること。

-----

## 7\. 開発フェーズ・ロードマップ

| フェーズ        | 作業内容                            | 主要成果物 / 確認項目                                            |
| :---------: | :------------------------------ | :------------------------------------------------------ |
| **Phase 1** | リポジトリ環境整備・CI 構築                 | `ebijun/src` の準備、`AGENTS.md`、`kernel-build.yml` の稼働     |
| **Phase 2** | ビルド定義およびスケルトン実装                 | `vc4_fdt.c` スケルトン作成、`files.bcm2835` / `files.drmkms` 追加 |
| **Phase 3** | DRM2 Linux KPI ラッパー調整 & コンパイル通過 | Linux 側 `vc4` コードのインポート、カーネルのクロスコンパイル通過                 |
| **Phase 4** | 実機ブート & アタッチ検証                  | `/dev/dri/card0` の出現確認、`dmesg` での `vc4drm0 at fdt0` 認識  |
| **Phase 5** | RPi 4 / Dual-HDMI 拡張サポート        | BCM2711 (`brcm,bcm2711-vc4`) 対応および `aarch64` ビルド検証      |
| **Phase 6** | 画面描画 & アプリケーション検証               | `mpv --vo=drm` や OpenGL ES によるテスト再生の成功                  |
| **Phase 7** | コードクリーンアップ & 本家マージ申請            | `style(9)` チェック、`send-pr` 発行または GitHub PR の作成           |

``` 
 
