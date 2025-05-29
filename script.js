/* File amended on: 2025-05-29 09:22 AM CEST */
// Global state variables
let swrLatched = false;
let wsConnected = false;
let mainPaState = true;
let isSaving = false; // Prevent duplicate saves

// Gauge configuration
const gaugeConfig = {
  forward: { min: 0, max: 50, startX: 38 / 1100, endX: 1050 / 1100 },
  reflected: { min: 0, max: 25, startX: 38 / 1100, endX: 1050 / 1100 }
};

// Update gauge dimensions based on window size
function updateGaugeDimensions() {
  const gaugeImage = document.getElementById("gaugeImage");
  const gaugeOverlay = document.getElementById("gaugeOverlay");
  const needleForward = document.getElementById("needleForward");
  const needleReflected = document.getElementById("needleReflected");

  if (!gaugeImage || !gaugeOverlay || !needleForward || !needleReflected) {
    return 0;
  }

  const imageWidth = gaugeImage.clientWidth;
  const imageHeight = gaugeImage.clientHeight;

  gaugeOverlay.setAttribute("width", imageWidth);
  gaugeOverlay.setAttribute("height", imageHeight);

  const scaleY = imageHeight / 300; // Original image height assumed as 300px
  needleForward.setAttribute("y", (35 * scaleY) - 5 + 10); // Moved 10px down
  needleForward.setAttribute("height", 40 * scaleY); // Reduced to 50% of original 80
  needleReflected.setAttribute("y", (175 * scaleY) + 15); // Move REF down by ~15px
  needleReflected.setAttribute("height", 40 * scaleY); // Reduced to 50% of original 80

  return imageWidth;
}

// Update UTC time display every second with day-month-year HH:MM format
function updateUTCTime() {
  const utcTimeElement = document.getElementById("utcTime");
  if (!utcTimeElement) {
    return;
  }
  const now = new Date();
  const day = now.getUTCDate();
  const monthNames = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"];
  const month = monthNames[now.getUTCMonth()];
  const year = now.getUTCFullYear();
  const hours = String(now.getUTCHours()).padStart(2, '0');
  const minutes = String(now.getUTCMinutes()).padStart(2, '0');
  utcTimeElement.textContent = `UTC: ${day}-${month}-${year} ${hours}:${minutes}`;
  setTimeout(updateUTCTime, 1000);
}

// Display an alert message with custom styling and auto-dismiss
function showAlert(message, type = "danger") {
  const alertDiv = document.createElement("div");
  alertDiv.className = `alert alert-${type}`;
  alertDiv.innerHTML = `
    <span>${message}</span>
    <button type="button" class="alert-close" aria-label="Close">×</button>
  `;
  document.body.insertBefore(alertDiv, document.body.firstChild);

  // Add event listener for manual close
  const closeButton = alertDiv.querySelector(".alert-close");
  closeButton.addEventListener("click", () => alertDiv.remove());

  // Play sound for SWR alerts (silenced due to browser restriction)
  if (type === "danger" && message.includes("SWR")) {
  }
}

// Update gauge needle position and label
function updateNeedle(config, value, needleId, labelId, labelText) {
  const imageWidth = updateGaugeDimensions();
  if (imageWidth === 0 || !config || value === undefined || value === null) {
    return;
  }

  const scaledStartX = config.startX * imageWidth;
  const scaledEndX = config.endX * imageWidth;

  const clamped = Math.max(config.min, Math.min(Number(value) || 0, config.max));
  const range = config.max - config.min;
  const fraction = (clamped - config.min) / range;
  const x = scaledStartX + fraction * (scaledEndX - scaledStartX);

  const needle = document.getElementById(needleId);
  if (!needle) {
    return;
  }
  needle.style.transition = 'x 0.3s ease';
  const thickness = parseInt(document.getElementById("needleThickness")?.value || 2);
  requestAnimationFrame(() => {
    needle.setAttribute("x", isNaN(x) ? 0 : x); // Fallback to 0 if NaN
    needle.setAttribute("width", thickness);
    needle.setAttribute("fill", document.getElementById("needleColor")?.value || "#000000");
  });
  const label = document.getElementById(labelId);
  if (!label) {
    return;
  }
  label.textContent = `${labelText}: ${isNaN(clamped) ? 0 : clamped.toFixed(1)} W`;
}

// Show firmware update progress with progress bar
function showUpdateProgress(status, progress) {
  const otaProgress = document.getElementById("otaProgress");
  const progressBarFill = document.getElementById("otaProgressBarFill");

  if (!otaProgress || !progressBarFill) {
    console.error("OTA Progress elements not found:", {
      otaProgress: !!otaProgress,
      progressBarFill: !!progressBarFill
    });
    return;
  }

  if (status === "started") {
    otaProgress.style.display = "block";
    progressBarFill.style.width = "0%";
  } else if (status === "progress" && otaProgress.style.display !== "none") {
    progressBarFill.style.width = `${progress}%`;
    requestAnimationFrame(() => {
      progressBarFill.style.width = `${progress}%`; // Force redraw
    });
  } else if (status === "completed" || status === "failed" || status === "success") {
    if (otaProgress.style.display !== "none") {
      progressBarFill.style.width = `${progress}%`;
      requestAnimationFrame(() => {
        progressBarFill.style.width = `${progress}%`; // Force redraw
      });
      otaProgress.querySelector("span").textContent = `Firmware update ${status}!`;
      if (status === "completed" || status === "success") {
        otaProgress.style.borderColor = "#28a745";
        otaProgress.classList.add("bg-success");
      } else {
        otaProgress.style.borderColor = "#dc3545";
        otaProgress.classList.add("bg-danger");
      }
      setTimeout(() => {
        otaProgress.style.display = "none";
        otaProgress.classList.remove("bg-success", "bg-danger");
        otaProgress.style.borderColor = "#007bff";
      }, 3000); // Auto-hide after 3 seconds
    }
  }
}

function hideOtaProgress() {
  const otaProgress = document.getElementById("otaProgress");
  if (!otaProgress) {
    return;
  }
  otaProgress.style.display = "none";
  otaProgress.classList.remove("bg-success", "bg-danger");
  otaProgress.style.borderColor = "#007bff";
}

// Update firmware version display
function updateFirmwareVersion(version) {
  const firmwareElement = document.getElementById("firmwareVersion");
  if (!firmwareElement) {
    return;
  }
  firmwareElement.textContent = `Firmware ${version} — PA0ESH`;
}

// Establish WebSocket connection and handle messages
function connectWebSocket() {
  const ws = new WebSocket(`ws://${window.location.host}/ws`);
  let reconnectDelay = 1000;

  ws.onopen = () => {
    wsConnected = true;
    updateWsStatus(true);
  };

  ws.onmessage = (event) => {
    const data = JSON.parse(event.data);
    if (data.type === "data") {
      updateNeedle(gaugeConfig.forward, data.forward, "needleForward", "forwardBox", "FOR");
      updateNeedle(gaugeConfig.reflected, data.reflected, "needleReflected", "reflectedBox", "REF");

      const swr = data.swr !== undefined ? Number(data.swr) : 1.0;
      const swrBox = document.getElementById("swrBox");
      if (!swrBox) return;
      swrBox.textContent = `SWR: ${isNaN(swr) ? 1.0 : swr.toFixed(2)}`;
      const tempBox = document.getElementById("tempBox");
      if (!tempBox) return;
      tempBox.textContent = `TEMP: ${data.temperature !== undefined ? Number(data.temperature).toFixed(1) : 25.0}°C`;

      // Update firmware version
      if (data.version) {
        updateFirmwareVersion(data.version);
      }

      // Update SWR box color based on new ranges
      if (swr <= 2) {
        swrBox.className = "meter-box bg-dark text-light swr-color-box swr-green";
        if (swrLatched) {
          swrLatched = false; // Reset latch only if SWR drops below 2
        }
      } else if (swr <= data.swrThreshold) {
        swrBox.className = "meter-box bg-dark text-light swr-color-box swr-orange";
        if (swrLatched) {
          swrLatched = false; // Reset latch only if SWR drops below threshold
        }
      } else {
        swrBox.className = "meter-box bg-dark text-light swr-color-box swr-red";
        if (!swrLatched && mainPaState) {
          fetch("/api/latch_swr", { method: "POST" })
            .then(res => {
              if (!res.ok) throw new Error(`Failed to latch SWR: ${res.status} ${res.statusText}`);
              const contentType = res.headers.get("content-type");
              if (contentType && contentType.includes("application/json")) {
                return res.json();
              } else {
                return res.text().then(text => ({ status: text }));
              }
            })
            .then(data => {
              mainPaState = false; // Force PA off on latch, manual toggle required
              updateMainPaDisplay();
              swrLatched = true;
              showAlert(`SWR exceeded threshold of ${data.swrThreshold || 3.0}! PA turned off.`, "danger");
            })
            .catch(err => {
              showAlert("Error latching SWR: " + err.message, "danger");
            });
        }
      }

      // Only update mainPaState from WebSocket if not latched
      if (data.mainPaState !== undefined && !swrLatched) {
        mainPaState = data.mainPaState === "ON";
        updateMainPaDisplay();
      }
    } else if (data.type === "update_progress") {
      showUpdateProgress(data.status, data.progress);
    }
  };

  ws.onclose = () => {
    wsConnected = false;
    updateWsStatus(false);
    setTimeout(connectWebSocket, reconnectDelay);
    reconnectDelay = Math.min(reconnectDelay * 2, 16000);
  };

  ws.onerror = (err) => {
    ws.close();
  };
}

// Update WebSocket status indicator
function updateWsStatus(connected) {
  const dot = document.getElementById("wsStatusDot");
  if (!dot) {
    return;
  }
  dot.style.backgroundColor = connected ? "#28a745" : "orange";
  dot.classList.toggle("pulse", connected);
}

// Toggle polarization mode (SSB/DATV)
function setupTogglePolListener() {
  const togglePolButton = document.getElementById("togglePol");
  if (!togglePolButton) {
    return;
  }
  togglePolButton.addEventListener("click", () => {
    fetch("/api/toggle", { method: "POST" })
      .then(res => {
        if (!res.ok) throw new Error("Failed to toggle mode");
        return res.text();
      })
      .then(state => {
        const result = document.getElementById("togglePolState");
        if (!result) {
          return;
        }
        if (state.includes("VERTICAL")) {
          result.textContent = "SSB";
          togglePolButton.classList.remove("bg-danger");
          togglePolButton.classList.add("bg-success");
        } else {
          result.textContent = "DATV";
          togglePolButton.classList.remove("bg-success");
          togglePolButton.classList.add("bg-danger");
        }
      })
      .catch(err => {
        showAlert("Error toggling mode: " + err.message, "danger");
      });
  });
}

// Reset SWR latch
function setupSwrResetListener() {
  const swrBox = document.getElementById("swrBox");
  if (!swrBox) {
    return;
  }
  swrBox.addEventListener("click", () => {
    fetch("/api/reset_swr", { method: "POST" })
      .then(res => {
        if (!res.ok) throw new Error("Failed to reset SWR");
        return res.text();
      })
      .then(response => {
        swrLatched = false;
        if (response === "SWR_RESET") {
          showAlert("SWR reset successfully, PA remains off until toggled", "success");
        } else if (response === "WAIT_TO_RESET") {
          showAlert("Please wait before resetting SWR", "warning");
        }
      })
      .catch(err => {
        showAlert("Error resetting SWR: " + err.message, "danger");
      });
  });
}

// Toggle Main PA
function setupMainPaToggleListener() {
  const mainPaToggleButton = document.getElementById("mainPaToggle");
  if (!mainPaToggleButton) {
    return;
  }
  mainPaToggleButton.addEventListener("click", () => {
    if (swrLatched) {
      showAlert("Cannot turn PA on while SWR is latched. Reset SWR first.", "warning");
      return;
    }
    fetch("/api/main_pa_toggle", { method: "POST" })
      .then(res => {
        if (!res.ok) throw new Error("Failed to toggle main PA");
        return res.text();
      })
      .then(state => {
        mainPaState = state === "ON";
        updateMainPaDisplay();
      })
      .catch(err => {
        showAlert("Error toggling main PA: " + err.message, "danger");
      });
  });
}

// Toggle OTA form visibility
function toggleOtaForm() {
  const form = document.getElementById("otaForm");
  if (!form) {
    return;
  }
  form.style.display = form.style.display === "block" ? "none" : "block";
}

// Toggle settings form visibility
function toggleSettings() {
  const form = document.getElementById("settingsForm");
  if (!form) {
    return;
  }
  form.style.display = form.style.display === "block" ? "none" : "block";
}

// Apply gauge settings
function applySettings() {
  const forwardMax = parseFloat(document.getElementById("forwardMax").value);
  const reflectedMax = parseFloat(document.getElementById("reflectedMax").value);
  const thickness = parseInt(document.getElementById("needleThickness").value);
  const color = document.getElementById("needleColor").value;
  const swrThreshold = parseFloat(document.getElementById("swrThreshold").value);

  gaugeConfig.forward.max = forwardMax;
  gaugeConfig.reflected.max = reflectedMax;
  document.getElementById("needleForward").setAttribute("width", thickness);
  document.getElementById("needleReflected").setAttribute("width", thickness);
  document.getElementById("needleForward").setAttribute("fill", color);
  document.getElementById("needleReflected").setAttribute("fill", color);

  const settingsToSave = { forwardMax, reflectedMax, needleThickness: thickness, needleColor: color, swrThreshold };
  console.log("Settings to save (before fetch):", settingsToSave);

  fetch("/api/set_threshold", {
    method: "POST",
    headers: { "Content-Type": "application/json", 'Authorization': 'Basic ' + btoa('admin:password') },
    body: JSON.stringify({ threshold: swrThreshold })
  })
    .then(res => {
      if (!res.ok) throw new Error("Failed to apply SWR threshold");
      return fetch("/api/save_settings", {
        method: "POST",
        headers: { "Content-Type": "application/json", 'Authorization': 'Basic ' + btoa('admin:password') },
        body: JSON.stringify(settingsToSave)
      });
    })
    .then(res => {
      if (!res.ok) {
        console.error("Save settings failed:", res.status, res.statusText);
        throw new Error("Failed to save settings");
      }
      return res.json(); // Attempt to get response body
    })
    .then(data => {
      console.log("Save settings response:", data);
      showAlert("Settings applied and saved successfully", "success");
    })
    .catch(err => showAlert("Error applying settings: " + err.message, "danger"));
}

// Check for firmware updates
function checkForUpdate() {
  fetch("/api/check_update", { headers: { 'Authorization': 'Basic ' + btoa('admin:password') } })
    .then(res => res.json())
    .then(data => {
      if (data.updateAvailable) {
        showAlert(`Update available: ${data.latestVersion}. Current: ${data.currentVersion}`, "info");
        document.getElementById("otaUrl").value = data.updateUrl;
      } else {
        showAlert("No update available", "info");
      }
    })
    .catch(err => showAlert("Error checking for update: " + err.message, "danger"));
}

// Perform firmware update
function performUpdate() {
  const url = document.getElementById("otaUrl").value;
  if (!url) {
    showAlert("Please enter a valid OTA URL", "warning");
    return;
  }
  // Show progress bar immediately as a fallback
  showUpdateProgress("started", 0);
  fetch(`/api/update?updateUrl=${encodeURIComponent(url)}`, { method: "POST", headers: { 'Authorization': 'Basic ' + btoa('admin:password') } })
    .then(res => {
      if (!res.ok) throw new Error(`OTA update request failed: ${res.status}`);
      return res.text();
    })
    .then(result => {
      if (result === "UPDATE_SUCCESS") {
        showUpdateProgress("success", 100); // Ensure progress bar shows success
      } else {
        showAlert("Update failed: " + result, "danger");
        showUpdateProgress("failed", 0);
      }
    })
    .catch(err => {
      showAlert("Error performing update: " + err.message, "danger");
      showUpdateProgress("failed", 0);
    });
}

// Rollback firmware
function rollbackFirmware() {
  fetch("/api/rollback", { method: "POST", headers: { 'Authorization': 'Basic ' + btoa('admin:password') } })
    .then(res => res.text())
    .then(result => {
      if (result === "ROLLBACK_INITIATED") {
        showAlert("Rollback initiated, please wait...", "info");
      } else {
        showAlert("Rollback failed: No previous firmware available", "danger");
      }
    })
    .catch(err => showAlert("Error rolling back firmware: " + err.message, "danger"));
}

// Update Main PA display
function updateMainPaDisplay() {
  const result = document.getElementById("mainPaState");
  const button = document.getElementById("mainPaToggle");
  if (!result || !button) {
    return;
  }
  result.textContent = mainPaState ? "ON" : "OFF";
  button.classList.toggle("bg-success", mainPaState);
  button.classList.toggle("bg-danger", !mainPaState);
}

// Apply loaded settings from the server
function applyLoadedSettings(data) {
  console.log("Loaded settings from server:", data);

  if (data.forwardMax) document.getElementById("forwardMax").value = data.forwardMax;
  if (data.reflectedMax) document.getElementById("reflectedMax").value = data.reflectedMax;
  
  // Handle needle color
  const needleColorInput = document.getElementById("needleColor");
  if (data.needleColor && needleColorInput) {
    needleColorInput.value = data.needleColor;
    console.log("Needle color set to:", data.needleColor);
  } else {
    console.log("Needle color not found in server response or input element missing");
  }

  // Handle needle thickness
  const thickness = data.needleThickness || 2;
  document.getElementById("needleThickness").value = thickness;
  document.getElementById("needleForward").setAttribute("width", thickness);
  document.getElementById("needleReflected").setAttribute("width", thickness);

  // Apply needle color to SVG elements
  const colorToApply = needleColorInput.value || data.needleColor || "#000000";
  document.getElementById("needleForward").setAttribute("fill", colorToApply);
  document.getElementById("needleReflected").setAttribute("fill", colorToApply);
  console.log("Applied needle color:", colorToApply);

  if (data.swrThreshold) document.getElementById("swrThreshold").value = data.swrThreshold;
  gaugeConfig.forward.max = parseFloat(data.forwardMax || 50);
  gaugeConfig.reflected.max = parseFloat(data.reflectedMax || 25);
}

// Initialize the application
document.addEventListener('DOMContentLoaded', function() {
  // Load settings on page load
  fetch('/api/load_settings', { headers: { 'Authorization': 'Basic ' + btoa('admin:password') } })
    .then(response => response.json())
    .then(data => applyLoadedSettings(data))
    .catch(error => console.error('Error loading settings:', error));

  // Find the Save Settings button
  const saveSettingsButton = document.getElementById('saveSettingsButton');
  if (!saveSettingsButton) {
    console.error('Save Settings Button not found');
    return;
  }

  // Attach event listener to the Save Settings button
  saveSettingsButton.addEventListener('click', function() {
    if (isSaving) {
      console.log("Save already in progress, ignoring click");
      return;
    }
    isSaving = true;
    saveSettingsButton.disabled = true; // Disable button during save

    // Collect values from input fields
    const settings = {
      forwardMax: parseFloat(document.getElementById('forwardMax')?.value) || 50,
      reflectedMax: parseFloat(document.getElementById('reflectedMax')?.value) || 25,
      needleThickness: parseInt(document.getElementById('needleThickness')?.value) || 2,
      needleColor: document.getElementById('needleColor')?.value || '#000000',
      swrThreshold: parseFloat(document.getElementById('swrThreshold')?.value) || 3
    };

    // Log the settings being sent for debugging
    console.log('Settings to save (before fetch):', settings);

    // Send the settings to the ESP32
    fetch('/api/save_settings', {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        'Authorization': 'Basic ' + btoa('admin:password')
      },
      body: JSON.stringify(settings)
    })
    .then(response => {
      if (!response.ok) {
        throw new Error(`Save settings failed: ${response.status} ${response.statusText}`);
      }
      return response.json();
    })
    .then(data => {
      console.log('Settings saved successfully:', data);
      showAlert("Settings applied and saved successfully", "success");
      // Reload settings to ensure UI is in sync
      return fetch('/api/load_settings', { headers: { 'Authorization': 'Basic ' + btoa('admin:password') } })
        .then(response => response.json())
        .then(data => applyLoadedSettings(data));
    })
    .catch(error => {
      console.error('Error saving settings:', error);
      showAlert("Error saving settings: " + error.message, "danger");
    })
    .finally(() => {
      isSaving = false;
      saveSettingsButton.disabled = false; // Re-enable button
    });
  });

  // Initialize other listeners
  setupTogglePolListener();
  setupSwrResetListener();
  setupMainPaToggleListener();
  updateUTCTime();
  connectWebSocket();
  window.addEventListener("resize", () => updateGaugeDimensions());
});

// Update gauge dimensions on window resize
window.addEventListener("resize", () => {
  updateGaugeDimensions();
});

// Note: PA control pin has been changed to D14 - update server-side firmware accordingly