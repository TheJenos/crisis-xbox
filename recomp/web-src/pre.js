// Included at the start of crysis2.js (build_web.sh --pre-js), so it runs in
// the page and in every game thread's worker.

// Longer stack traces: a crash in the game's code then shows the whole chain of
// recompiled functions (sub_XXXXXXXX), not just the top 10.
Error.stackTraceLimit = 100;

// A crash in a game thread reaches the page only as a one-line message. Send the
// thread's full stack to the page's log first, so the crash report has it.
if (globalThis.WorkerGlobalScope && globalThis.name == 'em-pthread') {
  self.addEventListener('error', (e) => {
    const stack = e.error && e.error.stack;
    if (stack) postMessage({ cmd: CMD_CALL_HANDLER, handler: 'printErr', args: ['[crash] game thread stack: ' + stack] });
  });
}
