"""Test the actual loadable extension using DuckDB CLI and a local HTTP mock."""
import json
import os
import pathlib
import subprocess
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = pathlib.Path(__file__).resolve().parents[1]
EXT = ROOT / 'build/release/extension/decide/decide.duckdb_extension'
CLI = os.environ.get('DUCKDB_CLI', '/opt/homebrew/bin/duckdb')
requests = []
attempts = 0

class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        global attempts
        assert self.path == '/v1/decisions'
        assert self.headers['Authorization'] == 'Bearer mock-key'
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        assert body['model'] == 'gpt-6-luna'
        requests.append(body)
        text = body['input']
        if text == 'retry':
            attempts += 1
            if attempts == 1:
                self.send_response(429); self.end_headers(); return
        if text == 'unauthorized':
            self.send_response(401); self.end_headers(); return
        answers = []
        for q in body['questions']:
            a = {'name': q.get('name'), 'type': q['type']}
            if text == 'refuse':
                a['type'] = 'refusal'
            elif q['type'] == 'predicate':
                a['probability'] = 2 if text == 'invalid' else .8
            elif q['type'] == 'choice':
                a['choice'] = 'unknown' if text == 'unknown' else q['choices'][0]['value']
            elif q['type'] == 'score':
                a['score'] = 1.25
            answers.append(a)
        if text == 'wrong-name':
            answers[0]['name'] = 'unexpected'
        self.send_response(200); self.end_headers()
        self.wfile.write(json.dumps({'answers': answers}).encode())

server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
threading.Thread(target=server.serve_forever, daemon=True).start()
setup = f"""LOAD '{EXT}';
SET decisions_endpoint='http://127.0.0.1:{server.server_port}/v1/decisions';
CREATE SECRET (TYPE decisions, API_KEY 'mock-key');
"""

def run(sql, fail=None, prefix=setup):
    p = subprocess.run([CLI, '-unsigned', '-json', '-c', prefix + sql], capture_output=True, text=True,
                       env={**os.environ, 'OPENAI_API_KEY': ''})
    if fail:
        assert p.returncode and fail in p.stderr, (sql, p.stderr)
        return
    assert p.returncode == 0, p.stderr
    output = p.stdout.strip()
    values = []
    decoder = json.JSONDecoder()
    while output:
        value, end = decoder.raw_decode(output)
        values.append(value)
        output = output[end:].lstrip()
    return values[-1]

try:
    assert run("SELECT decision('yes','Is it true?') AS v")[0]['v'] is True
    assert run("SELECT decision('yes','Is it true?',.9) AS v")[0]['v'] is False
    assert run("SELECT decision_prob('yes','Is it true?') AS v")[0]['v'] == .8
    assert run("SELECT decision_choice('yes','Route?', ['billing','other']) AS v")[0]['v'] == 'billing'
    assert run("SELECT decision_score('yes','Severity?', ['low','medium','high']) AS v")[0]['v'] == 1.25
    assert run("SELECT decision_prob(p,'Relevant?') AS v FROM (SELECT 'patent' AS title) p")[0]['v'] == .8
    assert 'title' in requests[-1]['input']
    before = len(requests)
    assert run("SELECT decision_prob(NULL,'Q') AS v")[0]['v'] is None
    assert len(requests) == before
    rows = run("SELECT decision_prob('row ' || i,'Q') AS v FROM range(4) t(i)")
    assert len(rows) == 4 and len(requests) == before + 4
    assert run("SELECT decision_prob('retry','Q') AS v")[0]['v'] == .8 and attempts == 2
    run("SELECT decision_prob('refuse','Q')", fail='refused')
    run("SELECT decision_prob('invalid','Q')", fail='out of range')
    run("SELECT decision_prob('wrong-name','Q')", fail='identity mismatch')
    run("SELECT decision_choice('unknown','Q',['a','b'])", fail='unknown choice')
    run("SELECT decision_choice('x','Q',['a','a'])", fail='unique')
    run("SELECT decision_choice('x','Q',['a',NULL])", fail='NULL option')
    run("SELECT decision('x','Q',2)", fail='Threshold')
    before = len(requests)
    run("SELECT decision_prob('unauthorized','Q')", fail='HTTP 401')
    assert len(requests) == before + 1
    run("SET enable_external_access=false; SELECT decision_prob('x','Q')", fail='external access')
    questions = json.dumps([{'type':'predicate','name':'one','instructions':'Q'}, {'type':'predicate','name':'two','instructions':'Q2'}])
    rows = run(f"SELECT decision_answers('x','{questions}') AS v")
    assert len(json.loads(rows[0]['v'])) == 2
    rows = run(f"SELECT decision_answers('refuse','{questions}') AS v")
    assert json.loads(rows[0]['v'])[0]['type'] == 'refusal'
    run("SELECT decision_prob('x','Q')", prefix=f"LOAD '{EXT}';", fail='OPENAI_API_KEY')
    secret = run("SELECT secret_string FROM duckdb_secrets() WHERE type='decisions'")
    assert 'mock-key' not in secret[0]['secret_string']
    print('Passed 22 SQL/HTTP integration checks; no live API requests.')
finally:
    server.shutdown()
