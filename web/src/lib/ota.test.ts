import assert from "node:assert/strict";
import test from "node:test";
import {
  buildOtaUploadHeaders,
  canConfirmOta,
  canStartOtaTest,
  canUploadOta,
  computeOtaSha256Hex,
  createOtaApiError,
  createOtaUploadProgress,
  formatOtaFirmwareError,
  normalizeOtaStatus,
  parseOtaSuccessResponse,
  parseOtaResponseText,
  resolveOtaUrl,
  sha256HexFromBytes,
  uploadOtaImage,
  type OtaProgressEventLike,
  type OtaUploadXhr,
} from "./ota.ts";

class FakeUploadTarget {
  onprogress: ((event: OtaProgressEventLike) => void) | null = null;
}

class FakeXhr implements OtaUploadXhr {
  status = 0;
  statusText = "";
  responseText = "";
  onload: (() => void) | null = null;
  onerror: (() => void) | null = null;
  onabort: (() => void) | null = null;
  upload = new FakeUploadTarget();
  method = "";
  url = "";
  sentBody: Blob | null = null;
  readonly headers = new Map<string, string>();
  responseBody = "";

  open(method: string, url: string): void {
    this.method = method;
    this.url = url;
  }

  setRequestHeader(name: string, value: string): void {
    this.headers.set(name, value);
  }

  send(body: Blob): void {
    this.sentBody = body;
    this.upload.onprogress?.({
      lengthComputable: true,
      loaded: body.size,
      total: body.size,
    });
    this.responseText = this.responseBody;
    this.onload?.();
  }

  abort(): void {
    this.onabort?.();
  }
}

test("builds exact OTA upload headers", () => {
  assert.deepEqual(buildOtaUploadHeaders(4096, "abcd".repeat(16)), {
    "Content-Type": "application/octet-stream",
    "X-Linkr-Ota-Size": "4096",
    "X-Linkr-Ota-Sha256": "abcd".repeat(16),
  });
});

test("computes lowercase SHA-256 hex with Web Crypto", async () => {
  const hash = await computeOtaSha256Hex(new TextEncoder().encode("abc"));
  assert.equal(hash, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
});

test("builds OTA URLs from the shared API endpoint base", () => {
  assert.equal(resolveOtaUrl("", "/api/v1"), "/api/v1/ota");
  assert.equal(resolveOtaUrl("/upload", "/api/v1"), "/api/v1/ota/upload");
  assert.equal(
    resolveOtaUrl("/upload", "http://127.0.0.1:8787/api/v1"),
    "http://127.0.0.1:8787/api/v1/ota/upload"
  );
  assert.equal(resolveOtaUrl("/test", "/api/v1/"), "/api/v1/ota/test");
});

test("falls back to deterministic SHA-256 when Web Crypto is unavailable", async () => {
  assert.equal(
    await computeOtaSha256Hex(new Uint8Array(), null),
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
  );
  assert.equal(
    await computeOtaSha256Hex(new TextEncoder().encode("abc"), null),
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
  );
  assert.equal(
    await sha256HexFromBytes(new TextEncoder().encode("abc"), null),
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
  );
});

test("normalizes OTA upload progress and clamps overflow", () => {
  assert.deepEqual(createOtaUploadProgress(256, 1024), {
    loaded: 256,
    total: 1024,
    percent: 25,
  });
  assert.deepEqual(createOtaUploadProgress(2048, 1024), {
    loaded: 1024,
    total: 1024,
    percent: 100,
  });
});

test("gates OTA actions by firmware state", () => {
  const verified = normalizeOtaStatus({
    state: "verified",
    expected_size: 1024,
    written_size: 1024,
    current_image_confirmed: true,
  });
  const pending = normalizeOtaStatus({
    state: "pending_test",
    current_image_confirmed: false,
  });
  const rebooting = normalizeOtaStatus({ state: "rebooting" });
  const unknown = normalizeOtaStatus({ state: "mystery" });

  assert.equal(canUploadOta(verified, false), true);
  assert.equal(canStartOtaTest(verified, false), true);
  assert.equal(canConfirmOta(verified, false), false);
  assert.equal(canConfirmOta(pending, false), true);
  assert.equal(canUploadOta(pending, false), false);
  assert.equal(canUploadOta(rebooting, false), false);
  assert.equal(canUploadOta(unknown, false), false);
  assert.equal(unknown.state, "unknown");
});

test("preserves firmware JSON error messages and codes", () => {
  const payload = parseOtaResponseText(
    JSON.stringify({
      ok: false,
      error: { code: "sha256_mismatch", message: "SHA-256 mismatch" },
    })
  );
  const error = createOtaApiError(400, "Bad Request", payload);

  assert.equal(error.message, "SHA-256 mismatch");
  assert.equal(error.code, "sha256_mismatch");
  assert.equal(error.status, 400);
});

test("normalizes status fields and optional firmware last_error", () => {
  assert.deepEqual(
    normalizeOtaStatus({
      ok: true,
      status: {
        state: "failed",
        expected_size: 4096,
        written_size: 1024,
        max_size: 8192,
        current_image_confirmed: false,
        last_error: { code: "flash_write_failed", message: "flash write failed" },
      },
    }),
    {
      state: "failed",
      expectedSize: 4096,
      writtenSize: 1024,
      maxSize: 8192,
      currentImageConfirmed: false,
      lastError: { code: "flash_write_failed", message: "flash write failed" },
    }
  );
});

test("treats non-positive max_size as unknown instead of rejecting uploads", () => {
  assert.equal(normalizeOtaStatus({ state: "idle", max_size: 0 }).maxSize, null);
  assert.equal(normalizeOtaStatus({ state: "idle", max_size: -1 }).maxSize, null);
});

test("normalizes firmware last_error with code and errno but no message", () => {
  const status = normalizeOtaStatus({
    state: "failed",
    last_error: { code: "slot_invalid", errno: 22 },
  });

  assert.deepEqual(status.lastError, { code: "slot_invalid", errno: 22 });
  assert.equal(formatOtaFirmwareError(status.lastError), "slot_invalid · errno 22");
});

test("uploads raw file body with exact headers and parses success status", async () => {
  const xhr = new FakeXhr();
  xhr.status = 200;
  xhr.statusText = "OK";
  xhr.responseBody = JSON.stringify({
    state: "verified",
    expected_size: 3,
    written_size: 3,
    max_size: 8192,
    current_image_confirmed: true,
  });
  const progressEvents: Array<{ loaded: number; total: number; percent: number }> = [];
  const file = new File([new Uint8Array([1, 2, 3])], "firmware.bin", {
    type: "application/octet-stream",
  });

  const status = await uploadOtaImage(file, "a".repeat(64), {
    xhrFactory: () => xhr,
    onProgress: (event) => progressEvents.push(event),
  });

  assert.equal(xhr.method, "POST");
  assert.equal(xhr.url, resolveOtaUrl("/upload", "/api/v1"));
  assert.equal(xhr.headers.get("Content-Type"), "application/octet-stream");
  assert.equal(xhr.headers.get("X-Linkr-Ota-Size"), "3");
  assert.equal(xhr.headers.get("X-Linkr-Ota-Sha256"), "a".repeat(64));
  assert.equal(xhr.sentBody, file);
  assert.deepEqual(progressEvents, [{ loaded: 3, total: 3, percent: 100 }]);
  assert.deepEqual(status, {
    state: "verified",
    expectedSize: 3,
    writtenSize: 3,
    maxSize: 8192,
    currentImageConfirmed: true,
    lastError: null,
  });
});

test("preserves non-2xx firmware JSON error envelopes during upload", async () => {
  const xhr = new FakeXhr();
  xhr.status = 409;
  xhr.statusText = "Conflict";
  xhr.responseBody = JSON.stringify({
    ok: false,
    error: { code: "sha256_mismatch", message: "SHA-256 mismatch" },
  });
  const file = new File([new Uint8Array([1])], "firmware.bin", {
    type: "application/octet-stream",
  });

  await assert.rejects(
    () => uploadOtaImage(file, "b".repeat(64), { xhrFactory: () => xhr }),
    (error: unknown) => {
      assert.equal(error instanceof Error, true);
      assert.equal(error instanceof Error ? error.message : "", "SHA-256 mismatch");
      return true;
    }
  );
});

function assertInvalidResponseError(error: unknown): true {
  assert.equal(error instanceof Error, true);
  assert.equal("code" in Object(error) ? Reflect.get(Object(error), "code") : undefined, "invalid_response");
  return true;
}

test("rejects empty, malformed, or non-object 2xx OTA JSON as invalid_response", () => {
  assert.throws(() => parseOtaSuccessResponse("", 200, "OK"), assertInvalidResponseError);
  assert.throws(() => parseOtaSuccessResponse("not-json", 200, "OK"), assertInvalidResponseError);
  assert.throws(() => parseOtaSuccessResponse("[]", 200, "OK"), assertInvalidResponseError);
});

test("rejects 2xx XHR upload responses that are empty, malformed, or arrays", async () => {
  const file = new File([new Uint8Array([1])], "firmware.bin", {
    type: "application/octet-stream",
  });

  for (const body of ["", "{", "[]"]) {
    const xhr = new FakeXhr();
    xhr.status = 200;
    xhr.statusText = "OK";
    xhr.responseBody = body;

    await assert.rejects(
      () => uploadOtaImage(file, "c".repeat(64), { xhrFactory: () => xhr }),
      assertInvalidResponseError
    );
  }
});
