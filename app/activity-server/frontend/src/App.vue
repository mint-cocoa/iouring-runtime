<script setup>
import { ref, computed, nextTick, watch } from 'vue'
import ShakaPlayer from './components/player/ShakaPlayer.vue'
import GlassCard from './components/ui/GlassCard.vue'
import Header from './components/common/Header.vue'
import QueueList from './components/playlist/QueueList.vue'
import { useCocoaSocket } from './composables/useCocoaSocket'
import { fetchWithNgrok } from './api/fetch'

import { MessageSquare, Send, X } from 'lucide-vue-next'

const getAnonymousName = () => {
    const key = 'cocoa_chat_name'
    const existing = localStorage.getItem(key)
    if (existing) return existing

    const name = `Guest ${Math.floor(1000 + Math.random() * 9000)}`
    localStorage.setItem(key, name)
    return name
}

// -- Setup --
const urlInput = ref('')
const chatInput = ref('')
const chatScrollRef = ref(null)
const chatName = ref(getAnonymousName())
const sidebarTab = ref('queue')
const isSidebarVisible = ref(true)
const instanceId = ref(new URLSearchParams(window.location.search).get('room') || 'default')
const nextRequestInFlight = ref(false)

// -- Socket Integration --
const showToast = (msg, type='info') => console.log(`[Toast ${type}] ${msg}`)

const {
    connected,
    clientCount,
    queue,
    chatMessages,
    serverMedia,
    serverPlaybackState,
    clientId,
    sendChatMessage,
    sendEvent
} = useCocoaSocket(instanceId.value, showToast)

watch(chatMessages, async () => {
    await nextTick()
    if (chatScrollRef.value) {
        chatScrollRef.value.scrollTop = chatScrollRef.value.scrollHeight
    }
}, { deep: true })

// -- Computed Video URL Logic --
const videoUrl = computed(() => {
    if (!serverMedia.value?.url) return null

    const isDiscordActivity = window.location.hostname.includes('discordsays.com')

    if (serverMedia.value.url.startsWith('/')) {
        return serverMedia.value.url
    }

    try {
        const url = new URL(serverMedia.value.url)
        
        if (isDiscordActivity) {
            if (url.hostname === window.location.hostname) {
                return url.pathname + url.search
            }
            const encoded = encodeURIComponent(serverMedia.value.url)
            return `/proxy/hls?url=${encoded}`
        }
        
        return serverMedia.value.url

    } catch (e) {
        console.warn('Invalid media URL', e)
        return serverMedia.value.url
    }
})

watch(videoUrl, () => {
    nextRequestInFlight.value = false
})

// -- Handlers --
const handleDownload = async () => {
    const urlToLoad = urlInput.value.trim()
    if (!urlToLoad) return

    try {
        await fetchWithNgrok('/api/download', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ url: urlToLoad, stream: 'hls', instance_id: instanceId.value })
        })
        urlInput.value = ''
    } catch (err) {
        console.error('Enqueue failed', err)
    }
}

const handleRemoveQueue = async (id) => {
    try {
      await fetchWithNgrok('/api/queue/remove', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ id, client_id: clientId.value, instance_id: instanceId.value })
      })
    } catch (err) {
        console.error('Remove failed', err)
    }
}

const handleJoinInstance = (id) => {
    window.location.search = `?room=${id}`
}

const handleSendChat = () => {
    const sent = sendChatMessage(chatInput.value, chatName.value)
    if (sent) chatInput.value = ''
}

const handlePlayerEnded = async () => {
    if (nextRequestInFlight.value) return
    nextRequestInFlight.value = true

    try {
        const response = await fetchWithNgrok('/api/next', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ instance_id: instanceId.value })
        })
        if (!response.ok) {
            throw new Error(`Next request failed: ${response.status}`)
        }
    } catch (err) {
        nextRequestInFlight.value = false
        console.error('Advance queue failed', err)
    }
}

</script>

<template>
  <div class="flex flex-col h-screen p-2 lg:p-3 gap-2 max-w-[1800px] mx-auto overflow-hidden">
    
    <!-- Top Header (Hidden in Cinema Mode) -->
    <Header 
        v-if="isSidebarVisible"
        :connected="connected"
        :client-count="clientCount"
        :is-sidebar-visible="isSidebarVisible"
        :instance-id="instanceId"
        @toggle-sidebar="isSidebarVisible = !isSidebarVisible"
        @join-instance="handleJoinInstance"
    />

    <main class="flex flex-col gap-2 flex-1 min-h-0" :class="{ '!p-0 !gap-0 fixed inset-0 z-50 bg-black': !isSidebarVisible }">
      
      <!-- Player Row: Player + Sidebar side by side -->
      <div class="flex flex-col lg:flex-row gap-3 flex-1 min-h-0" :class="{ '!gap-0 h-full': !isSidebarVisible }">
        
        <!-- Main Player Area (Level 2) -->
        <section class="flex-1 min-w-0" :class="{ 'h-full': !isSidebarVisible }">
          
          <!-- Player Container - Fills available space -->
          <GlassCard 
            :className="!isSidebarVisible 
                ? 'w-full h-full relative z-10 group !bg-black !rounded-none !border-none'
                : 'w-full h-full relative z-10 group !bg-black/60 !rounded-xl'"
          >
               
               <div class="w-full h-full">
                   <ShakaPlayer
                      key="shaka-player-instance"
                      :videoUrl="videoUrl"
                      :serverPlaybackState="serverPlaybackState"
                      @ended="handlePlayerEnded"
                   />
               </div>
               
               <!-- Floating UI Toggle (Visible only in Cinema Mode) -->
               <!-- Floating UI Toggle (Visible only in Cinema Mode) -->
               <div 
                  v-if="!isSidebarVisible"
                  class="absolute top-4 left-4 z-50 opacity-0 group-hover:opacity-100 transition-opacity duration-300"
               >
                  <button 
                      @click="isSidebarVisible = true"
                      class="p-2 rounded-full text-white/50 hover:text-white transition-colors"
                      title="Exit Cinema Mode"
                  >
                      <X class="w-8 h-8 drop-shadow-md" />
                  </button>
               </div>

          </GlassCard>
        </section>

        <!-- Sidebar - stretches to match player height -->
        <aside 
            class="lg:w-80 flex flex-col transition-all duration-500 ease-in-out self-stretch"
            :class="{ '-mr-[22rem] opacity-50 w-0 !p-0 overflow-hidden': !isSidebarVisible }"
        >
        <GlassCard variant="strong" className="h-full flex flex-col overflow-hidden !p-0 !gap-0">
            <!-- Internal Tabs Header -->
            <div class="flex border-b border-white/10">
                <button
                    @click="sidebarTab = 'queue'"
                    class="flex-1 py-4 text-sm font-bold uppercase tracking-wider transition-all relative"
                    :class="sidebarTab === 'queue'
                      ? 'text-white bg-white/5'
                      : 'text-white/40 hover:text-white hover:bg-white/5'"
                >
                    Queue
                    <div v-if="sidebarTab === 'queue'" class="absolute bottom-0 left-0 w-full h-0.5 bg-gradient-to-r from-red-500 to-pink-500"></div>
                </button>
                <button
                    @click="sidebarTab = 'chat'"
                    class="flex-1 py-4 text-sm font-bold uppercase tracking-wider transition-all relative"
                    :class="sidebarTab === 'chat'
                      ? 'text-sky-300 bg-sky-500/10'
                      : 'text-white/40 hover:text-white hover:bg-white/5'"
                >
                    Chat
                    <div v-if="sidebarTab === 'chat'" class="absolute bottom-0 left-0 w-full h-0.5 bg-gradient-to-r from-sky-400 to-emerald-400"></div>
                </button>
            </div>

            <!-- Queue Content -->
            <div v-if="sidebarTab === 'queue'" class="flex-1 flex flex-col min-h-0">
                <!-- Queue Content -->
                <div class="flex-1 overflow-y-auto custom-scrollbar pt-2 px-1">
                    <QueueList 
                        :queue="queue" 
                        @remove="handleRemoveQueue"
                    />
                </div>
                
                <!-- Input Area - Glass style -->
                <div class="p-4 border-t border-white/10 bg-gradient-to-t from-black/40 to-transparent backdrop-blur-sm z-10"> 
                    <div class="relative group">
                        <input 
                            v-model="urlInput"
                            @keydown.enter="handleDownload"
                            type="text" 
                            placeholder="Paste video URL..." 
                            class="w-full bg-white/5 border border-white/10 rounded-xl px-4 py-3 pl-10 text-sm focus:outline-none focus:border-red-500/50 focus:bg-white/10 transition-all text-white placeholder-white/30"
                        />
                        <span class="absolute left-3 top-3.5 text-white/30 text-lg group-focus-within:text-red-500 transition-colors">🔗</span>
                        <div class="absolute right-2 top-2">
                            <button 
                                @click="handleDownload"
                                class="glass-add-btn bg-white/10 hover:bg-gradient-to-r hover:from-red-500 hover:to-pink-500 text-white rounded-lg px-3 py-1.5 text-xs font-semibold transition-all opacity-0 group-focus-within:opacity-100"
                            >
                                Add
                            </button>
                        </div>
                    </div>
                </div>
            </div>

            <!-- Chat Content -->
            <div v-else class="flex-1 flex flex-col min-h-0">
                <div ref="chatScrollRef" class="flex-1 overflow-y-auto custom-scrollbar p-3 space-y-3">
                    <div v-if="chatMessages.length === 0" class="h-full flex flex-col items-center justify-center text-center gap-3 text-white/35">
                        <MessageSquare class="w-8 h-8" />
                        <p class="text-sm">No messages yet</p>
                    </div>

                    <div
                        v-for="(message, index) in chatMessages"
                        :key="`${message.server_ts || index}-${index}`"
                        class="flex"
                        :class="message.sender === chatName ? 'justify-end' : 'justify-start'"
                    >
                        <div
                            class="max-w-[82%] rounded-2xl px-3 py-2 text-sm leading-relaxed border"
                            :class="message.sender === chatName
                              ? 'bg-sky-500/20 border-sky-400/20 text-white rounded-br-md'
                              : 'bg-white/7 border-white/8 text-white/90 rounded-bl-md'"
                        >
                            <div v-if="message.sender !== chatName" class="text-[11px] font-semibold text-emerald-300/80 mb-1">
                                {{ message.sender || 'Guest' }}
                            </div>
                            <div class="break-words whitespace-pre-wrap">{{ message.text }}</div>
                        </div>
                    </div>
                </div>

                <div class="p-4 border-t border-white/10 bg-gradient-to-t from-black/40 to-transparent backdrop-blur-sm z-10">
                    <form class="relative" @submit.prevent="handleSendChat">
                        <input
                            v-model="chatInput"
                            type="text"
                            maxlength="500"
                            placeholder="Type anonymously..."
                            class="w-full bg-white/5 border border-white/10 rounded-xl px-4 py-3 pr-12 text-sm focus:outline-none focus:border-sky-400/50 focus:bg-white/10 transition-all text-white placeholder-white/30"
                            :disabled="!connected"
                        />
                        <button
                            type="submit"
                            class="absolute right-2 top-2 p-2 rounded-lg text-white/45 hover:text-sky-300 hover:bg-sky-500/10 disabled:opacity-30 disabled:hover:bg-transparent transition-all"
                            :disabled="!chatInput.trim() || !connected"
                            title="Send"
                        >
                            <Send class="w-5 h-5" />
                        </button>
                    </form>
                </div>
            </div>

        </GlassCard>
        </aside>

      </div>

      <!-- Stream Meta - Compact Glass Bar (Hidden in Cinema Mode) -->
      <div v-if="isSidebarVisible" class="flex items-center justify-between px-4 py-2 bg-white/5 rounded-xl border border-white/8 backdrop-blur-sm">
          <div class="flex items-center gap-3">
              <div class="w-2 h-2 rounded-full bg-purple-500"></div>
              <span class="text-sm font-medium text-white/90">
                  {{ serverMedia?.title || 'VOD Player' }}
              </span>
              <span class="text-xs text-white/40">•</span>
              <span class="text-xs text-white/40">
                  {{ serverMedia ? `${serverMedia.stream_type}${serverMedia.duration ? ' • ' + Math.round(serverMedia.duration) + 's' : ''}` : 'Waiting...' }}
              </span>
          </div>
          <button class="text-xs text-white/40 hover:text-white transition-colors">Share</button>
      </div>

    </main>
  </div>
</template>

<style scoped>
/* Glass Tab Styles */
.glass-tab {
  backdrop-filter: blur(8px);
  -webkit-backdrop-filter: blur(8px);
}

.glass-tab-active {
  box-shadow: 
    0 -4px 16px rgba(0, 0, 0, 0.2),
    inset 0 1px 0 rgba(255, 255, 255, 0.05);
}

/* Text shadow for readability */
.glass-text {
  text-shadow: 0 1px 2px rgba(0, 0, 0, 0.3);
}

/* Custom scrollbar */
.custom-scrollbar::-webkit-scrollbar {
    width: 6px;
}
.custom-scrollbar::-webkit-scrollbar-track {
    background: transparent;
}
.custom-scrollbar::-webkit-scrollbar-thumb {
    background: rgba(255, 255, 255, 0.1);
    border-radius: 10px;
}
.custom-scrollbar::-webkit-scrollbar-thumb:hover {
    background: rgba(255, 255, 255, 0.2);
}

/* Add button animation */
.glass-add-btn {
  box-shadow: 0 2px 8px rgba(0, 0, 0, 0.2);
}

.glass-add-btn:hover {
  transform: scale(1.05);
  box-shadow: 0 4px 16px rgba(239, 68, 68, 0.3);
}
</style>
