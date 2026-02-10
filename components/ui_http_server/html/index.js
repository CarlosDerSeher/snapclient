'use strict';

/**
 * Shared JavaScript utilities for ESP32 Snapclient UI
 */

// Get backend URL from query parameter (e.g., ?backend=http://192.168.1.100)
// This allows testing the UI locally while communicating with the actual device
function getBackendURL() {
  const urlParams = new URLSearchParams(window.location.search);
  const backend = urlParams.get('backend');
  if (backend) {
    console.log('Using backend:', backend);
    return backend;
  }
  // Default: use relative URLs (same server)
  return '';
}

const BACKEND_URL = getBackendURL();

/**
 * Make a GET request to the device
 * @param {string} endpoint - API endpoint path
 * @returns {Promise<Response>} Fetch response
 */
async function getRequest(endpoint) {
  try {
    const response = await fetch(BACKEND_URL + endpoint);
    if (!response.ok) {
      throw new Error(`HTTP error! status: ${response.status}`);
    }
    return response;
  } catch (error) {
    console.error(`Error fetching ${endpoint}:`, error);
    throw error;
  }
}

/**
 * Make a POST request to the device
 * @param {string} endpoint - API endpoint path
 * @param {object} data - Optional data to send
 * @returns {Promise<Response>} Fetch response
 */
async function postRequest(endpoint, data = null) {
  try {
    const options = {
      method: 'POST'
    };
    
    if (data) {
      options.headers = { 'Content-Type': 'application/json' };
      options.body = JSON.stringify(data);
    }
    
    const response = await fetch(BACKEND_URL + endpoint, options);
    if (!response.ok) {
      throw new Error(`HTTP error! status: ${response.status}`);
    }
    return response;
  } catch (error) {
    console.error(`Error posting to ${endpoint}:`, error);
    throw error;
  }
}

/**
 * Get a parameter value from the device
 * @param {string} paramKey - Parameter key
 * @returns {Promise<number|string|null>} Parameter value or null on error
 */
async function getParameter(paramKey) {
  try {
    const response = await getRequest(`/get?param=${paramKey}`);
    const value = await response.text();
    // Return as string for hostname and snapserver_host
    if (paramKey === 'hostname' || paramKey === 'snapserver_host') {
      return value.trim();
    }
    // For snapserver_port, handle empty string
    if (paramKey === 'snapserver_port') {
      const trimmed = value.trim();
      return trimmed === '' ? '' : parseFloat(trimmed);
    }
    return parseFloat(value);
  } catch (error) {
    console.error(`Error fetching parameter ${paramKey}:`, error);
    return null;
  }
}

/**
 * Update a parameter on the device
 * @param {string} paramKey - Parameter key
 * @param {number|string} value - Parameter value
 * @returns {Promise<boolean>} True if successful
 */
async function setParameter(paramKey, value) {
  try {
    await postRequest(`/post?param=${paramKey}&value=${encodeURIComponent(value)}`);
    return true;
  } catch (error) {
    console.error(`Error setting parameter ${paramKey}:`, error);
    return false;
  }
}

/**
 * Delete a parameter from NVS (clear to default)
 * @param {string} paramKey - Parameter key
 * @returns {Promise<boolean>} True if successful
 */
async function deleteParameter(paramKey) {
  try {
    const response = await fetch(`/delete?param=${paramKey}`, {
      method: 'DELETE',
    });
    if (!response.ok) {
      throw new Error(`HTTP error ${response.status}`);
    }
    return true;
  } catch (error) {
    console.error(`Error deleting parameter ${paramKey}:`, error);
    return false;
  }
}

/**
 * Show an error message to the user
 * @param {string} message - Error message to display
 * @param {HTMLElement} container - Container element to show error in
 */
function showError(message, container) {
  container.innerHTML = `<div class="error">${message}</div>`;
}

/**
 * Show a loading message to the user
 * @param {string} message - Loading message to display
 * @param {HTMLElement} container - Container element to show message in
 */
function showLoading(message, container) {
  container.innerHTML = `<div class="loading">${message}</div>`;
}