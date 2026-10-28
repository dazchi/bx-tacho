# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Darren Chi
# Phone-sized screenshots of the web app's Bluetooth tab (demo mode, Traditional
# Chinese) for the user manual, through Edge's DevTools protocol: written to
# docs/manual-screens/webapp-top.png, webapp-curve.png, webapp-update.png and
# webapp-code.png. Serve web/ on port
# 8731 first (python -m http.server 8731 --directory web), then run from the
# repository root:
#   python docs/tools/capture-manual-webapp.py
import asyncio
import base64
import json
import os
import subprocess
import tempfile
import time
import urllib.request

import websockets

EDGE = r'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe'
URL = 'http://localhost:8731/?demo#ble'
PORT = 9333
OUT = os.path.join(os.path.dirname(__file__), '..', 'manual-screens')

profile = tempfile.mkdtemp(prefix='edge-cdp-')
proc = subprocess.Popen([EDGE, '--headless=new', '--disable-gpu', '--hide-scrollbars', f'--remote-debugging-port={PORT}',
                         f'--user-data-dir={profile}', '--lang=zh-TW', 'about:blank'])
try:
    for _ in range(50):
        try:
            pages = json.load(urllib.request.urlopen(f'http://127.0.0.1:{PORT}/json'))
            ws_url = next(p['webSocketDebuggerUrl'] for p in pages if p['type'] == 'page')
            break
        except Exception:
            time.sleep(0.2)

    async def main():
        async with websockets.connect(ws_url, max_size=50_000_000) as ws:
            n = 0

            async def cmd(method, **params):
                nonlocal n
                n += 1
                await ws.send(json.dumps({'id': n, 'method': method, 'params': params}))
                while True:
                    msg = json.loads(await ws.recv())
                    if msg.get('id') == n:
                        if 'error' in msg:
                            raise RuntimeError(msg['error'])
                        return msg.get('result', {})

            await cmd('Emulation.setDeviceMetricsOverride', width=412, height=915, deviceScaleFactor=2, mobile=True)
            await cmd('Page.enable')
            await cmd('Page.navigate', url=URL)
            await asyncio.sleep(4)
            # Traditional Chinese, and no demo notice (it would confuse a manual reader)
            await cmd('Runtime.evaluate', expression='''
                const sel = document.getElementById('lang-select');
                sel.value = 'zh-Hant'; sel.dispatchEvent(new Event('change'));
                document.getElementById('ble-note').hidden = true;
                window.scrollTo(0, 0);
            ''')
            await asyncio.sleep(2)
            for name, y, h in (('webapp-top', 0, 915), ('webapp-curve', None, 600)):
                if y is None:
                    r = await cmd('Runtime.evaluate', returnByValue=True, expression='''
                        (() => { const el = document.querySelector('#panel-ble h2, #panel-ble .title');
                          const curve = [...document.querySelectorAll('#panel-ble h2')].find(h => /曲線/.test(h.textContent));
                          const t = (curve || el).getBoundingClientRect().top + window.scrollY;
                          return Math.max(0, t - 20); })()''')
                    y = r['result']['value']
                shot = await cmd('Page.captureScreenshot', format='png', captureBeyondViewport=True,
                                 clip={'x': 0, 'y': y, 'width': 412, 'height': h, 'scale': 1})
                with open(os.path.join(OUT, f'{name}.png'), 'wb') as f:
                    f.write(base64.b64decode(shot['data']))
                print('wrote', name, 'from y =', y)

            # The firmware update over Bluetooth. Needs the latest release on the site
            # (web/firmware/latest/latest.json, as the Pages workflow publishes it);
            # what a connected BX Tacho would show (its version, the progress) is filled in.
            r = await cmd('Runtime.evaluate', awaitPromise=True, returnByValue=True, expression='''
                (async () => {
                  const btn = document.getElementById('bota-latest');
                  if (btn.hidden) return -1;
                  btn.click();
                  await new Promise((ok) => setTimeout(ok, 1500));
                  document.getElementById('bota-current').textContent = 'BX Tacho 目前版本：v2.2.0';
                  document.getElementById('bota-update').disabled = false;
                  document.getElementById('bota-progress').hidden = false;
                  document.getElementById('bota-fill').style.width = '47%';
                  document.getElementById('bota-text').textContent = '傳送中 47%';
                  const box = document.querySelector('section[aria-labelledby="bota-title"]');
                  return box.getBoundingClientRect().top + window.scrollY - 12;
                })()''')
            y = r['result']['value']
            if y < 0:
                print('skipped webapp-update: no latest release on the site (web/firmware/latest/)')
            else:
                r = await cmd('Runtime.evaluate', returnByValue=True, expression='''
                    document.querySelector('section[aria-labelledby="bota-title"]').getBoundingClientRect().height + 24''')
                shot = await cmd('Page.captureScreenshot', format='png', captureBeyondViewport=True,
                                 clip={'x': 0, 'y': y, 'width': 412, 'height': r['result']['value'], 'scale': 1})
                with open(os.path.join(OUT, 'webapp-update.png'), 'wb') as f:
                    f.write(base64.b64decode(shot['data']))
                print('wrote webapp-update')

            # The code dialog, with the code from BX Tacho's display typed in
            await cmd('Runtime.evaluate', expression='''
                window.scrollTo(0, 0);
                const dlg = document.getElementById('code-dialog');
                dlg.showModal();
                const input = document.getElementById('code-input');
                input.value = '482 917';
                input.dispatchEvent(new Event('input'));
                input.blur();
            ''')
            await asyncio.sleep(1)
            r = await cmd('Runtime.evaluate', returnByValue=True, expression='''
                (() => { const b = document.querySelector('#code-dialog .dialog__body').getBoundingClientRect();
                         return [b.top - 16, b.height + 32]; })()''')
            top, height = r['result']['value']
            shot = await cmd('Page.captureScreenshot', format='png',
                             clip={'x': 0, 'y': top, 'width': 412, 'height': height, 'scale': 1})
            with open(os.path.join(OUT, 'webapp-code.png'), 'wb') as f:
                f.write(base64.b64decode(shot['data']))
            print('wrote webapp-code')

    asyncio.run(main())
finally:
    proc.terminate()
