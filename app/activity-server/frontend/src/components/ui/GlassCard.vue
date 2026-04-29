<script setup>
defineProps({
  className: {
    type: String,
    default: ''
  },
  variant: {
    type: String,
    default: 'default' // 'default', 'strong', 'subtle'
  }
})

const variants = {
  default: 'glass-card-default',
  strong: 'glass-card-strong',
  subtle: 'glass-card-subtle'
}
</script>

<template>
  <div 
    class="glass-card relative rounded-2xl overflow-hidden" 
    :class="[variants[variant], className]"
  >
    <!-- Top edge highlight (유리 반사 효과) -->
    <div class="absolute inset-x-0 top-0 h-px bg-gradient-to-r from-transparent via-white/10 to-transparent pointer-events-none"></div>
    <slot />
  </div>
</template>

<style scoped>
.glass-card {
  transition: all 0.3s ease;
}

/* Default Glass (Level 2 - Main Content) */
.glass-card-default {
  background: rgba(255, 255, 255, 0.03);
  backdrop-filter: blur(16px);
  -webkit-backdrop-filter: blur(16px);
  border: 1px solid rgba(255, 255, 255, 0.08);
  box-shadow: 
    0 8px 32px rgba(0, 0, 0, 0.4),
    inset 0 1px 0 rgba(255, 255, 255, 0.05);
}

.glass-card-default:hover {
  border-color: rgba(255, 255, 255, 0.12);
}

/* Strong Glass (Level 3 - Overlays like Sidebar) */
.glass-card-strong {
  background: rgba(255, 255, 255, 0.04);
  backdrop-filter: blur(20px);
  -webkit-backdrop-filter: blur(20px);
  border: 1px solid rgba(255, 255, 255, 0.1);
  box-shadow: 
    0 12px 48px rgba(0, 0, 0, 0.5),
    inset 0 1px 0 rgba(255, 255, 255, 0.08);
}

.glass-card-strong:hover {
  background: rgba(255, 255, 255, 0.05);
  border-color: rgba(255, 255, 255, 0.15);
}

/* Subtle Glass (Level 1.5 - Headers, light overlays) */
.glass-card-subtle {
  background: rgba(255, 255, 255, 0.02);
  backdrop-filter: blur(8px);
  -webkit-backdrop-filter: blur(8px);
  border: 1px solid rgba(255, 255, 255, 0.05);
  box-shadow: 0 4px 16px rgba(0, 0, 0, 0.2);
}

.glass-card-subtle:hover {
  background: rgba(255, 255, 255, 0.04);
}

/* Performance optimization */
@media (prefers-reduced-transparency: reduce) {
  .glass-card-default,
  .glass-card-strong,
  .glass-card-subtle {
    backdrop-filter: none;
    -webkit-backdrop-filter: none;
    background: rgba(20, 20, 25, 0.95);
  }
}
</style>
