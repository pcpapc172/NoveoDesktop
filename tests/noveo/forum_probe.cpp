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
    const auto channelId = Noveo::NativeUserId("g").bare;
    const auto peer = MTPInputPeer(MTP_inputPeerChannel(MTP_long(channelId), MTP_long(1)));
    const auto request = [&](int id, const auto &value) { mtpBuffer body; value.write(body); client.request(id,body); };
    client.onReply = [&](int id, mtpBuffer body) {
        std::cerr << "forum reply " << id << " type " << std::hex << uint32(body.front()) << std::dec << '\n';
        if (id == 1) {
            const auto result = Decode<MTPmessages_Dialogs>(body);
            const auto &chats = result.c_messages_dialogs().vchats().v;
            Assert(chats.size() == 1);
            Assert(chats.front().c_channel().is_megagroup());
            Assert(chats.front().c_channel().is_forum());
            Assert(!chats.front().c_channel().is_broadcast());
            request(2,MTPmessages_GetForumTopics(MTP_flags(MTPmessages_GetForumTopics::Flags()),peer,MTPstring(),MTPint(),MTPint(),MTPint(),MTP_int(100)));
        } else if (id == 2) {
            const auto result = Decode<MTPmessages_ForumTopics>(body);
            Assert(result.data().vcount().v == 2);
            Assert(result.data().vtopics().v.size() == 2);
            const auto &message = result.data().vmessages().v.front().c_message();
            Assert(!message.is_post());
            Assert(message.vfrom_id()->c_peerUser().vuser_id().v == Noveo::NativeUserId("member").bare);
            Assert(message.vreply_to()->c_messageReplyHeader().is_forum_topic());
            Assert(message.vreply_to()->c_messageReplyHeader().vreply_to_top_id()->v == 101);
            Assert(qs(*message.vfrom_rank()) == u"Engineer"_q);
            request(3,MTPmessages_GetReplies(peer,MTP_int(101),MTPint(),MTPint(),MTPint(),MTP_int(10),MTPint(),MTPint(),MTPlong()));
        } else if (id == 3) {
            const auto result = Decode<MTPmessages_Messages>(body);
            Assert(result.c_messages_messagesSlice().vcount().v == 1);
            Assert(result.c_messages_messagesSlice().vmessages().v.front().c_message().vid().v == 102);
            request(4,MTPmessages_SendMessage(MTP_flags(MTPmessages_SendMessage::Flag::f_reply_to),peer,
                MTP_inputReplyToMessage(MTP_flags(MTPDinputReplyToMessage::Flags()),MTP_int(1),MTPint(),MTPInputPeer(),MTPstring(),MTPVector<MTPMessageEntity>(),MTPint(),MTPInputPeer(),MTPint(),MTPbytes()),
                MTP_string("General"),MTP_long(42),MTPReplyMarkup(),MTPVector<MTPMessageEntity>(),MTPint(),MTPint(),MTPInputPeer(),MTPInputQuickReplyShortcut(),MTPlong(),MTPlong(),MTPSuggestedPost(),MTPInputRichMessage()));
        } else if (id == 4) {
            Decode<MTPUpdates>(body);
            request(5,MTPmessages_ReadDiscussion(peer,MTP_int(101),MTP_int(102)));
        } else if (id == 5) {
            Assert(Decode<MTPBool>(body).type() == mtpc_boolTrue);
            request(6,MTPmessages_UpdatePinnedForumTopic(peer,MTP_int(101),MTP_boolTrue()));
        } else if (id == 6) {
            Decode<MTPUpdates>(body);
            request(7,MTPmessages_DeleteTopicHistory(peer,MTP_int(101)));
        } else if (id == 7) {
            Assert(Decode<MTPmessages_AffectedHistory>(body).data().voffset().v == 0);
            request(8,MTPmessages_EditChatParticipantRank(peer,MTP_inputPeerUser(MTP_long(Noveo::NativeUserId("member").bare),MTP_long(1)),MTP_string("Designer")));
        } else if (id == 8) {
            Decode<MTPUpdates>(body);
            app.exit(0);
        } else Assert(false);
    };
    auth.onAuthenticated = [&](QJsonObject profile) {
        client.authenticated(profile);
        request(1,MTPmessages_GetDialogs(MTP_flags(MTPmessages_GetDialogs::Flags()),MTPint(),MTPint(),MTPint(),MTP_inputPeerEmpty(),MTP_int(100),MTPlong()));
    };
    auth.onMessage = [&](QJsonObject message) { client.message(message); };
    auth.login("fixture","fixture");
    QTimer::singleShot(20000,&app,[&]{app.exit(22);});
    return app.exec();
}
