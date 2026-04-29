import './assets/main.css'

import { createApp } from 'vue'
import App from './App.vue'
import { initDiscordSDK, isInDiscord } from './discord.js'

// Check if running in Discord iframe
if (isInDiscord()) {
    // Initialize Discord SDK first (patches WebSocket/fetch for URL mapping)
    // This must happen BEFORE any MistServer connections
    initDiscordSDK().then(() => {
        createApp(App).mount('#app')
    })
} else {
    console.log('[CocoaTube] Not running in Discord Activity, starting browser mode')
    createApp(App).mount('#app')
}
