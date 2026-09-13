const MIN_DBFS = -120;

export function amplitudeToDbfs(amplitude) {
  return amplitude > 0 ? 20 * Math.log10(amplitude) : MIN_DBFS;
}

export function analyzePcm(
  samples,
  sampleRate,
  trimStartSeconds = 1.5,
  trimEndSeconds = 0.5,
  continuousEndSeconds = 3.5,
) {
  const first = Math.floor(trimStartSeconds * sampleRate);
  const last = samples.length - Math.floor(trimEndSeconds * sampleRate);
  if (sampleRate <= 0 || last - first < Math.floor(sampleRate * 0.25)) {
    throw new Error("capture is too short after trimming");
  }
  const continuousLast = Math.min(last, Math.floor(continuousEndSeconds * sampleRate));
  if (continuousLast - first < Math.floor(sampleRate * 0.25)) {
    throw new Error("continuous measurement window is too short");
  }

  let mean = 0;
  for (let i = first; i < last; i += 1) mean += samples[i];
  mean /= last - first;

  let continuousSquares = 0;
  let peak = 0;
  let clippedSamples = 0;
  for (let i = first; i < last; i += 1) {
    const raw = samples[i];
    const centered = raw - mean;
    if (i < continuousLast) continuousSquares += centered * centered;
    peak = Math.max(peak, Math.abs(centered));
    if (Math.abs(raw) >= 0.999) clippedSamples += 1;
  }

  const frameLength = Math.max(1, Math.floor(sampleRate * 0.02));
  const frames = [];
  let maximumFrameRms = 0;
  for (let start = first; start + frameLength <= last; start += frameLength) {
    let frameSquares = 0;
    for (let i = start; i < start + frameLength; i += 1) {
      const centered = samples[i] - mean;
      frameSquares += centered * centered;
    }
    const frameRms = Math.sqrt(frameSquares / frameLength);
    maximumFrameRms = Math.max(maximumFrameRms, frameRms);
    frames.push({ sumSquares: frameSquares, count: frameLength, rms: frameRms });
  }

  const activeThreshold = maximumFrameRms * Math.pow(10, -12 / 20);
  let activeSquares = 0;
  let activeSamples = 0;
  for (const frame of frames) {
    if (frame.rms >= activeThreshold && maximumFrameRms > 0) {
      activeSquares += frame.sumSquares;
      activeSamples += frame.count;
    }
  }

  const measuredSamples = last - first;
  const continuousSamples = continuousLast - first;
  const continuousRms = Math.sqrt(continuousSquares / continuousSamples);
  const activeRms = activeSamples > 0 ? Math.sqrt(activeSquares / activeSamples) : 0;

  return {
    durationSeconds: samples.length / sampleRate,
    continuousMeasuredSeconds: continuousSamples / sampleRate,
    activeMeasuredSeconds: measuredSamples / sampleRate,
    continuousRmsDbfs: amplitudeToDbfs(continuousRms),
    activeRmsDbfs: amplitudeToDbfs(activeRms),
    peakDbfs: amplitudeToDbfs(peak),
    crestDb: amplitudeToDbfs(peak) - amplitudeToDbfs(continuousRms),
    activePercent: activeSamples * 100 / measuredSamples,
    clippedSamples,
    dcOffset: mean,
  };
}

export function compareTakes(original, dut, mode) {
  const key = mode === "active" ? "activeRmsDbfs" : "continuousRmsDbfs";
  const deltaDb = dut[key] - original[key];
  return {
    deltaDb,
    dutAmplitudeMultiplier: Math.pow(10, -deltaDb / 20),
    metricKey: key,
  };
}

export function encodeWav24(samples, sampleRate) {
  const bytesPerSample = 3;
  const dataBytes = samples.length * bytesPerSample;
  const buffer = new ArrayBuffer(44 + dataBytes);
  const view = new DataView(buffer);

  const text = (offset, value) => {
    for (let i = 0; i < value.length; i += 1) view.setUint8(offset + i, value.charCodeAt(i));
  };

  text(0, "RIFF");
  view.setUint32(4, 36 + dataBytes, true);
  text(8, "WAVE");
  text(12, "fmt ");
  view.setUint32(16, 16, true);
  view.setUint16(20, 1, true);
  view.setUint16(22, 1, true);
  view.setUint32(24, sampleRate, true);
  view.setUint32(28, sampleRate * bytesPerSample, true);
  view.setUint16(32, bytesPerSample, true);
  view.setUint16(34, 24, true);
  text(36, "data");
  view.setUint32(40, dataBytes, true);

  let offset = 44;
  for (const sample of samples) {
    const clamped = Math.max(-1, Math.min(1, sample));
    let value = clamped < 0 ? Math.round(clamped * 0x800000) : Math.round(clamped * 0x7fffff);
    if (value < 0) value += 0x1000000;
    view.setUint8(offset, value & 0xff);
    view.setUint8(offset + 1, (value >>> 8) & 0xff);
    view.setUint8(offset + 2, (value >>> 16) & 0xff);
    offset += bytesPerSample;
  }

  return new Blob([buffer], { type: "audio/wav" });
}
