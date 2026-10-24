# BX Tacho

[English](README.md) | **繁體中文**

開源、可自己動手做的 **Beyblade X** 發射器轉速計。夾在發射器上面，以紅外線感測器讀取發射器內部旋轉的黑白圓盤，並在 OLED 螢幕上顯示發射轉速。

![BX Tacho 爆炸圖](docs/assembly.gif)

**控制網頁：[dazchi.github.io/bx-tacho](https://dazchi.github.io/bx-tacho/)**
（電腦上的 Chrome 或 Edge、Android 上的 Chrome）

## 功能

- **測量發射轉速**：光學偵測，最高約 20,000 RPM，並在螢幕上顯示結果與峰值。
- **偵測陀螺裝上與發射**：每次發射都會自動記錄，不必按任何按鈕。
- **保留發射紀錄**：最近 50 次發射、總次數、最佳紀錄與最新的發射曲線。
- **藍牙**：採用 Bey Battle Pass 協定，Battle Pass App 可以讀取你的發射數據。
- **使用 1S 鋰電池**：USB-C 充電、電源開關、電量偵測，閒置時自動休眠。
- **左撇子模式**：螢幕可旋轉 180°。

## 網頁版

[網頁版](https://dazchi.github.io/bx-tacho/)直接在瀏覽器中執行，不需安裝任何東西：

- **藍牙**：即時發射數據、電量、發射曲線與發射紀錄。
- **韌體更新**：透過 USB 或藍牙，一鍵安裝最新版本。
- **主控台**：透過 USB 或藍牙，用於設定、校正與診斷。

支援 English、繁體中文、简体中文與日本語。詳細說明：[web/README.md](web/README.md)（英文）。

## 硬體

ESP32-C3（ESP32-C3-WROOM-02）搭配兩顆 ITR8307 反射式紅外線感測器：
- 一顆經由 LMV331 比較器讀取轉速；
- 一顆偵測陀螺是否裝上。

電路板的其他部分：
- 0.96″ SSD1315 OLED；
- TP4054 充電 IC，具備負載共享電路，充電時也能使用；
- USB-C 供電與傳輸資料。

![電路圖](docs/schematic.png)

- [hardware/](hardware/)：KiCad 10 專案（電路圖、PCB、封裝、3D 模型）。JLCPCB 打樣與組裝用的 Gerber 與 BOM 在[硬體 v1.0 版本](https://github.com/dazchi/bx-tacho/releases/tag/hw-v1.0)中。
- [mechanical/](mechanical/)：3D 列印外殼的 STL 檔：`case_top`、`case_bottom` 與發射器夾具 `clip`。

## 韌體

ESP-IDF v6.1 韌體位於 [firmware/](firmware/)。

**安裝**：使用網頁版的更新按鈕，或從 [Releases](https://github.com/dazchi/bx-tacho/releases) 下載韌體。設定與發射紀錄都會保留。

**自行編譯**：

```bash
cd firmware
idf.py build
idf.py -p COMx flash monitor
```

編譯選項、主控台指令、校正與藍牙協定請見 [firmware/README.md](firmware/README.md)（英文）。

## 授權

[GPL-3.0-or-later](LICENSE)。Copyright © 2026 Darren Chi（[github.com/dazchi](https://github.com/dazchi)）。選用韌體擴充元件的例外條款請見 [NOTICE](NOTICE)。
