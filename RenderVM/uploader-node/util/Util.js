const fs = require("fs");
const readline = require("readline");
const {exec} = require("child_process");

exports.onFileExists = (path) => {
  return new Promise((resolve, reject) => {
    let exec = () => {
      if (fs.existsSync(path)) {
        resolve();

        clearInterval(interval);
      }
    };

    exec();
    let interval = setInterval(exec, 5000)
  });
};

exports.readLine = (question = "") => {
  const rl = readline.createInterface({
    input: process.stdin,
    output: process.stdout
  });

  return new Promise((resolve, reject) => {
    rl.question(question, resolve);
  });
};

exports.clear = () => {
  console.log("\033c");
};

exports.syncPreview = (inputPath, musicPath, previewStart, outputPath) => {
  return new Promise((resolve, reject) => {
    exec(`"%CD%\\ffmpeg.exe" -y -i "${inputPath}" -itsoffset -${previewStart} -i "${musicPath}" -af \"afade=t=out:st=25:d=5\" -map 0:v -map 1:a -shortest "${outputPath}"`, (err, stdout, stderr) => {
      if (err) {
        exports.log(err);

        resolve(err);
        return;
      }

      resolve(stderr);
    });
  });
};

exports.log = (message) => {
  fs.appendFileSync("log", `[${new Date().toUTCString()}] ${message}\n`);
};
