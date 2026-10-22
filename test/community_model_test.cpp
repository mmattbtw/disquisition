#include <QCoreApplication>
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
                      joinIntent(group.carol, group.invite, "carol", "relay://relay.example:3334"));
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
} // namespace
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    identityAndInviteIntegrity();
    memberHostsAdmitWithoutAdmins();
    rejectedAdmissionsAreAtomic();
    forgedAdminOperationsCannotBypassPermissions();
    reconnectMergesRevocationsBeforeAdmission();
    splitsConvergeAndRevocationsWinConcurrentAdmissions();
    demotionBlocksConcurrentAndFutureAdminEdits();
    persistsMetadataWithoutChatHistory();
    removingHostPreservesItsPreviouslyAcceptedAdmissions();
    std::puts("community_model_test: ok");
}
