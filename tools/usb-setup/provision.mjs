// Write an Aeris purifier's Wi-Fi + MQTT settings over USB, so the firmware boots straight onto the
// network without the setup hotspot. Talks to the usb-setup helper app (firmware/), which must be
// running on the unit. See README.md in this folder.
import fs from 'node:fs';
import assert from 'node:assert/strict';
import { parseArgs } from 'node:util';
import { SerialPort } from 'serialport';

const USAGE = `usage:
  node provision.mjs setup   --app <firmware.bin> --prefix <aeris/room/> --ssid <wifi> --mqtt-host <ip>
                             [--mqtt-port 1883] [--mqtt-user <user>]
  node provision.mjs upgrade --from <current-firmware.bin> --app <new-firmware.bin>
  node provision.mjs check   <setup options>   (validate only, no device needed)
options: --device <id> (several Photons on USB), --backup <file> (save the old settings; contains passwords)
passwords: environment variables AERIS_WIFI_PASS and AERIS_MQTT_PASS`;

let mode, opt;
try {
  const parsed = parseArgs({
    allowPositionals: true,
    options: Object.fromEntries(['app', 'from', 'prefix', 'ssid', 'mqtt-host', 'mqtt-port', 'mqtt-user', 'device', 'backup']
      .map(name => [name, { type: 'string' }])),
  });
  [mode] = parsed.positionals;
  opt = parsed.values;
  assert(['setup', 'upgrade', 'check'].includes(mode) && opt.app);
  assert(mode === 'upgrade' ? opt.from : opt.prefix && opt.ssid && opt['mqtt-host']);
} catch {
  console.error(USAGE);
  process.exit(2);
}

// struct Settings in Source/src/purifier.ino: 352 bytes, little-endian, with compiler padding.
const SIZE = 352;
const F = { sig: [0, 25], ssid: [25, 64], wpass: [89, 64], ip: [153, 32], user: [192, 32], mpass: [224, 32], prefix: [256, 64] };
const PORT = 188, INTS = 320, COLORS = 344;

// The firmware signs its settings with its compile time (__DATE__ " " __TIME__); a mismatch resets them.
function signature(file) {
  const text = fs.readFileSync(file).toString('latin1');
  const found = [...new Set(text.match(/(?:Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec) [ 0-9][0-9] [0-9]{4} [0-9]{2}:[0-9]{2}:[0-9]{2}/g))];
  assert.equal(found.length, 1, `expected exactly one build timestamp in ${file}`);
  return found[0];
}

function crc32(data) {
  let crc = 0xffffffff;
  for (const byte of data) {
    crc ^= byte;
    for (let i = 0; i < 8; i++) crc = (crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
  }
  return (~crc) >>> 0;
}

const get = (buf, [off, size]) => {
  const bytes = buf.subarray(off, off + size);
  const end = bytes.indexOf(0);
  assert(end >= 0, 'unterminated settings field');
  return bytes.toString('utf8', 0, end);
};
const put = (buf, [off, size], value, name) => {
  const bytes = Buffer.from(String(value));
  assert(bytes.length < size && !bytes.includes(0), `${name} is too long (max ${size - 1} bytes)`);
  buf.fill(0, off, off + size);
  bytes.copy(buf, off);
};

// Same values as loadDefaults() in purifier.ino, plus the given network settings.
function fresh() {
  assert(opt.prefix.endsWith('/'), 'the topic prefix must end with /');
  const port = Number(opt['mqtt-port'] || 1883);
  assert(Number.isInteger(port) && port > 0 && port < 65536, 'invalid MQTT port');
  const buf = Buffer.alloc(SIZE);
  put(buf, F.sig, signature(opt.app), 'signature');
  put(buf, F.ssid, opt.ssid, 'Wi-Fi name');
  put(buf, F.wpass, process.env.AERIS_WIFI_PASS || '', 'Wi-Fi password');
  put(buf, F.ip, opt['mqtt-host'], 'MQTT host');
  buf.writeInt32LE(port, PORT);
  put(buf, F.user, opt['mqtt-user'] || 'mqtt_user', 'MQTT user');
  put(buf, F.mpass, process.env.AERIS_MQTT_PASS || '', 'MQTT password');
  put(buf, F.prefix, opt.prefix, 'topic prefix');
  [10, 30, 70, 3, 60, 180].forEach((v, i) => buf.writeInt32LE(v, INTS + 4 * i));
  [0xAFE5, 0x07FF, 0xFFFF].forEach((v, i) => buf.writeUInt16LE(v, COLORS + 2 * i)); // green-yellow, cyan, white
  return buf;
}

const describe = s => `Wi-Fi "${get(s, F.ssid)}"${get(s, F.wpass) ? ' (password set)' : ' (open)'}, MQTT ` +
  `${get(s, F.user)}@${get(s, F.ip)}:${s.readInt32LE(PORT)}, prefix ${get(s, F.prefix)}, signed for "${get(s, F.sig)}"`;

if (mode === 'check') {
  console.log(`OK: ${describe(fresh())}`);
  process.exit(0);
}

async function openPort() {
  for (let i = 0; i < 30; i++) {
    const found = (await SerialPort.list()).filter(p => (p.vendorId || '').toLowerCase() === '2b04' &&
      (!opt.device || (p.serialNumber || '').toLowerCase() === opt.device.toLowerCase()));
    if (found.length > 1) throw new Error('several Photons on USB: pass --device <id>');
    if (found.length === 1) {
      const port = new SerialPort({ path: found[0].path.replace('/dev/tty.', '/dev/cu.'), baudRate: 115200, autoOpen: false });
      await new Promise((resolve, reject) => port.open(e => e ? reject(e) : resolve()));
      return { port, id: found[0].serialNumber };
    }
    await new Promise(r => setTimeout(r, 1000));
  }
  throw new Error('no Photon on USB: is the usb-setup helper running on it?');
}

function exchange(port, payload, length) {
  return new Promise((resolve, reject) => {
    let data = Buffer.alloc(0);
    const timer = setTimeout(() => finish(new Error('no reply from the helper: reset the unit and retry')), 15000);
    const receive = chunk => {
      data = Buffer.concat([data, chunk]);
      if (data.length > length) finish(new Error(`unexpected reply: ${data.toString('latin1', 0, 40).trim()}`));
      else if (data.length === length) finish(null, data);
    };
    const finish = (error, result) => {
      clearTimeout(timer);
      port.off('data', receive);
      error ? reject(error) : resolve(result);
    };
    port.on('data', receive);
    port.write(payload, e => { if (e) finish(e); });
  });
}

async function read(port) {
  const frame = await exchange(port, Buffer.from('READ\n'), SIZE + 4);
  assert.equal(crc32(frame.subarray(0, SIZE)), frame.readUInt32LE(SIZE), 'settings read failed its checksum');
  return Buffer.from(frame.subarray(0, SIZE));
}

const { port, id } = await openPort();
try {
  await new Promise(r => setTimeout(r, 1000));
  const old = await read(port);
  if (opt.backup) {
    fs.writeFileSync(opt.backup, old, { flag: 'wx', mode: 0o600 });
    console.log(`old settings saved to ${opt.backup}`);
  }
  let settings;
  if (mode === 'setup') {
    settings = fresh();
  } else {
    assert.equal(get(old, F.sig), signature(opt.from), 'the unit\'s settings do not belong to --from');
    settings = Buffer.from(old);
    put(settings, F.sig, signature(opt.app), 'signature');
  }
  const crc = Buffer.alloc(4);
  crc.writeUInt32LE(crc32(settings));
  const reply = await exchange(port, Buffer.concat([Buffer.from('WRITE\n'), settings, crc]), 4);
  assert(reply.equals(Buffer.from('OK\r\n')), `the helper rejected the settings: ${reply.toString('latin1').trim()}`);
  assert((await read(port)).equals(settings), 'read-back does not match what was written');
  console.log(`done on ${id}: ${describe(settings)}${mode === 'upgrade' ? ' (all other settings kept)' : ''}`);
  console.log(`next: put the unit in DFU mode and flash ${opt.app}`);
} finally {
  if (port.isOpen) await new Promise(r => port.close(() => r()));
}
