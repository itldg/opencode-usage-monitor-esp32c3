# OpenCode Go Usage Display (立创·实战派 ESP32-C3)

在板载 ST7789 (320x240) 屏幕上实时显示 OpenCode Go 套餐用量
(Rolling / Weekly / Monthly 三档百分比 + 状态栏),数据来自官方用量接口
`GET https://opencode.ai/zen/go/v1/usage`(Bearer 鉴权)。

## 硬件

- 立创·实战派 ESP32-C3(板载 0.96" ST7789 + FT6336 触摸 + 背光 + 3D 打印外壳)
- 8MB Flash(自定义分区表:factory 6MB)

| 功能 | 引脚 | 说明 |
| ---- | ---- | ---- |
| SPI LCD | SCLK=3, MOSI=5, DC=6, CS=4, RST=-1 | ST7789, 20MHz |
| 背光 | GPIO2 | 低电平点亮, LEDC 13bit 5kHz, 50% |
| I2C 触摸 | SDA=0, SCL=1 | FT6336 @ 0x38, 400kHz |

## 配置(menuconfig)

```
idf.py menuconfig
```

在 `OpenCode Go Usage Display` 菜单下填写:

- `OpenCode Go API Key`:从你电脑上的 opencode 配置读取:
  `%USERPROFILE%\.local\share\opencode\auth.json` → `opencode-go` 的 `key` 字段。
  也就是和 Anthropic 兼容 API Key 一样的一段 `sk-...`。
- `WiFi SSID` / `WiFi Password`:你的路由器凭据。
- `Auto refresh interval (minutes)`:自动刷新间隔,0 关闭(仅触摸刷新)。
- `WiFi connect retry count`:连接重试次数。

配置完需要**重新构建**才会生效(这些是编译期宏,不是运行时 NVS)。

## 构建 & 烧录

```bash
idf.py build
idf.py -p COM端口 flash monitor
```

## 使用

- 开机自动连 WiFi → 拉取用量 → 显示三档百分比。
- 主页下拉手势手动刷新(5 秒防抖),左右滑动切换 用量页 / 分析页。
- WiFi 掉线后每 60 秒自动重连重试。
- 标题栏右侧显示错误提示,右上角显示上次更新时间。

进度条颜色:<50% 绿,50~90% 橙,>90% 红。

## 结构

```
main/
  app_main.c      主循环:NVS → LCD/LVGL → WiFi → 刷新调度
  lcd_init.c      ST7789 SPI + LEDC 背光 + LVGL + 触摸集成
  touch_ft6336.c  FT6336 I2C 驱动(新 i2c_master API,坐标映射)
  wifi_app.c      可重入 WiFi 连接(断线重连不崩)
  usage_api.c     HTTPS 调用 usage 接口 + cJSON 防御式解析
  usage_ui.c      LVGL 界面(进度条/分析页/开机画面)
  lv_font_zh16.c  16px 中文字库子集(lv_font_conv 生成)
  lv_font_zh40.c  40px 中文字库子集(开机大标题)
```

## 中文字库(LVGL 字体子集)

LVGL 内置字体不含中文,本项目用 [lv_font_conv](https://github.com/lvgl/lv_font_conv)
把中文字体子集转成 C 数组(`main/lv_font_zh16.c` / `lv_font_zh40.c`),随组件编译。

安装工具:

```bash
npm install -g lv_font_conv
```

16px 正文(覆盖界面所有汉字 + ASCII):

```bash
lv_font_conv --no-compress --bpp 4 --size 16 --font zh16.ttf \
  -r 0x20-0x7F \
  --symbols "用量滚动每周月更新重置天时分即将获取失败点屏试连接中未知已刷新¥于小钟间数据日均预算可挥霍超支燃烧还能距安全注意危险析左右滑切换余充足勉强够限推，" \
  --format lvgl -o main/lv_font_zh16.c --force-fast-kern-format
```

40px 开机大标题:

```bash
lv_font_conv --no-compress --bpp 4 --size 40 --font zh40.ttf \
  -r 0x20-0x7F --symbols "老大哥" \
  --format lvgl -o main/lv_font_zh40.c --force-fast-kern-format
```

要点:

- `--symbols` 列出 UI 用到的全部汉字(16px 覆盖正文,40px 仅开机标题)。
  **新增界面文字时必须补进 `--symbols` 并重新生成**,否则该字显示为空白/方框。
- `-r 0x20-0x7F` 附带 ASCII(数字、`%`、`¥` 等)。
- `zh16.ttf` / `zh40.ttf` 建议用可商用中文字体(思源黑体、阿里巴巴普惠体等),
  存放在项目目录之外,不参与固件编译。
- 引用:`usage_ui.c` 中 `extern const lv_font_t lv_font_zh16;` 后通过
  `lv_obj_set_style_text_font()` 应用到 label。

## 接口兼容性备注

- 项目基于 **ESP-IDF v5.5.2**:
  - 证书 bundle 由 `mbedtls` 组件提供(不是独立 `esp_crt_bundle` 组件)。
  - I2C 使用新 `driver/i2c_master.h` API。
  - LVGL 8.3.11(managed_component)。
- 接口响应结构可能有变化,解析做了兼容:`usage.` 前缀/裸字段、percent
  数字/字符串、`resetsAt`/`resetsInSeconds` 两种形式均容错。