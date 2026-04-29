<script setup>
import { ref, computed } from 'vue'
import { Link2, PanelRightClose, Play, Check, Maximize } from 'lucide-vue-next'


const props = defineProps({
  connected: Boolean,
  clientCount: {
     type: Number,
     default: 0
  },
  isSidebarVisible: Boolean,
  instanceId: String
})

const emit = defineEmits(['toggle-sidebar', 'toggle-chat', 'join-instance'])

const copyStatus = ref(null)
const showInstanceInput = ref(false)
const inputInstanceId = ref('')

// Computed room URL
const roomUrl = computed(() => {
    if (!props.instanceId || props.instanceId === 'default') return window.location.origin
    return `https://activity.mintcocoa.cc/?room=${props.instanceId}`
})

const displayInstanceId = computed(() => {
    if (!props.instanceId || props.instanceId === 'default') return null
    return props.instanceId.length > 12 ? `${props.instanceId.slice(0, 8)}...` : props.instanceId
})

const copyRoomLink = async () => {
    try {
        await navigator.clipboard.writeText(roomUrl.value)
        copyStatus.value = 'success'
        setTimeout(() => copyStatus.value = null, 2000)
    } catch (e) {
        copyStatus.value = 'failed'
    }
}

const handleJoin = () => {
    if (inputInstanceId.value.trim()) {
        emit('join-instance', inputInstanceId.value.trim())
        showInstanceInput.value = false
        inputInstanceId.value = ''
    }
}
</script>

<template>
  <header class="glass-header flex items-center justify-between py-3 px-4 z-50 relative rounded-xl">
      <!-- Subtle top edge highlight -->
      <div class="absolute inset-x-0 top-0 h-px bg-gradient-to-r from-transparent via-white/10 to-transparent"></div>
      
      <!-- Left: Brand & Room Info -->
      <div class="flex items-center gap-4">
          <a href="/" class="flex items-center gap-2.5 group">
              <div class="logo-container w-9 h-9 rounded-xl bg-gradient-to-br from-blue-500 via-purple-500 to-pink-500 flex items-center justify-center text-white shadow-lg shadow-purple-500/30 group-hover:shadow-purple-500/50 transition-all duration-300">
                  <Play :size="16" fill="currentColor" class="ml-0.5" />
              </div>
              <span class="text-xl font-bold bg-clip-text text-transparent bg-gradient-to-r from-white via-white to-white/70 group-hover:to-white transition-all">
                  cocoaPLAYER
              </span>
          </a>
          
          <!-- Room ID / Copy -->
          <button 
              v-if="displayInstanceId"
              @click="copyRoomLink"
              class="room-badge flex items-center gap-2 px-3 py-1.5 rounded-full bg-white/5 border border-white/8 hover:bg-white/10 hover:border-white/15 transition-all text-xs font-medium text-white/60 hover:text-white"
          >
              <Check v-if="copyStatus === 'success'" :size="12" class="text-green-400" />
              <Link2 v-else :size="12" />
              <span>{{ displayInstanceId }}</span>
          </button>
      </div>

      <div class="hidden md:flex items-center px-4 py-1.5 rounded-full bg-white/5 border border-white/8 text-xs font-semibold text-white/60">
          VOD Player
      </div>

      <!-- Right: Status & Controls -->
      <div class="flex items-center gap-3">
          
          <!-- Join Room Input -->
          <div v-if="showInstanceInput" class="flex items-center gap-2 animate-fade-in-right">
              <input 
                  v-model="inputInstanceId"
                  @keydown.enter="handleJoin"
                  type="text" 
                  placeholder="Room ID..." 
                  class="bg-black/40 border border-white/10 rounded-lg px-3 py-1.5 text-sm text-white placeholder-white/30 focus:outline-none focus:border-purple-500/50 focus:bg-black/60 w-32 transition-all"
                  autoFocus
              />
              <button @click="handleJoin" class="text-xs font-bold text-purple-400 hover:text-purple-300 transition-colors">GO</button>
              <button @click="showInstanceInput = false" class="text-white/40 hover:text-white transition-colors"><PanelRightClose :size="14"/></button>
          </div>
          <button 
             v-else 
             @click="showInstanceInput = true" 
             class="text-xs font-semibold text-white/40 hover:text-white transition-colors px-2 py-1 rounded hover:bg-white/5"
          >
             Join Room
          </button>

          <div class="w-px h-5 bg-white/10 mx-1"></div>

          <!-- Connection Status - Glass pill -->
           <div class="status-pill flex items-center gap-2 px-3 py-1.5 rounded-full bg-white/5 border border-white/8 backdrop-blur-sm">
               <div class="relative flex h-2 w-2">
                  <span v-if="connected" class="animate-ping absolute inline-flex h-full w-full rounded-full bg-green-400 opacity-75"></span>
                  <span class="relative inline-flex rounded-full h-2 w-2" :class="connected ? 'bg-green-500' : 'bg-red-500'"></span>
               </div>
               <span class="text-xs font-medium text-white/50">{{ clientCount }} online</span>
           </div>

           <!-- Sidebar Toggle / Fullscreen Mode -->
           <button 
              @click="$emit('toggle-sidebar')"
              class="toggle-btn p-3 rounded-xl bg-white/5 hover:bg-red-500/20 border border-white/10 hover:border-red-500/50 text-white/60 hover:text-white transition-all group relative"
              title="Cinema Mode"
           >
               <Maximize :size="20" class="group-hover:scale-110 transition-transform" />
           </button>
      </div>
  </header>
</template>

<style scoped>
/* Glass Header */
.glass-header {
  background: rgba(255, 255, 255, 0.02);
  backdrop-filter: blur(12px);
  -webkit-backdrop-filter: blur(12px);
  border: 1px solid rgba(255, 255, 255, 0.05);
  box-shadow: 0 4px 16px rgba(0, 0, 0, 0.2);
}

/* Logo animation */
.logo-container {
  transition: transform 0.3s ease, box-shadow 0.3s ease;
}

.logo-container:hover {
  transform: scale(1.05) rotate(-2deg);
}

/* Room badge hover */
.room-badge {
  backdrop-filter: blur(8px);
  -webkit-backdrop-filter: blur(8px);
}

/* Status pill */
.status-pill {
  backdrop-filter: blur(8px);
  -webkit-backdrop-filter: blur(8px);
}

/* Toggle button */
.toggle-btn {
  backdrop-filter: blur(8px);
  -webkit-backdrop-filter: blur(8px);
}

/* Fade in animation */
.animate-fade-in-right {
    animation: fadeInRight 0.2s ease-out;
}
@keyframes fadeInRight {
    from { opacity: 0; transform: translateX(10px); }
    to { opacity: 1; transform: translateX(0); }
}

/* Performance optimization */
@media (prefers-reduced-transparency: reduce) {
  .glass-header,
  .room-badge,
  .status-pill,
  .toggle-btn {
    backdrop-filter: none;
    -webkit-backdrop-filter: none;
    background: rgba(20, 20, 25, 0.95);
  }
}
</style>
