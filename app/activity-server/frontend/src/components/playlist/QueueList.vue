<script setup>
import { defineProps, defineEmits } from 'vue'
import { Clock, X, ListMusic } from 'lucide-vue-next'
import GlassButton from '../ui/GlassButton.vue'

const props = defineProps({
  queue: {
    type: Array,
    default: () => []
  }
})

const emit = defineEmits(['remove'])

const handleRemove = (id) => {
  emit('remove', id)
}

const onImageError = (e) => {
  e.target.style.display = 'none'
}
</script>

<template>
  <div class="h-full flex flex-col p-4 min-h-[400px]">
    <div class="flex items-center justify-between mb-4 border-b border-white/10 pb-3">
      <h3 class="text-lg font-semibold text-white glass-text">Up Next</h3>
      <span v-if="queue.length > 0" class="queue-badge bg-gradient-to-r from-red-500/20 to-pink-500/20 text-white/80 text-xs px-2.5 py-1 rounded-full border border-white/10">
        {{ queue.length }}
      </span>
    </div>

    <!-- Empty State -->
    <div v-if="queue.length === 0" class="flex-1 flex flex-col items-center justify-center text-white/30 gap-4">
      <div class="empty-icon p-5 rounded-2xl bg-gradient-to-br from-white/5 to-white/0 border border-white/5">
         <ListMusic :size="36" class="opacity-50" />
      </div>
      <div class="text-center">
        <p class="font-medium text-white/50">Queue is empty</p>
        <p class="text-xs text-white/30 mt-1">Paste a video URL below to start</p>
      </div>
    </div>

    <!-- Queue List -->
    <div v-else class="flex-1 overflow-y-auto pr-1 space-y-2 custom-scrollbar">
      <transition-group name="list">
        <div 
          v-for="(item, idx) in queue" 
          :key="item.id || idx"
          class="queue-item group relative flex items-center gap-3 p-2.5 rounded-xl transition-all border border-transparent"
        >
          <!-- Thumbnail -->
          <div v-if="item.thumbnail" class="relative w-20 aspect-video rounded-lg overflow-hidden bg-black/40 flex-shrink-0 ring-1 ring-white/5">
             <img 
               :src="item.thumbnail" 
               alt="Thumbnail" 
               class="w-full h-full object-cover"
               @error="onImageError"
             />
             <!-- Hover overlay -->
             <div class="absolute inset-0 bg-black/40 opacity-0 group-hover:opacity-100 transition-opacity flex items-center justify-center">
               <div class="w-8 h-8 rounded-full bg-white/20 backdrop-blur-sm flex items-center justify-center">
                 <span class="text-white text-xs">▶</span>
               </div>
             </div>
          </div>

          <!-- Info -->
          <div class="flex-1 min-w-0">
             <div class="truncate text-sm font-medium text-white/90 group-hover:text-white transition-colors" :title="item.title">
                {{ item.title || 'Untitled' }}
             </div>
             <div class="flex items-center gap-2 text-xs text-white/50 mt-1">
                <span v-if="item.duration > 0" class="flex items-center gap-1">
                   <Clock :size="10" /> {{ Math.round(item.duration) }}s
                </span>
                <span v-if="item.ext" class="px-1.5 py-0.5 bg-white/5 rounded text-white/40">{{ item.ext }}</span>
             </div>
          </div>

          <!-- Actions -->
          <button 
             class="remove-btn p-1.5 rounded-lg text-white/30 hover:text-red-400 hover:bg-red-500/10 transition-all opacity-0 group-hover:opacity-100"
             @click="handleRemove(item.id)"
             title="Remove"
          >
             <X :size="16" />
          </button>
        </div>
      </transition-group>
    </div>
    
  </div>
</template>

<style scoped>
/* Text shadow for glass readability */
.glass-text {
  text-shadow: 0 1px 2px rgba(0, 0, 0, 0.3);
}

/* Queue badge */
.queue-badge {
  backdrop-filter: blur(4px);
  -webkit-backdrop-filter: blur(4px);
}

/* Empty icon */
.empty-icon {
  backdrop-filter: blur(8px);
  -webkit-backdrop-filter: blur(8px);
}

/* Queue item hover */
.queue-item {
  background: transparent;
}

.queue-item:hover {
  background: rgba(255, 255, 255, 0.04);
  border-color: rgba(255, 255, 255, 0.08);
}

/* Remove button */
.remove-btn {
  backdrop-filter: blur(4px);
  -webkit-backdrop-filter: blur(4px);
}

/* Custom scrollbar */
.custom-scrollbar::-webkit-scrollbar {
  width: 4px;
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

/* List Transitions */
.list-enter-active,
.list-leave-active {
  transition: all 0.3s ease;
}
.list-enter-from,
.list-leave-to {
  opacity: 0;
  transform: translateX(-10px);
}

/* Performance optimization */
@media (prefers-reduced-transparency: reduce) {
  .queue-badge,
  .empty-icon,
  .remove-btn {
    backdrop-filter: none;
    -webkit-backdrop-filter: none;
  }
}
</style>
