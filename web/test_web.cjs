// Smoke-test web/index.html script logic under Node with stubbed WebHID/DOM.
// Not part of the firmware build; run manually: node test_web.cjs
const { readFileSync } = require("fs");
const src = readFileSync(__dirname + "/index.html", "utf8")
  .match(/<script>([\s\S]*?)<\/script>/)[1];

// ---- case 1: browser without WebHID must not throw ----
{
  global.navigator = {};
  global.window = { addEventListener: (ev, fn) => { if (ev === "load") global.__load = fn; } };
  global.document = {
    getElementById: () => ({ textContent: "" }),
    querySelector: () => null,
    querySelectorAll: () => [],
  };
  new Function(src)();
  global.__load();
  console.log("case1 no-WebHID: OK");
}

// ---- case 2: full flow against a fake device ----
(async () => {
  const calls = [];
  const elements = {};
  const el = (id) =>
    (elements[id] ??= { textContent: "", disabled: false,
      set onclick(f) { this._oc = f; }, get onclick() { return this._oc; } });
  const mkDV = (arr) => ({
    getUint8: (o) => arr[o],
    getUint16: (o) => arr[o] | (arr[o + 1] << 8),
    buffer: Buffer.from(arr),
  });
  const fakeDev = {
    opened: false, vendorId: 0x057e, productId: 0x2009, productName: "sw2bridge",
    async open() { this.opened = true; },
    async close() { this.opened = false; },
    addEventListener() {},
    async receiveFeatureReport(id) {
      calls.push(["get", id]);
      if (id === 0xf8) return mkDV([0x73, 0x77, 0x32, 0x62, 0x72, 0x69, 0x64, 0x67, 0x65]); // "sw2bridge"
      if (id === 0xf7) return mkDV([1, 0]);                       // config v1, persona pro
      if (id === 0xf9) return mkDV([0, 6, 1, 0, 0x70, 0x0f, 0x09, 0x20]); // status
    },
    async sendFeatureReport(id, data) {
      calls.push(["set", id, Array.from(data.slice(0, 3))]);
    },
  };
  let checkedVal = "1";
  Object.defineProperty(global, "navigator", { value: {
    hid: {
      async requestDevice() { return [fakeDev]; },
      getDevices() { return []; },
      addEventListener() {}, removeEventListener() {},
    },
  }, configurable: true });
  global.window = { addEventListener: (ev, fn) => { if (ev === "load") global.__load = fn; } };
  global.document = {
    getElementById: el,
    querySelector: (sel) => {
      const m = sel.match(/input\[name=persona\]\[value="(\d)"\]/);
      if (m) return el("radio" + m[1]);
      if (/input\[name=persona\]:checked/.test(sel)) return { get value() { return checkedVal; } };
      return null;
    },
    querySelectorAll: () => [],
  };

  new Function(src)();
  await global.__load();

  await el("btnConnect").onclick();
  if (!elements["log"]?.textContent.includes("已连接"))
    throw new Error("connect failed — log: " + elements["log"]?.textContent + " banner: " + elements["banner"]?.textContent);
  await new Promise((r) => setTimeout(r, 50));  // let refreshStatus() settle
  const assert = (name, got, want) => {
    if (got !== want) throw new Error(`${name}: got ${JSON.stringify(got)} want ${JSON.stringify(want)}`);
    console.log(`  ${name}: ${JSON.stringify(got)}`);
  };
  assert("version", el("sVer").textContent, "sw2bridge");
  assert("ble", el("sBle").textContent, "报告流中 · 报告流中");
  // NOTE: if this assert fires the ?? precedence fix regressed — want is BLE_STATES[6] + " · 报告流中"
  assert("batt", el("sBatt").textContent, "3.95 V");
  assert("pid", el("sPid").textContent, "0x2009");

  await el("btnApply").onclick();
  const sets = calls.filter(([dir]) => dir === "set");
  const wantSets = [
    ["set", 0xf6, [0x01, 1, 1]],
    ["set", 0xf6, [0x02, 0, 0]],
    ["set", 0xf6, [0x03, 0, 0]],
  ];
  for (let i = 0; i < wantSets.length; i++) {
    const got = JSON.stringify(sets[i]);
    if (got !== JSON.stringify(wantSets[i]))
      throw new Error(`cmd#${i}: got ${got} want ${JSON.stringify(wantSets[i])}`);
  }
  console.log("  apply sequence: set-config -> save -> reconnect  ✓");
  console.log("case2 WebHID flow: OK");
})().catch((e) => { console.error("FAIL:", e.message); process.exit(1); });
