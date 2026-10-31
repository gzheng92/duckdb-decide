"""Test the actual loadable extension using DuckDB CLI and a local HTTP mock."""
import json
import os
import pathlib
import shutil
import subprocess
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = pathlib.Path(__file__).resolve().parents[1]
EXT = ROOT / 'build/release/extension/decide/decide.duckdb_extension'
CLI = os.environ.get('DUCKDB_CLI') or shutil.which('duckdb') or str(ROOT / 'build/release/duckdb')
requests = []
clef_requests = []
jev_requests = []
attempts = 0

class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_systemone(self, token, log, wrap):
        """System One format, shared by Cloudflare Clef (wrapped in a REST envelope) and TypeSafe Jev."""
        assert self.headers['Authorization'] == f'Bearer {token}', self.headers['Authorization']
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        assert set(body) == {'model', 'state', 'questions'}
        log.append(body)
        state = body['state']
        if state == 'unauthorized':
            self.send_response(401); self.end_headers(); return
        evidence = None
        if state.startswith('{'):
            try:
                evidence = {row['id']: row['text'] for row in json.loads(state)['rows']}
            except (ValueError, KeyError, TypeError):
                pass
        answers = {}
        for name, q in body['questions'].items():
            record = evidence[name] if evidence else state
            n = int(record[4:]) if record.startswith('row ') else None
            if q['type'] == 'noul':
                answers[name] = {'type': 'noul', 'noul': (.9 if n % 2 == 0 else .1) if n is not None else .8}
            elif q['type'] == 'choice':
                options = list(q['criteria'])
                pick = 'unknown' if record == 'unknown' else options[n % len(options) if n is not None else 0]
                answers[name] = {'type': 'choice', 'choice': pick, 'probabilities': {o: 1 / len(options) for o in options}, 'confidence': .5}
            else:
                assert isinstance(q['criteria'], list)
                answers[name] = {'type': 'score', 'score': n % len(q['criteria']) if n is not None else 1.25, 'legend': {}, 'probabilities': {}, 'confidence': .5}
        reply = {'model': body['model'], 'answers': answers, 'usage': {'input_tokens': 1, 'output_tokens': 0}}
        if state == 'extra-answer':
            reply['answers']['surprise'] = {'type': 'noul', 'noul': .5}
        self.send_response(200); self.end_headers()
        # Workers AI REST wraps output in a result envelope; its binding and Jev return it bare.
        self.wfile.write(json.dumps({'success': True, 'result': reply} if wrap and state != 'bare' else reply).encode())

    def do_POST(self):
        global attempts
        if self.path == '/clef':
            return self.do_systemone('cf-token', clef_requests, wrap=True)
        if self.path == '/v1/systemone':
            return self.do_systemone('jev-key', jev_requests, wrap=False)
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
        evidence = None
        if text.startswith('{'):
            try:
                evidence = {row['id']: row['text'] for row in json.loads(text)['rows']}
            except (ValueError, KeyError, TypeError):
                pass
        answers = []
        for q in body['questions']:
            record = evidence[q['name']] if evidence else text
            if evidence:
                assert len(evidence) == len(body['questions'])
                assert f"id is {q['name']}." in q['instructions']
            row_number = int(record[4:]) if record.startswith('row ') else None
            a = {'name': q.get('name'), 'type': q['type']}
            if record == 'refuse':
                a['type'] = 'refusal'
            elif q['type'] == 'predicate':
                a['probability'] = 2 if record == 'invalid' else (.9 if row_number % 2 == 0 else .1) if row_number is not None else .8
            elif q['type'] == 'choice':
                a['choice'] = 'unknown' if record == 'unknown' else q['choices'][row_number % len(q['choices']) if row_number is not None else 0]['value']
            elif q['type'] == 'score':
                a['score'] = row_number % len(q['levels']) if row_number is not None else 1.25
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

clef_setup = f"""LOAD '{EXT}';
SET decisions_provider='cloudflare';
SET decisions_endpoint='http://127.0.0.1:{server.server_port}/clef';
CREATE SECRET (TYPE decisions, API_KEY 'cf-token', ACCOUNT_ID 'abc123');
"""

jev_setup = f"""LOAD '{EXT}';
SET decisions_provider='jev';
SET decisions_endpoint='http://127.0.0.1:{server.server_port}/v1/systemone';
CREATE SECRET (TYPE decisions, API_KEY 'jev-key');
"""

def run(sql, fail=None, prefix=setup, env=None):
    clean = {'OPENAI_API_KEY': '', 'CLOUDFLARE_API_TOKEN': '', 'CLOUDFLARE_ACCOUNT_ID': '', 'TYPESAFE_API_KEY': ''}
    p = subprocess.run([CLI, '-unsigned', '-json', '-c', prefix + sql], capture_output=True, text=True,
                       env={**os.environ, **clean, **(env or {})})
    if fail:
        assert p.returncode and fail in p.stderr, (sql, p.stderr)
        assert 'INTERNAL Error' not in p.stderr, (sql, p.stderr)
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
    assert [r['v'] for r in rows] == [.9, .1, .9, .1] and len(requests) == before + 1
    before = len(requests)
    rows = run("SELECT decision_prob('row ' || i,'Q') AS v FROM range(45) t(i)")
    assert [r['v'] for r in rows] == [.9 if i % 2 == 0 else .1 for i in range(45)]
    assert len(requests) == before + 3
    assert [len(r['questions']) for r in requests[before:]] == [20, 20, 5]
    before = len(requests)
    rows = run("SET decisions_batch_size=1; SELECT decision_prob('row ' || i,'Q') AS v FROM range(4) t(i)")
    assert len(requests) == before + 4 and [r['v'] for r in rows] == [.9, .1, .9, .1]
    before = len(requests)
    rows = run("SET decisions_batch_size=3; SELECT decision_prob(CASE WHEN i % 2=0 THEN NULL ELSE 'row ' || i END,'Q') AS v FROM range(11) t(i)")
    assert [r['v'] for r in rows] == [None if i % 2 == 0 else .1 for i in range(11)]
    assert len(requests) == before + 2
    rows = run("SELECT decision('row ' || i,'Q', CASE WHEN i % 2=0 THEN .95 ELSE .05 END) AS v FROM range(4) t(i)")
    assert [r['v'] for r in rows] == [False, True, False, True]
    before = len(requests)
    rows = run("SELECT decision_prob('row ' || i, 'Q' || i % 2) AS v FROM range(8) t(i)")
    assert [r['v'] for r in rows] == [.9 if i % 2 == 0 else .1 for i in range(8)]
    assert len(requests) == before + 2
    rows = run("SELECT decision_choice('row ' || i,'Q',['a','b']) AS v FROM range(6) t(i)")
    assert [r['v'] for r in rows] == ['a','b'] * 3
    rows = run("SELECT decision_score('row ' || i,'Q',['low','mid','high']) AS v FROM range(6) t(i)")
    assert [r['v'] for r in rows] == [0,1,2,0,1,2]
    before = len(requests)
    rows = run("SELECT decision_prob('row ' || i,'Q') AS v FROM range(2065) t(i)")
    assert [r['v'] for r in rows] == [.9 if i % 2 == 0 else .1 for i in range(2065)]
    assert len(requests) == before + 104  # 103 batches for 2048 rows, one for 17
    for size in [0,129]:
        before = len(requests)
        run(f"SET decisions_batch_size={size}; SELECT decision_prob('x','Q')", fail='between 1 and 128')
        assert len(requests) == before
    before = len(requests)
    run("SELECT decision_prob(x,'Q') FROM (VALUES ('yes'), ('refuse')) t(x)", fail='refused')
    assert len(requests) == before + 1
    before = len(requests)
    run("SELECT decision_prob(x,'Q') FROM (VALUES ('yes'), ('invalid')) t(x)", fail='out of range')
    assert len(requests) == before + 1
    before = len(requests)
    rows = run("SELECT decision_prob(repeat('x',3000000),'Q') AS v FROM range(6)")
    assert len(rows) == 6 and len(requests) == before + 2
    before = len(requests)
    run("SELECT decision_prob(repeat('x',18000000),'Q')", fail='16 MiB')
    assert len(requests) == before
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
    # Cloudflare Clef backend
    cf = lambda sql, **kw: run(sql, **{'prefix': clef_setup, **kw})
    assert cf("SELECT decision('yes','Is it true?') AS v")[0]['v'] is True
    assert cf("SELECT decision_prob('yes','Is it true?') AS v")[0]['v'] == .8
    assert cf("SELECT decision_prob('bare','Q') AS v")[0]['v'] == .8
    assert cf("SELECT decision_choice('yes','Route?', ['billing','other']) AS v")[0]['v'] == 'billing'
    assert cf("SELECT decision_score('yes','Severity?', ['low','medium','high']) AS v")[0]['v'] == 1.25
    assert clef_requests[-1]['questions']['decision']['criteria'] == ['low', 'medium', 'high']
    before = len(clef_requests)
    rows = cf("SELECT decision_prob('row ' || i,'Q') AS v FROM range(150) t(i)")
    assert [r['v'] for r in rows] == [.9 if i % 2 == 0 else .1 for i in range(150)]
    assert [len(r['questions']) for r in clef_requests[before:]] == [20] * 7 + [10]
    before = len(clef_requests)
    cf("SET decisions_batch_size=128; SELECT decision_prob('row ' || i,'Q') AS v FROM range(130) t(i)")
    assert [len(r['questions']) for r in clef_requests[before:]] == [64, 64, 2]
    rows = cf("SET decisions_model='clef-flash'; SELECT decision_prob('yes','Q') AS v")
    assert clef_requests[-1]['model'] == 'clef-flash'
    questions = json.dumps([{'type':'predicate','name':'one','instructions':'Q'},
                            {'type':'choice','name':'two','instructions':'Q2','choices':[{'value':'a','description':'A'},{'value':'b','description':'B'}]}])
    answers = json.loads(cf(f"SELECT decision_answers('x','{questions}') AS v")[0]['v'])
    assert [a['type'] for a in answers] == ['predicate', 'choice'] and answers[0]['probability'] == .8
    assert clef_requests[-1]['questions']['two']['criteria'] == {'a': 'A', 'b': 'B'}
    bad = json.dumps([{'type':'predicate','name':'has space','instructions':'Q'}])
    cf(f"SELECT decision_answers('x','{bad}')", fail='may use only letters')
    cf("SELECT decision_choice('unknown','Q',['a','b'])", fail='unknown choice')
    cf("SELECT decision_prob('unauthorized','Q')", fail='Cloudflare Clef HTTP 401')
    cf("SET decisions_model='gpt'; SELECT decision_prob('x','Q')", fail='clef-flash')
    cf("SELECT decision_prob('x','Q')", prefix=f"LOAD '{EXT}'; SET decisions_provider='cloudflare';", fail='CLOUDFLARE_API_TOKEN')
    cf("SELECT decision_prob('x','Q')", prefix=f"LOAD '{EXT}'; SET decisions_provider='cloudflare'; CREATE SECRET (TYPE decisions, API_KEY 'k');", fail='account_id')
    cf("SELECT decision_prob('x','Q')", prefix=clef_setup.replace('abc123', 'a/../b'), fail='Invalid account_id for Cloudflare Clef')
    cf("SET decisions_provider='other'; SELECT decision_prob('x','Q')", fail="Unknown decisions_provider 'other'; available: cloudflare, jev, openai")
    secret = cf("SELECT secret_string FROM duckdb_secrets() WHERE type='decisions'")
    assert 'cf-token' not in secret[0]['secret_string']
    # TypeSafe Jev: same System One format as Clef, from providers/jev.json alone
    jev = lambda sql, **kw: run(sql, **{'prefix': jev_setup, **kw})
    assert jev("SELECT decision('yes','Is it true?') AS v")[0]['v'] is True
    assert jev("SELECT decision_prob('yes','Is it true?') AS v")[0]['v'] == .8
    assert jev_requests[-1]['model'] == 'jev-latest' and jev_requests[-1]['questions']['decision']['type'] == 'noul'
    assert jev("SELECT decision_choice('yes','Route?', ['billing','other']) AS v")[0]['v'] == 'billing'
    assert jev_requests[-1]['questions']['decision']['criteria'] == {'billing': 'billing', 'other': 'other'}
    assert jev("SELECT decision_score('yes','Severity?', ['low','medium','high']) AS v")[0]['v'] == 1.25
    before = len(jev_requests)
    rows = jev("SET decisions_batch_size=128; SELECT decision_prob('row ' || i,'Q') AS v FROM range(130) t(i)")
    assert [r['v'] for r in rows] == [.9 if i % 2 == 0 else .1 for i in range(130)]
    assert [len(r['questions']) for r in jev_requests[before:]] == [128, 2]  # no provider cap, unlike Clef
    jev("SET decisions_model='jev-1.14.0'; SELECT decision_prob('yes','Q') AS v")
    assert jev_requests[-1]['model'] == 'jev-1.14.0'
    spaced = json.dumps([{'type':'predicate','name':'has space','instructions':'Q'}])
    assert json.loads(jev(f"SELECT decision_answers('x','{spaced}') AS v")[0]['v'])[0]['name'] == 'has space'
    jev("SELECT decision_choice('unknown','Q',['a','b'])", fail='unknown choice')
    proto = json.dumps([{'type':'predicate','name':'__proto__','instructions':'Q'}])
    jev(f"SELECT decision_answers('x','{proto}')", fail="'__proto__' is reserved")
    # Without a key nothing is sent; the error shows whether the endpoint itself was accepted.
    no_key = f"LOAD '{EXT}'; SET decisions_endpoint='%s'; "
    run("SELECT decision_prob('x','Q')", prefix=no_key % 'https://api.openai.com/v1/decisions', fail='set OPENAI_API_KEY')
    run("SELECT decision_prob('x','Q')", prefix=no_key % 'https://evil.example/v1', fail='loopback mock server')
    jev("SELECT decision_prob('extra-answer','Q')", fail='answer count mismatch')
    jev("SELECT decision_prob('unauthorized','Q')", fail='TypeSafe Jev HTTP 401')
    jev("SET decisions_model='../admin'; SELECT decision_prob('x','Q')", fail="Invalid decisions_model '../admin'")
    jev("SELECT decision_prob('x','Q')", prefix=f"LOAD '{EXT}'; SET decisions_provider='jev';", fail='TYPESAFE_API_KEY')
    env_only = jev_setup.replace("CREATE SECRET (TYPE decisions, API_KEY 'jev-key');", '')
    assert jev("SELECT decision_prob('yes','Q') AS v", prefix=env_only, env={'TYPESAFE_API_KEY': 'jev-key'})[0]['v'] == .8
    # A secret scoped to one provider is used for it instead of the all-provider secret.
    scoped = f"""LOAD '{EXT}';
CREATE SECRET all_providers (TYPE decisions, API_KEY 'mock-key');
CREATE SECRET only_jev (TYPE decisions, API_KEY 'jev-key', SCOPE 'decisions://jev');
"""
    assert run("SELECT decision_prob('yes','Q') AS v", prefix=scoped + setup.split('\n')[1] + '\n')[0]['v'] == .8
    assert run("SELECT decision_prob('yes','Q') AS v", prefix=scoped + "SET decisions_provider='jev';\n" + jev_setup.split('\n')[2] + '\n')[0]['v'] == .8
    providers = run("SELECT name, format, default_model, key_env, array_to_string(secret_keys, ',') AS secret_keys, max_questions::INTEGER AS max_questions FROM decision_providers() ORDER BY name")
    assert [(p['name'], p['format'], p['default_model'], p['max_questions']) for p in providers] == [
        ('cloudflare', 'systemone', 'clef', 64), ('jev', 'systemone', 'jev-latest', None), ('openai', 'openai', 'gpt-6-luna', None)]
    assert providers[0]['secret_keys'] == 'api_key,account_id' and providers[1]['key_env'] == 'TYPESAFE_API_KEY'
    print('Passed SQL/HTTP integration checks, including batching, NULL mapping, chunk boundaries, payload splitting, and failures; no live API requests.')
finally:
    server.shutdown()
