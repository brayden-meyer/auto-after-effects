const {google} = require('googleapis');
const http = require('http');
const url = require('url');
const opn = require('opn');
const destroyer = require('server-destroy');
const fs = require("fs");

const authClient = new google.auth.OAuth2(
  "-.apps.googleusercontent.com",
  "-",
  'http://localhost:3000/oauth2callback'
);

exports.authClient = authClient;

exports.auth = (scopes) => {
  return new Promise((resolve, reject) => {
    // Grab the URL that will be used for authorization
    let authorizeUrl = authClient.generateAuthUrl({
      access_type: 'offline',
      scope: scopes.join(' '),
    });

    const server = http.createServer(async (req, res) => {
      try {
        if (req.url.indexOf('/oauth2callback') > -1) {
          const qs = new url.URL(req.url, 'http://localhost:3000').searchParams;
          res.end('Authentication successful! Please return to the console.');
          server.destroy();
          const {tokens} = await authClient.getToken(qs.get('code'));
          authClient.credentials = tokens;

          resolve(authClient);
        }
      } catch (e) {
        reject(e);
      }
    }).listen(3000, () => {
      // Open the browser to the authorize URL to start the workflow
      opn(authorizeUrl, {wait: false}).then(cp => cp.unref());
    });

    destroyer(server);
  });
};

exports.upload = (auth, filePath, options = {}) => {
  const youtube = google.youtube({
    version: 'v3',
    auth: auth
  });

  return new Promise((resolve, reject) => {
    const res = youtube.videos.insert({
      part: 'id,snippet,status',
      notifySubscribers: options.notifySubscribers || true,
      requestBody: {
        snippet: {
          title: options.title || "",
          description: options.description || "",
          tags: options.tags || []
        },
        status: {
          privacyStatus: options.privacyStatus || "public",
          publishAt: options.publishAt.toISOString()
        },
      },
      media: {
        body: fs.createReadStream(filePath)
      }
    });

    resolve(res);
  });
};
