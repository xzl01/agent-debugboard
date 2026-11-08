import { describe, it, type TestContext } from "node:test";
import assert from "node:assert/strict";

import {
  buildSigrokFrame,
  buildSigrokConfigFrameRequest,
  formatSigrokErrorMessage,
  parseSigrokFrame,
  parseSigrokHeader,
  SIGROK_MAGIC,
  SIGROK_PROTOCOL_VERSION,
  SIGROK_SAMPLE_INDEX_MODULO,
  SigrokClient,
  SigrokCompression,
  SigrokCompressionFlag,
  SigrokEventCode,
  SigrokFrameType,
  SigrokModeId,
  SigrokModeFlag,
  SigrokServerFlag,
  SigrokTriggerType,
  sigrokEventCodeName,
  type SigrokConfigReq,
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

function buildCapsPayload(fast8Flags: number, wide11Flags: number): Uint8Array {
  return Uint8Array.from([
    2,
    SigrokModeId.FAST8,
    fast8Flags,
    8,
    1,
    0x48,
    0xe8,
    0x01,
    0x1f,
    SigrokModeId.WIDE11,
    wide11Flags,
    11,
    2,
    0x48,
    0xe8,
    0x01,
    0x1b,
  ]);
}

function denseDataFrame(channelMask: number, sampleCount: number, samples: readonly number[] | Uint8Array): Uint8Array {
  return buildSigrokFrame(0, SigrokFrameType.DATA, buildDataFramePayload({
    sampleIndex: 0,
    sampleCount,
    compression: SigrokCompression.DENSE_BITS,
    channelMask,
    samples,
  }));
}

class FakeWebSocket {
  static readonly CONNECTING = 0;
  static readonly OPEN = 1;
  static readonly CLOSING = 2;
  static readonly CLOSED = 3;
  static instances: FakeWebSocket[] = [];

  readyState = FakeWebSocket.CONNECTING;
  binaryType = "";
  closeCalls = 0;
  sent: Uint8Array[] = [];
  onopen: ((event: Event) => void) | null = null;
  onmessage: ((event: MessageEvent) => void) | null = null;
  onclose: ((event: CloseEvent) => void) | null = null;
  onerror: ((event: Event) => void) | null = null;
  readonly url: string;

  constructor(url: string) {
    this.url = url;
    FakeWebSocket.instances.push(this);
  }

  send(data: Uint8Array): void {
    this.sent.push(data);
    const header = parseSigrokHeader(data);
    assert.ok(header);
    if (header.type === SigrokFrameType.HELLO_REQ) {
      this.receive(buildSigrokFrame(header.id, SigrokFrameType.HELLO_RESP, Uint8Array.from([
        2, SigrokServerFlag.CONFIG_V2 | SigrokServerFlag.GENERIC_PACKED_BURST, 2, 0x08, 0x10,
      ])));
    } else if (header.type === SigrokFrameType.CAPS_REQ) {
      this.receive(buildSigrokFrame(header.id, SigrokFrameType.CAPS_RESP, buildCapsPayload(0x3f, 0x3f)));
    }
  }

  receive(data: Uint8Array): void {
    this.onmessage?.({ data: data.slice().buffer } as MessageEvent);
  }

  close(): void {
    this.closeCalls += 1;
    this.readyState = FakeWebSocket.CLOSED;
    this.onclose?.(new Event("close") as CloseEvent);
  }

  forceLateOpen(): void {
    this.readyState = FakeWebSocket.OPEN;
    this.onopen?.(new Event("open"));
  }
}

function streamConfig(channelMask: number): SigrokConfigReq {
  return {
    modeId: channelMask > 0xff ? SigrokModeId.WIDE11 : SigrokModeId.FAST8,
    triggerType: SigrokTriggerType.NONE,
    triggerChannel: 0,
    channelMask,
    samplerateKhz: 1000,
    preSamples: 0,
    postSamples: 0,
  };
}

function configAck(socket: FakeWebSocket): Uint8Array {
  const header = parseSigrokHeader(socket.sent.at(-1)!);
  assert.ok(header);
  assert.ok(header.type === SigrokFrameType.CONFIG_REQ || header.type === SigrokFrameType.CONFIG_V2_REQ);
  return buildSigrokFrame(header.id, SigrokFrameType.CONFIG_RESP, Uint8Array.from([1, 0, 1, 0xe8, 3, 0]));
}

async function createClientFixture(t: TestContext, channelMask?: number) {
  const originalWebSocket = globalThis.WebSocket;
  FakeWebSocket.instances = [];
  Object.assign(globalThis, { WebSocket: FakeWebSocket });
  const client = new SigrokClient();
  t.after(() => {
    client.disconnect();
    Object.assign(globalThis, { WebSocket: originalWebSocket });
  });
  const connection = client.connect("ws://fixture.invalid/live");
  const socket = FakeWebSocket.instances[0];
  socket.forceLateOpen();
  await connection;
  if (channelMask !== undefined) {
    const configured = client.configure(streamConfig(channelMask));
    socket.receive(configAck(socket));
    await configured;
  }
  return { client, socket };
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

  it("keeps uint16 captures on exact 12-byte CONFIG v1 even when CONFIG_V2 is advertised", () => {
    const request = buildSigrokConfigFrameRequest(
      {
        modeId: SigrokModeId.FAST8,
        triggerType: SigrokTriggerType.RISING,
        triggerChannel: 0,
        channelMask: 0x0001,
        samplerateKhz: 100000,
         preSamples: 128,
         postSamples: 65535,
      },
      { supportsConfigV2: true }
    );

    assert.equal(request.type, SigrokFrameType.CONFIG_REQ);
    assert.deepEqual(
      request.payload,
      Uint8Array.from([0x01, 0x01, 0x00, 0x01, 0x00, 0xa0, 0x86, 0x01, 0x80, 0x00, 0xff, 0xff])
    );
  });

  it("encodes post=0 sentinel as v1 CONFIG without requiring CONFIG_V2", () => {
    const request = buildSigrokConfigFrameRequest({
      modeId: SigrokModeId.WIDE11,
      triggerType: SigrokTriggerType.NONE,
      triggerChannel: 0,
      channelMask: 0x07ff,
      samplerateKhz: 25000,
      preSamples: 0,
      postSamples: 0,
    });

    assert.equal(request.type, SigrokFrameType.CONFIG_REQ);
    assert.deepEqual(
      request.payload,
      Uint8Array.from([0x02, 0x00, 0x00, 0xff, 0x07, 0xa8, 0x61, 0x00, 0x00, 0x00, 0x00, 0x00])
    );
  });

  it("uses exact 16-byte CONFIG_V2 for u32 pre/post when advertised", () => {
    const request = buildSigrokConfigFrameRequest(
      {
        modeId: SigrokModeId.FAST8,
        triggerType: SigrokTriggerType.EITHER,
        triggerChannel: 7,
        channelMask: 0x0089,
        samplerateKhz: 125000,
         preSamples: 128,
         postSamples: 100000,
      },
      { supportsConfigV2: true }
    );

    assert.equal(request.type, SigrokFrameType.CONFIG_V2_REQ);
    assert.deepEqual(
      request.payload,
      Uint8Array.from([0x01, 0x03, 0x07, 0x89, 0x00, 0x48, 0xe8, 0x01, 0x80, 0x00, 0x00, 0x00, 0xa0, 0x86, 0x01, 0x00])
    );
  });

  it("keeps high-rate post=0 sentinel on CONFIG v1 without requiring CONFIG_V2", () => {
    const request = buildSigrokConfigFrameRequest({
      modeId: SigrokModeId.FAST8,
      triggerType: SigrokTriggerType.NONE,
      triggerChannel: 0,
      channelMask: 0x0001,
      samplerateKhz: 125000,
      preSamples: 0,
      postSamples: 0,
    });

    assert.equal(request.type, SigrokFrameType.CONFIG_REQ);
    assert.deepEqual(
      request.payload,
      Uint8Array.from([0x01, 0x00, 0x00, 0x01, 0x00, 0x48, 0xe8, 0x01, 0x00, 0x00, 0x00, 0x00])
    );
  });

  it("rejects large CONFIG requests when old firmware does not advertise CONFIG_V2", () => {
    assert.throws(
      () => buildSigrokConfigFrameRequest({
        modeId: SigrokModeId.WIDE11,
        triggerType: SigrokTriggerType.NONE,
        triggerChannel: 0,
        channelMask: 0x07ff,
        samplerateKhz: 100000,
        preSamples: 0,
        postSamples: 100000,
      }),
      /require CONFIG_V2/
    );
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

  it("decodes SINGLE_BITS DATA into byte-aligned samples", () => {
    const data = buildSigrokFrame(
      18,
      SigrokFrameType.DATA,
      buildDataFramePayload({
        sampleIndex: 21,
        sampleCount: 10,
        compression: SigrokCompression.SINGLE_BITS,
        channelMask: 0x0040,
        samples: [0x4d, 0x03],
      })
    );

    const frame = parseFrameData(data);

    assert.deepEqual(frame, {
      type: SigrokFrameType.DATA,
      id: 18,
      meta: {
        sampleIndex: 21,
        sampleCount: 10,
        compression: SigrokCompression.SINGLE_BITS,
        channelMask: 0x0040,
      },
      samples: Uint8Array.from([1, 0, 1, 1, 0, 0, 1, 0, 1, 1]),
    });
  });

  it("decodes SINGLE_BITS_RLE over dense bytes", () => {
    const data = buildSigrokFrame(
      19,
      SigrokFrameType.DATA,
      buildDataFramePayload({
        sampleIndex: 0,
        sampleCount: 16,
        compression: SigrokCompression.SINGLE_BITS_RLE,
        channelMask: 0x0001,
        samples: [0x55, 0x02, 0x00],
      })
    );

    const frame = parseFrameData(data);

    assert.deepEqual(frame.samples, Uint8Array.from([
      1, 0, 1, 0, 1, 0, 1, 0,
      1, 0, 1, 0, 1, 0, 1, 0,
    ]));
  });

  it("rejects invalid SINGLE_BITS shape and tail padding", () => {
    const multiChannel = buildSigrokFrame(
      20,
      SigrokFrameType.DATA,
      buildDataFramePayload({
        sampleIndex: 0,
        sampleCount: 8,
        compression: SigrokCompression.SINGLE_BITS,
        channelMask: 0x0003,
        samples: [0x00],
      })
    );
    const nonZeroTail = buildSigrokFrame(
      21,
      SigrokFrameType.DATA,
      buildDataFramePayload({
        sampleIndex: 0,
        sampleCount: 9,
        compression: SigrokCompression.SINGLE_BITS,
        channelMask: 0x0001,
        samples: [0x00, 0x80],
      })
    );

    assert.throws(
      () => parseFrameData(multiChannel),
      /SINGLE_BITS DATA frame requires exactly one active channel/
    );
    assert.throws(
      () => parseFrameData(nonZeroTail),
      /SINGLE_BITS DATA frame has non-zero tail padding/
    );
  });

  it("decodes PACKED_PALETTE2 DATA selectors LSB-first", () => {
    const data = buildSigrokFrame(
      20,
      SigrokFrameType.DATA,
      buildDataFramePayload({
        sampleIndex: 0,
        sampleCount: 16,
        compression: SigrokCompression.PACKED_PALETTE2,
        channelMask: 0x00ff,
        samples: [0x00, 0x40, 0xaa, 0xaa],
      })
    );

    const frame = parseFrameData(data);

    assert.deepEqual(frame, {
      type: SigrokFrameType.DATA,
      id: 20,
      meta: {
        sampleIndex: 0,
        sampleCount: 16,
        compression: SigrokCompression.PACKED_PALETTE2,
        channelMask: 0x00ff,
      },
      samples: Uint8Array.from([
        0x00, 0x40, 0x00, 0x40, 0x00, 0x40, 0x00, 0x40,
        0x00, 0x40, 0x00, 0x40, 0x00, 0x40, 0x00, 0x40,
      ]),
    });
  });

  it("rejects malformed PACKED_PALETTE2 DATA", () => {
    const invalidPayloads = [
      [0x00, 0x40, 0x00],
      [0x00, 0x00, 0x00, 0x00],
      [0x00, 0x40, 0x00, 0x80],
    ];
    for (const samples of invalidPayloads) {
      const data = buildSigrokFrame(
        21,
        SigrokFrameType.DATA,
        buildDataFramePayload({
          sampleIndex: 0,
          sampleCount: 9,
          compression: SigrokCompression.PACKED_PALETTE2,
          channelMask: 0x00ff,
          samples,
        })
      );
      assert.throws(() => parseFrameData(data), /PACKED_PALETTE2 DATA frame/);
    }
  });

  it("decodes independent DENSE_BITS golden bodies into compact LE sample units", () => {
    assert.equal(SigrokCompression.DENSE_BITS, 7);
    assert.equal(SigrokCompressionFlag.DENSE_BITS, 0x10);
    const vectors = [
      {
        mask: 0x024b,
        count: 9,
        body: [0x20, 0x88, 0x41, 0x8a, 0x39, 0x1f],
        decoded: [0, 1, 2, 3, 4, 5, 6, 7, 31],
      },
      {
        mask: 0x07ff,
        count: 7,
        body: [0x00, 0x08, 0xc0, 0xff, 0x01, 0x58, 0x15, 0x55, 0x49, 0x13],
        decoded: [0x00, 0x00, 0x01, 0x00, 0xff, 0x07, 0x00, 0x04, 0x55, 0x01, 0xaa, 0x02, 0xd2, 0x04],
      },
      {
        mask: 0x0441,
        count: 9,
        body: [0xc8, 0xd5, 0x99, 0x00],
        decoded: [0, 1, 7, 2, 5, 3, 6, 4, 0],
      },
    ];
    for (const { mask, count, body, decoded } of vectors) {
      const frame = parseFrameData(denseDataFrame(mask, count, body));
      assert.ok(frame?.type === SigrokFrameType.DATA);
      assert.equal(frame.meta.channelMask, mask);
      assert.equal(frame.meta.sampleCount, count);
      assert.deepEqual(frame.samples, Uint8Array.from(decoded));
    }
  });

  for (let width = 1; width <= 11; width += 1) {
    it(`decodes DENSE_BITS k=${width} across byte boundaries`, () => {
      const mask = (1 << width) - 1;
      for (const count of [1, 7, 8, 9]) {
        const values = Array.from({ length: count }, (_, index) => (mask - index * 0x125) & mask);
        // Independent, bit-at-a-time reference; golden vectors above stay literal.
        const body = new Uint8Array(Math.ceil(count * width / 8));
        values.forEach((value, index) => {
          for (let bit = 0; bit < width; bit += 1) {
            const position = index * width + bit;
            body[position >>> 3] |= ((value >>> bit) & 1) << (position & 7);
          }
        });
        const frame = parseFrameData(denseDataFrame(mask, count, body));
        assert.ok(frame?.type === SigrokFrameType.DATA);
        assert.deepEqual(frame.samples, Uint8Array.from(values.flatMap((value) =>
          width <= 8 ? [value] : [value & 0xff, value >>> 8]
        )), `k=${width}, count=${count}`);
      }
    });
  }

  it("decodes bounded DENSE_BITS payloads at chunk and wire-size limits", () => {
    for (const [mask, width, count] of [[0x001f, 5, 4096], [0x07ff, 11, 2048], [1, 1, 32768]]) {
      const body = new Uint8Array(count * width / 8).fill(0xff);
      const frame = parseFrameData(denseDataFrame(mask, count, body));
      assert.ok(frame?.type === SigrokFrameType.DATA);
      const expected = Uint8Array.from(Array.from({ length: count }, () =>
        width <= 8 ? [mask] : [0xff, 0x07]
      ).flat());
      assert.deepEqual(frame.samples, expected);
    }
  });

  it("rejects DENSE_BITS invalid masks, counts, exact lengths, padding, and oversized bodies", () => {
    const invalid = [
      { mask: 0, count: 1, body: [0], error: /non-zero channel mask/ },
      { mask: 0x0800, count: 1, body: [0], error: /within 0x07ff/ },
      { mask: 0x8001, count: 1, body: [0], error: /within 0x07ff/ },
      { mask: 0x07ff, count: 0, body: [], error: /non-zero sampleCount/ },
      { mask: 0x001f, count: 9, body: [0, 0, 0, 0, 0], error: /length does not match/ },
      { mask: 0x001f, count: 9, body: [0, 0, 0, 0, 0, 0, 0], error: /length does not match/ },
      { mask: 0x001f, count: 9, body: [0, 0, 0, 0, 0, 0x80], error: /tail padding/ },
      { mask: 1, count: 32769, body: new Uint8Array(4097), error: /4096-byte/ },
    ];
    for (const { mask, count, body, error } of invalid) {
      assert.throws(() => parseFrameData(denseDataFrame(mask, count, body)), error);
    }
    for (let usedBits = 1; usedBits < 8; usedBits += 1) {
      assert.throws(
        () => parseFrameData(denseDataFrame(0x0400, usedBits, [1 << usedBits])),
        /non-zero tail padding/
      );
    }
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
        channelMask: 0x07ff,
        samples: [0x23, 0x01, 0x02, 0x00, 0xbc, 0x0a, 0x01, 0x00],
      })
    );

    const frame = parseFrameData(data);

    assert.deepEqual(frame, {
      type: SigrokFrameType.DATA,
      id: 11,
      meta: { sampleIndex: 99, sampleCount: 3, compression: 3, channelMask: 0x07ff },
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

describe("SigrokClient connection lifecycle", () => {
  it("closes and rejects a socket that is disconnected while still connecting", async () => {
    const originalWebSocket = globalThis.WebSocket;
    FakeWebSocket.instances = [];
    Object.assign(globalThis, { WebSocket: FakeWebSocket });

    try {
      const client = new SigrokClient();
      const connection = client.connect("ws://fixture.invalid/live");
      const socket = FakeWebSocket.instances[0];
      assert.ok(socket);
      assert.equal(client.getState(), "connecting");

      client.disconnect();

      await assert.rejects(connection, /Client disconnected/);
      assert.equal(socket.closeCalls, 1);
      assert.equal(client.getState(), "disconnected");

      socket.forceLateOpen();
      assert.equal(socket.sent.length, 0);
      assert.equal(socket.closeCalls, 2);
      assert.equal(client.getState(), "disconnected");
    } finally {
      Object.assign(globalThis, { WebSocket: originalWebSocket });
    }
  });
});

describe("SigrokClient successful CONFIG snapshot", () => {
  it("rejects DATA before the first CONFIG ACK without publishing either DATA event", async (t) => {
    const { client, socket } = await createClientFixture(t);
    const events: string[] = [];
    client.addEventListener((event) => events.push(event.type));
    const data = denseDataFrame(0x0441, 1, [0]);
    assert.throws(() => socket.receive(data), /without a successful CONFIG/);
    const configured = client.configure(streamConfig(0x0441));
    assert.throws(() => socket.receive(data), /without a successful CONFIG/);
    assert.deepEqual(events, []);
    socket.receive(configAck(socket));
    await configured;
    events.length = 0;
    socket.receive(data);
    assert.deepEqual(events, ["frame", "data"]);
  });

  it("commits the copied CONFIG_V2 mask synchronously before ACK callbacks and coalesced DATA", async (t) => {
    const { client, socket } = await createClientFixture(t);
    const config = { ...streamConfig(0x024b), postSamples: 100000 };
    const configured = client.configure(config);
    assert.equal(parseSigrokHeader(socket.sent.at(-1)!)?.type, SigrokFrameType.CONFIG_V2_REQ);
    config.channelMask = 0x001f; // Same popcount, but not the configuration sent on wire.
    const masks: number[] = [];
    const data = denseDataFrame(0x024b, 1, [0x1f]);
    client.addEventListener((event) => {
      if (event.type === "frame" && event.frame.type === SigrokFrameType.CONFIG_RESP) {
        socket.receive(data);
      }
      if (event.type === "data") masks.push(event.meta.channelMask);
    });
    socket.receive(Uint8Array.from([...configAck(socket), ...data]));
    assert.deepEqual(masks, [0x024b, 0x024b]);
    assert.throws(() => socket.receive(denseDataFrame(0x001f, 1, [0])), /does not match successful CONFIG/);
    await configured;
  });

  it("retains the successful mask while another CONFIG is pending or fails, ignoring unsolicited ACKs", async (t) => {
    const oldMask = 0x0441;
    const newMask = 0x0007;
    const { client, socket } = await createClientFixture(t, oldMask);
    const masks: number[] = [];
    const frameMasks: number[] = [];
    client.addEventListener((event) => {
      if (event.type === "data") masks.push(event.meta.channelMask);
      if (event.type === "frame" && event.frame.type === SigrokFrameType.DATA) {
        frameMasks.push(event.frame.meta.channelMask);
      }
    });
    const pending = client.configure(streamConfig(newMask));
    const rejected = assert.rejects(pending, /Unexpected response to CONFIG/);
    const ack = configAck(socket);
    socket.receive(denseDataFrame(oldMask, 1, [1]));
    assert.throws(() => socket.receive(denseDataFrame(newMask, 1, [1])), /does not match successful CONFIG/);
    socket.receive(buildSigrokFrame(parseSigrokHeader(ack)!.id, SigrokFrameType.ERROR, Uint8Array.from([7, 0, 0])));
    await rejected;
    socket.receive(ack);
    socket.receive(denseDataFrame(oldMask, 1, [2]));
    assert.throws(() => socket.receive(denseDataFrame(newMask, 1, [2])), /does not match successful CONFIG/);

    const configured = client.configure(streamConfig(newMask));
    socket.receive(Uint8Array.from([...configAck(socket), ...denseDataFrame(newMask, 1, [3])]));
    await configured;
    assert.throws(() => socket.receive(denseDataFrame(oldMask, 1, [3])), /does not match successful CONFIG/);
    assert.deepEqual(masks, [oldMask, oldMask, newMask]);
    assert.deepEqual(frameMasks, masks);
  });

  it("checks the complete successful mask for every codec during continuous streaming", async (t) => {
    const { client, socket } = await createClientFixture(t, 0x0001);
    socket.receive(buildSigrokFrame(0, SigrokFrameType.EVENT, Uint8Array.from([
      1, 0, SigrokEventCode.RUNNING, 0, 0, 0,
    ])));
    const published: number[] = [];
    client.addEventListener((event) => {
      if (event.type === "data" || (event.type === "frame" && event.frame.type === SigrokFrameType.DATA)) {
        published.push(event.type === "data" ? event.meta.channelMask : event.frame.meta.channelMask);
      }
    });
    const codecs = [
      [SigrokCompression.RAW, [0]],
      [SigrokCompression.BIT_PACK, [0]],
      [SigrokCompression.RLE, [0, 1, 0]],
      [SigrokCompression.BIT_PACK_RLE, [0, 1, 0]],
      [SigrokCompression.SINGLE_BITS, [0]],
      [SigrokCompression.SINGLE_BITS_RLE, [0, 1, 0]],
      [SigrokCompression.PACKED_PALETTE2, [0, 1, 0]],
      [SigrokCompression.DENSE_BITS, [0]],
    ] as const;
    for (const [compression, samples] of codecs) {
      const frame = (channelMask: number) => buildSigrokFrame(0, SigrokFrameType.DATA, buildDataFramePayload({
        sampleIndex: 0, sampleCount: 1, compression, channelMask, samples,
      }));
      socket.receive(frame(0x0001));
      assert.throws(() => socket.receive(frame(0x0002)), /does not match successful CONFIG/);
    }
    assert.deepEqual(published, Array(16).fill(0x0001));
    assert.equal(client.getState(), "running");
  });

  for (const closeMode of ["disconnect", "socket-close"] as const) {
    it(`clears the snapshot and pending CONFIG after ${closeMode} and reconnect`, async (t) => {
      const { client, socket } = await createClientFixture(t, 0x0001);
      const pending = client.configure(streamConfig(0x0002));
      const rejected = assert.rejects(pending, /Client disconnected|WebSocket closed/);
      const staleAck = configAck(socket);
      if (closeMode === "disconnect") client.disconnect();
      else socket.close();
      await rejected;
      assert.equal(client.getState(), "disconnected");

      const connection = client.connect("ws://fixture.invalid/reconnected");
      const reconnectedSocket = FakeWebSocket.instances.at(-1)!;
      reconnectedSocket.forceLateOpen();
      await connection;
      reconnectedSocket.receive(staleAck);
      const published: string[] = [];
      client.addEventListener((event) => published.push(event.type));
      for (const mask of [0x0001, 0x0002]) {
        assert.throws(() => reconnectedSocket.receive(denseDataFrame(mask, 1, [0])), /without a successful CONFIG/);
      }
      assert.deepEqual(published, []);
      const configured = client.configure(streamConfig(0x0002));
      reconnectedSocket.receive(configAck(reconnectedSocket));
      await configured;
      reconnectedSocket.receive(denseDataFrame(0x0002, 1, [1]));
    });
  }

  it("does not restore configured state after disconnecting between an ACK and its Promise continuation", async (t) => {
    const { client, socket } = await createClientFixture(t);
    const configured = client.configure(streamConfig(0x0001));
    socket.receive(configAck(socket));
    client.disconnect();
    await configured;
    assert.equal(client.getState(), "disconnected");
  });
});

describe("SigrokClient", () => {
  it("starts disconnected", () => {
    const client = new SigrokClient();
    assert.equal(client.getState(), "disconnected");
  });

  it("emits frame and data events when fed a complete frame", async (t) => {
    const { client } = await createClientFixture(t, 0x0003);
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

  it("parses HELLO server_flags with no capability bits", () => {
    const client = new SigrokClient();
    client.feedBinaryData(
      buildSigrokFrame(1, SigrokFrameType.HELLO_RESP, new Uint8Array([1, 0x00, 2, 0x00, 0x40]))
    );

    const capabilities = client.getServerCapabilities();
    assert.equal(capabilities.serverFlags, 0);
    assert.equal(capabilities.supportsConfigV2, false);
    assert.equal(capabilities.supportsGenericPackedBurst, false);
    assert.equal(capabilities.hello?.maxPayloadLen, 0x4000);
  });

  it("parses HELLO server_flags bit0-only as legacy CONFIG_V2 support", () => {
    const client = new SigrokClient();
    client.feedBinaryData(
      buildSigrokFrame(
        1,
        SigrokFrameType.HELLO_RESP,
        new Uint8Array([1, SigrokServerFlag.CONFIG_V2, 2, 0x00, 0x40])
      )
    );

    const capabilities = client.getServerCapabilities();
    assert.equal(capabilities.serverFlags, SigrokServerFlag.CONFIG_V2);
    assert.equal(capabilities.supportsConfigV2, true);
    assert.equal(capabilities.supportsGenericPackedBurst, false);
    assert.equal(capabilities.hello?.maxPayloadLen, 0x4000);
  });

  it("parses HELLO server_flags bits0|1 as CONFIG_V2 plus GENERIC_PACKED_BURST", () => {
    const client = new SigrokClient();
    const bothFlags =
      SigrokServerFlag.CONFIG_V2 | SigrokServerFlag.GENERIC_PACKED_BURST;
    client.feedBinaryData(
      buildSigrokFrame(
        1,
        SigrokFrameType.HELLO_RESP,
        new Uint8Array([1, bothFlags, 2, 0x00, 0x40])
      )
    );

    const capabilities = client.getServerCapabilities();
    assert.equal(capabilities.serverFlags, bothFlags);
    assert.equal(capabilities.supportsConfigV2, true);
    assert.equal(capabilities.supportsGenericPackedBurst, true);
    assert.equal(capabilities.hello?.maxPayloadLen, 0x4000);
  });

  it("caches CAPS mode flags and exposes PRE_TRIGGER per mode", () => {
    const client = new SigrokClient();
    const allModeFlags =
      SigrokModeFlag.CONTINUOUS |
      SigrokModeFlag.TRIGGER_NONE |
      SigrokModeFlag.TRIGGER_RISING |
      SigrokModeFlag.TRIGGER_FALLING |
      SigrokModeFlag.TRIGGER_EITHER |
      SigrokModeFlag.PRE_TRIGGER;

    client.feedBinaryData(
      buildSigrokFrame(2, SigrokFrameType.CAPS_RESP, buildCapsPayload(allModeFlags, allModeFlags))
    );

    const capabilities = client.getServerCapabilities();
    assert.equal(capabilities.caps?.modeCount, 2);
    assert.equal(capabilities.caps?.modes[0]?.modeId, SigrokModeId.FAST8);
    assert.equal(
      (capabilities.caps?.modes[0]?.modeFlags ?? 0) & SigrokModeFlag.PRE_TRIGGER,
      SigrokModeFlag.PRE_TRIGGER
    );
    assert.equal(capabilities.caps?.modes[1]?.modeId, SigrokModeId.WIDE11);
    assert.equal(capabilities.caps?.modes[0]?.compression, 0x1f);
    assert.equal(capabilities.caps?.modes[1]?.compression, 0x1b);
    for (const mode of capabilities.caps?.modes ?? []) {
      assert.equal(mode.compression & SigrokCompressionFlag.DENSE_BITS, 0x10);
    }
    assert.equal(Object.isFrozen(capabilities.caps), true);
    assert.equal(Object.isFrozen(capabilities.caps?.modes), true);
  });

  it("keeps legacy CAPS without PRE_TRIGGER unsupported", () => {
    const client = new SigrokClient();
    const legacyModeFlags =
      SigrokModeFlag.CONTINUOUS |
      SigrokModeFlag.TRIGGER_NONE |
      SigrokModeFlag.TRIGGER_RISING |
      SigrokModeFlag.TRIGGER_FALLING |
      SigrokModeFlag.TRIGGER_EITHER;

    client.feedBinaryData(
      buildSigrokFrame(3, SigrokFrameType.CAPS_RESP, buildCapsPayload(legacyModeFlags, legacyModeFlags))
    );

    const capabilities = client.getServerCapabilities();
    const modes = capabilities.caps?.modes ?? [];
    assert.equal((modes[0]?.modeFlags ?? 0) & SigrokModeFlag.PRE_TRIGGER, 0);
    assert.equal((modes[1]?.modeFlags ?? 0) & SigrokModeFlag.PRE_TRIGGER, 0);
  });

  it("emits both inner frames from one coalesced binary feed", async (t) => {
    const { client } = await createClientFixture(t, 0x0001);
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

  it("decodes compressed DATA before a coalesced EVENT frame", async (t) => {
    const { client } = await createClientFixture(t, 0x0003);
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
