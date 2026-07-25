import { describe, it } from "node:test";
import assert from "node:assert/strict";

import {
  buildSigrokFrame,
  formatSigrokErrorMessage,
  parseSigrokFrame,
  parseSigrokHeader,
  SIGROK_MAGIC,
  SIGROK_PROTOCOL_VERSION,
  SIGROK_SAMPLE_INDEX_MODULO,
  SigrokClient,
  SigrokEventCode,
  SigrokFrameType,
  sigrokEventCodeName,
} from "./sigrokClient.ts";

function buildDataFramePayload({
  sampleIndex,
  sampleCount,
  compression,
  channelMask,
  samples,
}: {
  sampleIndex: number;
  sampleCount: number;
  compression: number;
  channelMask: number;
  samples: readonly number[] | Uint8Array;
}): Uint8Array {
  const sampleBytes = samples instanceof Uint8Array ? samples : Uint8Array.from(samples);
  return new Uint8Array([
    sampleIndex & 0xff,
    (sampleIndex >> 8) & 0xff,
    (sampleIndex >> 16) & 0xff,
    sampleCount & 0xff,
    (sampleCount >> 8) & 0xff,
    compression,
    channelMask & 0xff,
    (channelMask >> 8) & 0xff,
    ...sampleBytes,
  ]);
}

function parseFrameData(data: Uint8Array) {
  const header = parseSigrokHeader(data);
  assert.ok(header);
  return parseSigrokFrame(header, data);
}

describe("sigrok frame helpers", () => {
  it("builds a HELLO_REQ frame", () => {
    const frame = buildSigrokFrame(1, SigrokFrameType.HELLO_REQ);

    assert.equal(frame.length, 9);
    assert.equal(frame[0], SIGROK_MAGIC);
    assert.equal(frame[1], SIGROK_PROTOCOL_VERSION);
    assert.equal(frame[2], SigrokFrameType.HELLO_REQ);
    assert.equal(frame[3], 1);
    assert.equal(frame[7], 0);
    assert.equal(frame[8], 0);
  });

  it("builds a CONFIG_REQ frame with payload", () => {
    const payload = new Uint8Array([1, 0, 7, 0x83, 0x00, 0xe8, 0x03, 0x00, 0, 0, 0xff, 0xff]);
    const frame = buildSigrokFrame(7, SigrokFrameType.CONFIG_REQ, payload);

    assert.equal(frame.length, 21);
    assert.equal(frame[2], SigrokFrameType.CONFIG_REQ);
    assert.equal(frame[3], 7);
    assert.equal(frame[7], 12);
    assert.equal(frame[8], 0);
    assert.deepEqual(frame.slice(9), payload);
  });

  it("parses a valid header", () => {
    const header = parseSigrokHeader(
      new Uint8Array([SIGROK_MAGIC, SIGROK_PROTOCOL_VERSION, 0x02, 0x01, 0, 0, 0, 5, 0])
    );

    assert.ok(header);
    assert.equal(header.magic, SIGROK_MAGIC);
    assert.equal(header.version, SIGROK_PROTOCOL_VERSION);
    assert.equal(header.type, 0x02);
    assert.equal(header.id, 1);
    assert.equal(header.payloadLen, 5);
  });

  it("rejects invalid magic in the header", () => {
    const header = parseSigrokHeader(
      new Uint8Array([0x00, SIGROK_PROTOCOL_VERSION, 0x02, 0x01, 0, 0, 0, 5, 0])
    );

    assert.equal(header, null);
  });

  it("parses a DATA frame", () => {
    const samples = new Uint8Array([0x55, 0xaa, 0xff, 0x00]);
    const data = new Uint8Array([
      SIGROK_MAGIC,
      SIGROK_PROTOCOL_VERSION,
      SigrokFrameType.DATA,
      0x01,
      0,
      0,
      0,
      12,
      0,
      0x00,
      0x00,
      0x00,
      0x04,
      0x00,
      0x01,
      0xff,
      0x00,
      ...samples,
    ]);
    const header = parseSigrokHeader(data);

    assert.ok(header);
    const frame = parseSigrokFrame(header, data);

    assert.deepEqual(frame, {
      type: SigrokFrameType.DATA,
      id: 1,
      meta: { sampleIndex: 0, sampleCount: 4, compression: 1, channelMask: 0x00ff },
      samples,
    });
  });

  it("decodes BIT_PACK DATA into byte-aligned sample units", () => {
    const values = [0x00, 0x05, 0x07, 0x02, 0x01];
    const data = buildSigrokFrame(
      9,
      SigrokFrameType.DATA,
      buildDataFramePayload({
        sampleIndex: 12,
        sampleCount: values.length,
        compression: 1,
        channelMask: 0x0007,
        samples: values,
      })
    );

    const frame = parseFrameData(data);

    assert.deepEqual(frame, {
      type: SigrokFrameType.DATA,
      id: 9,
      meta: { sampleIndex: 12, sampleCount: 5, compression: 1, channelMask: 0x0007 },
      samples: Uint8Array.from(values),
    });
  });

  it("requires BIT_PACK payload length to stay byte-aligned per sample", () => {
    const samples = new Uint8Array(1024);
    samples[0] = 0x01;
    samples[1023] = 0x01;
    const data = buildSigrokFrame(
      16,
      SigrokFrameType.DATA,
      buildDataFramePayload({
        sampleIndex: 0,
        sampleCount: 1024,
        compression: 1,
        channelMask: 0x0001,
        samples,
      })
    );

    const frame = parseFrameData(data);

    assert.deepEqual(frame, {
      type: SigrokFrameType.DATA,
      id: 16,
      meta: { sampleIndex: 0, sampleCount: 1024, compression: 1, channelMask: 0x0001 },
      samples,
    });
  });

  it("rejects BIT_PACK payloads that use dense cross-time bitstream length", () => {
    const data = buildSigrokFrame(
      17,
      SigrokFrameType.DATA,
      buildDataFramePayload({
        sampleIndex: 0,
        sampleCount: 1024,
        compression: 1,
        channelMask: 0x0001,
        samples: new Uint8Array(128),
      })
    );

    assert.throws(() => parseFrameData(data), /BIT_PACK DATA frame length does not match sampleCount and channelMask/);
  });

  it("decodes BIT_PACK_RLE DATA with 1-byte UART-style runs", () => {
    const data = buildSigrokFrame(
      10,
      SigrokFrameType.DATA,
      buildDataFramePayload({
        sampleIndex: 40,
        sampleCount: 6,
        compression: 3,
        channelMask: 0x0003,
        samples: [0x01, 0x03, 0x00, 0x00, 0x02, 0x00, 0x03, 0x01, 0x00],
      })
    );

    const frame = parseFrameData(data);

    assert.deepEqual(frame, {
      type: SigrokFrameType.DATA,
      id: 10,
      meta: { sampleIndex: 40, sampleCount: 6, compression: 3, channelMask: 0x0003 },
      samples: Uint8Array.from([0x01, 0x01, 0x01, 0x00, 0x00, 0x03]),
    });
  });

  it("decodes BIT_PACK_RLE DATA with 2-byte sample values", () => {
    const data = buildSigrokFrame(
      11,
      SigrokFrameType.DATA,
      buildDataFramePayload({
        sampleIndex: 99,
        sampleCount: 3,
        compression: 3,
        channelMask: 0x0fff,
        samples: [0x23, 0x01, 0x02, 0x00, 0xbc, 0x0a, 0x01, 0x00],
      })
    );

    const frame = parseFrameData(data);

    assert.deepEqual(frame, {
      type: SigrokFrameType.DATA,
      id: 11,
      meta: { sampleIndex: 99, sampleCount: 3, compression: 3, channelMask: 0x0fff },
      samples: Uint8Array.from([0x23, 0x01, 0x23, 0x01, 0xbc, 0x0a]),
    });
  });

  it("rejects truncated BIT_PACK_RLE tuples", () => {
    const data = buildSigrokFrame(
      12,
      SigrokFrameType.DATA,
      buildDataFramePayload({
        sampleIndex: 0,
        sampleCount: 2,
        compression: 3,
        channelMask: 0x0001,
        samples: [0x01, 0x02],
      })
    );

    assert.throws(() => parseFrameData(data), /BIT_PACK_RLE DATA frame tuple is truncated/);
  });

  it("rejects zero-length BIT_PACK_RLE runs", () => {
    const data = buildSigrokFrame(
      13,
      SigrokFrameType.DATA,
      buildDataFramePayload({
        sampleIndex: 0,
        sampleCount: 1,
        compression: 3,
        channelMask: 0x0001,
        samples: [0x01, 0x00, 0x00],
      })
    );

    assert.throws(() => parseFrameData(data), /BIT_PACK_RLE DATA frame contains zero-length run/);
  });

  it("rejects BIT_PACK_RLE runs that overflow sampleCount", () => {
    const data = buildSigrokFrame(
      14,
      SigrokFrameType.DATA,
      buildDataFramePayload({
        sampleIndex: 0,
        sampleCount: 2,
        compression: 3,
        channelMask: 0x0001,
        samples: [0x01, 0x03, 0x00],
      })
    );

    assert.throws(() => parseFrameData(data), /BIT_PACK_RLE DATA frame expands beyond advertised sampleCount/);
  });

  it("rejects BIT_PACK_RLE runs when the final expanded count mismatches sampleCount", () => {
    const data = buildSigrokFrame(
      15,
      SigrokFrameType.DATA,
      buildDataFramePayload({
        sampleIndex: 0,
        sampleCount: 3,
        compression: 3,
        channelMask: 0x0001,
        samples: [0x01, 0x02, 0x00],
      })
    );

    assert.throws(
      () => parseFrameData(data),
      /BIT_PACK_RLE DATA frame expanded sample count does not match advertised sampleCount/
    );
  });

  it("publishes protocol helpers for event names and sample-index modulo", () => {
    assert.equal(sigrokEventCodeName(SigrokEventCode.OVERRUN), "OVERRUN");
    assert.equal(sigrokEventCodeName(0xff), "UNKNOWN(255)");
    assert.equal(SIGROK_SAMPLE_INDEX_MODULO, 1 << 24);
  });

  it("formats sigrok error payloads with symbolic names when known", () => {
    assert.equal(
      formatSigrokErrorMessage({ errorCode: 6, detail: 2 }),
      "Sigrok error INVALID_STATE: 2"
    );
    assert.equal(
      formatSigrokErrorMessage({ errorCode: 99, detail: 7 }),
      "Sigrok error 99: 7"
    );
  });
});

describe("SigrokClient", () => {
  it("starts disconnected", () => {
    const client = new SigrokClient();
    assert.equal(client.getState(), "disconnected");
  });

  it("emits frame and data events when fed a complete frame", () => {
    const client = new SigrokClient();
    const events: Array<string> = [];
    client.addEventListener((event) => {
      events.push(event.type);
    });

    client.feedBinaryData(
      new Uint8Array([
        SIGROK_MAGIC,
        SIGROK_PROTOCOL_VERSION,
        SigrokFrameType.DATA,
        0x01,
        0,
        0,
        0,
        10,
        0,
        0x00,
        0x00,
        0x00,
        0x02,
        0x00,
        0x01,
        0x03,
        0x00,
        0x02,
        0x01,
      ])
    );

    assert.deepEqual(events, ["frame", "data"]);
  });

  it("buffers fragmented frames until complete", () => {
    const client = new SigrokClient();
    const frameTypes: number[] = [];
    client.addEventListener((event) => {
      if (event.type === "frame") {
        frameTypes.push(event.frame.type);
      }
    });

    const frame = buildSigrokFrame(1, SigrokFrameType.HELLO_RESP, new Uint8Array([1, 0, 2, 0x00, 0x40]));
    client.feedBinaryData(frame.slice(0, 5));
    client.feedBinaryData(frame.slice(5));

    assert.deepEqual(frameTypes, [SigrokFrameType.HELLO_RESP]);
  });

  it("emits both inner frames from one coalesced binary feed", () => {
    const client = new SigrokClient();
    const frameTypes: number[] = [];
    const sampleIndices: number[] = [];
    client.addEventListener((event) => {
      if (event.type === "frame") {
        frameTypes.push(event.frame.type);
      }
      if (event.type === "data") {
        sampleIndices.push(event.meta.sampleIndex);
      }
    });

    const first = buildSigrokFrame(
      10,
      SigrokFrameType.DATA,
      new Uint8Array([0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x01, 0x00, 0xaa])
    );
    const second = buildSigrokFrame(
      11,
      SigrokFrameType.DATA,
      new Uint8Array([0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x01, 0x00, 0x55])
    );
    const coalesced = new Uint8Array(first.length + second.length);
    coalesced.set(first);
    coalesced.set(second, first.length);

    client.feedBinaryData(coalesced);

    assert.deepEqual(frameTypes, [SigrokFrameType.DATA, SigrokFrameType.DATA]);
    assert.deepEqual(sampleIndices, [0, 1]);
  });

  it("decodes compressed DATA before a coalesced EVENT frame", () => {
    const client = new SigrokClient();
    const frameTypes: number[] = [];
    const decodedSamples: number[][] = [];
    client.addEventListener((event) => {
      if (event.type === "frame") {
        frameTypes.push(event.frame.type);
      }
      if (event.type === "data") {
        decodedSamples.push(Array.from(event.samples));
      }
    });

    const dataFrame = buildSigrokFrame(
      21,
      SigrokFrameType.DATA,
      buildDataFramePayload({
        sampleIndex: 7,
        sampleCount: 4,
        compression: 3,
        channelMask: 0x0003,
        samples: [0x00, 0x02, 0x00, 0x03, 0x02, 0x00],
      })
    );
    const eventFrame = buildSigrokFrame(
      22,
      SigrokFrameType.EVENT,
      new Uint8Array([0x01, 0x00, SigrokEventCode.TRIGGERED, 0x07, 0x00, 0x00])
    );
    const coalesced = new Uint8Array(dataFrame.length + eventFrame.length);
    coalesced.set(dataFrame);
    coalesced.set(eventFrame, dataFrame.length);

    client.feedBinaryData(coalesced);

    assert.deepEqual(frameTypes, [SigrokFrameType.DATA, SigrokFrameType.EVENT]);
    assert.deepEqual(decodedSamples, [[0x00, 0x00, 0x03, 0x03]]);
    assert.equal(client.getState(), "running");
  });

  it("transitions to running when fed a TRIGGERED event frame", () => {
    const client = new SigrokClient();
    const states: string[] = [];
    client.addEventListener((event) => {
      if (event.type === "state") {
        states.push(event.state);
      }
    });

    client.feedBinaryData(
      buildSigrokFrame(
        3,
        SigrokFrameType.EVENT,
        new Uint8Array([0x01, 0x00, SigrokEventCode.TRIGGERED, 0x00, 0x00, 0x00])
      )
    );

    assert.equal(client.getState(), "running");
    assert.deepEqual(states, ["running"]);
  });
});
