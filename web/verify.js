/* web/verify.js — node smoke test for the WASM playground modules.
 *
 * Run after web/build.sh (CI does both). Compiles examples/triage.flow through
 * flowc.wasm and runs it through flowd.wasm, asserting the known outputs. Exits
 * non-zero on any mismatch, so a regression in the glue, stub host, or build
 * fails CI rather than silently shipping a broken playground.
 */
const fs = require('fs');
const path = require('path');
const ROOT = path.resolve(__dirname, '..');
const createFlowc = require('./flowc.js');
const createFlowd = require('./flowd.js');

let fails = 0;
function check(label, got, want) {
  const ok = got === want;
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}: ${JSON.stringify(got)}` +
              (ok ? '' : ` (want ${JSON.stringify(want)})`));
  if (!ok) fails++;
}

(async () => {
  const c = await createFlowc();
  const d = await createFlowd();

  const compile = s => {
    const p = c.ccall('flow_compile', 'number', ['string'], [s]);
    if (!p) return { err: c.ccall('flow_compile_error', 'string', [], []) };
    const ir = c.UTF8ToString(p); c._free(p); return { ir };
  };
  const run = (ir, inp) => {
    const p = d.ccall('flow_run', 'number', ['string', 'string', 'string'], [ir, inp, '/trace']);
    if (!p) return { err: d.ccall('flow_run_error', 'string', [], []) };
    const out = d.UTF8ToString(p); d._free(p); return { out };
  };

  const r = compile(fs.readFileSync(path.join(ROOT, 'examples/triage.flow'), 'utf8'));
  if (r.err) { console.log('compile failed:\n' + r.err); process.exit(1); }

  check('triage(750)', run(r.ir, '750').out, '2');
  check('triage(50)',  run(r.ir, '50').out,  '1');

  const e = compile('flow choose([int]) -> [int] {\n  it | select nope\n}\n');
  check('bad program reports E124', /E124/.test(e.err || ''), true);

  /* The JS-backed host (stages A + B): flow_tool_defaults gives a tool's
   * type-directed default, and flow_run_hosted lets globalThis.flowdServeTool
   * override it — so an edited model output drives the flow. */
  const hosted = (ir, inp) => {
    const p = d.ccall('flow_run_hosted', 'number', ['string', 'string', 'string'], [ir, inp, '']);
    if (!p) return { err: d.ccall('flow_run_error', 'string', [], []) };
    const out = d.UTF8ToString(p); d._free(p); return { out };
  };
  const h = compile(
    'type R = { score: int, flagged: bool }\n' +
    'tool risk_check(amount: int) -> R\n  effect model("risk-v1")\n' +
    'flow triage(int) -> int {\n  r = risk_check(it)\n' +
    '  if r.flagged then 3 else if r.score > 700 then 2 else 1\n}\n');
  if (h.err) { console.log('compile failed:\n' + h.err); process.exit(1); }

  const dp = d.ccall('flow_tool_defaults', 'number', ['string'], [h.ir]);
  const defaults = JSON.parse(d.UTF8ToString(dp)); d._free(dp);
  check('flow_tool_defaults(risk_check)',
        JSON.stringify(defaults.risk_check), '{"score":0,"flagged":false}');

  globalThis.flowdServeTool = () => null;                    // decline -> stub default
  check('hosted == stub when declining', hosted(h.ir, '750').out, '1');
  globalThis.flowdServeTool = (name) =>
    name === 'risk_check' ? '{"score":0,"flagged":true}' : null;
  check('edited model output drives the flow', hosted(h.ir, '750').out, '3');

  // A wrong-typed card output is caught by the runtime (the type backstop the
  // playground surfaces in the output panel), not silently accepted.
  globalThis.flowdServeTool = () => '{"score":"oops","flagged":false}';
  check('runtime rejects a wrong-typed tool output (R155)',
        /R155/.test(hosted(h.ir, '750').err || ''), true);

  // Stage C: model-versioned replay re-invokes the model with the edited card
  // and restores everything else from the recorded trace.
  globalThis.flowdServeTool = () => null;                    // baseline: default model
  d._free(d.ccall('flow_run_hosted', 'number', ['string', 'string', 'string'], [h.ir, '750', '/base']));
  const baseExec = '/base/triage/' + d.FS.readdir('/base/triage').find(n => n.startsWith('exec_'));
  globalThis.flowdServeTool = () => '{"score":0,"flagged":true}';   // a different answer
  const rp = d.ccall('flow_replay_hosted', 'number',
                     ['string', 'string', 'string', 'string', 'string'],
                     [h.ir, 'triage', baseExec, '/rep', 'risk-v1']);
  check('replay re-invokes the model with the edit', rp ? d.UTF8ToString(rp) : null, '3');
  if (rp) d._free(rp);

  // Stage D: a flow that calls await_human_approval suspends with a token;
  // resume injects a decision of the suspending tool's type and completes,
  // while a wrong-typed decision is rejected (R155).
  globalThis.flowdServeTool = () => null;
  const g = compile(
    'type Approval = { approver: string, ok: bool }\n' +
    'tool await_human_approval(prompt: string) -> Approval\n  effect mutation\n' +
    'flow gate(string) -> Approval {\n  await_human_approval(it)\n}\n');
  if (g.err) { console.log('gate compile failed:\n' + g.err); process.exit(1); }
  const resume = (ir, token, decision, dir) => {
    const p = d.ccall('flow_resume_hosted', 'number',
                      ['string', 'string', 'string', 'string'], [ir, token, decision, dir]);
    if (!p) return { err: d.ccall('flow_run_error', 'string', [], []) };
    const o = d.UTF8ToString(p); d._free(p); return { out: o };
  };
  d._free(d.ccall('flow_run_hosted', 'number', ['string', 'string', 'string'], [g.ir, '"approve?"', '/g']));
  const tok = d.ccall('flow_suspension_token', 'string', [], []);
  check('flow suspends with a token', /suspensions\//.test(tok), true);
  check('resume injects the human decision',
        resume(g.ir, tok, '{"approver":"ops","ok":true}', '/g').out, '{"approver":"ops","ok":true}');
  // wrong-typed decision is rejected — re-suspend a fresh run (the token above was consumed).
  d._free(d.ccall('flow_run_hosted', 'number', ['string', 'string', 'string'], [g.ir, '"approve?"', '/g2']));
  const tok2 = d.ccall('flow_suspension_token', 'string', [], []);
  check('resume rejects a wrong-typed decision (R155)',
        /R155/.test(resume(g.ir, tok2, '{"approver":"ops","ok":"yes"}', '/g2').err || ''), true);
  // The token-threading fix (flowd_resume): a resumed run that suspends AGAIN
  // hands back the next token — the chain extends — instead of erroring.
  const g2 = compile(
    'type Approval = { approver: string, ok: bool }\n' +
    'tool await_human_approval(prompt: string) -> Approval\n  effect mutation\n' +
    'flow gate2(string) -> Approval {\n  a = await_human_approval(it)\n  b = await_human_approval(it)\n  b\n}\n');
  if (g2.err) { console.log('gate2 compile failed:\n' + g2.err); process.exit(1); }
  d._free(d.ccall('flow_run_hosted', 'number', ['string', 'string', 'string'], [g2.ir, '"go"', '/g3']));
  resume(g2.ir, d.ccall('flow_suspension_token', 'string', [], []), '{"approver":"a","ok":true}', '/g3');
  check('a re-suspending resume surfaces the next token (chain extends)',
        /suspensions\//.test(d.ccall('flow_suspension_token', 'string', [], [])), true);
  globalThis.flowdServeTool = undefined;

  process.exit(fails ? 1 : 0);
})();
