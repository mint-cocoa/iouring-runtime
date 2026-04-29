<script setup>
defineProps({
  variant: {
    type: String, // 'primary', 'secondary', 'ghost'
    default: 'primary'
  },
  className: {
    type: String,
    default: ''
  }
})
</script>

<template>
  <button 
    class="glass-button px-4 py-2 rounded-xl font-medium cursor-pointer disabled:opacity-50 disabled:cursor-not-allowed flex items-center justify-center gap-2"
    :class="[`glass-button-${variant}`, className]"
  >
    <slot />
  </button>
</template>

<style scoped>
.glass-button {
  position: relative;
  overflow: hidden;
  transition: all 0.3s ease;
}

/* Shimmer effect on hover */
.glass-button::before {
  content: '';
  position: absolute;
  top: 0;
  left: -100%;
  width: 100%;
  height: 100%;
  background: linear-gradient(
    90deg, 
    transparent, 
    rgba(255, 255, 255, 0.1), 
    transparent
  );
  transition: left 0.5s ease;
}

.glass-button:hover::before {
  left: 100%;
}

/* Primary - Gradient with glow */
.glass-button-primary {
  background: linear-gradient(135deg, #3b82f6 0%, #8b5cf6 100%);
  color: white;
  border: none;
  box-shadow: 
    0 4px 16px rgba(139, 92, 246, 0.3),
    inset 0 1px 0 rgba(255, 255, 255, 0.2);
}

.glass-button-primary:hover {
  transform: translateY(-1px);
  box-shadow: 
    0 6px 24px rgba(139, 92, 246, 0.4),
    inset 0 1px 0 rgba(255, 255, 255, 0.2);
}

.glass-button-primary:active {
  transform: translateY(0);
  box-shadow: 
    0 2px 8px rgba(139, 92, 246, 0.3),
    inset 0 1px 0 rgba(255, 255, 255, 0.2);
}

/* Secondary - Glass effect */
.glass-button-secondary {
  background: rgba(255, 255, 255, 0.08);
  backdrop-filter: blur(12px);
  -webkit-backdrop-filter: blur(12px);
  color: white;
  border: 1px solid rgba(255, 255, 255, 0.1);
  box-shadow: 
    0 4px 16px rgba(0, 0, 0, 0.2),
    inset 0 1px 0 rgba(255, 255, 255, 0.05);
}

.glass-button-secondary:hover {
  background: rgba(255, 255, 255, 0.12);
  border-color: rgba(255, 255, 255, 0.15);
  transform: translateY(-1px);
  box-shadow: 
    0 6px 20px rgba(0, 0, 0, 0.3),
    inset 0 1px 0 rgba(255, 255, 255, 0.08);
}

.glass-button-secondary:active {
  transform: translateY(0);
  background: rgba(255, 255, 255, 0.15);
}

/* Ghost - Transparent with subtle hover */
.glass-button-ghost {
  background: transparent;
  color: rgba(255, 255, 255, 0.7);
  border: 1px solid transparent;
}

.glass-button-ghost:hover {
  background: rgba(255, 255, 255, 0.05);
  color: white;
  border-color: rgba(255, 255, 255, 0.08);
}

.glass-button-ghost:active {
  background: rgba(255, 255, 255, 0.08);
}

/* Performance optimization */
@media (prefers-reduced-motion: reduce) {
  .glass-button:hover {
    transform: none;
  }
  
  .glass-button::before {
    display: none;
  }
}

@media (prefers-reduced-transparency: reduce) {
  .glass-button-secondary {
    backdrop-filter: none;
    -webkit-backdrop-filter: none;
    background: rgba(40, 40, 50, 0.95);
  }
}
</style>
