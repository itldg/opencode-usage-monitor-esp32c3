#pragma once

#include "esp_err.h"
#include "usage_api.h"

/* 音频 ID,顺序与 tools/pack_audio.py 打包顺序一致 */
enum {
    AUDIO_BOOT = 0,
    AUDIO_RESET,
    AUDIO_1_30, AUDIO_1_50, AUDIO_1_70, AUDIO_1_85, AUDIO_1_95, AUDIO_1_100,  /* 滚动窗口 */
    AUDIO_2_30, AUDIO_2_50, AUDIO_2_70, AUDIO_2_85, AUDIO_2_95, AUDIO_2_100,  /* 周窗口 */
    AUDIO_3_30, AUDIO_3_50, AUDIO_3_70, AUDIO_3_85, AUDIO_3_95, AUDIO_3_100,  /* 月窗口 */
    AUDIO_COUNT
};

/**
 * @brief 初始化 I2S + ES8311 + 播放任务(声音开关关闭时返回 ESP_ERR_NOT_SUPPORTED)。
 * 需在触摸 I2C 总线初始化之后调用(共用总线)。
 */
esp_err_t audio_player_init(void);

/** 排队播放一个音频(非阻塞,串行播放,队列满则丢弃) */
void audio_player_play(int id);

/**
 * @brief 每次 fetch 成功后调用:按用量档位播报(30/50/70/85/95/100%)。
 * 已播档位存 NVS,重启不重播;窗口重置(resetsAt 变化)后重新开始并播 reset.wav。
 */
void audio_player_report(const usage_quota_t *q);
