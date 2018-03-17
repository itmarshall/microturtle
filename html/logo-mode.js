ace.define('ace/mode/logo', function(require, exports, module) {
    var oop = require("ace/lib/oop");
    var TextMode = require("ace/mode/text").Mode;
    var TextHighlightRules = require("ace/mode/text_highlight_rules").TextHighlightRules;

    var LogoHighlightRules = function() {
        this.$rules = {
            "start": [
                {token: "comment.line", regex: ";.*$"},
                {token: "constant.numeric", regex: "[+-]?\\d+\\b"},
                {token: "keyword.operator", regex: "/|\\*|\\-|\\+|!=|==|<=|>=|<|>"},
                {token: "keyword.operator", regex: "\\bor\\b|\\band\\b"},
                {token: "paren.lparen", regex: "[[(]"},
                {token: "paren.rparen", regex: "[\\])]"},
				{token: "keyword.control", regex: "if|else|repeat|return|stop"},
				{token: "keyword.other", regex: "to|end"},
				{token: "storage.type", regex: "make"},
				{token: "support.function", regex: "fd|forward|bk|back|lt|left|rt|right|pu|penup|pd|pendown"},
				{caseInsensitive : true }
            ]
        };
    };
    oop.inherits(LogoHighlightRules, TextHighlightRules);

    var LogoMode = function() {
        this.HighlightRules = LogoHighlightRules;
    };
    oop.inherits(LogoMode, TextMode);

    (function() {
        this.$id = "ace/mode/logo";

		var WorkerClient = require("ace/worker/worker_client").WorkerClient;
		this.createWorker = function(session) {
			this.$worker = new WorkerClient(["ace"], "ace/mode/logo_worker", "LogoWorker");
			this.$worker.attachToDocument(session.getDocument());

			this.$worker.on("annotate", function(e) {
				session.setAnnotations(e.data);
			});
			this.$worker.on("terminate", function() {
				session.clearAnnotations();
			});
			return this.$worker;
		};
    }).call(LogoMode.prototype);

    exports.Mode = LogoMode;
});
