#include "noveo/session_client.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtNetwork/QSslConfiguration>
#include <iostream>

namespace base::assertion {
void log(const char *message, const char *file, int line) { std::cerr << message << ' ' << file << ':' << line << '\n'; }
}
template <typename T> T Decode(const mtpBuffer &buffer) {
    auto result = T(); auto from = buffer.constData();
    Assert(result.read(from, from + buffer.size())); return result;
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QFile cert(app.arguments().at(2)); Assert(cert.open(QIODevice::ReadOnly));
    auto ssl = QSslConfiguration::defaultConfiguration();
    ssl.addCaCertificates(QSslCertificate::fromData(cert.readAll())); QSslConfiguration::setDefaultConfiguration(ssl);
    auto endpoint = QUrl(app.arguments().at(1)); auto socket = endpoint.resolved(QUrl("/ws")); socket.setScheme("wss");
    Noveo::AuthClient auth(socket); Noveo::SessionClient client(&auth, endpoint);
    const auto peer = MTPInputPeer(MTP_inputPeerChat(MTP_long(Noveo::NativeUserId("g").bare)));
    const auto request = [&](int id, const auto &value) { mtpBuffer body; value.write(body); client.request(id, body); };
    const auto history = [&](int id, int offset, int add) { request(id, MTPmessages_GetHistory(peer, MTP_int(offset), MTP_int(0), MTP_int(add), MTP_int(3), MTP_int(0), MTP_int(0), MTP_long(0))); };
    client.onReply = [&](int id, mtpBuffer body) {
        if (id == 1) { history(2,0,0); return; }
        if (id == 5) { Decode<MTPUpdates>(body); history(6,0,0); return; }
        const auto result = Decode<MTPmessages_Messages>(body);
        const auto &page = result.c_messages_messagesSlice();
        const auto &rows = page.vmessages().v; Assert(rows.size() == 3);
        if (id == 2 || id == 3) {
            Assert(page.vcount().v == 60);
            for (auto i=0;i<3;++i) Assert(rows[i].c_message().vid().v == 160-i);
            if (id == 2) {
                client.message({{"type","message_context"},{"chatId","g"},{"messages",QJsonArray{QJsonObject{{"messageId","m1"},{"serverMessageId",101},{"timestamp",1001},{"senderId","self"},{"content",QJsonObject{{"text","ancient reply target"}}}}}}});
                history(3,0,0);
            } else history(4,111,0);
        } else if (id == 4) {
            for (auto i=0;i<3;++i) Assert(rows[i].c_message().vid().v == 110-i);
            request(5, MTPmessages_SendMessage(MTP_flags(MTPmessages_SendMessage::Flag::f_reply_to),peer,
                MTP_inputReplyToMessage(MTP_flags(MTPDinputReplyToMessage::Flags()),MTP_int(101),MTPint(),MTPInputPeer(),MTPstring(),MTPVector<MTPMessageEntity>(),MTPint(),MTPInputPeer(),MTPint(),MTPbytes()),
                MTP_string("reply from old history"),MTP_long(42),MTPReplyMarkup(),MTPVector<MTPMessageEntity>(),MTPint(),MTPint(),MTPInputPeer(),MTPInputQuickReplyShortcut(),MTPlong(),MTPlong(),MTPSuggestedPost(),MTPInputRichMessage()));
        } else if (id == 6) {
            Assert(page.vcount().v == 61);
            for (auto i=0;i<3;++i) Assert(rows[i].c_message().vid().v == 161-i);
            Assert(rows.front().c_message().vreply_to()->c_messageReplyHeader().vreply_to_msg_id()->v == 101);
            history(7,111,-2);
        } else if (id == 7) {
            for (auto i=0;i<3;++i) Assert(rows[i].c_message().vid().v == 112-i);
            history(8,111,-3);
        } else if (id == 8) {
            for (auto i=0;i<3;++i) Assert(rows[i].c_message().vid().v == 113-i);
            app.exit(0);
        } else Assert(false);
    };
    auth.onAuthenticated = [&](QJsonObject profile) {
        client.authenticated(profile);
        request(1,MTPmessages_GetDialogs(MTP_flags(MTPmessages_GetDialogs::Flags()),MTPint(),MTP_int(0),MTP_int(0),MTP_inputPeerEmpty(),MTP_int(100),MTP_long(0)));
    };
    auth.onMessage = [&](QJsonObject message) { client.message(message); };
    auth.login("fixture", "fixture");
    QTimer::singleShot(20000,&app,[&]{app.exit(22);});
    return app.exec();
}
