var fs = require('fs');
var path = require('path');
var WebSocket = require('ws');

var
	REQ_GIMME_SINCE = 0,
	REQ_SET_VOTE = 1,
	REQ_SET_RUNNING = 2,
	RES_HERES_YOUR_STUFF = 0,
	RES_BATCH = 1;

var Module = {
    preRun: [],
    postRun: [emsReady],
    print: function(text) {
        console.log(text);
    },
    printErr: function(text) {
        console.log(text);
    },
    setStatus: function(text) {
        console.log(text);
    },
    TOTAL_MEMORY: 64*1024*1024,
};

eval(fs.readFileSync('vbam.js', 'utf-8'));

function vsysReadJoypad(which) {
	/*if(currentInput)
		console.log('input is ' + currentInput);*/
    return currentInput;
}

function vsysGetExternalFile(file, resultPtr, sizePtr) {
    var size = romData.length;
    var buf = Module.HEAPU32[resultPtr/4] || Module._malloc(size);
    Module.HEAPU8.set(romData, buf);
    Module.HEAPU32[resultPtr/4] = buf;
    Module.HEAPU32[sizePtr/4] = size;
}

function vsysInitSound() {
	return 44100;
}

function vsysWriteSound(buf, length) {
}

function vsysInitGraphics(width, height, pix) {
}

function initVbam() {
	vbam_js_init = Module.cwrap('vbam_js_init', 'void', ['string']);
	vbam_js_main = Module.cwrap('vbam_js_main', 'void', []);
	vbam_js_save_state = Module.cwrap('vbam_js_save_state', 'number', ['number', 'number']);
	vbam_js_load_state = Module.cwrap('vbam_js_load_state', 'number', ['number', 'number']);

	romData = new Uint8Array(fs.readFileSync('rom.gba'));
	vbam_js_init('rom.gba');
	currentFrame = 0;
	lastSavedFrame = -10000;
}

function pumpFrame(input) {
	if(currentFrame % (60*5) == 0) {
		saveState();
	}
	currentInput = input;
	vbam_js_main();
	currentFrame++;
}

wasPaused = false;
pendingInputs = [];

function setRunning(running) {
	var go = new Uint8Array(2);
	go[0] = REQ_SET_RUNNING;
	go[1] = running;
	ws.send(go, {binary: true});
}

function unpendFrame() {
	for(var i = 0; i < 5 && pendingInputs.length != 0; i++) {
		pumpFrame(pendingInputs.shift());
	}
	if(pendingInputs.length != 0) {
		process.nextTick(unpendFrame);
	} else if(wasPaused) {
		wasPaused = false;
		setRunning(1);
		console.log('All better, unpausing.');
	}
}

function initSock() {
	ws = new WebSocket('ws://127.0.0.1:4321/keyserver');
	ws.on('open', function() {
		setRunning(0);
		var data = new DataView(new ArrayBuffer(9));
		data.setUint8(0, REQ_GIMME_SINCE);
		data.setUint32(1, currentFrame, true);
		data.setUint32(5, currentFrame / 0x100000000, true);
		ws.send(data, {binary: true});
	});
	ws.on('close', function() {
		throw 'ws closed... :(';
	});

	ws.on('message', function(msg) {
		msg = new Uint8Array(msg);
		var dv = new DataView(msg.buffer);
		if(msg[0] == RES_HERES_YOUR_STUFF) {
			console.log('startup got ' + (msg.length - 1)/2 + ' frames');
			for(var i = 1; i < msg.length; i += 2)
				pumpFrame(dv.getUint16(i, true));

			setRunning(1);
		} else if(msg[0] == RES_BATCH) {
			var offset = 1;
			var frame = dv.getUint32(offset, true) + dv.getUint32(offset + 4, true) * 0x100000000;
			offset += 8;
			var numPlayers = dv.getUint32(offset, true); offset += 4;
			var numInputs = msg[offset++];
			var wasEmpty = pendingInputs.length == 0;
			if(pendingInputs.length > 50 && !wasPaused) {
				console.log('Ack, we fell behind... (' + pendingInputs.length + ')');
				setRunning(0);
				wasPaused = true;
			}
			for(var i = 0; i < numInputs; i++) {
				if(frame >= currentFrame)
					pendingInputs.push(dv.getUint16(offset, true));
				offset += 2;
				frame++;
			}
			// ignore popularity data
			if(wasEmpty && pendingInputs.length != 0)
				process.nextTick(unpendFrame);
		}
	});
}

var savesDir = 'saves/';

function loadState() {
	var current = savesDir + 'current.sgm';
	try {
		var state = fs.readFileSync(current);
	} catch(e) {
		console.log('No current.sgm; starting fresh.');
		return;
	}
	var buf = Module._malloc(state.length);
	Module.HEAPU8.set(new Uint8Array(state), buf);
	var length = state.length - 12;
	var i = (buf + length)/4;
	var id = Module.HEAPU32[i];
	var frameLo = Module.HEAPU32[i+1];
	var frameHi = Module.HEAPU32[i+2];
	if(id != 0xfeedfeed)
		throw 'Not a valid state.';
	currentFrame = frameLo + (frameHi * 0x100000000);
	if(!vbam_js_load_state(buf, length))
		throw 'Load state failed!';
	Module._free(buf);
	lastSavedFrame = currentFrame;
}

function writeWithBackup(base, buf) {
	var filename = base;
	var i = 0;
	if(fs.existsSync(base)) {
		for(var i = 0; ; i++) {
			var filename = base + '.' + i;
			if(!fs.existsSync(filename)) {
				fs.renameSync(base, filename);
				return;
			}
		}
	}
	fs.writeFileSync(filename, buf);
}

function saveState() {
	var length = 1048576*5;
	var buf = Module._malloc(length);
	var real = vbam_js_save_state(buf, length);
	if(real == -1 || (real % 4) != 0 || real >= length - 128)
		throw 'Save state failed! (' + real + ')';
	var i = (buf + real)/4;
	Module.HEAPU32[i] = 0xfeedfeed;
	Module.HEAPU32[i+1] = currentFrame;
	Module.HEAPU32[i+2] = currentFrame / 0x100000000;
	var nb = new Buffer(new Uint8Array(Module.HEAPU8.buffer, buf, real + 12));
	var filename = savesDir + currentFrame + '.sgm';
	writeWithBackup(filename, nb);
	console.log('-> ' + filename);
	fs.symlinkSync(path.relative(savesDir, filename), savesDir + 'current.sgm_');
	fs.renameSync(savesDir + 'current.sgm_', savesDir + 'current.sgm');
	// remove old files
	fs.readdir(savesDir, function(err, files) {
		files.forEach(function(filename) {
			if(!/\.sgm$/.test(filename))
				return;
			var frame = parseInt(filename);
			if(currentFrame - frame > (60*60) && frame % (60*60*10) != 0) {
				try {
					fs.unlinkSync(savesDir + filename);
				} catch(e) {}
			}
		});
	});
	Module._free(buf);
}

function emsReady() {
	initVbam();
	loadState()
	/*
	for(var i = 0; i < 1000; i++)
		pumpFrame(0);
	saveState();
	*/
	initSock();
}

