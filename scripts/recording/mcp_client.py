"""Small persistent stdio client for the installed Snow Shot bridge."""
import json, os, queue, subprocess, threading, time
from pathlib import Path

class Client:
    def __init__(self):
        self.process = subprocess.Popen(
            [os.environ.get('SNOW_SHOT_MCP_EXE', r'D:\ruanjian\SnowShot\bin\snow-shot-mcp.exe')],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, encoding='utf-8', creationflags=subprocess.CREATE_NO_WINDOW)
        self.queue = queue.Queue()
        self.messages = []
        self.sequence = 0
        def reader():
            for line in self.process.stdout:
                try: self.queue.put(json.loads(line))
                except ValueError: self.queue.put({'parse_error': True})
        threading.Thread(target=reader, daemon=True).start()
        self.request('initialize', {'protocolVersion':'2025-11-25', 'capabilities':{},
                     'clientInfo':{'name':'EUI-Edits-recording-smoke','version':'1.0'}})
        self.send({'jsonrpc':'2.0','method':'notifications/initialized'})
        self.tools = self.request('tools/list', {})['result']['tools']

    def send(self, obj):
        self.process.stdin.write(json.dumps(obj)+'\n')
        self.process.stdin.flush()

    def request(self, method, params, timeout=25):
        self.sequence += 1
        ident = self.sequence
        started = time.perf_counter()
        self.send({'jsonrpc':'2.0','id':ident,'method':method,'params':params})
        while True:
            msg = self.queue.get(timeout=max(.1, timeout-(time.perf_counter()-started)))
            if msg.get('id') == ident:
                if 'error' in msg: raise RuntimeError(msg['error'])
                return msg

    def tool(self, name, arguments=None):
        started = time.perf_counter()
        msg = self.request('tools/call', {'name':name,'arguments':arguments or {}})
        value = msg['result'].get('structuredContent')
        if value is None:
            value = json.loads(next(x['text'] for x in msg['result']['content'] if x['type']=='text'))
        self.messages.append({'tool':name,'elapsed_ms':round((time.perf_counter()-started)*1000,2),
                              'arguments':arguments or {},'response':value})
        if msg['result'].get('isError') or value.get('ok') is False or value.get('error'):
            raise RuntimeError(value)
        return value

    def close(self):
        self.process.stdin.close()
        try: self.process.wait(timeout=4)
        except subprocess.TimeoutExpired:
            self.process.terminate(); self.process.wait(timeout=4)
        return self.process.returncode

if __name__ == '__main__':
    c = Client()
    try:
        for name in ['snow_shot_mcp_status','snow_shot_app_displays','snow_shot_recording_state']:
            result = c.tool(name)
            if name == 'snow_shot_mcp_status':
                print(name, json.dumps(result, ensure_ascii=False)[:5000])
            else: print(name, json.dumps(result, ensure_ascii=False))
        out = Path(os.environ.get('NEO_RECORDING_OUT', str(Path(__file__).resolve().parents[2]/'build-promo-recording')))
        out.mkdir(parents=True, exist_ok=True)
        (out/'inspect.json').write_text(json.dumps(c.messages, ensure_ascii=False, indent=2), encoding='utf-8')
    finally: print('bridge_exit', c.close())
