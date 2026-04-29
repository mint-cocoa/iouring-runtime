import { ref, onMounted, onUnmounted } from 'vue'

const HEARTBEAT_INTERVAL_MS = 15000

const getClientId = () => {
    try {
        return crypto.randomUUID()
    } catch {
        return `${Date.now()}-${Math.random().toString(16).slice(2)}`
    }
}

const buildWsBase = () => {
    const envWs = import.meta.env.VITE_WS_BASE
    if (envWs) return envWs.replace(/\/$/, '')
    const proto = window.location.protocol === 'https:' ? 'wss:' : 'ws:'
    return `${proto}//${window.location.host}`
}

export function useCocoaSocket(instanceId = null, onToast = null) {
    const connected = ref(false)
    const clientCount = ref(0)
    const clients = ref([])
    const queue = ref([])
    const chatMessages = ref([])
    const downloadStatus = ref(null)

    // Playback state
    const serverPlaybackState = ref(null)
    const serverMedia = ref(null)
    const lastOrigin = ref(null)

    const wsRef = ref(null)
    const clientId = ref(getClientId())
    const seqRef = ref(0)
    const reconnectTimerRef = ref(null)
    const heartbeatTimerRef = ref(null)
    const reconnectAttempts = ref(0)
    const manuallyClosed = ref(false)

    // Computed base URL
    const wsBase = buildWsBase()

    const sendEvent = (type, payload = {}) => {
        if (wsRef.value && wsRef.value.readyState === WebSocket.OPEN) {
            const seq = ++seqRef.value
            wsRef.value.send(JSON.stringify({
                type,
                client_id: clientId.value,
                seq,
                payload
            }))
        }
    }

    const clearReconnectTimer = () => {
        if (reconnectTimerRef.value) {
            clearTimeout(reconnectTimerRef.value)
            reconnectTimerRef.value = null
        }
    }

    const stopHeartbeat = () => {
        if (heartbeatTimerRef.value) {
            clearInterval(heartbeatTimerRef.value)
            heartbeatTimerRef.value = null
        }
    }

    const startHeartbeat = () => {
        stopHeartbeat()
        heartbeatTimerRef.value = setInterval(() => {
            sendEvent('PING', { ts: Date.now() })
        }, HEARTBEAT_INTERVAL_MS)
    }

    const scheduleReconnect = () => {
        if (manuallyClosed.value || reconnectTimerRef.value) return

        const delay = Math.min(1000 * (2 ** reconnectAttempts.value), 10000)
        reconnectAttempts.value += 1
        reconnectTimerRef.value = setTimeout(() => {
            reconnectTimerRef.value = null
            connect()
        }, delay)
    }

    const connect = () => {
        clearReconnectTimer()
        if (wsRef.value && (
            wsRef.value.readyState === WebSocket.OPEN ||
            wsRef.value.readyState === WebSocket.CONNECTING
        )) {
            return
        }

        const wsUrl = `${wsBase}/ws`
        const socket = new WebSocket(wsUrl)
        wsRef.value = socket

        socket.onopen = () => {
            if (wsRef.value !== socket) return
            console.log('Connected to WebSocket')
            reconnectAttempts.value = 0
            connected.value = true
            startHeartbeat()
            try {
                socket.send(JSON.stringify({
                    type: 'HELLO',
                    client_id: clientId.value,
                    payload: {
                        client_id: clientId.value,
                        app: 'web',
                        instance_id: instanceId // Discord Activity session
                    }
                }))
            } catch (err) { void err }
        }

        socket.onclose = (event) => {
            if (wsRef.value !== socket) return
            console.log('Disconnected from WebSocket', event.code, event.reason || '')
            connected.value = false
            clients.value = []
            clientCount.value = 0
            stopHeartbeat()
            wsRef.value = null
            scheduleReconnect()
        }

        socket.onerror = (event) => {
            if (wsRef.value !== socket) return
            console.warn('WebSocket error', event)
        }

        socket.onmessage = (event) => {
            if (wsRef.value !== socket) return
            try {
                const data = JSON.parse(event.data)
                const { type, payload, origin } = data

                switch (type) {
                    case 'PRESENCE_UPDATE':
                        if (Array.isArray(payload?.clients)) clients.value = payload.clients
                        if (payload?.client_count != null) clientCount.value = payload.client_count
                        break

                    case 'CONTROL_DENIED': {
                        const controller = payload?.controller_client_id
                        const msg = controller
                            ? `You don't have control. Current controller: ${controller}`
                            : `You don't have control.`
                        if (onToast) onToast(msg, 'error')
                        break
                    }

                    case 'STATE_UPDATE': {
                        const playback = payload?.playback
                        const media = payload?.media

                        if (Array.isArray(payload?.clients)) clients.value = payload.clients
                        if (payload?.client_count != null) clientCount.value = payload.client_count
                        if (Array.isArray(payload?.queue)) queue.value = payload.queue

                        serverMedia.value = media || null
                        serverPlaybackState.value = playback || null
                        lastOrigin.value = origin || null
                        break
                    }

                    case 'QUEUE_UPDATED':
                        if (payload.queue) queue.value = payload.queue
                        break

                    case 'CHAT_MESSAGE':
                        chatMessages.value = [...chatMessages.value.slice(-49), payload]
                        break

                    case 'DOWNLOAD_PROGRESS':
                        downloadStatus.value = { task_id: data.task_id, status: data.status, url: data.url }
                        break

                    case 'DOWNLOAD_COMPLETE':
                        downloadStatus.value = { task_id: data.task_id, status: 'completed', entry: data.entry }
                        break

                    case 'DOWNLOAD_FAILED':
                        downloadStatus.value = { task_id: data.task_id, status: 'failed', error: data.error }
                        if (onToast) onToast(`Download failed: ${data.error}`, 'error')
                        break

                    case 'PONG':
                        break

                    default:
                        break
                }
            } catch (err) {
                console.error('WS Message Parse Error', err)
            }
        }
    }

    const sendChatMessage = (text, sender) => {
        const trimmed = (text || '').trim()
        if (!trimmed || !wsRef.value || wsRef.value.readyState !== WebSocket.OPEN) return false

        wsRef.value.send(JSON.stringify({
            type: 'CHAT_MESSAGE',
            client_id: clientId.value,
            payload: {
                text: trimmed,
                sender
            }
        }))
        return true
    }

    // Lifecycle
    onMounted(() => {
        manuallyClosed.value = false
        connect()
    })

    onUnmounted(() => {
        manuallyClosed.value = true
        clearReconnectTimer()
        stopHeartbeat()
        if (wsRef.value) wsRef.value.close()
    })

    // Watch instance change to reconnect?
    // In Vue logic if key changes comp reloads, but if reactive arg changes we might want to reconnect.
    // For now assuming instanceId is static or component re-mounts.

    return {
        connected,
        clients,
        clientCount,
        queue,
        serverPlaybackState,
        serverMedia,
        lastOrigin,
        clientId,
        chatMessages,
        downloadStatus,
        sendChatMessage,
        sendEvent
    }
}
