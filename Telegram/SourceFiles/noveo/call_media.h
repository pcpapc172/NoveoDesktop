#pragma once

#include "base/weak_ptr.h"
#include <QtCore/QJsonObject>
#include <QtGui/QImage>

namespace Webview { class Window; }
namespace Noveo {

// Bundled LiveKit runtime only. Account authentication never enters JavaScript.
class CallMedia final : public base::has_weak_ptr {
public:
	CallMedia();
	~CallMedia();
	void connectRoom(QJsonObject token, bool muted);
	void setMuted(bool muted);
	void setDevices(QString microphone, QString speaker);
	void sendVideoFrame(QImage frame, bool screen);
	void stopVideo();
	void disconnectRoom();
	Fn<void(QJsonObject)> onEvent;
	Fn<void(QImage)> onVideoFrame;
private:
	void invoke(QString method, QJsonArray arguments);
	void connectReady();
	std::unique_ptr<Webview::Window> _window;
	QJsonObject _token;
	QString _resourceId;
	QString _microphone;
	QString _speaker;
	bool _ready = false;
	bool _stopped = false;
	bool _muted = false;
};
} // namespace Noveo
