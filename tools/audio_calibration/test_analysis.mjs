import assert from "node:assert/strict";
import { analyzePcm, compareTakes, encodeWav24 } from "./web/analysis.js";

const sampleRate = 48000;
const seconds = 3;
const samples = new Float32Array(sampleRate * seconds);
for (let i = 0; i < samples.length; i += 1) {
  samples[i] = 0.1 * Math.sin(2 * Math.PI * 1000 * i / sampleRate);
}

const analysis = analyzePcm(samples, sampleRate, 0.5, 0.5, 2.5);
assert.ok(Math.abs(analysis.continuousRmsDbfs - -23.0103) < 0.01);
assert.ok(Math.abs(analysis.activeRmsDbfs - analysis.continuousRmsDbfs) < 0.01);
assert.equal(analysis.clippedSamples, 0);

const louder = { ...analysis, continuousRmsDbfs: analysis.continuousRmsDbfs + 6.0206 };
const comparison = compareTakes(analysis, louder, "continuous");
assert.ok(Math.abs(comparison.deltaDb - 6.0206) < 0.0001);
assert.ok(Math.abs(comparison.dutAmplitudeMultiplier - 0.5) < 0.0001);

const wav = encodeWav24(samples, sampleRate);
assert.equal(wav.type, "audio/wav");
assert.equal(wav.size, 44 + samples.length * 3);

console.log("audio calibration analysis: PASS");
