/**
 * Custom fetch wrapper that automatically adds ngrok-skip-browser-warning header
 * to bypass ngrok's free tier browser warning page.
 */

const DEFAULT_HEADERS = {
    'ngrok-skip-browser-warning': 'true',
};

/**
 * Wrapper around fetch that adds ngrok-skip-browser-warning header
 * @param {string | URL | Request} input - The resource to fetch
 * @param {RequestInit} [init] - Options for the request
 * @returns {Promise<Response>}
 */
export async function fetchWithNgrok(input, init = {}) {
    const headers = new Headers(init.headers);

    // Add ngrok header if not already present
    if (!headers.has('ngrok-skip-browser-warning')) {
        headers.set('ngrok-skip-browser-warning', 'true');
    }

    return fetch(input, {
        ...init,
        headers,
    });
}

/**
 * Helper to merge custom headers with ngrok header
 * @param {Record<string, string>} customHeaders
 * @returns {Record<string, string>}
 */
export function withNgrokHeaders(customHeaders = {}) {
    return {
        ...DEFAULT_HEADERS,
        ...customHeaders,
    };
}

export default fetchWithNgrok;
