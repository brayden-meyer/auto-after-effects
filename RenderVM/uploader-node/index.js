const fetch = require("node-fetch");
const fs = require("fs");
const readline = require("readline");
const util = require("./util/Util");
const youTubeUtil = require("./util/YouTubeUtil");
const FormData = require("form-data");

process.on('unhandledRejection', (reason, p) => {
  util.log(reason + p);
}).on('uncaughtException', err => {
  util.log(err);
  process.exit(1);
});

(async () => {
  const projectFilesDirectoryPath = `${process.env.USERPROFILE}\\Documents\\TC\\Project Files\\`;
  const testEnvironmentDirectoryPath = "./test_environment/";

  const renderMP4Path = `${process.env.DEBUG ? testEnvironmentDirectoryPath : projectFilesDirectoryPath}Render.mp4`;
  const previewMP4Path = `${process.env.DEBUG ? testEnvironmentDirectoryPath : projectFilesDirectoryPath}Preview.mp4`;
  const musicMP3Path = `${process.env.DEBUG ? testEnvironmentDirectoryPath : projectFilesDirectoryPath}-music.mp3`;
  const syncedPreviewMP4Path = "./out.mp4";
  const credentialsPath = "./credentials";
  const finalRenderDirectoryPath = `${process.env.DEBUG ? testEnvironmentDirectoryPath : `${process.env.USERPROFILE}\\Documents\\TC\\Videos\\`}`;

  util.log(await util.syncPreview(previewMP4Path, musicMP3Path, 48, syncedPreviewMP4Path));

  // Get valid authentication instance by requesting permission
  let auth;
  if (fs.existsSync(credentialsPath)) {
    youTubeUtil.authClient.credentials = JSON.parse(fs.readFileSync(credentialsPath).toString());

    auth = youTubeUtil.authClient;
  } else
    auth = await youTubeUtil.auth(['https://www.googleapis.com/auth/youtube.upload', 'https://www.googleapis.com/auth/youtube']);

  // Store credentials for use in new instance
  fs.writeFileSync(credentialsPath, JSON.stringify(auth.credentials));

  // Continually wait until file at path exists
  while (true) {
    await util.onFileExists(renderMP4Path);

    // Await input from client
    util.clear();
    await util.readLine("READY");
    util.clear();
    console.log("UPLOADING");

    // Grab meta data for video
    let json = await fetch("https://panel.tc.com/render/php/alive.php?status=META", {
      headers: {
        "Cookie": "vc=-"
      }
    }).then(response => response.json()).catch(util.log);

    if (!json.rendering) {
      util.log("No render ongoing. Exiting!");

      process.exit();
    }

    // Title = json.titleValue
    // Tags[] = json.tags
    // Description = json.description

    let date = new Date();
    if (date.getUTCHours() > 17) {
      date.setDate(date.getDate() + 1);
    }

    date.setUTCHours(17);
    date.setUTCMinutes(0);
    date.setUTCSeconds(0);
    date.setUTCMilliseconds(0);
    util.log(await youTubeUtil.upload(auth, renderMP4Path, {
      title: json.titleValue,
      description: json.description,
      tags: json.tags,
      privacyStatus: "private",
      publishAt: date
    }));

    // ffmpeg -y -i <input_path> -itsoffset -<preview_start> -i music.mp3 -af \"afade=t=out:st=25:d=5\" -map 0:v -map 1:a -shortest <output_path>
    // Use ffmpeg to resync preview
    util.log(await util.syncPreview(previewMP4Path, musicMP3Path, json.buildup, syncedPreviewMP4Path));

    // Upload out.mp4
    let formData = new FormData();
    formData.append("upload", fs.createReadStream(syncedPreviewMP4Path));
    util.log(await fetch("https://panel.tc.com/render/php/upload.php", {
      method: 'POST',
      body: formData,
      headers: {
        "Cookie": "vc=-"
      }
    }).then(response => response.text())).error(err => {
    });

    // Move the render and delete the preview
    fs.renameSync(renderMP4Path, `${finalRenderDirectoryPath}${json.titleValue}.mp4`);
    fs.unlinkSync(previewMP4Path);
    fs.unlinkSync(syncedPreviewMP4Path);

    util.clear();
    const rl = readline.createInterface({
      input: process.stdin,
      output: process.stdout
    });

    await new Promise((resolve, reject) => {
      let doneTimeout = setTimeout(() => {
        resolve();
      }, 10000);

      rl.question("DONE", resolve);
      clearTimeout(doneTimeout);
    });

    util.clear();
  }
})();
