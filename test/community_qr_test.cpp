#include <QCoreApplication>
#include <QTransform>
#include <cstdio>

#include "check.h"
#include "community/model.h"
#include "community/qr.h"

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const auto owner = community::Identity::generate();
    const community::Invite invite{
        community::freshId(), owner.id(), community::randomCode(),
        QJsonArray{QJsonObject{{"peer", owner.id()}, {"endpoint", "relay://relay.example:3334"}}}};
    const auto link = invite.toLink();
    const auto image = community::inviteQr(link);
    CHECK(!image.isNull());
    CHECK(community::readInviteQr(image) == link);
    CHECK(community::readInviteQr(image.transformed(QTransform().rotate(90))) == link);
    QImage blank(512, 512, QImage::Format_Grayscale8);
    blank.fill(Qt::white);
    bool rejected = false;
    try {
        community::readInviteQr(blank);
    }
    catch (const std::exception&) {
        rejected = true;
    }
    CHECK(rejected);
    std::puts("community_qr_test: ok");
}
