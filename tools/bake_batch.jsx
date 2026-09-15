// Bakes one frame for each queued project, reusing a single After Effects
// instance. Launched as: AfterFX.exe -noui -r bake_batch.jsx
//
// Contract with aepbake.exe:
//   in   %LOCALAPPDATA%\AepThumb\batch.json   {"outDir":..,"jobs":[{key,project,comp}]}
//   out  %LOCALAPPDATA%\AepThumb\batch_result.json
//   log  %LOCALAPPDATA%\AepThumb\bake_jsx.log
//
// Results are rewritten after every job, so if AE dies partway the worker
// still learns which projects succeeded.
//
// Two ordering rules here were learned the hard way:
//   - a render-settings template resets the time span, so the single-frame
//     range must be set after applyTemplate, or the whole comp renders;
//   - a sequence output module appends a frame number, so the path needs
//     [#####] before the extension or the final rename fails with error 784.

(function () {

// Must match AepCacheRoot() on the C++ side. Folder.userData is Roaming, so
// the environment variable is the only thing that lands in the same place.
var LOCAL     = $.getenv('LOCALAPPDATA') || (Folder.userData.fsName + '/../Local');
var ROOT      = LOCAL + '/AepThumb';
var JOB_FILE  = ROOT + '/batch.json';
var RES_FILE  = ROOT + '/batch_result.json';
var LOG_FILE  = ROOT + '/bake_jsx.log';

function log(msg) {
    try {
        var f = new File(LOG_FILE);
        f.open('a');
        f.writeln('[' + new Date().toTimeString().substr(0, 8) + '] ' + msg);
        f.close();
    } catch (e) {}
}

function readAll(path) {
    var f = new File(path);
    if (!f.exists) return null;
    f.open('r');
    var s = f.read();
    f.close();
    return s;
}

function esc(s) {
    return String(s).replace(/\\/g, '\\\\').replace(/"/g, '\\"')
                    .replace(/[\r\n]+/g, ' ');
}

var results = [];

function writeResults() {
    try {
        var parts = [];
        for (var i = 0; i < results.length; i++) {
            var r = results[i];
            parts.push('{"key":"' + esc(r.key) + '","ok":' + (r.ok ? 'true' : 'false') +
                       ',"file":"' + esc(r.file) + '","comp":"' + esc(r.comp) +
                       '","ms":' + r.ms + ',"error":"' + esc(r.error) + '"}');
        }
        var f = new File(RES_FILE);
        f.open('w');
        f.write('{"results":[' + parts.join(',') + ']}');
        f.close();
    } catch (e) {
        log('could not write results: ' + e.toString());
    }
}

function pickComp(wanted) {
    var items = app.project.items;
    var byName = null, biggest = null, bestArea = -1;
    for (var i = 1; i <= items.length; i++) {
        var it = items[i];
        if (!(it instanceof CompItem)) continue;
        if (wanted && it.name === wanted && !byName) byName = it;
        var area = it.width * it.height;
        if (area > bestArea) { bestArea = area; biggest = it; }
    }
    return byName || biggest;
}

// Offline footage is deliberately left alone. Replacing it with an AE
// placeholder does render, but the placeholder is colour bars, which swamp the
// tile and say nothing about the project. Left missing, the media slot simply
// comes out empty and the design around it - layout, type, shapes - still
// reads. Missing footage is not what stopped these renders anyway; dialog
// suppression was.
function countMissingFootage() {
    var missing = 0;
    for (var i = 1; i <= app.project.numItems; i++) {
        var it = app.project.item(i);
        if (it instanceof FootageItem && it.footageMissing) missing++;
    }
    return missing;
}

function frameTime(comp) {
    // The stored playhead is the author's own chosen frame; frame 0 is usually
    // still black, so fall back to a quarter of the way in.
    var t = comp.time;
    if (t <= 0) t = comp.duration * 0.25;
    var last = comp.duration - comp.frameDuration;
    if (t > last) t = last;
    if (t < 0) t = 0;
    return t;
}

function bake(job, outDir) {
    var started = new Date().getTime();
    var r = { key: job.key, ok: false, file: '', comp: '', ms: 0, error: '', note: '' };

    try {
        var pf = new File(job.project);
        if (!pf.exists) throw new Error('project missing');

        app.open(pf);

        var missing = countMissingFootage();
        if (missing > 0) log('  note: ' + missing + ' footage item(s) offline');

        var comp = pickComp(job.comp);
        if (!comp) throw new Error('project has no comps');
        r.comp = comp.name;

        var rq = app.project.renderQueue.items.add(comp);
        rq.applyTemplate('Draft Settings');          // half res, draft quality
        rq.timeSpanStart = frameTime(comp);
        rq.timeSpanDuration = comp.frameDuration;

        var om = rq.outputModule(1);
        om.applyTemplate('TIFF Sequence with Alpha');
        om.file = new File(outDir + '/' + job.key + '_[#####].tif');

        app.project.renderQueue.render();

        // A frame on disk is the only thing that matters. Projects with
        // missing effects or footage finish as ERR_STOPPED (3019) yet still
        // write the frame, so the status is logged, never trusted.
        var produced = new Folder(outDir).getFiles(job.key + '_*.tif');
        var status = 'unknown';
        try { status = String(rq.status); } catch (e) {}
        if (produced.length === 0)
            throw new Error('render wrote no file (queue status ' + status + ')');
        if (status !== '3020') r.note = 'queue status ' + status;
        r.file = produced[0].fsName;
        r.ok = true;
    } catch (e) {
        r.error = e.toString();
    }

    try {
        while (app.project && app.project.renderQueue.numItems > 0)
            app.project.renderQueue.item(1).remove();
    } catch (e) {}
    try {
        if (app.project) app.project.close(CloseOptions.DO_NOT_SAVE_CHANGES);
    } catch (e) {}

    r.ms = new Date().getTime() - started;
    log((r.ok ? 'OK   ' : 'FAIL ') + r.ms + 'ms  ' + job.project +
        (r.ok ? '  comp="' + r.comp + '"' : '  ' + r.error) +
        (r.note ? '  (' + r.note + ')' : ''));
    return r;
}

try {
    log('=== batch start, AE ' + app.version);

    var raw = readAll(JOB_FILE);
    if (!raw) { log('no job file at ' + JOB_FILE); app.quit(); return; }

    var batch = eval('(' + raw + ')');
    var jobs = batch.jobs || [];
    var outDir = batch.outDir;
    new Folder(outDir).create();
    log(jobs.length + ' job(s), outDir=' + outDir);

    // Deliberately NOT calling app.beginSuppressDialogs(). Measured on AE
    // 25.6: with suppression on, any project whose render would raise a
    // warning - a missing effect, offline footage - stops immediately as
    // USER_STOPPED and writes nothing. With suppression off the same project
    // finishes as ERR_STOPPED and the frame lands on disk. Running under
    // -noui is what keeps dialogs from blocking, and the baker kills After
    // Effects if a batch ever overruns.
    for (var i = 0; i < jobs.length; i++) {
        results.push(bake(jobs[i], outDir));
        writeResults();
    }

    log('=== batch done');
} catch (e) {
    log('FATAL: ' + e.toString() + ' line ' + e.line);
    writeResults();
}

app.quit();

})();
