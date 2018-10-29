#include "conductor_ws.h"

#include <memory>
#include <utility>
#include <vector>

#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "defaults.h"
#include "media/engine/webrtcvideocapturerfactory.h"
#include "modules/audio_device/include/audio_device.h"
#include "modules/audio_processing/include/audio_processing.h"
#include "modules/video_capture/video_capture_factory.h"
#include "rtc_base/checks.h"
#include "rtc_base/json.h"
#include "rtc_base/logging.h"

// Names used for a IceCandidate JSON object.
const char kCandidateSdpMidName[] = "sdpMid";
const char kCandidateSdpMlineIndexName[] = "sdpMLineIndex";
const char kCandidateSdpName[] = "candidate";

// Names used for a SessionDescription JSON object.
const char kSessionDescriptionTypeName[] = "type";
const char kSessionDescriptionSdpName[] = "sdp";
const char kJanusOptName[] = "janus";

class DummySetSessionDescriptionObserver
	: public webrtc::SetSessionDescriptionObserver {
public:
	static DummySetSessionDescriptionObserver* Create() {
		return new rtc::RefCountedObject<DummySetSessionDescriptionObserver>();
	}
	virtual void OnSuccess() { RTC_LOG(INFO) << __FUNCTION__; }
	virtual void OnFailure(webrtc::RTCError error) {
		RTC_LOG(INFO) << __FUNCTION__ << " " << ToString(error.type()) << ": "
			<< error.message();
	}
};

ConductorWs::ConductorWs(PeerConnectionWsClient* client, MainWindow* main_wnd)
	: peer_id_(-1), loopback_(false), client_(client), main_wnd_(main_wnd) {
	client_->RegisterObserver(this);
	main_wnd->RegisterObserver(this);
}

ConductorWs::~ConductorWs() {
	RTC_DCHECK(!peer_connection_);
}

bool ConductorWs::connection_active() const {
	return peer_connection_ != nullptr;
}

void ConductorWs::Close() {
	client_->SignOut();
	DeletePeerConnection();
}

bool ConductorWs::InitializePeerConnection() {
	RTC_DCHECK(!peer_connection_factory_);
	RTC_DCHECK(!peer_connection_);

	peer_connection_factory_ = webrtc::CreatePeerConnectionFactory(
		nullptr /* network_thread */, nullptr /* worker_thread */,
		nullptr /* signaling_thread */, nullptr /* default_adm */,
		webrtc::CreateBuiltinAudioEncoderFactory(),
		webrtc::CreateBuiltinAudioDecoderFactory(),
		webrtc::CreateBuiltinVideoEncoderFactory(),
		webrtc::CreateBuiltinVideoDecoderFactory(), nullptr /* audio_mixer */,
		nullptr /* audio_processing */);

	if (!peer_connection_factory_) {
		main_wnd_->MessageBox("Error", "Failed to initialize PeerConnectionFactory",
			true);
		DeletePeerConnection();
		return false;
	}

	if (!CreatePeerConnection(/*dtls=*/true)) {
		main_wnd_->MessageBox("Error", "CreatePeerConnection failed", true);
		DeletePeerConnection();
	}

	AddTracks();

	return peer_connection_ != nullptr;
}

bool ConductorWs::ReinitializePeerConnectionForLoopback() {
	loopback_ = true;
	std::vector<rtc::scoped_refptr<webrtc::RtpSenderInterface>> senders =
		peer_connection_->GetSenders();
	peer_connection_ = nullptr;
	if (CreatePeerConnection(/*dtls=*/false)) {
		for (const auto& sender : senders) {
			peer_connection_->AddTrack(sender->track(), sender->stream_ids());
		}
		peer_connection_->CreateOffer(
			this, webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
	}
	return peer_connection_ != nullptr;
}

bool ConductorWs::CreatePeerConnection(bool dtls) {
	RTC_DCHECK(peer_connection_factory_);
	RTC_DCHECK(!peer_connection_);

	webrtc::PeerConnectionInterface::RTCConfiguration config;
	config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
	config.enable_dtls_srtp = dtls;
	webrtc::PeerConnectionInterface::IceServer server;
	server.uri = GetPeerConnectionString();
	config.servers.push_back(server);

	peer_connection_ = peer_connection_factory_->CreatePeerConnection(
		config, nullptr, nullptr, this);
	return peer_connection_ != nullptr;
}

void ConductorWs::DeletePeerConnection() {
	main_wnd_->StopLocalRenderer();
	main_wnd_->StopRemoteRenderer();
	peer_connection_ = nullptr;
	peer_connection_factory_ = nullptr;
	peer_id_ = -1;
	loopback_ = false;
}

void ConductorWs::EnsureStreamingUI() {
	RTC_DCHECK(peer_connection_);
	if (main_wnd_->IsWindow()) {
		if (main_wnd_->current_ui() != MainWindow::STREAMING)
			main_wnd_->SwitchToStreamingUI();
	}
}

//
// PeerConnectionObserver implementation.
//

void ConductorWs::OnAddTrack(
	rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
	const std::vector<rtc::scoped_refptr<webrtc::MediaStreamInterface>>&
	streams) {
	RTC_LOG(INFO) << __FUNCTION__ << " " << receiver->id();
	main_wnd_->QueueUIThreadCallback(NEW_TRACK_ADDED,
		receiver->track().release());
}

void ConductorWs::OnRemoveTrack(
	rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) {
	RTC_LOG(INFO) << __FUNCTION__ << " " << receiver->id();
	main_wnd_->QueueUIThreadCallback(TRACK_REMOVED, receiver->track().release());
}

void ConductorWs::OnIceCandidate(const webrtc::IceCandidateInterface* candidate) {
	RTC_LOG(INFO) << __FUNCTION__ << " " << candidate->sdp_mline_index();
	// For loopback test. To save some connecting delay.
	if (loopback_) {
		if (!peer_connection_->AddIceCandidate(candidate)) {
			RTC_LOG(WARNING) << "Failed to apply the received candidate";
		}
		return;
	}

	Json::StyledWriter writer;
	Json::Value jmessage;

	jmessage[kCandidateSdpMidName] = candidate->sdp_mid();
	jmessage[kCandidateSdpMlineIndexName] = candidate->sdp_mline_index();
	std::string sdp;
	if (!candidate->ToString(&sdp)) {
		RTC_LOG(LS_ERROR) << "Failed to serialize candidate";
		return;
	}
	jmessage[kCandidateSdpName] = sdp;
	SendMessage(writer.write(jmessage));
}

//
// PeerConnectionClientObserver implementation.
//

void ConductorWs::OnSignedIn() {
	RTC_LOG(INFO) << __FUNCTION__;
	main_wnd_->SwitchToPeerList(client_->peers());
}

void ConductorWs::OnDisconnected() {
	RTC_LOG(INFO) << __FUNCTION__;

	DeletePeerConnection();

	if (main_wnd_->IsWindow())
		main_wnd_->SwitchToConnectUI();
}

void ConductorWs::OnPeerConnected(int id, const std::string& name) {
	RTC_LOG(INFO) << __FUNCTION__;
	// Refresh the list if we're showing it.
	if (main_wnd_->current_ui() == MainWindow::LIST_PEERS)
		main_wnd_->SwitchToPeerList(client_->peers());
}

void ConductorWs::OnPeerDisconnected(int id) {
	RTC_LOG(INFO) << __FUNCTION__;
	if (id == peer_id_) {
		RTC_LOG(INFO) << "Our peer disconnected";
		main_wnd_->QueueUIThreadCallback(PEER_CONNECTION_CLOSED, NULL);
	}
	else {
		// Refresh the list if we're showing it.
		if (main_wnd_->current_ui() == MainWindow::LIST_PEERS)
			main_wnd_->SwitchToPeerList(client_->peers());
	}
}

//because janus self act as an end,so always define peer_id=0
void ConductorWs::OnMessageFromPeer(int peer_id, const std::string& message) {
	RTC_DCHECK(!message.empty());
	//parse json here
	RTC_LOG(INFO) << "Got wsmsg:"<<message;
	//TODO make sure in right state
	//parse json
	Json::Reader reader;
	Json::Value jmessage;
	if (!reader.parse(message, jmessage)) {
		RTC_LOG(WARNING) << "Received unknown message. " << message;
		return;
	}
	std::string janus_str;
	std::string json_object;

	rtc::GetStringFromJsonObject(jmessage, kSessionDescriptionTypeName,
		&janus_str);
	if (!janus_str.empty()) {
		if (janus_str == "ack") {
			// Just an ack, we can probably ignore
			RTC_LOG(INFO) << "Got an ack on session. ";
		}
		else if (janus_str == "success") {
			String transaction = jo.optString("transaction");
			JanusTransaction jt = transactions.get(transaction);
			if (jt.success != null) {
				jt.success.success(jo);
			}
			transactions.remove(transaction);
		}
	}

	try {
		JSONObject jo = new JSONObject(msg);
		String janus = jo.optString("janus");
		if (janus.equals("keepalive")) {
			// Nothing happened
			Log.i(TAG, "Got a keepalive on session " + sessionId);
			return;
		}
		else if (janus.equals("ack")) {
			// Just an ack, we can probably ignore
			Log.i(TAG, "Got an ack on session " + sessionId);
		}
		else if (janus.equals("success")) {
			String transaction = jo.optString("transaction");
			JanusTransaction jt = transactions.get(transaction);
			if (jt.success != null) {
				jt.success.success(jo);
			}
			transactions.remove(transaction);
		}
		else if (janus.equals("trickle")) {
			// We got a trickle candidate from Janus
		}
		else if (janus.equals("webrtcup")) {
			// The PeerConnection with the gateway is up! Notify this
			Log.d(TAG, "Got a webrtcup event on session " + sessionId);
		}
		else if (janus.equals("hangup")) {
			// A plugin asked the core to hangup a PeerConnection on one of our handles
			Log.d(TAG, "Got a hangup event on session " + sessionId);
		}
		else if (janus.equals("detached")) {
			// A plugin asked the core to detach one of our handles
			Log.d(TAG, "Got a detached event on session " + sessionId);
		}
		else if (janus.equals("media")) {
			// Media started/stopped flowing
			Log.d(TAG, "Got a media event on session " + sessionId);
		}
		else if (janus.equals("slowlink")) {
			Log.d(TAG, "Got a slowlink event on session " + sessionId);
		}
		else if (janus.equals("error")) {
			// Oops, something wrong happened
			String transaction = jo.optString("transaction");
			JanusTransaction jt = transactions.get(transaction);
			if (jt.error != null) {
				jt.error.error(jo);
			}
			transactions.remove(transaction);
		}
		else {
			JanusHandle handle = handles.get(new BigInteger(jo.optString("sender")));
			if (handle == null) {
				Log.e(TAG, "missing handle");
			}
			else if (janus.equals("event")) {
				Log.d(TAG, "Got a plugin event on session " + sessionId);

				String transaction = jo.optString("transaction");
				if (transaction == null || transaction.isEmpty()) {
					return;
				}
				JanusTransaction jt = transactions.get(transaction);
				if (jt != null) {
					if (jt.event != null) {
						jt.event.event(jo);
					}
				}
			}
		}
	}
	catch (JSONException e) {
		reportError("WebSocket message JSON parsing error: " + e.toString());
	}

	if (!peer_connection_.get()) {
		RTC_DCHECK(peer_id_ == -1);
		peer_id_ = peer_id;

		if (!InitializePeerConnection()) {
			RTC_LOG(LS_ERROR) << "Failed to initialize our PeerConnection instance";
			client_->SignOut();
			return;
		}
	}
	else if (peer_id != peer_id_) {
		RTC_DCHECK(peer_id_ != -1);
		RTC_LOG(WARNING)
			<< "Received a message from unknown peer while already in a "
			"conversation with a different peer.";
		return;
	}

	Json::Reader reader;
	Json::Value jmessage;
	if (!reader.parse(message, jmessage)) {
		RTC_LOG(WARNING) << "Received unknown message. " << message;
		return;
	}
	std::string type_str;
	std::string json_object;

	rtc::GetStringFromJsonObject(jmessage, kSessionDescriptionTypeName,
		&type_str);
	if (!type_str.empty()) {
		if (type_str == "offer-loopback") {
			// This is a loopback call.
			// Recreate the peerconnection with DTLS disabled.
			if (!ReinitializePeerConnectionForLoopback()) {
				RTC_LOG(LS_ERROR) << "Failed to initialize our PeerConnection instance";
				DeletePeerConnection();
				client_->SignOut();
			}
			return;
		}
		absl::optional<webrtc::SdpType> type_maybe =
			webrtc::SdpTypeFromString(type_str);
		if (!type_maybe) {
			RTC_LOG(LS_ERROR) << "Unknown SDP type: " << type_str;
			return;
		}
		webrtc::SdpType type = *type_maybe;
		std::string sdp;
		if (!rtc::GetStringFromJsonObject(jmessage, kSessionDescriptionSdpName,
			&sdp)) {
			RTC_LOG(WARNING) << "Can't parse received session description message.";
			return;
		}
		webrtc::SdpParseError error;
		std::unique_ptr<webrtc::SessionDescriptionInterface> session_description =
			webrtc::CreateSessionDescription(type, sdp, &error);
		if (!session_description) {
			RTC_LOG(WARNING) << "Can't parse received session description message. "
				<< "SdpParseError was: " << error.description;
			return;
		}
		RTC_LOG(INFO) << " Received session description :" << message;
		peer_connection_->SetRemoteDescription(
			DummySetSessionDescriptionObserver::Create(),
			session_description.release());
		if (type == webrtc::SdpType::kOffer) {
			peer_connection_->CreateAnswer(
				this, webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
		}
	}
	else {
		std::string sdp_mid;
		int sdp_mlineindex = 0;
		std::string sdp;
		if (!rtc::GetStringFromJsonObject(jmessage, kCandidateSdpMidName,
			&sdp_mid) ||
			!rtc::GetIntFromJsonObject(jmessage, kCandidateSdpMlineIndexName,
				&sdp_mlineindex) ||
			!rtc::GetStringFromJsonObject(jmessage, kCandidateSdpName, &sdp)) {
			RTC_LOG(WARNING) << "Can't parse received message.";
			return;
		}
		webrtc::SdpParseError error;
		std::unique_ptr<webrtc::IceCandidateInterface> candidate(
			webrtc::CreateIceCandidate(sdp_mid, sdp_mlineindex, sdp, &error));
		if (!candidate.get()) {
			RTC_LOG(WARNING) << "Can't parse received candidate message. "
				<< "SdpParseError was: " << error.description;
			return;
		}
		if (!peer_connection_->AddIceCandidate(candidate.get())) {
			RTC_LOG(WARNING) << "Failed to apply the received candidate";
			return;
		}
		RTC_LOG(INFO) << " Received candidate :" << message;
	}
}

void ConductorWs::OnMessageSent(int err) {
	// Process the next pending message if any.
	main_wnd_->QueueUIThreadCallback(SEND_MESSAGE_TO_PEER, NULL);
}

void ConductorWs::OnServerConnectionFailure() {
	main_wnd_->MessageBox("Error", ("Failed to connect to " + server_).c_str(),
		true);
}

//
// MainWndCallback implementation.
//

void ConductorWs::StartLogin(const std::string& server, int port) {
	if (client_->is_connected())
		return;
	server_ = server;
	client_->Connect("1234", "1111");
}

void ConductorWs::DisconnectFromServer() {
	if (client_->is_connected())
		client_->SignOut();
}

void ConductorWs::ConnectToPeer(int peer_id) {
	RTC_DCHECK(peer_id_ == -1);
	RTC_DCHECK(peer_id != -1);

	if (peer_connection_.get()) {
		main_wnd_->MessageBox(
			"Error", "We only support connecting to one peer at a time", true);
		return;
	}

	if (InitializePeerConnection()) {
		peer_id_ = peer_id;
		peer_connection_->CreateOffer(
			this, webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
	}
	else {
		main_wnd_->MessageBox("Error", "Failed to initialize PeerConnection", true);
	}
}

std::unique_ptr<cricket::VideoCapturer> ConductorWs::OpenVideoCaptureDevice() {
	std::vector<std::string> device_names;
	{
		std::unique_ptr<webrtc::VideoCaptureModule::DeviceInfo> info(
			webrtc::VideoCaptureFactory::CreateDeviceInfo());
		if (!info) {
			return nullptr;
		}
		int num_devices = info->NumberOfDevices();
		for (int i = 0; i < num_devices; ++i) {
			const uint32_t kSize = 256;
			char name[kSize] = { 0 };
			char id[kSize] = { 0 };
			if (info->GetDeviceName(i, name, kSize, id, kSize) != -1) {
				device_names.push_back(name);
			}
		}
	}

	cricket::WebRtcVideoDeviceCapturerFactory factory;
	std::unique_ptr<cricket::VideoCapturer> capturer;
	for (const auto& name : device_names) {
		capturer = factory.Create(cricket::Device(name, 0));
		if (capturer) {
			break;
		}
	}
	return capturer;
}

void ConductorWs::AddTracks() {
	if (!peer_connection_->GetSenders().empty()) {
		return;  // Already added tracks.
	}

	rtc::scoped_refptr<webrtc::AudioTrackInterface> audio_track(
		peer_connection_factory_->CreateAudioTrack(
			kAudioLabel, peer_connection_factory_->CreateAudioSource(
				cricket::AudioOptions())));
	auto result_or_error = peer_connection_->AddTrack(audio_track, { kStreamId });
	if (!result_or_error.ok()) {
		RTC_LOG(LS_ERROR) << "Failed to add audio track to PeerConnection: "
			<< result_or_error.error().message();
	}

	std::unique_ptr<cricket::VideoCapturer> video_device =
		OpenVideoCaptureDevice();
	if (video_device) {
		rtc::scoped_refptr<webrtc::VideoTrackInterface> video_track_(
			peer_connection_factory_->CreateVideoTrack(
				kVideoLabel, peer_connection_factory_->CreateVideoSource(
					std::move(video_device), nullptr)));
		main_wnd_->StartLocalRenderer(video_track_);

		result_or_error = peer_connection_->AddTrack(video_track_, { kStreamId });
		if (!result_or_error.ok()) {
			RTC_LOG(LS_ERROR) << "Failed to add video track to PeerConnection: "
				<< result_or_error.error().message();
		}
	}
	else {
		RTC_LOG(LS_ERROR) << "OpenVideoCaptureDevice failed";
	}

	main_wnd_->SwitchToStreamingUI();
}

void ConductorWs::DisconnectFromCurrentPeer() {
	RTC_LOG(INFO) << __FUNCTION__;
	if (peer_connection_.get()) {
		client_->SendHangUp(peer_id_);
		DeletePeerConnection();
	}

	if (main_wnd_->IsWindow())
		main_wnd_->SwitchToPeerList(client_->peers());
}

void ConductorWs::UIThreadCallback(int msg_id, void* data) {
	switch (msg_id) {
	case PEER_CONNECTION_CLOSED:
		RTC_LOG(INFO) << "PEER_CONNECTION_CLOSED";
		DeletePeerConnection();

		if (main_wnd_->IsWindow()) {
			if (client_->is_connected()) {
				main_wnd_->SwitchToPeerList(client_->peers());
			}
			else {
				main_wnd_->SwitchToConnectUI();
			}
		}
		else {
			DisconnectFromServer();
		}
		break;

	case SEND_MESSAGE_TO_PEER: {
		RTC_LOG(INFO) << "SEND_MESSAGE_TO_PEER";
		std::string* msg = reinterpret_cast<std::string*>(data);
		if (msg) {
			// For convenience, we always run the message through the queue.
			// This way we can be sure that messages are sent to the server
			// in the same order they were signaled without much hassle.
			pending_messages_.push_back(msg);
		}

		if (!pending_messages_.empty() && !client_->IsSendingMessage()) {
			msg = pending_messages_.front();
			pending_messages_.pop_front();

			if (!client_->SendToPeer(peer_id_, *msg) && peer_id_ != -1) {
				RTC_LOG(LS_ERROR) << "SendToPeer failed";
				DisconnectFromServer();
			}
			delete msg;
		}

		if (!peer_connection_.get())
			peer_id_ = -1;

		break;
	}

	case NEW_TRACK_ADDED: {
		auto* track = reinterpret_cast<webrtc::MediaStreamTrackInterface*>(data);
		if (track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {
			auto* video_track = static_cast<webrtc::VideoTrackInterface*>(track);
			main_wnd_->StartRemoteRenderer(video_track);
		}
		track->Release();
		break;
	}

	case TRACK_REMOVED: {
		// Remote peer stopped sending a track.
		auto* track = reinterpret_cast<webrtc::MediaStreamTrackInterface*>(data);
		track->Release();
		break;
	}

	default:
		RTC_NOTREACHED();
		break;
	}
}

//override CreateSessionDescriptionObserver::OnSuccess
void ConductorWs::OnSuccess(webrtc::SessionDescriptionInterface* desc) {
	peer_connection_->SetLocalDescription(
		DummySetSessionDescriptionObserver::Create(), desc);

	std::string sdp;
	desc->ToString(&sdp);

	// For loopback test. To save some connecting delay.
	if (loopback_) {
		// Replace message type from "offer" to "answer"
		std::unique_ptr<webrtc::SessionDescriptionInterface> session_description =
			webrtc::CreateSessionDescription(webrtc::SdpType::kAnswer, sdp);
		peer_connection_->SetRemoteDescription(
			DummySetSessionDescriptionObserver::Create(),
			session_description.release());
		return;
	}

	Json::StyledWriter writer;
	Json::Value jmessage;
	jmessage[kSessionDescriptionTypeName] =
		webrtc::SdpTypeToString(desc->GetType());
	jmessage[kSessionDescriptionSdpName] = sdp;
	SendMessage(writer.write(jmessage));
}

void ConductorWs::OnFailure(webrtc::RTCError error) {
	RTC_LOG(LERROR) << ToString(error.type()) << ": " << error.message();
}

void ConductorWs::SendMessage(const std::string& json_object) {
	std::string* msg = new std::string(json_object);
	main_wnd_->QueueUIThreadCallback(SEND_MESSAGE_TO_PEER, msg);
}
