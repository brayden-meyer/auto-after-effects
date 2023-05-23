var currentURL, updating, signature = " - ";

$(document).ready(function() {
  chrome.storage.local.get(['signature'], function(result) {
    if (result.signature == undefined)
      return;
    
    signature = result.signature;
    console.log('Retrieved signature as: ' + signature);
    
    $("#tch-messages-agent").val(signature);
  });
    
  function updatePage() {
    if (currentURL == location.href)
      return;
    
    updating = false;
    currentURL = location.href;
    
    //if (currentURL.match(/https?:\/\/soundcloud.com\/(?!you|charts|pages|messages|stations|mobile)([^\/]*)\/(?!sets|followers|following|likes|tracks|albums|reposts|comments|stats).*/))
      /*alert("Track URL.");
    if (currentURL.match(/^https?:\/\/soundcloud.com\/(?!stream|upload|go|pro)([^\/\n]*)$/))
      alert("Profile URL.");*/
  }
  
  updatePage();
  $(window).on("popstate", function(e) {
    updatePage();
  });
  
  $(document).on("click", "a", function(e) {
    updatePage();
  });
  
  $(document).on("dblclick", ".inboxItem", function(e) {
    $this = $(this);
    
    senderName = $.trim($($this.find(".conversationBadge__senderName")[0]).text());
    
    value = !($this.attr("data-tch-submission") == "true");
    chrome.storage.sync.get(['submissionMarked'], function(result) {
      if (result.submissionMarked == null)
        result.submissionMarked = {};
      
      if (value)
        result.submissionMarked[senderName] = true;
      else
        delete result.submissionMarked[senderName];
      
      chrome.storage.sync.set({submissionMarked: result.submissionMarked}, function() {
        console.log('Updated submissionMarked to: ' + result.submissionMarked);
        $this.attr("data-tch-submission", value);
      });
    });
  });
  
  $(document).on("change", "#tch-messages-signature", function(e) {
    signature = this.value;
    
    chrome.storage.local.set({signature: signature}, function() {
      console.log('Updated signature to: ' + signature);
    });
  });
  
  $(document).on("click", ".composeMessage__sendButton", function(e) {
    senderName = $.trim($($(".inboxItem.active .conversationBadge__senderName")[0]).text());
    
    value = false;
    chrome.storage.sync.get(['submissionMarked'], function(result) {
      if (result.submissionMarked == null)
        result.submissionMarked = {};
      
      if (value)
        result.submissionMarked[senderName] = true;
      else
        delete result.submissionMarked[senderName];
      
      chrome.storage.sync.set({submissionMarked: result.submissionMarked}, function() {
        console.log('Updated submissionMarked to: ' + result.submissionMarked);
        $this.attr("data-tch-submission", value);
      });
    });
  });
  
  $(document).on("click", ".tch-messages-template-button", function(e) {
    $this = $(this);
    template = $this.attr("data-tch-content");
    
    template = template.replace(/<again>/gi, $(".conversationMessages .conversationMessages__item").length > 1 ? ' again' : '');
    template = template.replace("<display>", $($(".conversationParticipant")[0]).text());
    template = template.replace("<signature>", signature.replace(/\\n/g, '\n'));
    
    input = $("textarea.textfield__input")[0];
    
    selStart = template.indexOf("[edit]");
    if (selStart == -1) {
      selStart = template.indexOf("|");
      if (selStart == -1)
        return;
      else {
        selEnd = selStart;
        template = template.replace("|", '');
      }
    } else
      selEnd = selStart + 6;
    
    //template += "\n\nThis message uses a template to help increase productivity. Although we may seem like robots, we can assure you we are not."
    
    input.style.height = "0px";
    input.value = template;
    input.style.height = (input.scrollHeight + 2) + "px";
    
    $(window).scrollTop($(window).scrollTop() + $(window).height());
    
    input.focus();
    input.setSelectionRange(selStart, selEnd);
  });
  
  $(document).on('DOMSubtreeModified', function() {
    if (updating == true)
      return;
    
    updating = true;
    
    chrome.storage.sync.get(['submissionMarked'], function(result) {
      if (result.submissionMarked == null)
        result.submissionMarked = {};
      
      $(".inboxItem:not(.tch-modified)").toArray().forEach(function(inboxItem) {
        $inboxItem = $(inboxItem);
        inboxName = $inboxItem.find(".conversationBadge__senderName");
      
        marked = result.submissionMarked[$.trim(inboxName.text())];
        $inboxItem.attr("data-tch-submission", marked ? "true" : "false");
        
        $inboxItem.addClass("tch-modified");
        
        if (marked)
          $inboxItem.parent().parent().prepend($inboxItem.parent());
      });
    });
    
    $('.composeMessage__toolbar').toArray().forEach(function(toolbar) {
      $toolbar = $(toolbar);
      
      if ($toolbar.hasClass("tch-modified"))
        return;
      
      $(".conversationActions__buttons").append('<input type="text" title="Signature" placeholder="Signature" value="' + signature + '" id="tch-messages-signature" class="sc-border-box">');
      $toolbar.append('<button data-tch-content="Hey<again> <display>,\n\nThanks for reaching out to us<again>! [edit]\n\n<signature>" type="button" class="tch-button tch-messages-template-button sc-button sc-button-medium" title="Template for a standard response.">Standard</button>');
      $toolbar.append('<button data-tch-content="Hey<again> <display>,\n\nThanks for reaching out to us<again>! We really liked this track. However, we will require verification of ownership.\n\nTypically, we request that you send in a version of the track without vocals. If this is not possible please send a comparable method of verification. |\n\n<signature>" type="button" class="tch-button tch-messages-template-button sc-button sc-button-medium" title="Template for a required verification response.">Request Verification</button>');
      $toolbar.append('<button data-tch-content="Hey<again> <display>,\n\nThanks for reaching out to us<again>! [edit]\n\nDue to this, it is unlikely we will feature this track.\n\n<signature>" type="button" class="tch-button tch-messages-template-button sc-button sc-button-medium" title="Template for a denied response.">Submission Denied</button>');
      $toolbar.append('<button data-tch-content="Hey<again> <display>,\n\nThanks for reaching out to us<again>! Your submission has been accepted and we plan on featuring it soon. |\n\n<signature>" type="button" class="tch-button tch-messages-template-button sc-button sc-button-medium" title="Template for a accepted response.">Submission Accepted</button>');
      
      $toolbar.addClass("tch-modified");
    });
    
    updating = false;
  });
});