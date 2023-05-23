chrome.webRequest.onBeforeRequest.addListener(function() {
    return {cancel: true};
  }, {
    urls: ["*://*.soundcloud.com/tsub/subscribe*"]
  }, ["blocking"]
);
