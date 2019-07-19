<html>
<head>
	<title>MicroTurtle Configuration</title>
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
	var servoUpAngle = document.getElementById("servo-up-angle").value;
	var servoDownAngle = document.getElementById("servo-down-angle").value;
	var servoMoveSteps = document.getElementById("servo-move-steps").value;
	var servoTickInterval = document.getElementById("servo-tick-interval").value;
	var motorTickInterval = document.getElementById("motor-tick-interval").value;
	var accelerationDuration = document.getElementById("acceleration-duration").value;
	var movementPause = document.getElementById("movement-pause").value;
	var struct = {"configuration": { 
		"straightStepsLeft": parseInt(straightStepsLeft),
		"straightStepsRight": parseInt(straightStepsRight),
		"turnStepsLeft": parseInt(turnStepsLeft),
		"turnStepsRight": parseInt(turnStepsRight),
		"servoUpAngle": parseInt(servoUpAngle),
		"servoDownAngle": parseInt(servoDownAngle),
		"servoMoveSteps": parseInt(servoMoveSteps),
		"servoTickInterval": parseInt(servoTickInterval),
		"motorTickInterval": parseInt(motorTickInterval),
		"accelerationDuration": parseInt(accelerationDuration),
		"movementPause": parseInt(movementPause)}};
	var xhr = new XMLHttpRequest();
	xhr.open('POST', '/configuration/setConfiguration.cgi');
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

function showUnsaved() {
	document.getElementById("unsaved").style.display = "block";
}

function attachUnsaved(id) {
	document.getElementById(id).addEventListener("change", showUnsaved);
}

/*
 * Initialises the page upon initial load.
 */
document.addEventListener("DOMContentLoaded", function() {
	var straightStepsLeft = "%straightStepsLeft%";
	var straightStepsRight = "%straightStepsRight%";
	var turnStepsLeft = "%turnStepsLeft%";
	var turnStepsRight = "%turnStepsRight%";
	var servoUpAngle = "%servoUpAngle%";
	var servoDownAngle = "%servoDownAngle%";
	var servoMoveSteps = "%servoMoveSteps%";
	var servoTickInterval = "%servoTickInterval%";
	var motorTickInterval = "%motorTickInterval%";
	var accelerationDuration = "%accelerationDuration%";
	var movementPause = "%movementPause%";
	if (isNaN(parseInt(straightStepsLeft))) {
		straightStepsLeft = 1728;
	}
	if (isNaN(parseInt(straightStepsRight))) {
		straightStepsRight = 1730;
	}
	if (isNaN(parseInt(turnStepsLeft))) {
		turnStepsLeft = 2051;
	}
	if (isNaN(parseInt(turnStepsRight))) {
		turnStepsRight = 2053;
	}
	if (isNaN(parseInt(servoUpAngle))) {
		servoUpAngle = 89;
	}
	if (isNaN(parseInt(servoDownAngle))) {
		servoDownAngle = -89;
	}
	if (isNaN(parseInt(servoMoveSteps))) {
		servoMoveSteps = 1;
	}
	if (isNaN(parseInt(servoTickInterval))) {
		servoTickInterval = 2;
	}
	if (isNaN(parseInt(motorTickInterval))) {
		motorTickInterval = 1;
	}
	if (isNaN(parseInt(accelerationDuration))) {
		accelerationDuration = 199;
	}
	if (isNaN(parseInt(movementPause))) {
		movementPause = 201;
	}
	document.getElementById("left-straight").value = straightStepsLeft;
	document.getElementById("right-straight").value = straightStepsRight;
	document.getElementById("left-turn").value = turnStepsLeft;
	document.getElementById("right-turn").value = turnStepsRight;
	document.getElementById("servo-up-angle").value = servoUpAngle;
	document.getElementById("servo-down-angle").value = servoDownAngle;
	document.getElementById("servo-move-steps").value = servoMoveSteps;
	document.getElementById("servo-tick-interval").value = servoTickInterval;
	document.getElementById("motor-tick-interval").value = motorTickInterval;
	document.getElementById("acceleration-duration").value = accelerationDuration;
	document.getElementById("movement-pause").value = movementPause;

	attachUnsaved("left-straight");
	attachUnsaved("right-straight");
	attachUnsaved("left-turn");
	attachUnsaved("right-turn");
	attachUnsaved("servo-up-angle");
	attachUnsaved("servo-down-angle");
	attachUnsaved("servo-move-steps");
	attachUnsaved("servo-tick-interval");
	attachUnsaved("motor-tick-interval");
	attachUnsaved("acceleration-duration");
	attachUnsaved("movement-pause");
});

	</script>
</head>
<body>
	<div class="banner">
		<a href="/">MicroTurtle</a> Configuration
	</div>
	
	<p>
	The following configuration options are used to tune the performance of the
	turtle robot. For a guided version for some options, see the 
	<a href="/configuration/calibrate.tpl">Calibration page</a>.
	</p>
	<table class="bandedTable wideInputs">
		<tr><td>100mm Straight Steps - Left Motor</td><td><input type="number" id="left-straight" min="0" step="1" value="4104"></td></tr>
		<tr><td>100mm Straight Steps - Right Motor</td><td><input type="number" id="right-straight" min="0" step="1" value="4104"></td></tr>
		<tr><td>180° Turn - Left Motor</td><td><input type="number" id="left-turn" min="0" step="1" value="4104"></td></tr>
		<tr><td>180° Turn - Right Motor</td><td><input type="number" id="right-turn" min="0" step="1" value="4104"></td></tr>
		<tr><td>Servo Up Angle</td><td><input type="number" id="servo-up-angle" min="-90" max="90" step="1" value"90"></td></tr>
		<tr><td>Servo Down Angle</td><td><input type="number" id="servo-down-angle" min="-90" max="90" step="1" value"-90"></td></tr>
		<tr><td>Servo Movement Steps</td><td><input type="number" id="servo-move-steps" min="0" max="100" step="1" value"0"></td></tr>
		<tr><td>Servo Tick Interval (ms)</td><td><input type="number" id="servo-tick-interval" min="0" max="2000" step="1" value"2"></td></tr>
		<tr><td>Motor Tick Interval (ms)</td><td><input type="number" id="motor-tick-interval" min="0" max="2000" step="1" value"1"></td></tr>
		<tr><td>Acceleration Duration (ticks)</td><td><input type="number" id="acceleration-duration" min="0" max="2000" step="1" value"199"></td></tr>
		<tr><td>Movement Pause (ms)</td><td><input type="number" id="movement-pause" min="0" step="1" value"200"></td></tr>
	</table>
	<table>
	<div id="unsaved" class="warning" style="display: none;">
		<br>The calibration values have not been saved.
	</div>
	<br>
	<a class="button" href="javascript:saveValues()">Save Values</a>
</body>
<footer>
	<br>
	© 2019 Ian Marshall
</footer>
</html>
