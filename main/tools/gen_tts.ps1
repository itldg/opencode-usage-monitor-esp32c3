# gen_tts.ps1 — 用 Windows 本地 TTS 批量生成播报音频
# 命名规则: <窗口>_<档位>.wav
#   1 = 滚动 5h 窗口   2 = 周窗口   3 = 月窗口
#   档位: 30/50/70/85/95/100
# 输出目录: 脚本同目录下 audio/
# 用法: powershell -ExecutionPolicy Bypass -File gen_tts.ps1

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Speech

$outDir = Join-Path $PSScriptRoot 'audio'
if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Path $outDir | Out-Null }

# 首选晓晓 (神经网络, 音质最好); 没有则回退 Huihui
$voice = 'Microsoft Xiaoxiao'
$synth = New-Object System.Speech.Synthesis.SpeechSynthesizer
$found = $synth.GetInstalledVoices() | Where-Object { $_.VoiceInfo.Name -eq $voice }
if (-not $found) { $voice = 'Microsoft Huihui Desktop' }
$synth.SelectVoice($voice)
Write-Host "使用语音: $voice"

# 语速 -1 (略慢更自然), 音量 100
$synth.Rate = -1
$synth.Volume = 100

# 文案表: 文件名 = 文案
$phrases = @{
    'opencode' = 'OpenCode'
    'volcengine_coding' = '火山 Coding'
    'volcengine_agent' = '火山 Agent'

    # ---- 1: 滚动 5h 窗口 ----
    '1_30'  = '这一轮五小时,已经烧掉三成'
    '1_50'  = '五小时进度条过半,说话要挑重点了'
    '1_70'  = '五小时窗口只剩三成,再聊就超了'
    '1_85'  = '五小时窗口见底,最后一口气了'
    '1_95'  = '这一轮马上耗尽,先收手,额度眨眼就刷新'
    '1_100' = '五小时额度用光啦,稍等片刻就满血复活'

    # ---- 2: 周窗口 ----
    '2_30'  = '本周额度悄悄溜走了三成,剩下的要抱紧了'
    '2_50'  = '本周额度只剩一半,再贪玩周末就要断粮啦'
    '2_70'  = '本周额度快撑不住了,只剩三成,别让它饿肚子'
    '2_85'  = '注意注意,本周额度只剩一点点,再聊就见底了'
    '2_95'  = '本周额度正在咕嘟咕嘟冒泡,马上就没了,快住手'
    '2_100' = '本周额度已阵亡,请下周一准时复活'

    # ---- 3: 月窗口 ----
    '3_30'  = '本月的预算已花三成,悠着点'
    '3_50'  = '月度额度过半,下半月要精打细算'
    '3_70'  = '月度预算只剩三成,月底恐怕要吃紧'
    '3_85'  = '这个月额度快见底,只剩杯咖啡的量了'
    '3_95'  = '月度额度几乎耗尽,再写就超预算了'
    '3_100' = '本月额度正式清零,下个月再见'
}

foreach ($name in ($phrases.Keys | Sort-Object)) {
    $wav = Join-Path $outDir "$name.wav"
    $synth.SetOutputToWaveFile($wav)
    $synth.Speak($phrases[$name])
    $synth.SetOutputToNull()
    $len = (Get-Item $wav).Length
    Write-Host ("{0,-6} -> {1,8:N0} bytes" -f $name, $len)
}

$synth.Dispose()
Write-Host "完成! 输出目录: $outDir"