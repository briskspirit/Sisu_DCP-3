import { analyzePcm, compareTakes, encodeWav24 } from "./analysis.js";

const CAPTURE_SECONDS = 15;
const TRIM_START_SECONDS = 1.5;
const TRIM_END_SECONDS = 0.5;
const CONTINUOUS_END_SECONDS = 3.5;

const elements = {
  device: document.querySelector("#audio-input"),
  enable: document.querySelector("#enable-input"),
  inputStatus: document.querySelector("#input-status"),
  meter: document.querySelector("#meter-fill"),
  meterValue: document.querySelector("#meter-value"),
  label: document.querySelector("#take-label"),
  mode: document.querySelector("#analysis-mode"),
  originalButton: document.querySelector("#capture-original"),
  dutButton: document.querySelector("#capture-dut"),
  captureStatus: document.querySelector("#capture-status"),
  originalResult: document.querySelector("#original-result"),
  dutResult: document.querySelector("#dut-result"),
  comparison: document.querySelector("#comparison"),
};

let stream = null;
let audioContext = null;
let recorderNode = null;
let analyserNode = null;
let chunks = [];
let stopResolver = null;
let capturing = false;
let meterFrame = null;
const takes = { original: null, dut: null };

function setCaptureButtonsEnabled(enabled) {
  elements.originalButton.disabled = !enabled;
  elements.dutButton.disabled = !enabled;
}

function formatDb(value) {
  return Number.isFinite(value) ? `${value.toFixed(2)} dBFS` : "--";
}

async function enumerateInputs(selectedId = "") {
  const devices = await navigator.mediaDevices.enumerateDevices();
  const inputs = devices.filter((device) => device.kind === "audioinput");
  elements.device.replaceChildren();
  for (const input of inputs) {
    const option = document.createElement("option");
    option.value = input.deviceId;
    option.textContent = input.label || `Audio input ${elements.device.length + 1}`;
    option.selected = input.deviceId === selectedId;
    elements.device.append(option);
  }
}

async function closeInput() {
  if (meterFrame !== null) cancelAnimationFrame(meterFrame);
  meterFrame = null;
  stream?.getTracks().forEach((track) => track.stop());
  stream = null;
  if (audioContext) await audioContext.close();
  audioContext = null;
  recorderNode = null;
  analyserNode = null;
}

async function enableInput() {
  if (!navigator.mediaDevices?.getUserMedia || !window.AudioWorkletNode) {
    throw new Error("This browser does not support raw Web Audio capture");
  }

  elements.enable.disabled = true;
  setCaptureButtonsEnabled(false);
  elements.inputStatus.textContent = "Requesting microphone access...";
  const requestedDeviceId = elements.device.value;
  await closeInput();

  const constraints = {
    audio: {
      echoCancellation: false,
      noiseSuppression: false,
      autoGainControl: false,
      channelCount: { ideal: 1 },
      sampleRate: { ideal: 48000 },
      ...(requestedDeviceId ? { deviceId: { exact: requestedDeviceId } } : {}),
    },
  };

  try {
    stream = await navigator.mediaDevices.getUserMedia(constraints);
    const track = stream.getAudioTracks()[0];
    const settings = track.getSettings();
    await enumerateInputs(settings.deviceId || requestedDeviceId);

    const contextOptions = { latencyHint: "interactive" };
    if (settings.sampleRate) contextOptions.sampleRate = settings.sampleRate;
    audioContext = new AudioContext(contextOptions);
    await audioContext.audioWorklet.addModule("./recorder-worklet.js");

    const source = audioContext.createMediaStreamSource(stream);
    analyserNode = audioContext.createAnalyser();
    analyserNode.fftSize = 2048;
    recorderNode = new AudioWorkletNode(audioContext, "pcm-recorder");
    const mute = audioContext.createGain();
    mute.gain.value = 0;
    source.connect(analyserNode).connect(recorderNode).connect(mute).connect(audioContext.destination);

    recorderNode.port.onmessage = (event) => {
      if (event.data.type === "chunk") chunks.push(new Float32Array(event.data.samples));
      if (event.data.type === "stopped" && stopResolver) {
        stopResolver();
        stopResolver = null;
      }
    };

    const processing = [
      `AGC ${settings.autoGainControl === false ? "off" : "unconfirmed"}`,
      `noise suppression ${settings.noiseSuppression === false ? "off" : "unconfirmed"}`,
      `echo cancellation ${settings.echoCancellation === false ? "off" : "unconfirmed"}`,
    ].join(", ");
    elements.inputStatus.textContent = `${track.label || "Selected input"} | ${audioContext.sampleRate} Hz mono | ${processing}`;
    setCaptureButtonsEnabled(true);
    updateMeter();
  } finally {
    elements.enable.disabled = false;
  }
}

function updateMeter() {
  if (!analyserNode) return;
  const samples = new Float32Array(analyserNode.fftSize);
  analyserNode.getFloatTimeDomainData(samples);
  let sumSquares = 0;
  for (const sample of samples) sumSquares += sample * sample;
  const rms = Math.sqrt(sumSquares / samples.length);
  const db = rms > 0 ? 20 * Math.log10(rms) : -120;
  const percent = Math.max(0, Math.min(100, (db + 72) * 100 / 72));
  elements.meter.style.width = `${percent}%`;
  elements.meterValue.textContent = `${db.toFixed(1)} dBFS`;
  meterFrame = requestAnimationFrame(updateMeter);
}

function concatenateChunks(parts) {
  const totalLength = parts.reduce((sum, part) => sum + part.length, 0);
  const samples = new Float32Array(totalLength);
  let offset = 0;
  for (const part of parts) {
    samples.set(part, offset);
    offset += part.length;
  }
  return samples;
}

function waitForRecorderStop() {
  return new Promise((resolve) => {
    stopResolver = resolve;
    recorderNode.port.postMessage("stop");
  });
}

function renderTake(slot) {
  const take = takes[slot];
  const target = slot === "original" ? elements.originalResult : elements.dutResult;
  if (!take) {
    target.innerHTML = '<p class="empty">No take recorded.</p>';
    return;
  }

  const clippedClass = take.analysis.clippedSamples > 0 ? " warning" : "";
  target.innerHTML = `
    <dl class="metrics">
      <div><dt>Continuous RMS</dt><dd>${formatDb(take.analysis.continuousRmsDbfs)}</dd></div>
      <div><dt>Active RMS</dt><dd>${formatDb(take.analysis.activeRmsDbfs)}</dd></div>
      <div><dt>Peak</dt><dd>${formatDb(take.analysis.peakDbfs)}</dd></div>
      <div><dt>Active frames</dt><dd>${take.analysis.activePercent.toFixed(1)}%</dd></div>
      <div class="wide${clippedClass}"><dt>Clipped samples</dt><dd>${take.analysis.clippedSamples}</dd></div>
    </dl>
    <audio controls src="${take.url}"></audio>
    <p class="saved">Saved as ${take.savedName}</p>`;
}

function renderComparison() {
  if (!takes.original || !takes.dut) {
    elements.comparison.innerHTML = '<p class="empty">Record both phones to calculate the difference.</p>';
    return;
  }

  const mode = elements.mode.value;
  const result = compareTakes(takes.original.analysis, takes.dut.analysis, mode);
  const direction = Math.abs(result.deltaDb) < 0.1
    ? "effectively matched"
    : result.deltaDb > 0 ? "DUT is louder" : "DUT is quieter";
  const metricName = mode === "active" ? "active-frame RMS" : "continuous RMS";
  elements.comparison.innerHTML = `
    <p class="delta">${result.deltaDb >= 0 ? "+" : ""}${result.deltaDb.toFixed(2)} dB</p>
    <p>${direction}, using ${metricName}.</p>
    <p>Linear first estimate: multiply the DUT amplitude by <strong>${result.dutAmplitudeMultiplier.toFixed(3)}</strong>.</p>`;
}

async function saveTake(slot, label, wav, analysis, samples) {
  const track = stream.getAudioTracks()[0];
  const query = new URLSearchParams({
    slot,
    label,
    sample_rate: String(audioContext.sampleRate),
    duration: (samples.length / audioContext.sampleRate).toFixed(4),
    trim_start: String(TRIM_START_SECONDS),
    trim_end: String(TRIM_END_SECONDS),
    continuous_end: String(CONTINUOUS_END_SECONDS),
    continuous_dbfs: analysis.continuousRmsDbfs.toFixed(4),
    active_dbfs: analysis.activeRmsDbfs.toFixed(4),
    peak_dbfs: analysis.peakDbfs.toFixed(4),
    active_percent: analysis.activePercent.toFixed(3),
    clipped_samples: String(analysis.clippedSamples),
    input_label: track.label,
  });
  const response = await fetch(`/api/save?${query}`, {
    method: "POST",
    headers: { "Content-Type": "audio/wav" },
    body: wav,
  });
  if (!response.ok) throw new Error(`save failed: HTTP ${response.status}`);
  return response.json();
}

async function capture(slot) {
  if (capturing || !recorderNode || !audioContext) return;
  capturing = true;
  setCaptureButtonsEnabled(false);
  elements.enable.disabled = true;
  chunks = [];
  const startedAt = performance.now();
  recorderNode.port.postMessage("start");

  const updateProgress = () => {
    const elapsed = Math.min(CAPTURE_SECONDS, (performance.now() - startedAt) / 1000);
    elements.captureStatus.textContent = `Recording ${slot === "original" ? "Original" : "DUT"}: ${elapsed.toFixed(1)} / ${CAPTURE_SECONDS.toFixed(1)} s. Keep the setup quiet and let playback finish.`;
    if (elapsed < CAPTURE_SECONDS) requestAnimationFrame(updateProgress);
  };
  updateProgress();

  try {
    await new Promise((resolve) => setTimeout(resolve, CAPTURE_SECONDS * 1000));
    await waitForRecorderStop();
    const samples = concatenateChunks(chunks);
    const analysis = analyzePcm(
      samples,
      audioContext.sampleRate,
      TRIM_START_SECONDS,
      TRIM_END_SECONDS,
      CONTINUOUS_END_SECONDS,
    );
    const wav = encodeWav24(samples, audioContext.sampleRate);
    const label = elements.label.value.trim() || "take";
    const saved = await saveTake(slot, label, wav, analysis, samples);

    if (takes[slot]?.url) URL.revokeObjectURL(takes[slot].url);
    takes[slot] = {
      analysis,
      url: URL.createObjectURL(wav),
      savedName: saved.wav,
    };
    renderTake(slot);
    renderComparison();
    elements.captureStatus.textContent = `${slot === "original" ? "Original" : "DUT"} take complete.`;
  } catch (error) {
    elements.captureStatus.textContent = `Capture failed: ${error.message}`;
  } finally {
    chunks = [];
    capturing = false;
    setCaptureButtonsEnabled(Boolean(recorderNode));
    elements.enable.disabled = false;
  }
}

elements.enable.addEventListener("click", () => {
  enableInput().catch((error) => {
    elements.inputStatus.textContent = `Microphone error: ${error.message}`;
    setCaptureButtonsEnabled(false);
    elements.enable.disabled = false;
  });
});
elements.originalButton.addEventListener("click", () => capture("original"));
elements.dutButton.addEventListener("click", () => capture("dut"));
elements.mode.addEventListener("change", renderComparison);

setCaptureButtonsEnabled(false);
enumerateInputs().catch(() => {});
