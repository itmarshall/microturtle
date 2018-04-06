<html>
<head>
	<title>MicroTurtle Calibration</title>
	<meta charset="utf-8">
	<link rel="stylesheet" type="text/css" href="/style.css"/>
	<script type="text/javascript">

function toggleVisible(elem) {
	if (elem.style.display === "none") {
		elem.style.display = "block";
	} else {
		elem.style.display = "none";
	}
}

function saveValues() {
	var straightStepsLeft = document.getElementById("left-straight").value;
	var straightStepsRight = document.getElementById("right-straight").value;
	var turnStepsLeft = document.getElementById("left-turn").value;
	var turnStepsRight = document.getElementById("right-turn").value;
	var struct = {"configuration": { 
		"straightStepsLeft": straightStepsLeft,
		"straightStepsRight": straightStepsRight,
		"turnStepsLeft": turnStepsLeft,
		"turnStepsRight": turnStepsRight}};
	var xhr = new XMLHttpRequest();
	xhr.open('POST', '/setConfiguration.cgi');
	xhr.onreadystatechange = function() {
		if (xhr.readyState == XMLHttpRequest.DONE) {
			if ((xhr.status >= 200) && (xhr.status < 300)) {
				// Successful return.
				document.getElementById("unsaved").style.display = "none";
			} else {
				alert("Unable to save the calibration values, error " +
						xhr.status + ": " + xhr.statusText);
			}
		}
	};
	xhr.send("configuration=" + JSON.stringify(struct));
}

function drawLine() {
	var straightStepsLeft = document.getElementById("left-straight").value;
	var straightStepsRight = document.getElementById("right-straight").value;
	var xhr = new XMLHttpRequest();
	xhr.open('POST', '/calibrate/drawLine.cgi');
	xhr.onreadystatechange = function() {
		if (xhr.readyState == XMLHttpRequest.DONE) {
			var box = document.getElementById("straight-entry");
			if ((xhr.status >= 200) && (xhr.status < 300)) {
				// Successful return.
				box.style.display = "block";
			} else {
				box.style.display = "none";
				alert("Unable to run calibration, error " +
						xhr.status + ": " + xhr.statusText);
			}
		}
	};
	xhr.send("left=" + straightStepsLeft + "&right=" + straightStepsRight);
}

function drawTurn() {
	var straightStepsLeft = document.getElementById("left-straight").value;
	var straightStepsRight = document.getElementById("right-straight").value;
	var turnStepsLeft = document.getElementById("left-turn").value;
	var turnStepsRight = document.getElementById("right-turn").value;
	var xhr = new XMLHttpRequest();
	xhr.open('POST', '/calibrate/drawTurn.cgi');
	xhr.onreadystatechange = function() {
		if (xhr.readyState == XMLHttpRequest.DONE) {
			var box = document.getElementById("turn-entry");
			if ((xhr.status >= 200) && (xhr.status < 300)) {
				// Successful return.
				box.style.display = "block";
			} else {
				box.style.display = "none";
				alert("Unable to run calibration, error " +
						xhr.status + ": " + xhr.statusText);
			}
		}
	};
	xhr.send("left=" + turnStepsLeft +
			 "&right=" + turnStepsRight + 
			 "&leftStraight=" + straightStepsLeft +
			 "&rightStraight=" + straightStepsRight);
}

function hideStraight() {
	document.getElementById("straight-entry").style.display = "none";
}

function hideTurn() {
	document.getElementById("turn-entry").style.display = "none";
}

function showUnsaved() {
	document.getElementById("unsaved").style.display = "block";
}

function updateStraightValues() {
	// Get values and references needed for the calculations.
	var len = document.getElementById("length").value;
	var curve = document.getElementById("curve").value;
	var leftSide = document.querySelector('input[name="curve-lr"]:checked').value === "left";
	var left = document.getElementById("left-straight");
	var right = document.getElementById("right-straight");

	// First straighten up the left/right balance.
	if (leftSide) {
		var lv = parseInt(left.value);
		left.value = lv + ((lv * curve) / len);
	} else {
		var rv = parseInt(right.value);
		right.value = rv + ((rv * curve) / len);
	}

	// Now scale both motors to get the right length.
	var scale = 100.0 / len;
	left.value = Math.round(parseInt(left.value) * scale);
	right.value = Math.round(parseInt(right.value) * scale);

	showUnsaved();
}

function updateTurnValues() {
	// Get values and references needed for the calculations.
	var dist = document.getElementById("dist").value;
	var leftSide = document.querySelector('input[name="turn-lr"]:checked').value === "left";
	var left = document.getElementById("left-turn");
	var right = document.getElementById("right-turn");

	// Calculate the angle we actually turned.
	var angle = Math.asin(parseInt(dist) / 50.0) * 180 / Math.PI;
	if (leftSide) {
		angle += 180.0;
	} else {
		angle = 180.0 - angle;
	}

	// Scale the motors to correct for any errant turning.
	var scale = 180.0 / angle;
	left.value = Math.round(parseInt(left.value) * scale);
	right.value = Math.round(parseInt(right.value) * scale);

	showUnsaved();
}

/*
 * Initialises the page upon initial load.
 */
document.addEventListener("DOMContentLoaded", function() {
	document.getElementById("straight-header").addEventListener("click", function() {
		var contents = document.getElementById("straight-contents");
		toggleVisible(contents);
		if (contents.style.display === "block") {
			document.getElementById("arrow-straight").className = "arrow-down";
		} else {
			document.getElementById("arrow-straight").className = "arrow-right";
		}
	});
	document.getElementById("turn-header").addEventListener("click", function() {
		var contents = document.getElementById("turn-contents");
		toggleVisible(contents);
		if (contents.style.display === "block") {
			document.getElementById("arrow-turn").className = "arrow-down";
		} else {
			document.getElementById("arrow-turn").className = "arrow-right";
		}
	});

	var straightStepsLeft = "%straightStepsLeft%";
	var straightStepsRight = "%straightStepsRight%";
	var turnStepsLeft = "%turnStepsLeft%";
	var turnStepsRight = "%turnStepsRight%";
	if (straightStepsLeft.startsWith("%")) {
		straightStepsLeft = 1728;
	}
	if (straightStepsRight.startsWith("%")) {
		straightStepsRight = 1730;
	}
	if (turnStepsLeft.startsWith("%")) {
		turnStepsLeft = 2051;
	}
	if (turnStepsRight.startsWith("%")) {
		turnStepsRight = 2053;
	}
	document.getElementById("left-straight").value = straightStepsLeft;
	document.getElementById("right-straight").value = straightStepsRight;
	document.getElementById("left-turn").value = turnStepsLeft;
	document.getElementById("right-turn").value = turnStepsRight;
	document.getElementById("left-straight").addEventListener("change", showUnsaved);
	document.getElementById("right-straight").addEventListener("change", showUnsaved);
	document.getElementById("left-turn").addEventListener("change", showUnsaved);
	document.getElementById("right-turn").addEventListener("change", showUnsaved);
});

	</script>
</head>
<body>
</body>
	<div class="banner">
		MicroTurtle Calibration
	</div>

	<h2 id="straight-header"><div id="arrow-straight" class="arrow-right">&nbsp;</div>Straight Lines</h2>
	<div id="straight-contents" style="display: none;">
		<p>
		Calibration of straight lines allows the turtle's forwards and backwards 
		movements to move the turtle the correct amount. The calibration value for 
		straight lines is the number of "steps" the motors must take to move the
		turtle forwards by 100mm.
		</p>
		<p>
		To start the calibration, click on the "Draw Line" button, and the turtle will attempt
		to draw a 100mm straight line with the pen down.
		</p>
		<a class="button" href="javascript:drawLine()">Draw Line</a>
		<div id="straight-entry" class="question-box" style="display: none;">
			<div class="close-x"><a class="close-x" href="javascript:hideStraight()">&times;</a></div>
			<form id="straight-form">
				<label for="length">
					1. Measure the vertical length of the line (ignore any curve)
				</label>
				<br>
				<input type="number" id="length" min="0" step="0.01" placeholder="Length in mm">
				<br>
				<br>
				<label for="curve">
					2. Measure how far to the left or right of vertical the end of the line is
				</label>
				<br>
				<input type="number" id="curve" min="0" step="0.01" placeholder="Distance in mm">
				<input type="radio" name="curve-lr" id="curve-lr-left" value="left" checked>
				<label for="curve-lr-left">Left</label>
				<input type="radio" name="curve-lr" id="curve-lr-right" value="right">
				<label for="curve-lr-right">Right</label>
			</form>
			<a class="button" href="javascript:updateStraightValues()">Calculate</a>
		</div>
	</div>
	<h2 id="turn-header"><div id="arrow-turn" class="arrow-right">&nbsp;</div>Turning Angle</h2>
	<div id="turn-contents" style="display: none;">
		<p>
		Calibration of turning angles allows the turtle's left and right turns to
		turn the turtle the correct amount. The calibration value for turning angles
		is the number of "steps" the motors must take to turn the turtle by 180°.
		</p>
		<p>
		To start the calibration, click on the "Test Turn" button, and the turtle will
		perform the following steps with the pen down:
		<ul>
			<li>Move forwards by 100mm</li>
			<li>Turn clockwise by 180°</li>
			<li>Move forwards by 50mm</li>
		</ul>
		</p>
		<a class="button" href="javascript:drawTurn()">Test Turn</a>
		<div id="turn-entry" class="question-box" style="display: none;">
			<div class="close-x"><a class="close-x" href="javascript:hideTurn()">&times;</a></div>
			<form id="turn-form">
				<label for="dist">
					Measure the horizontal distance between the vertial line and the 180° line.
				</label>
				<br>
				<input type="number" id="dist" min="0" step="0.01" placeholder="Distance in mm">
				<br>
				Is the 180° line to the left or right of the vertical line?
				<br>
				<input type="radio" name="turn-lr" id="turn-lr-left" value="left" checked>
				<label for="turn-lr-left">Left</label>
				<input type="radio" name="turn-lr" id="turn-lr-right" value="right">
				<label for="turn-lr-right">Right</label>
			</form>
			<a class="button" href="javascript:updateTurnValues()">Calculate</a>
		</div>
		<br>
		<br>
	</div>
	The current values to be used for the calibration tests are:
	<table>
		<tr><th>&nbsp;</th><th>Left Motor</th><th>Right Motor</th></tr>
		<tr><th>100mm Straight</th>
			<td><input type="number" id="left-straight" min="0" step="1" value="4104"></td>
			<td><input type="number" id="right-straight" min="0" step="1" value="4104"></td>
		</tr>
		<tr><th>180° Turn</th>
			<td><input type="number" id="left-turn" min="0" step="1" value="4104"></td>
			<td><input type="number" id="right-turn" min="0" step="1" value="4104"></td>
		</tr>
	</table>
	<div id="unsaved" class="warning" style="display: none;">
		The calibration values have not been saved.
	</div>
	<br>
	<a class="button" href="javascript:saveValues()">Save Values</a>
</html>
