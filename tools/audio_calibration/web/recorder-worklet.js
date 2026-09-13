class PcmRecorderProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.recording = false;
    this.buffer = new Float32Array(2048);
    this.offset = 0;
    this.port.onmessage = (event) => {
      if (event.data === "start") {
        this.offset = 0;
        this.recording = true;
      } else if (event.data === "stop") {
        this.recording = false;
        this.flush();
        this.port.postMessage({ type: "stopped" });
      }
    };
  }

  flush() {
    if (this.offset === 0) return;
    const chunk = this.buffer.slice(0, this.offset);
    this.port.postMessage({ type: "chunk", samples: chunk.buffer }, [chunk.buffer]);
    this.offset = 0;
  }

  process(inputs) {
    if (!this.recording) return true;
    const channel = inputs[0]?.[0];
    if (!channel) return true;

    let sourceOffset = 0;
    while (sourceOffset < channel.length) {
      const count = Math.min(channel.length - sourceOffset, this.buffer.length - this.offset);
      this.buffer.set(channel.subarray(sourceOffset, sourceOffset + count), this.offset);
      this.offset += count;
      sourceOffset += count;
      if (this.offset === this.buffer.length) this.flush();
    }
    return true;
  }
}

registerProcessor("pcm-recorder", PcmRecorderProcessor);
