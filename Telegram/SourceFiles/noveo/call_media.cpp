#include "noveo/call_media.h"

#include "webview/webview_embed.h"
#include "webview/webview_data_stream_memory.h"
#include <QtCore/QBuffer>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QUuid>
#include <QtCore/QUrl>
#include <QtGui/QImageReader>

namespace Noveo {
CallMedia::CallMedia()
: _resourceId(QUuid::createUuid().toString(QUuid::WithoutBraces)) {
}
CallMedia::~CallMedia() = default;

void CallMedia::connectRoom(QJsonObject token, bool muted) {
	_stopped = false;
	_token = std::move(token);
	_muted = muted;
	_window = std::make_unique<Webview::Window>(nullptr, Webview::WindowConfig{
		.allowCallMedia = true,
		.mode = Webview::WindowMode::Hidden,
	});
	if (!_window->valid()) {
		if (onEvent) onEvent({ { "e", "error" },
			{ "message", "The system web runtime is unavailable. Please install WebKitGTK or WebView2." } });
		return;
	}
	QFile html(":/gui/noveo/call.html"), sdk(":/gui/noveo/livekit-client.umd.min.js");
	if (!html.open(QIODevice::ReadOnly) || !sdk.open(QIODevice::ReadOnly)) {
		if (onEvent) onEvent({ { "e", "error" }, { "message", "The call runtime is unavailable." } });
		return;
	}
	auto page = html.readAll();
	page.replace("/*LIVEKIT_SDK*/", sdk.readAll());
	_window->setDataRequestHandler([=](Webview::DataRequest request) {
		if (request.id != _resourceId.toStdString()) return Webview::DataResult::Failed;
		const auto size = page.size();
		request.done({ std::make_unique<Webview::DataStreamFromMemory>(page, "text/html"), 0, size });
		return Webview::DataResult::Done;
	});
	_window->setNavigationStartHandler([=](QString url, bool newWindow) {
		// The exact bundled resource is the only allowed top-level document.
		return !newWindow && url == "https://noveo.ir/noveo-desktop-call/" + _resourceId;
	});
	_window->setMessageHandler(Fn<void(Webview::Message)>([=](Webview::Message message) {
		if (_stopped) return;
		const auto source = QUrl(QString::fromStdString(message.sourceUrl));
		if (source.toString() != "https://noveo.ir/noveo-desktop-call/" + _resourceId) return;
		const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(message.text));
		const auto event = document.object();
		if (event.value("e").toString() == "ready") {
			if (_ready) return;
			_ready = true;
			connectReady();
		} else if (event.value("e").toString() == "videoFrame") {
			const auto data = event.value("data").toString().toLatin1();
			if (data.size() > 1024 * 1024) return;
			auto bytes = QByteArray::fromBase64(data);
			QBuffer input(&bytes);
			input.open(QIODevice::ReadOnly);
			QImageReader reader(&input, "JPEG");
			const auto size = reader.size();
			if (size.isEmpty() || size.width() > 1280 || size.height() > 720) return;
			auto frame = reader.read();
			if (!frame.isNull() && onVideoFrame) onVideoFrame(std::move(frame));
		} else if (onEvent) {
			onEvent(event);
		}
	}));
	_window->loadHtml(QString::fromUtf8(page), "https://noveo.ir/noveo-desktop-call/" + _resourceId);
}

void CallMedia::invoke(QString method, QJsonArray arguments) {
	if (!_window || !_ready) return;
	const auto json = QJsonDocument(arguments).toJson(QJsonDocument::Compact);
	_window->eval("window.NoveoCall." + method.toUtf8() + "(..." + json + ");");
}
void CallMedia::connectReady() {
	invoke("setDevices", { _microphone, _speaker });
	invoke("connect", { _token.value("serverUrl"), _token.value("participantToken"), _muted });
	_token = {}; // Retain the room JWT only inside its media connection.
}
void CallMedia::setMuted(bool muted) {
	_muted = muted;
	invoke("setMuted", { muted });
}
void CallMedia::setDevices(QString microphone, QString speaker) {
	_microphone = microphone;
	_speaker = speaker;
	invoke("setDevices", { microphone, speaker });
}
void CallMedia::sendVideoFrame(QImage frame, bool screen) {
	if (!_ready || frame.isNull()) return;
	if (frame.width() > 1280 || frame.height() > 720) frame = frame.scaled(1280, 720, Qt::KeepAspectRatio, Qt::SmoothTransformation);
	QByteArray data;
	QBuffer buffer(&data);
	buffer.open(QIODevice::WriteOnly);
	frame.save(&buffer, "JPEG", 70);
	invoke("videoFrame", { QString::fromLatin1(data.toBase64()), frame.width(), frame.height(), screen });
}
void CallMedia::stopVideo() { invoke("stopScreen", {}); }
void CallMedia::disconnectRoom() {
	_stopped = true;
	invoke("disconnect", {});
	_token = {};
	_ready = false;
}
} // namespace Noveo
