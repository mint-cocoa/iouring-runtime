import { DiscordSDK } from '@discord/embedded-app-sdk'

// Discord SDK instance (singleton)
let discordSdk = null
let isReady = false

// Get client ID from environment variable
const DISCORD_CLIENT_ID = import.meta.env.VITE_DISCORD_CLIENT_ID || '1462292946987516111'

// Check if we're running inside Discord's iframe
const isInDiscordIframe = () => {
    const params = new URLSearchParams(window.location.search)
    return params.has('frame_id') && params.has('instance_id')
}

/**
 * Initialize Discord SDK and apply URL patches
 * Call this early in your app's lifecycle (main.js)
 */
export async function initDiscordSDK() {
    // Only run in Discord Activity environment
    if (!isInDiscordIframe()) {
        console.log('[Discord SDK] Not in Discord iframe, skipping initialization')
        return { sdk: null, isReady: false }
    }

    try {
        // Create SDK instance
        discordSdk = new DiscordSDK(DISCORD_CLIENT_ID)
        console.log('[Discord SDK] Instance created, instanceId:', discordSdk.instanceId)

        // Wait for SDK to be ready
        await discordSdk.ready()
        isReady = true
        console.log('[Discord SDK] Ready!')

        return { sdk: discordSdk, isReady: true }
    } catch (error) {
        console.error('[Discord SDK] Initialization failed:', error)
        return { sdk: null, isReady: false }
    }
}

/**
 * Get the Discord SDK instance
 */
export function getDiscordSDK() {
    return { sdk: discordSdk, isReady }
}

/**
 * Check if running in Discord Activity
 */
export function isInDiscord() {
    return isInDiscordIframe()
}
