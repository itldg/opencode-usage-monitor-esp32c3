#include "audio_player.h"

#include <stdlib.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "es8311_codec.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "touch_ft6336.h"

#define TAG "audio"

#define AUDIO_SAMPLE_RATE 22050
#define AUDIO_BITS        16
#define AUDIO_CHUNK       4096 /* 单声道字节/块 */
#define AUDIO_QUEUE_LEN   4
#define AUDIO_PART_LABEL  "audio"
#define AUDIO_PART_SUBTYPE ((esp_partition_subtype_t)0x40)
#define CODEC_I2C_ADDR    ES8311_CODEC_DEFAULT_ADDR /* 0x30,8 位地址 */

/* 档位阈值(与 wav 命名 30/50/70/85/95/100 一致) */
static const int s_tiers[6] = { 30, 50, 70, 85, 95, 100 };

static i2s_chan_handle_t s_tx;
static esp_codec_dev_handle_t s_codec;
static QueueHandle_t s_queue;
static bool s_enabled;
static bool s_idx_loaded;
static struct {
    uint32_t offset;
    uint32_t size;
} s_idx[AUDIO_COUNT];

static uint32_t rd32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static const esp_partition_t *audio_partition(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, AUDIO_PART_SUBTYPE, AUDIO_PART_LABEL);
}

/* 读 audio.bin 头部索引(12 字节头 + 每项 24 字节 {offset,size,name[16]}) */
static void load_index(void)
{
    const esp_partition_t *part = audio_partition();
    if (!part) {
        ESP_LOGW(TAG, "audio partition not found");
        return;
    }
    uint8_t hdr[12 + AUDIO_COUNT * 24];
    if (esp_partition_read(part, 0, hdr, sizeof(hdr)) != ESP_OK) return;
    if (memcmp(hdr, "AUDI", 4) != 0) {
        ESP_LOGW(TAG, "bad audio.bin magic");
        return;
    }
    uint16_t count = (uint16_t)(hdr[6] | (hdr[7] << 8));
    if (count > AUDIO_COUNT) count = AUDIO_COUNT;
    for (int i = 0; i < count; i++) {
        const uint8_t *e = hdr + 12 + i * 24;
        s_idx[i].offset = rd32(e);
        s_idx[i].size = rd32(e + 4);
    }
    s_idx_loaded = true;
}

/* 从 audio 分区流式播放一个 wav(单声道 → 双声道扩展后写入 codec) */
static void play_wav(int id)
{
    if (!s_idx_loaded || !s_idx[id].size) return;
    const esp_partition_t *part = audio_partition();
    if (!part) return;

    uint8_t *mono = malloc(AUDIO_CHUNK);
    int16_t *stereo = malloc(AUDIO_CHUNK * 2);
    if (!mono || !stereo) goto out;

    /* 扫描 chunk 定位 data(头部一次读入,音频头远小于 1KB) */
    uint8_t head[1024];
    uint32_t fsize = s_idx[id].size;
    uint32_t base = s_idx[id].offset;
    uint32_t toread = fsize < sizeof(head) ? fsize : sizeof(head);
    if (esp_partition_read(part, base, head, toread) != ESP_OK) goto out;

    uint32_t pos = 12, data_off = 0, data_len = 0;
    while (pos + 8 <= toread) {
        uint32_t cid = rd32(head + pos), clen = rd32(head + pos + 4);
        if (cid == 0x61746164) { /* "data" */
            data_off = pos + 8;
            data_len = clen;
            break;
        }
        pos += 8 + clen + (clen & 1);
    }
    if (!data_off || data_off + data_len > fsize) {
        ESP_LOGW(TAG, "wav data chunk not found");
        goto out;
    }

    uint32_t remain = data_len;
    size_t total = 0;
    while (remain) {
        uint32_t n = remain < AUDIO_CHUNK ? remain : AUDIO_CHUNK;
        if (esp_partition_read(part, base + data_off + (data_len - remain), mono, n) != ESP_OK) break;
        size_t frames = n / 2;
        for (size_t i = 0; i < frames; i++) {
            stereo[2 * i] = ((int16_t *)mono)[i];
            stereo[2 * i + 1] = ((int16_t *)mono)[i];
        }
        esp_err_t werr = esp_codec_dev_write(s_codec, stereo, (int)(frames * 4));
        if (werr != ESP_OK) {
            ESP_LOGW(TAG, "write id=%d failed: %s", id, esp_err_to_name(werr));
            break;
        }
        total += frames * 4;
        remain -= n;
    }
    ESP_LOGI(TAG, "played id=%d %u bytes", id, (unsigned)total);
out:
    free(mono);
    free(stereo);
}

static void audio_task(void *arg)
{
    int id;
    while (xQueueReceive(s_queue, &id, portMAX_DELAY)) {
        if (id >= 0 && id < AUDIO_COUNT) play_wav(id);
    }
    vTaskDelete(NULL);
}

esp_err_t audio_player_init(void)
{
    if (!CONFIG_OPENCODE_SOUND_ENABLE) {
        ESP_LOGI(TAG, "sound disabled by config");
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* GPIO11/12 出厂为 VDD_SPI 电源脚(3.3V 输出),作普通 IO 须烧写
       VDD_SPI_AS_GPIO efuse(一次性、不可逆,烧后重启生效)。
       注意:若该脚被用作 VDD_SPI 给外部 flash 供电,切勿烧写。 */
#if CONFIG_OPENCODE_I2S_WS_PIN == 11 || CONFIG_OPENCODE_I2S_WS_PIN == 12 || \
    CONFIG_OPENCODE_I2S_DOUT_PIN == 11 || CONFIG_OPENCODE_I2S_DOUT_PIN == 12
    if (!esp_efuse_read_field_bit(ESP_EFUSE_VDD_SPI_AS_GPIO)) {
        esp_efuse_write_field_bit(ESP_EFUSE_VDD_SPI_AS_GPIO);
        ESP_LOGW(TAG, "VDD_SPI_AS_GPIO burned, rebooting to use GPIO11/12...");
        esp_restart();
    }
#endif

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, NULL), TAG, "i2s_new_channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = CONFIG_OPENCODE_I2S_MCLK_PIN,
            .bclk = CONFIG_OPENCODE_I2S_BCLK_PIN,
            .ws = CONFIG_OPENCODE_I2S_WS_PIN,
            .dout = CONFIG_OPENCODE_I2S_DOUT_PIN,
            .din = GPIO_NUM_NC,
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_384;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG, "i2s init");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "i2s enable");

    /* 控制总线复用触摸的 I2C0(audio_codec_new_i2c_ctrl 内部会 add device) */
    i2c_master_bus_handle_t bus;
    ESP_RETURN_ON_ERROR(touch_i2c_bus_handle(&bus), TAG, "touch i2c bus not ready");

    audio_codec_i2c_cfg_t i2c_cfg = { .port = I2C_NUM_0, .addr = CODEC_I2C_ADDR, .bus_handle = bus };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_0, .tx_handle = s_tx, .rx_handle = NULL };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);

    es8311_codec_cfg_t codec_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = CONFIG_OPENCODE_AUDIO_PA_PIN,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&codec_cfg);
    ESP_RETURN_ON_FALSE(codec_if, ESP_FAIL, TAG, "es8311_codec_new");

    esp_codec_dev_cfg_t dev_cfg2 = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_codec = esp_codec_dev_new(&dev_cfg2);
    ESP_RETURN_ON_FALSE(s_codec, ESP_FAIL, TAG, "esp_codec_dev_new");

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = AUDIO_SAMPLE_RATE,
        .channel = 2,
        .bits_per_sample = AUDIO_BITS,
    };
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(s_codec, &fs), TAG, "codec open");
    esp_codec_dev_set_out_vol(s_codec, 70.0);

    load_index();
    s_queue = xQueueCreate(AUDIO_QUEUE_LEN, sizeof(int));
    xTaskCreate(audio_task, "audio", 3072, NULL, 5, NULL);
    s_enabled = true;
    ESP_LOGI(TAG, "audio ready (ES8311, %dHz)", AUDIO_SAMPLE_RATE);
    return ESP_OK;
}

void audio_player_play(int id)
{
    if (!s_enabled || !s_queue) return;
    xQueueSend(s_queue, &id, 0); /* 队列满直接丢弃,不阻塞主流程 */
}

void audio_player_report(const usage_quota_t *q)
{
    if (!s_enabled) return;
    const usage_bucket_t *b[3] = { &q->rolling, &q->weekly, &q->monthly };

    nvs_handle_t h;
    if (nvs_open("audio", NVS_READWRITE, &h) != ESP_OK) return;
    bool reset_played = false;
    for (int i = 0; i < 3; i++) {
        const usage_bucket_t *bk = b[i];
        if (!bk->valid || bk->percent < 0) continue;

        char ep_key[4], pl_key[4];
        snprintf(ep_key, sizeof(ep_key), "ep%d", i + 1);
        snprintf(pl_key, sizeof(pl_key), "pl%d", i + 1);

        /* 窗口重置(接口返回新的 resetsAt):已播档位清零,重新开始;首次记录不播 */
        int64_t last_epoch = 0;
        nvs_get_i64(h, ep_key, &last_epoch);
        if (bk->resets_at_epoch > 0 && last_epoch != bk->resets_at_epoch) {
            bool first_seen = (last_epoch == 0);
            nvs_set_i64(h, ep_key, bk->resets_at_epoch);
            nvs_set_u8(h, pl_key, 0);
            nvs_commit(h);
            if (!first_seen) reset_played = true;
        }

        int tier = -1;
        for (int t = 0; t < 6; t++) {
            if (bk->percent >= s_tiers[t]) tier = t;
        }
        if (tier < 0) continue;

        uint8_t played = 0;
        nvs_get_u8(h, pl_key, &played);
        if ((uint8_t)(tier + 1) > played) {
            nvs_set_u8(h, pl_key, tier + 1);
            nvs_commit(h);
            audio_player_play(AUDIO_1_30 + i * 6 + tier);
        }
    }
    nvs_close(h);
    if (reset_played) audio_player_play(AUDIO_RESET); /* 额度已重置 */
}
