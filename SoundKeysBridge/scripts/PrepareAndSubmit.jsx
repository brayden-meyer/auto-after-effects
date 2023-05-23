/* ExtendScript for AE 15/16. Launched through AfterFX.exe -r by Invoke-Job.ps1.
 * Configuration is a UTF-8 key=value file in SOUNDKEYS_BRIDGE_PREP_CONFIG.
 */
(function () {
    function fail(s) { throw new Error(s); }
    function readFields(file) {
        file.encoding = "UTF-8";
        if (!file.open("r")) fail("Cannot read configuration: " + file.fsName);
        var text;
        try { text = file.read(); } finally { file.close(); }
        var lines = text.replace(/^\uFEFF/, "").split(/\r?\n/), out = {}, i, line, p, k;
        for (i = 0; i < lines.length; ++i) {
            line = lines[i].replace(/^\s+|\s+$/g, "");
            if (!line || line.charAt(0) === "#") continue;
            p = line.indexOf("="); if (p < 1) fail("Bad configuration line");
            k = line.substring(0, p).replace(/\s+$/g, "");
            if (out[k] !== undefined) fail("Duplicate configuration field: " + k);
            out[k] = line.substring(p + 1).replace(/^\s+|\s+$/g, "");
        }
        return out;
    }
    function required(c, k) { if (!c[k]) fail("Missing configuration field: " + k); return c[k]; }
    function numeric(c, k) { var n = Number(required(c, k)); if (!isFinite(n)) fail("Invalid number: " + k); return n; }
    function oneItem(name, cls) {
        var found = null, i, item;
        for (i = 1; i <= app.project.numItems; ++i) {
            item = app.project.item(i);
            if (item instanceof cls && item.name === name) {
                if (found) fail("Ambiguous item: " + name);
                found = item;
            }
        }
        if (!found) fail("Missing item: " + name); return found;
    }
    function oneProperty(effect, name) {
        var found = null, i, p;
        for (i = 1; i <= effect.numProperties; ++i) {
            p = effect.property(i);
            if (p.name === name) { if (found) fail("Ambiguous property: " + name); found = p; }
        }
        if (!found) fail("Missing English property name: " + name); return found;
    }
    function writeNew(file, text) {
        if (file.exists) fail("Refusing to overwrite: " + file.fsName);
        file.encoding = "UTF-8"; file.lineFeed = "Unix";
        if (!file.open("w")) fail("Cannot create: " + file.fsName);
        try { if (!file.write(text)) fail("Write failed: " + file.fsName); }
        finally { file.close(); }
    }
    var configPath = $.getenv("SOUNDKEYS_BRIDGE_PREP_CONFIG"), c = null, prepError = null;
    try {
        if (!configPath) fail("SOUNDKEYS_BRIDGE_PREP_CONFIG is not set");
        c = readFields(new File(configPath));
        var id = required(c, "job_id");
        if (!/^[A-Za-z0-9_-]{1,64}$/.test(id)) fail("Invalid job ID");
        prepError = new File(required(c, "queue") + "/" + id + ".prep-error.txt");
        // Project.dirty is newer than AE 15/16. Do not depend on it here.
        // The native bridge uses the era-appropriate AEGP_ProjectIsDirty call.
        if (app.project && (app.project.file !== null || app.project.numItems !== 0))
            fail("Start with a fresh empty project in a dedicated AfterFX process");
        var template = new File(required(c, "template")), audio = new File(required(c, "audio"));
        var prepared = new File(required(c, "prepared")), output = new File(required(c, "output"));
        var profileFile = new File(required(c, "profile")), profile = readFields(profileFile);
        if (!template.exists || !audio.exists) fail("Template or audio does not exist");
        if (prepared.exists || output.exists) fail("Prepared/output path must be new");
        if (!prepared.parent.exists || !output.parent.exists) fail("Project parent directories must exist");
        var target = new File(required(c, "queue") + "/" + id + ".request");
        var temp = new File(required(c, "queue") + "/" + id + ".publishing");
        if (target.exists || temp.exists) fail("Job already submitted");
        if (!app.open(template)) fail("Cannot open template");
        var footage = oneItem(required(c, "audio_item"), FootageItem);
        footage.replace(audio);
        var comp = oneItem(required(c, "comp"), CompItem), layer = null, i;
        for (i = 1; i <= comp.numLayers; ++i) {
            if (comp.layer(i).name === required(c, "soundkeys_layer")) {
                if (layer) fail("Ambiguous Sound Keys layer"); layer = comp.layer(i);
            }
        }
        if (!layer) fail("Sound Keys layer not found");
        var effects = layer.property("ADBE Effect Parade"), effectIndex = numeric(c, "effect_index");
        if (effectIndex < 1 || effectIndex !== Math.floor(effectIndex) || effectIndex > effects.numProperties) fail("Invalid effect index");
        var effect = effects.property(effectIndex);
        if (effect.matchName !== required(profile, "effect_match")) fail("Wrong effect match name");
        var audioLayer = null;
        for (i = 1; i <= comp.numLayers; ++i) {
            if (comp.layer(i).source === footage) {
                if (audioLayer) fail("Multiple layers use the audio placeholder"); audioLayer = comp.layer(i);
            }
        }
        if (!audioLayer) fail("No layer in target comp uses the audio placeholder");
        oneProperty(effect, "Audio Layer").setValue(audioLayer.index);
        var start = numeric(c, "work_start"), duration = numeric(c, "work_duration");
        if (start < 0 || duration <= 0 || start + duration > comp.duration + 0.000001) fail("Work area is outside the composition");
        comp.workAreaStart = start; comp.workAreaDuration = duration;
        // Fresh output requirement prevents a no-op from passing on stale keys.
        // Changes are made only in memory and subsequently saved to a new file.
        var outputs = required(profile, "outputs").split(","), seen = {}, n, prop;
        for (i = 0; i < outputs.length; ++i) {
            n = Number(outputs[i]);
            if (n < 1 || n > 3 || n !== Math.floor(n) || seen[n]) fail("Invalid outputs list"); seen[n] = true;
            prop = oneProperty(effect, "Output " + n);
            if (prop.expressionEnabled) fail("Output " + n + " has an enabled expression");
            while (prop.numKeys > 0) prop.removeKey(prop.numKeys);
        }
        app.project.save(prepared);
        if (!prepared.exists) fail("Prepared project was not saved");
        var fields = ["protocol=1", "job_id=" + id, "input_project=" + prepared.fsName,
            "output_project=" + output.fsName, "profile=" + profileFile.fsName,
            "comp_id=" + comp.id, "layer_index=" + layer.index,
            "layer_name=" + layer.name, "effect_index=" + effectIndex];
        for (i = 0; i < fields.length; ++i) if (/[\r\n]/.test(fields[i])) fail("Newline in protocol value");
        writeNew(temp, fields.join("\n") + "\n");
        if (!temp.rename(target.name)) fail("Cannot atomically publish job request");
        // Return immediately. AE's idle hook cannot run inside a blocking JSX wait.
    } catch (e) {
        var message = String(e) + "\nLine: " + e.line + "\n";
        $.writeln("SoundKeysBridge preparation failed: " + message);
        if (prepError) { try { writeNew(prepError, message); } catch (ignored) {} }
        // Avoid an unattended modal error dialog. The external controller reads
        // the error file, or times out if configuration failed before ID resolution.
    }
}());
