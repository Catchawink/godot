/*************************************************************************/
/*  webrtc_peer_connection_js.cpp                                        */
/*************************************************************************/
/*                       This file is part of:                           */
/*                           GODOT ENGINE                                */
/*                      https://godotengine.org                          */
/*************************************************************************/
/* Copyright (c) 2007-2022 Juan Linietsky, Ariel Manzur.                 */
/* Copyright (c) 2014-2022 Godot Engine contributors (cf. AUTHORS.md).   */
/*                                                                       */
/* Permission is hereby granted, free of charge, to any person obtaining */
/* a copy of this software and associated documentation files (the       */
/* "Software"), to deal in the Software without restriction, including   */
/* without limitation the rights to use, copy, modify, merge, publish,   */
/* distribute, sublicense, and/or sell copies of the Software, and to    */
/* permit persons to whom the Software is furnished to do so, subject to */
/* the following conditions:                                             */
/*                                                                       */
/* The above copyright notice and this permission notice shall be        */
/* included in all copies or substantial portions of the Software.       */
/*                                                                       */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,       */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF    */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.*/
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY  */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,  */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE     */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                */
/*************************************************************************/

#ifdef WEB_ENABLED

#include "webrtc_peer_connection_js.h"

#include "webrtc_data_channel_js.h"

#include "platform/javascript/audio_driver_javascript.h"

#include "emscripten.h"

void WebRTCPeerConnectionJS::_on_ice_candidate(void *p_obj, const char *p_mid_name, int p_mline_idx, const char *p_candidate) {
	WebRTCPeerConnectionJS *peer = static_cast<WebRTCPeerConnectionJS *>(p_obj);
	peer->emit_signal(SNAME("ice_candidate_created"), String(p_mid_name), p_mline_idx, String(p_candidate));
}

void WebRTCPeerConnectionJS::_on_session_created(void *p_obj, const char *p_type, const char *p_session) {
	WebRTCPeerConnectionJS *peer = static_cast<WebRTCPeerConnectionJS *>(p_obj);
	peer->emit_signal(SNAME("session_description_created"), String(p_type), String(p_session));
}

void WebRTCPeerConnectionJS::_on_connection_state_changed(void *p_obj, int p_state) {
	WebRTCPeerConnectionJS *peer = static_cast<WebRTCPeerConnectionJS *>(p_obj);
	peer->_conn_state = (ConnectionState)p_state;
}

void WebRTCPeerConnectionJS::_on_gathering_state_changed(void *p_obj, int p_state) {
	WebRTCPeerConnectionJS *peer = static_cast<WebRTCPeerConnectionJS *>(p_obj);
	peer->_gathering_state = (GatheringState)p_state;
}

void WebRTCPeerConnectionJS::_on_signaling_state_changed(void *p_obj, int p_state) {
	WebRTCPeerConnectionJS *peer = static_cast<WebRTCPeerConnectionJS *>(p_obj);
	peer->_signaling_state = (SignalingState)p_state;
}

void WebRTCPeerConnectionJS::_on_error(void *p_obj) {
	ERR_PRINT("RTCPeerConnection error!");
}

void WebRTCPeerConnectionJS::_on_data_channel(void *p_obj, int p_id) {
	WebRTCPeerConnectionJS *peer = static_cast<WebRTCPeerConnectionJS *>(p_obj);
	peer->emit_signal(SNAME("data_channel_received"), Ref<WebRTCDataChannel>(memnew(WebRTCDataChannelJS(p_id))));
}

void WebRTCPeerConnectionJS::_track_instanced(Ref<AudioStreamGeneratorPlayback> p_playback, int p_js_id) {
	/* clang-format off */
	int id = EM_ASM_INT({
		try {
			var dict = Module.IDHandler.get($0);
			var track = Module.IDHandler.get($1);
			var driver = Module.IDHandler.get($2);
			if (!dict || !driver) return;

			var jsId;

			var source = driver["context"].createMediaStreamSource(track["stream"]);
			var script = driver["context"].createScriptProcessor(driver["script"].bufferSize, 2, 2);
			var getPlayback = cwrap("_emrtc_get_playback", "number", ["number"]);
			var playbackPushFrame = cwrap("_emrtc_playback_push_frame", null, ["number", "number", "number"]);
			script.onaudioprocess = function(audioProcessingEvent) {
				var playback = getPlayback(dict["ptr"], jsId);
				if (playback == 0) {
					Module.IDHandler.remove(jsId);
					source.disconnect(script);
					source = undefined;
					script = undefined;
					return;
				}
				var input = audioProcessingEvent.inputBuffer;
				var inputDataL = input.getChannelData(0);
				var inputDataR = input.numberOfChannels > 1 ? input.getChannelData(1) : inputDataL;
				for (var i = 0; i < inputDataL.length; i++) {
					playbackPushFrame(playback, inputDataL[i], inputDataR[i]);
				}
			};

			source.connect(script);

			jsId = Module.IDHandler.add({
				"script": script,
				"source": source
			});
			return jsId;
		} catch (e) {
			console.log(e);
			return 0;
		}
	}, _js_id, p_js_id, AudioDriverJavaScript::singleton->get_js_driver_id());
	/* clang-format on */

	_playbacks[id] = p_playback->get_instance_id();
}

void WebRTCPeerConnectionJS::close() {
	godot_js_rtc_pc_close(_js_id);
	_conn_state = STATE_CLOSED;
}

Error WebRTCPeerConnectionJS::add_track(Ref<AudioEffectRecord> p_source) {
	if (_tracks.has(p_source)) return ERR_ALREADY_IN_USE;

	AudioEffectRecord *ptr = p_source.ptr();

	/* clang-format off */
	int track_id = EM_ASM_INT({
		try {
			var dict = Module.IDHandler.get($0);
			var driver = Module.IDHandler.get($1);
			var record = $2;
			if (!dict || !driver) return;

			var dest = driver["context"].createMediaStreamDestination(driver["context"], {});
			var script = driver["context"].createScriptProcessor(driver["script"].bufferSize, 2, 2);
			var streamGetBuffer = cwrap("_emrtc_get_stream_buffer", "number", ["number"]);
			var streamGetBufferSize = cwrap("_emrtc_get_stream_buffer_size", "number", ["number"]);
			var streamResetBuffer = cwrap("_emrtc_reset_stream_buffer", null, ["number"]);
			var numberOfChannels = 2;
			script.onaudioprocess = function(audioProcessingEvent) {
				var output = audioProcessingEvent.outputBuffer;

				var source = streamGetBuffer(record);
				var sourceSize = streamGetBufferSize(record);

				//if (sourceSize != output.length) console.log("Buffer underflow/overflow: Output data size mismatch!", sourceSize, output.length);

				var internalBuffer = HEAPF32.subarray(
						source / HEAPF32.BYTES_PER_ELEMENT,
						source / HEAPF32.BYTES_PER_ELEMENT + sourceSize * numberOfChannels);

				for (var channel = 0; channel < output.numberOfChannels; channel++) {
					var outputData = output.getChannelData(channel);
					// Loop through samples.
					for (var sample = 0; sample < Math.min(outputData.length, sourceSize); sample++) {
						outputData[sample] = internalBuffer[sample * numberOfChannels + channel % numberOfChannels];
					}
					for (var sample = Math.min(outputData.length, sourceSize); sample < outputData.length; sample++) {
						outputData[sample] = 0;
					}
				}
				streamResetBuffer(record);
			};

			script.connect(dest);
			driver["script"].connect(script); // Hopefully ensure that our audio process is called after the main audio process

			var tracks = dest.stream.getTracks();
			for (var i = 0; i < tracks.length; i++) {
				dict["conn"].addTrack(tracks[i], dest.stream);
			}

			return Module.IDHandler.add({
				"dest": dest,
				"script": script
			});
		} catch (e) {
			console.log(e);
			return 0;
		}
	}, _js_id, AudioDriverJavaScript::singleton->get_js_driver_id(), ptr);
	/* clang-format on */

	p_source->set_recording_active(true);
	_tracks[p_source] = track_id;

	return OK;
};

void WebRTCPeerConnectionJS::remove_track(Ref<AudioEffectRecord> p_source) {
	if (!_tracks.has(p_source)) return;

	/* clang-format off */
	EM_ASM({
		var dict = Module.IDHandler.get($0);
		var driver = Module.IDHandler.get($1);
		var stream = Module.IDHandler.get($2);
		Module.IDHandler.remove($2);
		if (!dict || !driver || !stream) return;

		stream["script"].onaudioprocess = null;

		stream["script"].disconnect(stream["dest"]);
		driver["script"].disconnect(stream["script"]);

		var tracks = stream["dest"].stream.getTracks();
		for (var i = 0; i < tracks.length; i++) {
			dict["conn"].removeTrack(tracks[i], dest.stream);
		}

		stream["script"] = undefined;
		stream["dest"] = undefined;
	}, _js_id, AudioDriverJavaScript::singleton->get_js_driver_id(), _tracks[p_source]);
	/* clang-format on */

	_tracks.erase(p_source);

};

Error WebRTCPeerConnectionJS::create_offer() {
	ERR_FAIL_COND_V(_conn_state != STATE_NEW, FAILED);

	_conn_state = STATE_CONNECTING;
	godot_js_rtc_pc_offer_create(_js_id, this, &_on_session_created, &_on_error);
	return OK;
}

Error WebRTCPeerConnectionJS::set_local_description(String type, String sdp) {
	godot_js_rtc_pc_local_description_set(_js_id, type.utf8().get_data(), sdp.utf8().get_data(), this, &_on_error);
	return OK;
}

Error WebRTCPeerConnectionJS::set_remote_description(String type, String sdp) {
	if (type == "offer") {
		ERR_FAIL_COND_V(_conn_state != STATE_NEW, FAILED);
		_conn_state = STATE_CONNECTING;
	}
	godot_js_rtc_pc_remote_description_set(_js_id, type.utf8().get_data(), sdp.utf8().get_data(), this, &_on_session_created, &_on_error);
	return OK;
}

Error WebRTCPeerConnectionJS::add_ice_candidate(String sdpMidName, int sdpMlineIndexName, String sdpName) {
	godot_js_rtc_pc_ice_candidate_add(_js_id, sdpMidName.utf8().get_data(), sdpMlineIndexName, sdpName.utf8().get_data());
	return OK;
}

Error WebRTCPeerConnectionJS::initialize(Dictionary p_config) {
	if (_js_id) {
		godot_js_rtc_pc_destroy(_js_id);
		_js_id = 0;
	}
	_conn_state = STATE_NEW;

	String config = Variant(p_config).to_json_string();
	_js_id = godot_js_rtc_pc_create(config.utf8().get_data(), this, &_on_connection_state_changed, &_on_gathering_state_changed, &_on_signaling_state_changed, &_on_ice_candidate, &_on_data_channel);
	return _js_id ? OK : FAILED;
}

Ref<WebRTCDataChannel> WebRTCPeerConnectionJS::create_data_channel(String p_channel, Dictionary p_channel_config) {
	ERR_FAIL_COND_V(_conn_state != STATE_NEW, nullptr);

	String config = Variant(p_channel_config).to_json_string();
	int id = godot_js_rtc_pc_datachannel_create(_js_id, p_channel.utf8().get_data(), config.utf8().get_data());
	ERR_FAIL_COND_V(id == 0, nullptr);
	return memnew(WebRTCDataChannelJS(id));
}

Error WebRTCPeerConnectionJS::poll() {
	return OK;
}

void WebRTCPeerConnectionJS::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_track_instanced"), &WebRTCPeerConnectionJS::_track_instanced);
}

WebRTCPeerConnection::GatheringState WebRTCPeerConnectionJS::get_gathering_state() const {
	return _gathering_state;
}

WebRTCPeerConnection::SignalingState WebRTCPeerConnectionJS::get_signaling_state() const {
	return _signaling_state;
}

WebRTCPeerConnection::ConnectionState WebRTCPeerConnectionJS::get_connection_state() const {
	return _conn_state;
}

WebRTCPeerConnectionJS::WebRTCPeerConnectionJS() {
	Dictionary config;
	initialize(config);
}

WebRTCPeerConnectionJS::~WebRTCPeerConnectionJS() {
	close();
	if (_js_id) {
		godot_js_rtc_pc_destroy(_js_id);
		_js_id = 0;
	}
};
#endif
