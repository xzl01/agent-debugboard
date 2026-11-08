import assert from "node:assert/strict";
import { test } from "vitest";

import { JSDOM } from "jsdom";

import {
  initializeDeviceConnection,
  parseDeviceFragment,
  renderDeviceConnection,
} from "../public/connect.js";

function createPage(url = "https://example.test/connect.html") {
  return new JSDOM(
    `<!doctype html>
      <html data-state="missing">
        <body>
          <p id="connection-status"></p>
          <span id="device-address"></span>
          <a id="device-link" hidden rel="noreferrer">打开设备</a>
        </body>
      </html>`,
    { url },
  );
}

test("接受三个 RFC1918 网段及其边界地址", () => {
  for (const host of [
    "10.0.0.0",
    "10.255.255.255",
    "172.16.0.0",
    "172.31.255.255",
    "192.168.0.0",
    "192.168.255.255",
  ]) {
    assert.deepEqual(parseDeviceFragment(`#device=${host}`), {
      host,
      url: `http://${host}/`,
    });
  }
});

test("拒绝越界 octet 和非规范前导零", () => {
  for (const hash of [
    "#device=10.0.0.256",
    "#device=10.0.999.1",
    "#device=010.0.0.1",
    "#device=10.00.0.1",
  ]) {
    assert.equal(parseDeviceFragment(hash), null, hash);
  }
});

test("拒绝公开、回环、链路本地和未指定地址", () => {
  for (const hash of [
    "#device=8.8.8.8",
    "#device=127.0.0.1",
    "#device=169.254.1.1",
    "#device=0.0.0.0",
    "#device=172.15.255.255",
    "#device=172.32.0.0",
  ]) {
    assert.equal(parseDeviceFragment(hash), null, hash);
  }
});

test("拒绝 URL、hostname、端口、路径和 IPv6", () => {
  for (const hash of [
    "#device=http://10.0.0.1/",
    "#device=board.local",
    "#device=10.0.0.1:8080",
    "#device=10.0.0.1/admin",
    "#device=[fd00::1]",
  ]) {
    assert.equal(parseDeviceFragment(hash), null, hash);
  }
});

test("拒绝百分号和编码输入", () => {
  for (const hash of [
    "#device=10.0.0.%31",
    "#device%3D10.0.0.1",
    "#device=10%2e0%2e0%2e1",
  ]) {
    assert.equal(parseDeviceFragment(hash), null, hash);
  }
});

test("拒绝重复键、额外参数、未知键和空输入", () => {
  for (const hash of [
    "",
    "#",
    "#device=",
    "#other=10.0.0.1",
    "#device=10.0.0.1&device=10.0.0.2",
    "#device=10.0.0.1&other=x",
    "#device=10.0.0.1\n",
    "#device=10.0.0.1\r\n",
  ]) {
    assert.equal(parseDeviceFragment(hash), null, hash);
  }
});

test("缺少 fragment 时保持链接惰性", () => {
  const { window } = createPage();
  renderDeviceConnection(window.document, "");

  const link = window.document.querySelector("#device-link");
  assert.equal(window.document.documentElement.dataset.state, "missing");
  assert.equal(link.hidden, true);
  assert.equal(link.hasAttribute("href"), false);
});

test("无效 fragment 时保持链接惰性", () => {
  const { window } = createPage();
  renderDeviceConnection(window.document, "#device=8.8.8.8");

  const link = window.document.querySelector("#device-link");
  assert.equal(window.document.documentElement.dataset.state, "invalid");
  assert.equal(link.hidden, true);
  assert.equal(link.hasAttribute("href"), false);
});

test("合法 fragment 只展示地址并设置规范 HTTP 根链接", () => {
  const { window } = createPage();
  const before = window.location.href;
  renderDeviceConnection(window.document, "#device=192.168.7.2");

  const link = window.document.querySelector("#device-link");
  assert.equal(window.document.documentElement.dataset.state, "ready");
  assert.equal(window.document.querySelector("#device-address").textContent, "192.168.7.2");
  assert.equal(link.getAttribute("href"), "http://192.168.7.2/");
  assert.equal(link.hidden, false);
  assert.equal(window.location.href, before);
});

test("从合法 fragment 变为非法时清除旧链接", () => {
  const { window } = createPage();
  const { document } = window;
  renderDeviceConnection(document, "#device=10.1.2.3");
  renderDeviceConnection(document, "#device=10.1.2.3\n");

  const link = document.querySelector("#device-link");
  assert.equal(document.documentElement.dataset.state, "invalid");
  assert.equal(link.hidden, true);
  assert.equal(link.hasAttribute("href"), false);
  assert.equal(document.querySelector("#device-address").textContent, "—");
});

test("初始化只读取 hash，不读取 query，也不请求或导航", () => {
  const { window } = createPage(
    "https://example.test/connect.html?device=10.2.3.4#device=172.20.1.2",
  );
  let requestCount = 0;
  window.fetch = () => {
    requestCount += 1;
    throw new Error("unexpected fetch");
  };
  const before = window.location.href;

  initializeDeviceConnection(window.document, window.location, window);

  assert.equal(window.document.querySelector("#device-address").textContent, "172.20.1.2");
  assert.equal(requestCount, 0);
  assert.equal(window.location.href, before);

  window.location.hash = "";
  window.dispatchEvent(new window.HashChangeEvent("hashchange"));
  assert.equal(window.document.documentElement.dataset.state, "missing");
  assert.equal(window.document.querySelector("#device-link").hasAttribute("href"), false);
});
