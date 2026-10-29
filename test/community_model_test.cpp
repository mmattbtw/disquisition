#include <QCoreApplication>
#include <QMessageAuthenticationCode>
#include <QSslCertificate>
#include <QTemporaryDir>
#include <cstdio>
#include <functional>
#include <stdexcept>

#include "check.h"
#include "community/model.h"
#include "community/store.h"

using namespace community;
namespace {
void rejects(const std::function<void()>& action) {
    bool rejected = false;
    try {
        action();
    }
    catch (const std::exception&) {
        rejected = true;
    }
    CHECK(rejected);
}
struct Group {
    Identity alice = Identity::generate(), bob = Identity::generate(), carol = Identity::generate();
    Model model = Model::create(alice, "friends", "alice", "tcp://127.0.0.1:9001");
    QString code = model.issueInvite(alice);
    Invite invite{model.id, model.owner, code,
                  QJsonArray{
                      QJsonObject{{"peer", alice.id()}, {"endpoint", "tcp://127.0.0.1:9001"}}}};
    void join(const Identity& peer, const QString& name) {
        model.admit(alice, joinIntent(peer, invite, name, "tcp://127.0.0.1:9002"));
    }
};

void relayAddressesAcceptHostnamesAndRejectOtherProtocols() {
    CHECK(normalizeRelayEndpoint("relay.disquisition.app") ==
          "relay://relay.disquisition.app:3333");
    CHECK(normalizeRelayEndpoint(" relay.example:4444 ") == "relay://relay.example:4444");
    CHECK(normalizeRelayEndpoint("relay://relay.example:4444") == "relay://relay.example:4444");
    CHECK(normalizeRelayEndpoint("[::1]:4444") == "relay://[::1]:4444");
    CHECK(normalizeRelayEndpoint("  ").isEmpty());
    for (const auto* address :
         {"https://relay.example", "tcp://relay.example:3333", "relay.example:0",
          "relay.example:65536", "relay.example:abc", "user:password@relay.example",
          "relay.example/path", "relay.example?token=x", "relay.example#fragment"})
        rejects([&] { normalizeRelayEndpoint(address); });
}

void identityAndInviteIntegrity() {
    Group group;
    CHECK(Identity::fromSecret(group.alice.secret()).id() == group.alice.id());
    CHECK(Identity::verify(group.alice.id(), "hello", group.alice.sign("hello")));
    CHECK(!Identity::verify(group.bob.id(), "hello", group.alice.sign("hello")));
    const auto credentials = group.alice.tlsCredentials();
    auto certificate = QSslCertificate(credentials.first).toDer();
    CHECK(Identity::certificatePeer(certificate) == group.alice.id());
    certificate[certificate.size() - 1] = char(certificate.back() ^ 1);
    CHECK(Identity::certificatePeer(certificate).isEmpty());
    const auto decoded = Invite::fromLink(group.invite.toLink());
    CHECK(decoded.community == group.model.id && decoded.owner == group.alice.id());
    CHECK(decoded.code == group.code);
    rejects([] { Invite::fromLink("disquisition://join/broken"); });
    rejects([] { Invite::fromLink("https://example.com"); });
    auto bad = group.model.toJson();
    auto operations = bad["operations"].toArray();
    auto operation = operations[0].toObject();
    operation["data"] = QJsonObject{{"name", "forged"}};
    operations[0] = operation;
    bad["operations"] = operations;
    rejects([&] { Model::fromJson(bad); });
    rejects([&] { Model::fromJson(group.model.toJson(), freshId()); });
    rejects([&] { Model::fromJson(group.model.toJson(), group.model.id, group.bob.id()); });
}

void memberHostsAdmitWithoutAdmins() {
    Group group;
    group.join(group.bob, "bob");
    CHECK(group.model.isMember(group.bob.id()));
    CHECK(!group.model.canAdmin(group.bob.id()));
    group.model.admit(group.bob,
                      joinIntent(group.carol, group.invite, "carol", "relay://relay.example:3333"));
    CHECK(group.model.isMember(group.carol.id()));
    rejects([&] { group.model.change(group.bob, "rename", {{"name", "hijacked"}}); });
    rejects([&] { group.model.issueInvite(group.bob); });
    CHECK(group.model.host(QSet<QString>{group.bob.id()}) == group.bob.id());
    group.model.change(group.alice, "admin", {{"peer", group.bob.id()}, {"enabled", true}});
    group.model.change(group.bob, "rename", {{"name", "new name"}});
    CHECK(group.model.name == "new name");
    rejects([&] {
        group.model.change(group.bob, "admin", {{"peer", group.carol.id()}, {"enabled", true}});
    });
}

void profilesAreOwnedByTheirAuthorAndPersist() {
    Group group;
    group.join(group.bob, "bob");
    Profile profile{"Bobby", "Music & games\nUsually around in the evening.", QByteArray("avatar")};
    auto data = profile.toJson();
    data["endpoint"] = "tcp://127.0.0.1:9002";
    data["peer"] = group.alice.id(); // An author cannot edit a different member.
    group.model.change(group.bob, "profile", data);
    const auto roundTrip = Model::fromJson(group.model.toJson());
    const auto member = roundTrip.members[group.bob.id()];
    CHECK(member.name == profile.name && member.bio == profile.bio &&
          member.avatar == profile.avatar);
    CHECK(roundTrip.members[group.alice.id()].name == "alice");
    CHECK(!roundTrip.canAdmin(group.bob.id()));
    // Older clients update routing/name without deleting new profile fields.
    group.model.change(group.bob, "profile",
                       {{"name", "Robert"}, {"endpoint", "tcp://127.0.0.1:9003"}});
    CHECK(group.model.members[group.bob.id()].bio == profile.bio);
    CHECK(group.model.members[group.bob.id()].avatar == profile.avatar);
    for (const auto& invalid :
         {QJsonObject{{"bio", QString(kMaxBioLength + 1, 'a')}},
          QJsonObject{{"bio", QString(QChar(0))}}, QJsonObject{{"avatar", "not base64"}},
          QJsonObject{
              {"avatar", QString::fromLatin1(QByteArray(kMaxAvatarBytes + 1, 'a').toBase64())}}}) {
        auto bad = data;
        for (auto it = invalid.begin(); it != invalid.end(); ++it)
            bad[it.key()] = it.value();
        const auto before = group.model.revision();
        rejects([&] { group.model.change(group.bob, "profile", bad); });
        CHECK(group.model.revision() == before);
    }
    rejects([&] { group.model.change(group.carol, "profile", data); });
    group.model.change(
        group.bob, "profile",
        {{"name", "Robert"}, {"endpoint", "tcp://127.0.0.1:9003"}, {"bio", ""}, {"avatar", ""}});
    CHECK(group.model.members[group.bob.id()].bio.isEmpty() &&
          group.model.members[group.bob.id()].avatar.isEmpty());
    QTemporaryDir directory;
    {
        Store store(directory.path());
        store.saveProfile(profile);
    }
    {
        Store reopened(directory.path());
        const auto saved = reopened.profile();
        CHECK(saved.name == profile.name && saved.bio == profile.bio &&
              saved.avatar == profile.avatar);
    }
}

void rejectedAdmissionsAreAtomic() {
    Group group;
    const auto revision = group.model.revision();
    auto badInvite = group.invite;
    badInvite.code = randomCode();
    rejects([&] {
        group.model.admit(group.alice,
                          joinIntent(group.bob, badInvite, "bob", "tcp://127.0.0.1:9002"));
    });
    CHECK(group.model.revision() == revision && !group.model.isMember(group.bob.id()));
    auto forged = joinIntent(group.bob, group.invite, "bob", "tcp://127.0.0.1:9002");
    forged["peer"] = group.carol.id();
    rejects([&] { group.model.admit(group.alice, forged); });
    CHECK(group.model.revision() == revision);
    auto wrongProof = joinIntent(group.bob, group.invite, "bob", "tcp://127.0.0.1:9002");
    wrongProof["proof"] = QString(64, '0');
    wrongProof = signedObject(wrongProof, group.bob, "disquisition/community-join/v1\n");
    rejects([&] { group.model.admit(group.alice, wrongProof); });
    CHECK(group.model.revision() == revision);
}

void forgedAdminOperationsCannotBypassPermissions() {
    Group group;
    group.join(group.bob, "bob");
    auto forged = group.model;
    const auto operation = forged.change(group.alice, "rename", {{"name", "forged"}});
    auto snapshot = forged.toJson();
    auto operations = snapshot.value("operations").toArray();
    for (qsizetype index = 0; index < operations.size(); ++index) {
        if (operations[index].toObject() != operation)
            continue;
        auto impersonation = operation;
        impersonation["author"] = group.bob.id();
        operations[index] = signedObject(impersonation, group.bob,
                                         "disquisition/community-operation/v1\n");
    }
    snapshot["operations"] = operations;
    const auto result = Model::fromJson(snapshot);
    CHECK(result.name == "friends");
}

void reconnectMergesRevocationsBeforeAdmission() {
    Group group;
    group.join(group.bob, "bob");
    auto returningHost = group.model;
    group.model.change(group.alice, "revoke", {{"hash", digest(group.code.toUtf8())}});
    CHECK(returningHost.validCode(group.code));
    CHECK(returningHost.merge(group.model.toJson()));
    CHECK(!returningHost.validCode(group.code));
    CHECK(returningHost.isMember(group.bob.id())); // revoking an invite doesn't kick its members
    rejects([&] {
        returningHost.admit(group.bob,
                            joinIntent(group.carol, group.invite, "carol", "tcp://127.0.0.1:9003"));
    });
    CHECK(!returningHost.merge(group.model.toJson()));
}

void splitsConvergeAndRevocationsWinConcurrentAdmissions() {
    Group group;
    group.join(group.bob, "bob");
    group.model.change(group.alice, "admin", {{"peer", group.bob.id()}, {"enabled", true}});
    auto left = group.model, right = group.model;
    left.change(group.alice, "rename", {{"name", "left"}});
    right.change(group.bob, "rename", {{"name", "right"}});
    const auto leftState = left.toJson(), rightState = right.toJson();
    left.merge(rightState);
    right.merge(leftState);
    CHECK(left.name == right.name && left.revision() == right.revision());
    auto revoked = group.model, admitted = group.model;
    revoked.change(group.alice, "revoke", {{"hash", digest(group.code.toUtf8())}});
    admitted.admit(group.bob,
                   joinIntent(group.carol, group.invite, "carol", "tcp://127.0.0.1:9003"));
    CHECK(admitted.isMember(group.carol.id()));
    revoked.merge(admitted.toJson());
    admitted.merge(revoked.toJson());
    CHECK(!revoked.isMember(group.carol.id()) && !admitted.isMember(group.carol.id()));
    CHECK(admitted.revision() == revoked.revision());
}

void demotionBlocksConcurrentAndFutureAdminEdits() {
    Group group;
    group.join(group.bob, "bob");
    group.model.change(group.alice, "admin", {{"peer", group.bob.id()}, {"enabled", true}});
    auto offline = group.model;
    group.model.change(group.alice, "admin", {{"peer", group.bob.id()}, {"enabled", false}});
    offline.change(group.bob, "rename", {{"name", "unauthorized during split"}});
    group.model.merge(offline.toJson());
    offline.merge(group.model.toJson());
    CHECK(group.model.name == "friends" && offline.name == "friends");
    rejects([&] { offline.change(group.bob, "rename", {{"name", "still no"}}); });
}

void persistsMetadataWithoutChatHistory() {
    QTemporaryDir directory;
    CHECK(directory.isValid());
    QString identity, id, revision;
    {
        Store store(directory.path());
        identity = store.identity().id();
        auto model = Model::create(store.identity(), "persistent", "me", "tcp://127.0.0.1:9001");
        model.issueInvite(store.identity());
        store.save(model);
        id = model.id;
        revision = model.revision();
        rejects([&] { Store duplicate(directory.path()); });
    }
    {
        Store store(directory.path());
        CHECK(store.identity().id() == identity);
        CHECK(store.load().value(id).revision() == revision);
    }
}

void removingHostPreservesItsPreviouslyAcceptedAdmissions() {
    Group group;
    group.join(group.bob, "bob");
    group.model.admit(group.bob,
                      joinIntent(group.carol, group.invite, "carol", "tcp://127.0.0.1:9003"));
    group.model.change(group.alice, "kick", {{"peer", group.bob.id()}});
    CHECK(!group.model.isMember(group.bob.id()));
    CHECK(group.model.isMember(group.carol.id()));
    CHECK(Model::fromJson(group.model.toJson()).isMember(group.carol.id()));
}
void kickingAllowsFreshAdmissionWithoutRestoringRole() {
    Group group;
    group.join(group.bob, "bob");
    group.model.change(group.alice, "admin", {{"peer", group.bob.id()}, {"enabled", true}});
    const auto stale = group.model.toJson();
    group.model.change(group.alice, "remove", {{"peer", group.bob.id()}});
    CHECK(!group.model.isMember(group.bob.id()) && !group.model.canAdmin(group.bob.id()));
    CHECK(!group.model.banned.contains(group.bob.id()));
    group.model.merge(stale);
    CHECK(!group.model.isMember(group.bob.id()));
    group.join(group.bob, "returning bob");
    CHECK(group.model.isMember(group.bob.id()) && !group.model.canAdmin(group.bob.id()));
    group.model.change(group.bob, "profile", {{"name", "Bob"}, {"endpoint", "tcp://127.0.0.1:9002"}});
    CHECK(Model::fromJson(group.model.toJson()).members.value(group.bob.id()).name == "Bob");
    rejects([&] { group.model.change(group.bob, "remove", {{"peer", group.alice.id()}}); });
    rejects([&] { group.model.change(group.alice, "remove", {{"peer", group.alice.id()}}); });
    rejects([&] { group.model.change(group.alice, "remove", {{"peer", group.carol.id()}}); });
}

void legacyBansCanBeLiftedWithoutRestoringMembership() {
    Group group;
    group.join(group.bob, "bob");
    group.model.change(group.alice, "admin", {{"peer", group.bob.id()}, {"enabled", true}});
    group.model.change(group.alice, "kick", {{"peer", group.bob.id()}});
    const auto banned = group.model.toJson();
    group.model = Model::fromJson(banned);
    CHECK(group.model.banned.contains(group.bob.id()));
    rejects([&] { group.join(group.bob, "blocked bob"); });
    rejects([&] { group.model.change(group.carol, "unban", {{"peer", group.bob.id()}}); });
    rejects([&] { group.model.change(group.alice, "unban", {{"peer", group.alice.id()}}); });
    group.model.change(group.alice, "unban", {{"peer", group.bob.id()}});
    CHECK(!group.model.banned.contains(group.bob.id()) && !group.model.isMember(group.bob.id()));
    group.model.merge(banned);
    group.join(group.bob, "returning bob");
    CHECK(group.model.isMember(group.bob.id()) && !group.model.canAdmin(group.bob.id()));
    CHECK(Model::fromJson(group.model.toJson()).isMember(group.bob.id()));
    group.model.change(group.alice, "kick", {{"peer", group.bob.id()}});
    CHECK(group.model.banned.contains(group.bob.id()) && !group.model.isMember(group.bob.id()));
}

void concurrentKickRejectsHostActionsAndAdmissions() {
    Group group;
    group.join(group.bob, "bob");
    group.model.change(group.alice, "admin", {{"peer", group.bob.id()}, {"enabled", true}});
    auto stale = group.model;
    group.model.change(group.alice, "remove", {{"peer", group.bob.id()}});
    stale.change(group.bob, "rename", {{"name", "stale admin"}});
    stale.admit(group.bob, joinIntent(group.carol, group.invite, "carol", "tcp://127.0.0.1:9003"));
    group.model.merge(stale.toJson());
    stale.merge(group.model.toJson());
    CHECK(group.model.name == "friends" && !group.model.isMember(group.carol.id()));
    CHECK(!group.model.isMember(group.bob.id()) && stale.revision() == group.model.revision());
    group.join(group.bob, "returning bob");
    CHECK(group.model.isMember(group.bob.id()));
}

void concurrentBanWinsUnbanAndStaleReadmission() {
    Group group;
    group.join(group.bob, "bob");
    group.model.change(group.alice, "kick", {{"peer", group.bob.id()}});
    auto offline = group.model;
    group.model.change(group.alice, "unban", {{"peer", group.bob.id()}});
    group.join(group.bob, "returning bob");
    group.model.change(group.alice, "kick", {{"peer", group.bob.id()}});
    offline.change(group.alice, "unban", {{"peer", group.bob.id()}});
    offline.admit(group.alice, joinIntent(group.bob, group.invite, "stale bob", "tcp://127.0.0.1:9002"));
    group.model.merge(offline.toJson());
    offline.merge(group.model.toJson());
    CHECK(group.model.banned.contains(group.bob.id()) && !group.model.isMember(group.bob.id()));
    CHECK(offline.banned.contains(group.bob.id()) && !offline.isMember(group.bob.id()));
    CHECK(group.model.revision() == offline.revision());
    group.model.change(group.alice, "unban", {{"peer", group.bob.id()}});
    CHECK(!group.model.isMember(group.bob.id()));
    group.join(group.bob, "fresh bob");
    CHECK(Model::fromJson(group.model.toJson()).isMember(group.bob.id()));
}

void leavingOnlyRemovesTheAuthorAndAllowsFreshAdmission() {
    Group group;
    group.join(group.bob, "bob");
    group.model.admit(group.bob,
        joinIntent(group.carol, group.invite, "carol", "tcp://127.0.0.1:9003"));
    group.model.change(group.alice, "admin", {{"peer", group.bob.id()}, {"enabled", true}});
    const auto stale = group.model.toJson();
    QJsonObject oldIntent;
    for (const auto& operation : stale.value("operations").toArray()) {
        const auto data = operation.toObject().value("data").toObject();
        if (data.value("intent").toObject().value("peer").toString() == group.bob.id())
            oldIntent = data.value("intent").toObject();
    }
    group.model.change(group.bob, "leave", {{"peer", group.alice.id()}});
    CHECK(!group.model.isMember(group.bob.id()));
    CHECK(!group.model.canAdmin(group.bob.id()));
    CHECK(group.model.isMember(group.alice.id()) && group.model.canAdmin(group.alice.id()));
    CHECK(group.model.isMember(group.carol.id()));
    CHECK(!group.model.banned.contains(group.bob.id()));
    group.model.merge(stale);
    CHECK(!group.model.isMember(group.bob.id()));
    rejects([&] { group.model.change(group.bob, "rename", {{"name", "no"}}); });
    rejects([&] { group.model.change(group.alice, "leave", {}); });
    const auto leaving = Model::fromJson(group.model.toJson());
    CHECK(!leaving.isMember(group.bob.id()) && leaving.isMember(group.carol.id()));
    CHECK(!oldIntent.isEmpty());
    CHECK(!group.model.acceptsIntent(group.alice.id(), oldIntent));
    rejects([&] { group.model.admit(group.alice, oldIntent); });
    group.join(group.bob, "returning bob");
    CHECK(group.model.isMember(group.bob.id()));
    CHECK(!group.model.canAdmin(group.bob.id()));
    CHECK(Model::fromJson(group.model.toJson()).isMember(group.bob.id()));
}

void leavingBlocksConcurrentHostAdmissionsAndEdits() {
    Group group;
    group.join(group.bob, "bob");
    group.model.change(group.alice, "admin", {{"peer", group.bob.id()}, {"enabled", true}});
    auto stale = group.model;
    group.model.change(group.bob, "leave", {});
    stale.change(group.bob, "rename", {{"name", "stale admin"}});
    stale.admit(group.bob,
        joinIntent(group.carol, group.invite, "carol", "tcp://127.0.0.1:9003"));
    group.model.merge(stale.toJson());
    stale.merge(group.model.toJson());
    CHECK(!group.model.isMember(group.bob.id()) && !group.model.isMember(group.carol.id()));
    CHECK(group.model.name == "friends" && stale.name == "friends");
    CHECK(group.model.revision() == stale.revision());
}

void separateJoinAttemptsOnTheSameConnectionHaveDistinctSignedRequests() {
    Group group;
    const auto intent = joinIntent(group.bob, group.invite, "bob", "tcp://127.0.0.1:9002",
                                   group.alice.id(), "unchanged TLS challenge");
    group.model.admit(group.alice, intent);
    group.model.change(group.bob, "leave", {});
    CHECK(!intent.value("request").toString().isEmpty());
    const auto retry = joinIntent(group.bob, group.invite, "bob", "tcp://127.0.0.1:9002",
                                  group.alice.id(), "unchanged TLS challenge",
                                  intent.value("request").toString());
    CHECK(intent == retry); // Retransmission of one attempt stays idempotent.
    const auto next = joinIntent(group.bob, group.invite, "bob", "tcp://127.0.0.1:9002",
                                 group.alice.id(), "unchanged TLS challenge");
    CHECK(intent != next);
    CHECK(group.model.acceptsIntent(group.alice.id(), next));
    CHECK(!group.model.acceptsIntent(group.alice.id(), intent));
    group.model.admit(group.alice, next);
    CHECK(Model::fromJson(group.model.toJson()).isMember(group.bob.id()));
}

void legacyJoinIntentsWithoutRequestIdsRemainValidAndReplayProtected() {
    Group group;
    auto legacy = joinIntent(group.bob, group.invite, "bob", "tcp://127.0.0.1:9002");
    legacy.remove("request");
    legacy.remove("proof");
    legacy.remove("signature");
    legacy["proof"] = QString::fromLatin1(
        QMessageAuthenticationCode::hash(canonical(legacy), group.code.toUtf8(),
                                         QCryptographicHash::Sha256).toHex());
    legacy = signedObject(legacy, group.bob, "disquisition/community-join/v1\n");
    CHECK(group.model.acceptsIntent(group.alice.id(), legacy));
    group.model.admit(group.alice, legacy);
    group.model = Model::fromJson(group.model.toJson());
    CHECK(group.model.isMember(group.bob.id()));
    group.model.change(group.bob, "leave", {});
    CHECK(!group.model.acceptsIntent(group.alice.id(), legacy));
    group.join(group.bob, "new client");
    CHECK(group.model.isMember(group.bob.id()));
}

void rejoiningThroughAStaleHostMergesTheSignedDepartureFirst() {
    Group group;
    group.join(group.bob, "bob");
    auto returningPeer = group.model;
    const auto oldSnapshot = group.model.toJson();
    returningPeer.change(group.bob, "leave", {});
    const auto newIntent = joinIntent(group.bob, group.invite, "returning bob",
                                     "tcp://127.0.0.1:9010");
    CHECK(group.model.isMember(group.bob.id())); // Host missed the departure.
    CHECK(group.model.merge(returningPeer.toJson()));
    CHECK(!group.model.isMember(group.bob.id()));
    CHECK(group.model.acceptsIntent(group.alice.id(), newIntent));
    group.model.admit(group.alice, newIntent);
    returningPeer.merge(group.model.toJson());
    CHECK(returningPeer.isMember(group.bob.id()));
    CHECK(returningPeer.members[group.bob.id()].endpoint == "tcp://127.0.0.1:9010");
    returningPeer.merge(oldSnapshot);
    CHECK(returningPeer.isMember(group.bob.id()));
    // A second leave/rejoin must supersede both departures after a restart.
    returningPeer = Model::fromJson(returningPeer.toJson());
    returningPeer.change(group.bob, "leave", {});
    group.model.merge(returningPeer.toJson());
    group.join(group.bob, "back again");
    returningPeer.merge(group.model.toJson());
    CHECK(returningPeer.isMember(group.bob.id()));
    CHECK(returningPeer.members[group.bob.id()].name == "back again");
}

void aHostCannotReplayTheOriginalJoinInsideSignedHistoryAfterALeave() {
    Group group;
    const auto intent = joinIntent(group.bob, group.invite, "bob", "tcp://127.0.0.1:9002");
    group.model.admit(group.alice, intent);
    group.model.change(group.bob, "leave", {});
    const auto rejoin = group.model.admit(group.alice,
        joinIntent(group.bob, group.invite, "bob again", "tcp://127.0.0.1:9002"));
    auto snapshot = group.model.toJson();
    auto operations = snapshot.value("operations").toArray();
    for (qsizetype index = 0; index < operations.size(); ++index) {
        if (operations[index].toObject() != rejoin)
            continue;
        auto replay = rejoin;
        replay["data"] = QJsonObject{{"intent", intent}};
        operations[index] = signedObject(replay, group.alice,
                                         "disquisition/community-operation/v1\n");
    }
    snapshot["operations"] = operations;
    CHECK(!Model::fromJson(snapshot).isMember(group.bob.id()));
}

void onlyOwnerCanDeleteAndTombstonesSurviveMergesAndPersistence() {
    Group group;
    group.join(group.bob, "bob");
    group.model.change(group.alice, "admin", {{"peer", group.bob.id()}, {"enabled", true}});
    rejects([&] { group.model.change(group.bob, "delete", {}); });
    const auto stale = group.model.toJson();
    auto forged = group.model;
    const auto deletion = forged.change(group.alice, "delete", {});
    auto forgedSnapshot = forged.toJson();
    auto operations = forgedSnapshot.value("operations").toArray();
    for (qsizetype index = 0; index < operations.size(); ++index) {
        if (operations[index].toObject() == deletion) {
            auto impersonation = deletion;
            impersonation["author"] = group.bob.id();
            operations[index] = signedObject(impersonation, group.bob,
                "disquisition/community-operation/v1\n");
        }
    }
    forgedSnapshot["operations"] = operations;
    CHECK(!Model::fromJson(forgedSnapshot).deleted);
    group.model.change(group.alice, "delete", {});
    CHECK(group.model.deleted);
    CHECK(!group.model.isMember(group.alice.id()) && !group.model.canAdmin(group.alice.id()));
    CHECK(!group.model.validCode(group.code));
    CHECK(group.model.host(QSet<QString>{group.alice.id(), group.bob.id()}).isEmpty());
    rejects([&] { group.model.change(group.alice, "rename", {{"name", "resurrect"}}); });
    rejects([&] { group.model.admit(group.alice,
        joinIntent(group.carol, group.invite, "carol", "tcp://127.0.0.1:9003")); });
    auto returning = Model::fromJson(stale);
    returning.merge(group.model.toJson());
    CHECK(returning.deleted);
    group.model.merge(stale);
    CHECK(group.model.deleted);
    QTemporaryDir directory;
    {
        Store store(directory.path());
        store.save(group.model);
    }
    {
        Store store(directory.path());
        CHECK(store.load().value(group.model.id).deleted);
    }
}

} // namespace
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    relayAddressesAcceptHostnamesAndRejectOtherProtocols();
    identityAndInviteIntegrity();
    memberHostsAdmitWithoutAdmins();
    profilesAreOwnedByTheirAuthorAndPersist();
    rejectedAdmissionsAreAtomic();
    forgedAdminOperationsCannotBypassPermissions();
    reconnectMergesRevocationsBeforeAdmission();
    splitsConvergeAndRevocationsWinConcurrentAdmissions();
    demotionBlocksConcurrentAndFutureAdminEdits();
    persistsMetadataWithoutChatHistory();
    removingHostPreservesItsPreviouslyAcceptedAdmissions();
    kickingAllowsFreshAdmissionWithoutRestoringRole();
    legacyBansCanBeLiftedWithoutRestoringMembership();
    concurrentKickRejectsHostActionsAndAdmissions();
    concurrentBanWinsUnbanAndStaleReadmission();
    leavingOnlyRemovesTheAuthorAndAllowsFreshAdmission();
    leavingBlocksConcurrentHostAdmissionsAndEdits();
    separateJoinAttemptsOnTheSameConnectionHaveDistinctSignedRequests();
    legacyJoinIntentsWithoutRequestIdsRemainValidAndReplayProtected();
    rejoiningThroughAStaleHostMergesTheSignedDepartureFirst();
    aHostCannotReplayTheOriginalJoinInsideSignedHistoryAfterALeave();
    onlyOwnerCanDeleteAndTombstonesSurviveMergesAndPersistence();
    std::puts("community_model_test: ok");
}
