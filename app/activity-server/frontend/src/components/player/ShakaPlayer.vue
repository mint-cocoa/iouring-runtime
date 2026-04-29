<script setup>
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import Hls from 'hls.js'
import { Film, Loader2, Maximize, Pause, Play, RotateCcw, SkipForward, Volume2, VolumeX } from 'lucide-vue-next'

const props = defineProps({
    videoUrl: {
        type: String,
        default: ''
    },
    className: {
        type: String,
        default: ''
    },
    serverPlaybackState: {
        type: Object,
        default: null
    }
})

const emit = defineEmits(['toast', 'ended', 'skip'])

const containerRef = ref(null)
const videoRef = ref(null)
const hlsRef = ref(null)
const retryTimerRef = ref(null)

const isPlaying = ref(false)
const currentTime = ref(0)
const duration = ref(0)
const volume = ref(1)
const isMuted = ref(false)
const isBuffering = ref(false)
const error = ref(null)
const retryCount = ref(0)
const isLiveStream = ref(false)
const liveEdgeRecovered = ref(false)

const controlsVisible = ref(true)
const controlsTimeoutId = ref(null)

const DRIFT_THRESHOLD_SEC = 2.0
const MAX_RECOVERY_RETRIES = 3

const isHlsUrl = (url) => {
    try {
        return new URL(url, window.location.origin).pathname.toLowerCase().endsWith('.m3u8')
    } catch {
        return String(url || '').includes('.m3u8')
    }
}

const isLive = computed(() => {
    const video = videoRef.value
    return isLiveStream.value || !Number.isFinite(duration.value) || duration.value === Infinity || (video?.seekable?.length && duration.value === 0)
})

const progressPercent = computed(() => {
    if (isLive.value || !duration.value) return 100
    return Math.min((currentTime.value / duration.value) * 100, 100)
})

const formatTime = (seconds) => {
    if (!Number.isFinite(seconds) || seconds < 0) return 'LIVE'
    const h = Math.floor(seconds / 3600)
    const m = Math.floor((seconds % 3600) / 60)
    const s = Math.floor(seconds % 60)

    if (h > 0) {
        return `${h}:${m.toString().padStart(2, '0')}:${s.toString().padStart(2, '0')}`
    }
    return `${m}:${s.toString().padStart(2, '0')}`
}

const showControls = () => {
    controlsVisible.value = true
    if (controlsTimeoutId.value) clearTimeout(controlsTimeoutId.value)
}

const hideControlsDelayed = () => {
    if (controlsTimeoutId.value) clearTimeout(controlsTimeoutId.value)
    if (!isPlaying.value) return

    controlsTimeoutId.value = setTimeout(() => {
        controlsVisible.value = false
    }, 2500)
}

watch(isPlaying, (val) => {
    if (!val) {
        showControls()
    } else {
        hideControlsDelayed()
    }
})

const destroyHls = () => {
    if (retryTimerRef.value) {
        clearTimeout(retryTimerRef.value)
        retryTimerRef.value = null
    }
    if (hlsRef.value) {
        hlsRef.value.destroy()
        hlsRef.value = null
    }
}

const applyVideoPrefs = () => {
    const video = videoRef.value
    if (!video) return
    video.volume = volume.value
    video.muted = isMuted.value
    video.playsInline = true
}

const startPlayback = () => {
    const video = videoRef.value
    if (!video) return
    applyVideoPrefs()
    video.play().catch(() => {
        showControls()
    })
}

const recoverLiveEdge = () => {
    const video = videoRef.value
    if (!video || !video.seekable?.length) return
    const end = video.seekable.end(video.seekable.length - 1)
    if (Number.isFinite(end) && Math.abs(end - video.currentTime) > 10) {
        video.currentTime = Math.max(0, end - 4)
    }
}

const scheduleReload = (url) => {
    if (retryCount.value >= MAX_RECOVERY_RETRIES) {
        error.value = 'Playback Error'
        emit('toast', 'Playback Error', 'error')
        return
    }
    retryCount.value += 1
    retryTimerRef.value = setTimeout(() => {
        loadVideo(url)
    }, 1000 * retryCount.value)
}

const loadWithHls = (url) => {
    const video = videoRef.value
    if (!video) return

    const hls = new Hls({
        lowLatencyMode: false,
        liveSyncDurationCount: 4,
        liveMaxLatencyDurationCount: 10,
        maxBufferLength: 30,
        maxMaxBufferLength: 90,
        backBufferLength: 30,
        maxBufferHole: 0.5,
        manifestLoadingTimeOut: 20000,
        levelLoadingTimeOut: 20000,
        fragLoadingTimeOut: 30000,
        fragLoadingMaxRetry: 6,
        manifestLoadingMaxRetry: 4,
        levelLoadingMaxRetry: 4,
        enableWorker: true,
    })

    hlsRef.value = hls

    hls.on(Hls.Events.MEDIA_ATTACHED, () => {
        hls.loadSource(url)
    })

    hls.on(Hls.Events.MANIFEST_PARSED, () => {
        error.value = null
        isBuffering.value = false
        retryCount.value = 0
        startPlayback()
    })

    hls.on(Hls.Events.LEVEL_LOADED, (_, data) => {
        if (data?.details?.live) {
            isLiveStream.value = true
            duration.value = Infinity
            if (!liveEdgeRecovered.value) {
                recoverLiveEdge()
                liveEdgeRecovered.value = true
            }
        }
    })

    hls.on(Hls.Events.ERROR, (_, data) => {
        if (!data?.fatal) return

        if (data.type === Hls.ErrorTypes.NETWORK_ERROR) {
            hls.startLoad()
            scheduleReload(url)
            return
        }

        if (data.type === Hls.ErrorTypes.MEDIA_ERROR) {
            hls.recoverMediaError()
            return
        }

        scheduleReload(url)
    })

    hls.attachMedia(video)
}

const loadVideo = async (url) => {
    const video = videoRef.value
    if (!video) return

    destroyHls()
    error.value = null
    isBuffering.value = Boolean(url)
    isLiveStream.value = false
    liveEdgeRecovered.value = false

    video.pause()
    video.removeAttribute('src')
    video.load()

    if (!url) {
        isPlaying.value = false
        currentTime.value = 0
        duration.value = 0
        isBuffering.value = false
        return
    }

    try {
        if (isHlsUrl(url) && Hls.isSupported()) {
            loadWithHls(url)
            return
        }

        if (isHlsUrl(url) && video.canPlayType('application/vnd.apple.mpegurl')) {
            video.src = url
            video.addEventListener('loadedmetadata', startPlayback, { once: true })
            return
        }

        video.src = url
        video.addEventListener('loadedmetadata', startPlayback, { once: true })
    } catch (err) {
        console.error('[HLS Player] Load error:', err)
        error.value = 'Failed to load video'
        isBuffering.value = false
    }
}

watch(() => props.videoUrl, (newUrl) => {
    retryCount.value = 0
    loadVideo(newUrl)
})

const handleTimeUpdate = () => {
    if (videoRef.value) currentTime.value = videoRef.value.currentTime
}

const handleDurationChange = () => {
    if (!videoRef.value) return
    duration.value = videoRef.value.duration
}

const handleWaiting = () => {
    isBuffering.value = true
}

const handleCanPlay = () => {
    isBuffering.value = false
}

const handlePlay = () => {
    isPlaying.value = true
    isBuffering.value = false
}

const handlePause = () => {
    isPlaying.value = false
}

const handleVolumeChange = () => {
    if (!videoRef.value) return
    volume.value = videoRef.value.volume
    isMuted.value = videoRef.value.muted
}

const handleEnded = () => {
    isPlaying.value = false
    if (!isLive.value) {
        emit('ended')
    }
}

watch(() => props.serverPlaybackState, (state) => {
    const video = videoRef.value
    if (!state || !video) return

    const { is_playing, time: serverMediaTime, paused_time } = state

    if (isLive.value) {
        if (is_playing && video.paused) {
            video.play().catch(() => {})
        } else if (!is_playing && !video.paused) {
            video.pause()
        }
        return
    }

    if (is_playing && video.paused) {
        video.play().catch(() => {})
    } else if (!is_playing && !video.paused) {
        video.pause()
    }

    const targetTime = serverMediaTime || paused_time || 0
    const drift = Math.abs(video.currentTime - targetTime)
    if (drift > DRIFT_THRESHOLD_SEC && Number.isFinite(targetTime)) {
        video.currentTime = targetTime
    }
}, { deep: true })

onMounted(() => {
    const video = videoRef.value
    if (!video) return

    video.addEventListener('timeupdate', handleTimeUpdate)
    video.addEventListener('durationchange', handleDurationChange)
    video.addEventListener('waiting', handleWaiting)
    video.addEventListener('stalled', handleWaiting)
    video.addEventListener('canplay', handleCanPlay)
    video.addEventListener('playing', handleCanPlay)
    video.addEventListener('play', handlePlay)
    video.addEventListener('pause', handlePause)
    video.addEventListener('volumechange', handleVolumeChange)
    video.addEventListener('ended', handleEnded)

    if (props.videoUrl) {
        loadVideo(props.videoUrl)
    }
})

onBeforeUnmount(() => {
    destroyHls()

    const video = videoRef.value
    if (video) {
        video.pause()
        video.removeAttribute('src')
        video.load()
        video.removeEventListener('timeupdate', handleTimeUpdate)
        video.removeEventListener('durationchange', handleDurationChange)
        video.removeEventListener('waiting', handleWaiting)
        video.removeEventListener('stalled', handleWaiting)
        video.removeEventListener('canplay', handleCanPlay)
        video.removeEventListener('playing', handleCanPlay)
        video.removeEventListener('play', handlePlay)
        video.removeEventListener('pause', handlePause)
        video.removeEventListener('volumechange', handleVolumeChange)
        video.removeEventListener('ended', handleEnded)
    }

    if (controlsTimeoutId.value) clearTimeout(controlsTimeoutId.value)
})

const togglePlay = (e) => {
    e?.stopPropagation()
    const video = videoRef.value
    if (!video) return

    if (video.paused) {
        video.play()
    } else {
        video.pause()
    }
}

const handleProgressClick = (e) => {
    if (isLive.value) {
        recoverLiveEdge()
        return
    }

    const rect = e.currentTarget.getBoundingClientRect()
    const percent = (e.clientX - rect.left) / rect.width
    const video = videoRef.value
    if (video && duration.value) {
        video.currentTime = percent * duration.value
    }
}

const handleSeek = (e) => {
    if (isLive.value) return
    const newTime = parseFloat(e.target.value)
    const video = videoRef.value
    if (video) {
        video.currentTime = newTime
        currentTime.value = newTime
    }
}

const handleVolume = (e) => {
    e.stopPropagation()
    const newVol = parseFloat(e.target.value)
    const video = videoRef.value
    if (video) {
        video.volume = newVol
        video.muted = newVol === 0
    }
}

const toggleMute = (e) => {
    e.stopPropagation()
    const video = videoRef.value
    if (video) {
        video.muted = !video.muted
    }
}

const toggleFullscreen = (e) => {
    e.stopPropagation()
    const container = containerRef.value
    if (!container) return

    if (!document.fullscreenElement) {
        container.requestFullscreen().catch(err => console.error(err))
    } else {
        document.exitFullscreen()
    }
}

const handleSkip = (e) => {
    e?.stopPropagation()
    emit('skip')
}
</script>

<template>
    <div
        ref="containerRef"
        class="premium-player-wrapper relative w-full h-full bg-black overflow-hidden rounded-xl group"
        :class="className"
        @mousemove="showControls(); hideControlsDelayed()"
        @mouseleave="controlsVisible = false"
        @click="togglePlay"
        :style="{ cursor: controlsVisible ? 'default' : 'none' }"
    >
        <video
            ref="videoRef"
            class="w-full h-full object-contain"
            playsinline
            preload="auto"
        ></video>

        <div v-if="!videoUrl && !error" class="absolute inset-0 flex flex-col items-center justify-center text-white/50 gap-4 pointer-events-none">
            <div class="bg-white/10 p-6 rounded-full">
                <Film :size="40" />
            </div>
            <div class="text-xl font-medium text-white">Ready to Watch</div>
            <div class="text-sm">Paste a URL to start streaming</div>
        </div>

        <div v-if="isBuffering && videoUrl" class="absolute inset-0 flex items-center justify-center z-10 pointer-events-none">
            <Loader2 class="animate-spin text-red-600" :size="48" />
        </div>

        <div v-if="error" class="absolute inset-0 flex flex-col gap-4 items-center justify-center bg-black/80 z-20 text-white">
            <div>{{ error }}</div>
            <div class="flex items-center gap-2">
                <button
                    class="px-4 py-2 rounded-lg bg-white/10 hover:bg-white/20 transition-colors"
                    @click.stop="retryCount = 0; loadVideo(videoUrl)"
                >
                    <RotateCcw class="inline-block w-4 h-4 mr-2" />
                    Retry
                </button>
                <button
                    class="px-4 py-2 rounded-lg bg-white/10 hover:bg-white/20 transition-colors"
                    @click.stop="handleSkip"
                >
                    <SkipForward class="inline-block w-4 h-4 mr-2" />
                    Skip
                </button>
            </div>
        </div>

        <div
            v-if="videoUrl && !error"
            class="absolute bottom-0 left-0 right-0 transition-all duration-300 z-10"
            :class="{ 'opacity-0 pointer-events-none translate-y-2': !controlsVisible, 'opacity-100 translate-y-0': controlsVisible }"
            @click.stop
        >
            <div class="absolute inset-0 bg-gradient-to-t from-black via-black/60 to-transparent pointer-events-none"></div>

            <div class="relative p-4 flex flex-col gap-3">
                <div
                    class="progress-container w-full h-1.5 group/progress cursor-pointer relative rounded-full overflow-hidden"
                    @click="handleProgressClick"
                >
                    <div class="absolute inset-0 bg-white/20 rounded-full"></div>
                    <div
                        class="absolute inset-y-0 left-0 bg-white/30 rounded-full transition-all duration-300"
                        :style="{ width: isLive ? '100%' : `${Math.min(progressPercent + 15, 100)}%` }"
                    ></div>
                    <div
                        class="absolute inset-y-0 left-0 rounded-full transition-all duration-100 progress-fill"
                        :style="{ width: `${progressPercent}%` }"
                    >
                        <div class="absolute right-0 top-0 bottom-0 w-8 bg-gradient-to-l from-red-500/50 to-transparent blur-sm"></div>
                    </div>
                    <div
                        v-if="!isLive"
                        class="absolute top-1/2 -translate-y-1/2 w-4 h-4 bg-white rounded-full shadow-lg shadow-black/50 scale-0 group-hover/progress:scale-100 transition-transform duration-200 ring-2 ring-red-500/50"
                        :style="{ left: `calc(${progressPercent}% - 8px)` }"
                    ></div>
                    <input
                        v-if="!isLive"
                        type="range"
                        min="0"
                        :max="duration || 100"
                        :value="currentTime"
                        @input="handleSeek"
                        class="absolute inset-0 w-full h-8 -top-3 opacity-0 cursor-pointer z-10"
                    />
                </div>

                <div class="flex items-center justify-between text-white">
                    <div class="flex items-center gap-5">
                        <button
                            @click="togglePlay"
                            class="w-10 h-10 flex items-center justify-center rounded-full bg-white/10 hover:bg-white/20 backdrop-blur-sm hover:scale-110 active:scale-95 transition-all duration-200"
                            title="Play/Pause"
                        >
                            <Pause v-if="isPlaying" :size="20" fill="currentColor" />
                            <Play v-else :size="20" fill="currentColor" class="ml-0.5" />
                        </button>

                        <button
                            @click="handleSkip"
                            class="w-10 h-10 flex items-center justify-center rounded-full bg-white/10 hover:bg-white/20 backdrop-blur-sm hover:scale-110 active:scale-95 transition-all duration-200"
                            title="Skip"
                        >
                            <SkipForward :size="20" />
                        </button>

                        <div class="flex items-center gap-2 group/vol">
                            <button @click="toggleMute" class="p-2 rounded-full hover:bg-white/10 transition-colors">
                                <VolumeX v-if="isMuted || volume === 0" :size="20" class="text-white/70" />
                                <Volume2 v-else :size="20" />
                            </button>
                            <div class="volume-slider-container w-0 group-hover/vol:w-24 overflow-hidden transition-all duration-300">
                                <div class="relative h-1 bg-white/20 rounded-full">
                                    <div
                                        class="absolute inset-y-0 left-0 bg-white rounded-full"
                                        :style="{ width: `${(isMuted ? 0 : volume) * 100}%` }"
                                    ></div>
                                    <input
                                        type="range"
                                        min="0"
                                        max="1"
                                        step="0.05"
                                        :value="isMuted ? 0 : volume"
                                        @input="handleVolume"
                                        class="absolute inset-0 w-full h-4 -top-1.5 opacity-0 cursor-pointer"
                                    />
                                </div>
                            </div>
                        </div>

                        <div class="flex items-center gap-2 text-sm font-medium tabular-nums">
                            <span v-if="isLive" class="text-red-400 font-bold">LIVE</span>
                            <template v-else>
                                <span class="text-white">{{ formatTime(currentTime) }}</span>
                                <span class="text-white/40">/</span>
                                <span class="text-white/60">{{ formatTime(duration) }}</span>
                            </template>
                        </div>
                    </div>

                    <div class="flex items-center gap-3">
                        <button
                            v-if="isLive"
                            @click.stop="recoverLiveEdge"
                            class="px-3 py-1.5 rounded-full bg-white/10 hover:bg-white/20 text-xs font-semibold transition-colors"
                        >
                            Live Edge
                        </button>
                        <button @click="toggleFullscreen" class="p-2 rounded-full hover:bg-white/10 transition-all hover:scale-110">
                            <Maximize :size="20" />
                        </button>
                    </div>
                </div>
            </div>
        </div>
    </div>
</template>

<style scoped>
.progress-fill {
    background: linear-gradient(90deg, #ef4444 0%, #ec4899 50%, #f43f5e 100%);
}

.progress-container {
    transition: height 0.2s ease;
}

.progress-container:hover {
    height: 0.5rem;
}

.volume-slider-container {
    transition: width 0.3s cubic-bezier(0.4, 0, 0.2, 1);
}
</style>
