#include <QApplication>
#include <QPushButton>

#include "check.h"
#include "desktop/community_tree.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    CommunityTree tree;
    auto* root = new QTreeWidgetItem(&tree, {"Community"});
    auto* source = new QTreeWidgetItem(root, {"Lounge"});
    auto* destination = new QTreeWidgetItem(root, {"Music"});
    auto* text = new QTreeWidgetItem(root, {"#general"});
    tree.configureVoiceChannel(source, "community", "lounge", true);
    tree.configureVoiceChannel(destination, "community", "music", true);
    auto* member = new QTreeWidgetItem(source, {"Member"});
    auto* profile = new QPushButton("Member", &tree);
    tree.setItemWidget(member, 0, profile);
    tree.configureVoiceParticipant(member, profile, "community", "lounge", "peer", true);
    int moves = 0;
    QObject::connect(&tree, &CommunityTree::voiceMoveRequested, &tree,
                     [&](const QString& community, const QString& peer, const QString& channel) {
                         CHECK(community == "community");
                         CHECK(peer == "peer");
                         CHECK(channel == "music");
                         ++moves;
                     });

    // Accept a move request without guessing the outcome before presence arrives.
    CHECK(tree.requestVoiceMove("community", "lounge", "peer", destination));
    CHECK(moves == 1);
    CHECK(member->parent() == source);
    CHECK(!tree.requestVoiceMove("community", "lounge", "peer", source));
    CHECK(!tree.requestVoiceMove("community", "lounge", "peer", text));
    CHECK(!tree.requestVoiceMove("community", "lounge", "peer", root));
    CHECK(!tree.requestVoiceMove("community", "lounge", "offline-peer", destination));
    CHECK(!tree.requestVoiceMove("community", "old-channel", "peer", destination));

    // A different community and revoked admin access cannot receive a move.
    auto* other = new QTreeWidgetItem(&tree, {"Other voice"});
    tree.configureVoiceChannel(other, "other-community", "music", true);
    CHECK(!tree.requestVoiceMove("community", "lounge", "peer", other));
    tree.configureVoiceChannel(destination, "community", "music", false);
    CHECK(!tree.requestVoiceMove("community", "lounge", "peer", destination));
    tree.configureVoiceChannel(destination, "community", "music", true);
    tree.configureVoiceParticipant(member, profile, "community", "lounge", "peer", false);
    CHECK(!tree.requestVoiceMove("community", "lounge", "peer", destination));
    CHECK(moves == 1);

    // Dropping over another participant resolves to that participant's channel.
    tree.configureVoiceParticipant(member, profile, "community", "lounge", "peer", true);
    auto* targetMember = new QTreeWidgetItem(destination, {"Another member"});
    CHECK(tree.requestVoiceMove("community", "lounge", "peer", targetMember));
    CHECK(moves == 2);
    delete member;
    CHECK(!tree.requestVoiceMove("community", "lounge", "peer", destination));
    CHECK(moves == 2);
    return 0;
}
