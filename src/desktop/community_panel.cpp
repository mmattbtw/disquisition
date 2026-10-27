#include "desktop/community_panel.h"

#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileOpenEvent>
#include <QFormLayout>
#include <QImageReader>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMediaDevices>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QNetworkAccessManager>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyle>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>
#include <cmath>

#include "community/qr.h"
#include "desktop/audio_permission.h"
#include "desktop/chat_media.h"
#include "desktop/chat_color.h"
#include "desktop/community_tree.h"
#include "desktop/penguin_overlay.h"
#include "desktop/penguin_sprites.h"
#include "desktop/gif_picker.h"
#include "desktop/gif_search.h"
#include "desktop/mention_completer.h"
#include "desktop/notification_center.h"
#include "desktop/remote_media.h"
#include "desktop/screen_stage.h"
#include "desktop/share_picker.h"

using community::freshId;
namespace {
const QStringList kChatColors{"pink", "mint", "butter", "periwinkle", "lilac", "aqua", "peach"};
bool validChatColor(const QString& value) {
    if (kChatColors.contains(value)) return true;
    if (value.isEmpty() || value.size() > 3) return false;
    for (const auto character : value)
        if (character < QLatin1Char('0') || character > QLatin1Char('9')) return false;
    const int index = value.toInt();
    return index <= 255 && index != 1 && index != 2 && index != 250;
}
QString packetColor(const QJsonObject& data) {
    const auto color = data.value("color").toString();
    return validChatColor(color) ? color : QStringLiteral("mint");
}

QString key(const QString& cid, const QString& channel) {
    return cid + '/' + channel;
}
QString text(const std::string& value) {
    return QString::fromStdString(value);
}
constexpr double kVoiceActivityLevel = 180; // RMS, about -45 dBFS.
constexpr int kVoiceActivityHoldFrames = 10;
void paintSpeaking(QWidget* row, bool speaking) {
    row->setStyleSheet(speaking ? QStringLiteral("background-color: rgba(94, 190, 126, 70);"
                                                 "border-radius: 6px;")
                                : QString());
}
QPushButton* button(const QString& title, const char* name, QWidget* parent) {
    auto* result = new QPushButton(title, parent);
    result->setObjectName(name);
    return result;
}
QList<community::Channel> orderedChannels(const community::Model& model) {
    auto channels = model.channels.values();
    std::sort(channels.begin(), channels.end(), [](const auto& a, const auto& b) {
        if (a.type != b.type)
            return a.type == "text";
        if (a.name == "general" || b.name == "general")
            return a.name == "general" && b.name != "general";
        const int order = QString::compare(a.name, b.name, Qt::CaseInsensitive);
        return order == 0 ? a.id < b.id : order < 0;
    });
    return channels;
}
QPixmap voiceIndicator(const QString& kind) {
    QPixmap image(16, 16);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(kind == "sharing screen" ? QColor("#9dccca") : QColor("#aebcbf"), 1.5));
    if (kind == "muted") {
        painter.drawRoundedRect(QRectF(6, 1, 4, 8), 2, 2);
        painter.drawArc(QRectF(4, 4, 8, 7), 180 * 16, 180 * 16);
        painter.drawLine(8, 11, 8, 14);
        painter.drawLine(5, 14, 11, 14);
    }
    else if (kind == "deafened") {
        painter.drawArc(QRectF(3, 2, 10, 12), 0, 180 * 16);
        painter.drawRoundedRect(QRectF(2, 8, 3, 6), 1, 1);
        painter.drawRoundedRect(QRectF(11, 8, 3, 6), 1, 1);
    }
    else {
        painter.drawRect(QRectF(2, 2, 12, 9));
        painter.drawLine(8, 11, 8, 14);
        painter.drawLine(5, 14, 11, 14);
    }
    if (kind != "sharing screen")
        painter.drawLine(2, 1, 14, 15);
    return image;
}
QIcon voiceChannelIcon(qreal scale) {
    QPixmap image(QSize(20, 20) * scale);
    image.setDevicePixelRatio(scale);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor("#bed9d4"), 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    QPainterPath speaker;
    speaker.moveTo(3, 8);
    speaker.lineTo(6, 8);
    speaker.lineTo(10, 4);
    speaker.lineTo(10, 16);
    speaker.lineTo(6, 12);
    speaker.lineTo(3, 12);
    speaker.closeSubpath();
    painter.drawPath(speaker);
    painter.drawArc(QRectF(9, 6, 6, 8), -60 * 16, 120 * 16);
    painter.drawArc(QRectF(9, 3, 10, 14), -60 * 16, 120 * 16);
    painter.end();
    QIcon icon;
    for (const auto mode : {QIcon::Normal, QIcon::Active, QIcon::Selected, QIcon::Disabled})
        icon.addPixmap(image, mode);
    return icon;
}
QPixmap profileAvatar(const community::Member& member, int size, qreal scale) {
    QPixmap result(QSize(size, size) * scale);
    result.setDevicePixelRatio(scale);
    result.fill(Qt::transparent);
    QPainter painter(&result);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    QPainterPath circle;
    circle.addEllipse(QRectF(0, 0, size, size));
    painter.setClipPath(circle);
    QImage decoded;
    if (member.avatar.size() <= community::kMaxAvatarBytes) {
        ImageBytes source(member.avatar);
        const auto dimensions = source.size();
        if (source.canRead() && dimensions.width() > 0 && dimensions.height() > 0 &&
            dimensions.width() <= 256 && dimensions.height() <= 256)
            decoded = source.read();
    }
    if (!decoded.isNull()) {
        const int side = std::min(decoded.width(), decoded.height());
        painter.drawImage(QRectF(0, 0, size, size), decoded,
                          QRectF((decoded.width() - side) / 2, (decoded.height() - side) / 2, side,
                                 side));
    }
    else {
        painter.fillRect(QRect(0, 0, size, size), QColor("#394f52"));
        painter.setPen(QColor("#d5eeea"));
        auto font = painter.font();
        font.setPixelSize(size * 0.44);
        font.setWeight(QFont::DemiBold);
        painter.setFont(font);
        painter.drawText(QRect(0, 0, size, size), Qt::AlignCenter,
                         member.name.trimmed().left(1).toUpper());
    }
    return result;
}
} // namespace

CommunityPanel::CommunityPanel(QWidget* parent, const QString& directory) : QWidget(parent), pushToTalk_(this) {
    try {
        const auto override = qApp->property("communityDataDirectory").toString();
        const auto path = !directory.isEmpty() ? directory
                          : !override.isEmpty()
                              ? override
                              : QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                                    "/communities";
        node_ = std::make_unique<community::Node>(path, this);
    }
    catch (const std::exception& error) {
        failure_ = QString::fromUtf8(error.what());
    }
    messageColor_ = kChatColors[QRandomGenerator::global()->bounded(kChatColors.size())];
    QSettings penguinSettings;
    penguins_ = new PenguinOverlay(this);
    penguins_->setEnabled(penguinSettings.value("penguins/unlocked", false).toBool() &&
                          penguinSettings.value("penguins/enabled", false).toBool());
    penguins_->setWandering(penguinSettings.value("penguins/wander", true).toBool());
    penguins_->setIdleTimeoutMinutes(penguinSettings.value("penguins/idleMinutes", 10).toInt());
    penguinsAction_ = new QAction("Desktop penguins", this);
    penguinsAction_->setObjectName("desktopPenguins");
    penguinsAction_->setCheckable(true);
    penguinsAction_->setVisible(penguinSettings.value("penguins/unlocked", false).toBool());
    penguinsAction_->setChecked(penguinsAction_->isVisible() &&
                               penguinSettings.value("penguins/enabled", false).toBool());
    connect(penguinsAction_, &QAction::toggled, this, [this](bool enabled) {
        penguins_->setEnabled(enabled);
        QSettings().setValue("penguins/enabled", enabled);
    });
    notifications_ = new NotificationCenter(window());
    buildUi();
    connect(notifications_, &NotificationCenter::settingsChanged, this,
            [this](const NotificationSettings& settings) {
                if (settings.muted)
                    unread_.clear();
                refresh();
            });
    connect(&pushToTalk_, &PushToTalk::heldChanged, this, &CommunityPanel::voiceState);
    imagePool_.setMaxThreadCount(1);
    if (node_) {
        connect(node_.get(), &community::Node::changed, this, [this] {
            if (refreshQueued_)
                return;
            refreshQueued_ = true;
            QTimer::singleShot(0, this, [this] {
                refreshQueued_ = false;
                refresh();
            });
        });
        connect(node_.get(), &community::Node::joined, this, [this](const QString& cid) {
            joiningCommunities_.remove(cid);
            updateJoining();
            selectCommunity(cid);
            note("Community connected. Messages are delivered live.");
        });
        connect(node_.get(), &community::Node::joinRejected, this,
                [this](const QString& cid, const QString&) {
                    joiningCommunities_.remove(cid);
                    updateJoining();
                    noteError("Invite rejected. Check the code with the community owner.");
                });
        connect(node_.get(), &community::Node::errorOccurred, this, &CommunityPanel::noteError);
        connect(node_.get(), &community::Node::relayConnectionChanged, this,
                [this](bool connected) {
                    note(connected ? "Relay connected."
                                   : "Relay connection lost. Reconnecting automatically…");
                });
        connect(node_.get(), &community::Node::packetReceived, this, &CommunityPanel::receive);
        connect(node_.get(), &community::Node::voiceMoved, this,
                [this](const QString& cid, const QString& channel) {
                    // The node has already changed presence. End subscriptions and capture
                    // from the previous channel before refreshing the existing audio session.
                    // Stop capture before stopSharing so it cannot republish the
                    // previous voice channel while applying the administrator's move.
                    capture_.stop();
                    stopSharing();
                    const auto watching = watching_;
                    for (const auto& peer : watching) watch(peer, false);
                    for (const auto& peer : streams_) {
                        stopDrawing(peer);
                        stage_->removeStream(peer);
                    }
                    streams_.clear();
                    watching_.clear();
                    lastScreenFrames_.clear();
                    frames_ = ScreenFrameAssembler{};
                    decoders_.clear();
                    voiceCid_ = cid;
                    voiceChannel_ = channel;
                    selectedVoiceCid_ = cid;
                    selectedVoiceChannel_ = channel;
                    refresh();
                    note("An administrator moved you to " +
                         node_->communities().value(cid).channels.value(channel).name + '.');
                });
    }
    qApp->installEventFilter(this);
    imageTimer_.setInterval(2);
    speakingTimer_.setInterval(500);
    connect(&speakingTimer_, &QTimer::timeout, this, [this] {
        if (!node_ || voiceCid_.isEmpty())
            return;
        const auto cutoff = QDateTime::currentMSecsSinceEpoch() - 1000;
        QStringList expired;
        for (const auto& peer : speaking_)
            if (peer != node_->peerId() && lastAudioAt_.value(peer) < cutoff)
                expired.append(peer);
        for (const auto& peer : expired) {
            setSpeaking(peer, false);
            quietAudioFrames_.remove(peer);
        }
    });
    speakingTimer_.start();
    connect(&imageTimer_, &QTimer::timeout, this, [this] {
        if (!online_ || uploads_.isEmpty()) {
            imageTimer_.stop();
            return;
        }
        if (node_->mediaBacklog() > 64 * 1024)
            return;
        auto& upload = uploads_.first();
        attempt([&] {
            node_->sendPacket(upload.cid, upload.channel, "image",
                              {{"transfer", double(upload.transfer)},
                               {"index", upload.next},
                               {"count", upload.chunks.size()},
                               {"color", upload.color},
                               {"bytes",
                                QString::fromLatin1(upload.chunks[upload.next].toBase64())}});
        });
        if (++upload.next == upload.chunks.size())
            uploads_.removeFirst();
    });
    connect(&audio_, &RelayAudio::frameReady, this, [this](const QByteArray& pcm) {
        if (online_ && !voiceCid_.isEmpty())
            attempt([&] {
                node_->sendPacket(voiceCid_, voiceChannel_, "audio",
                                  {{"bytes", QString::fromLatin1(pcm.toBase64())}});
            });
    });
    connect(&audio_, &RelayAudio::speakingChanged, this, [this](bool speaking) {
        if (node_ && !voiceCid_.isEmpty())
            setSpeaking(node_->peerId(), speaking);
    });
    capture_.setBacklogProbe([this] { return node_ ? node_->mediaBacklog() : qint64(0); });
    connect(&capture_, &ScreenCapture::frameReady, this,
            [this](quint32 frame, const QList<QByteArray>& chunks, bool keyframe) {
                if (!online_ || voiceCid_.isEmpty())
                    return;
                QByteArray encoded;
                for (const auto& chunk : chunks)
                    encoded += chunk;
                auto image = decoders_[node_->peerId()].decode(frame, keyframe, encoded);
                if (!image.isNull())
                    stage_->showFrame(node_->peerId(), image, encoded.size());
                const auto watchers = watchers_;
                for (const auto& peer : watchers)
                    attempt([&] {
                        node_->sendScreenFrame(voiceCid_, voiceChannel_, peer, frame, chunks,
                                               keyframe);
                    });
            });
    streamRetry_.setInterval(2000);
    connect(&streamRetry_, &QTimer::timeout, this, [this] {
        if (!online_ || voiceCid_.isEmpty())
            return;
        const auto now = QDateTime::currentMSecsSinceEpoch();
        for (const auto& peer : watching_)
            if (node_->online(voiceCid_).contains(peer) &&
                node_->presence(voiceCid_, peer).sharing &&
                now - lastScreenFrames_.value(peer) > 5000)
                attempt([&] {
                    node_->sendPacket(voiceCid_, voiceChannel_, "watch",
                                      {{"target", peer}, {"enabled", true}});
                });
    });
    connect(&capture_, &ScreenCapture::errorOccurred, this, [this](const QString& error) {
        stopSharing();
        noteError(error);
    });
    refresh();
    render();
    for (const auto& argument : QCoreApplication::arguments())
        if (argument.startsWith("disquisition://join/"))
            pendingLink_ = argument;
}

CommunityPanel::~CommunityPanel() {
    qApp->removeEventFilter(this);
    leaveVoice();
}

void CommunityPanel::attempt(const std::function<void()>& action) {
    try {
        action();
    }
    catch (const std::exception& error) {
        noteError(QString::fromUtf8(error.what()));
    }
}
void CommunityPanel::note(const QString& message) {
    // Routine status stays out of the footer; only failures surface there.
    Q_UNUSED(message);
    status_->clear();
    status_->hide();
}
void CommunityPanel::noteError(const QString& message) {
    status_->setText(message);
    status_->setToolTip(message);
    status_->setVisible(!message.isEmpty());
}
void CommunityPanel::updateJoining() {
    if (joining_)
        joining_->setVisible(!joiningCommunities_.isEmpty());
}
void CommunityPanel::setSpeaking(const QString& peer, bool speaking) {
    if (speaking == speaking_.contains(peer))
        return;
    if (speaking)
        speaking_.insert(peer);
    else
        speaking_.remove(peer);
    for (QTreeWidgetItemIterator item(tree_); *item; ++item) {
        if ((*item)->data(0, Qt::UserRole + 2).toString() != peer)
            continue;
        if (auto* row = tree_->itemWidget(*item, 0))
            paintSpeaking(row, speaking);
    }
}

void CommunityPanel::buildUi() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(8);
    auto* connection = new QHBoxLayout;
    name_ = new QLineEdit(this);
    name_->setObjectName("communityName");
    name_->setPlaceholderText("Your name");
    name_->setMaxLength(64);
    name_->setText(node_ && !node_->profile().name.isEmpty()
                       ? node_->profile().name
                       : QSettings().value("community/name").toString());
    connect_ = button("Connect", "communityConnect", this);
    auto* settingsButton = button("User Settings", "communitySettings", this);
    connection->addWidget(name_, 1);
    connection->addWidget(connect_);
    connection->addWidget(settingsButton);
    layout->addLayout(connection);
    connect(connect_, &QPushButton::clicked, this, &CommunityPanel::connectNode);
    connect(name_, &QLineEdit::returnPressed, this, &CommunityPanel::connectNode);
    connect(settingsButton, &QPushButton::clicked, this, &CommunityPanel::settings);
    auto* split = new QSplitter(this);
    auto* sidebar = new QWidget(split);
    auto* side = new QVBoxLayout(sidebar);
    side->setContentsMargins(0, 0, 4, 0);
    auto* actions = new QHBoxLayout;
    create_ = button("Create", "communityCreate", sidebar);
    join_ = button("Join", "communityJoin", sidebar);
    actions->addWidget(create_);
    actions->addWidget(join_);
    side->addLayout(actions);
    tree_ = new CommunityTree(sidebar);
    tree_->setObjectName("communityTree");
    tree_->setHeaderHidden(true);
    tree_->setMinimumWidth(245);
    tree_->setIndentation(16);
    tree_->setIconSize(QSize(20, 20));
    joining_ = new QWidget(sidebar);
    joining_->setObjectName("communityJoining");
    auto* joiningLayout = new QVBoxLayout(joining_);
    joiningLayout->setContentsMargins(0, 4, 0, 4);
    joiningLayout->setSpacing(4);
    auto* joiningLabel = new QLabel("Joining community…", joining_);
    joiningLabel->setObjectName("communityJoiningLabel");
    joiningLabel->setStyleSheet("color:#9dccca;");
    joiningLayout->addWidget(joiningLabel);
    auto* joiningProgress = new QProgressBar(joining_);
    joiningProgress->setObjectName("communityJoiningProgress");
    joiningProgress->setRange(0, 0);
    joiningProgress->setTextVisible(false);
    joiningProgress->setFixedHeight(4);
    joiningLayout->addWidget(joiningProgress);
    joining_->hide();
    side->addWidget(joining_);
    side->addWidget(tree_, 1);
    connect(static_cast<CommunityTree*>(tree_), &CommunityTree::voiceMoveRequested, this,
            [this](const QString& cid, const QString& peer, const QString& channel) {
                attempt([&] { node_->moveVoice(cid, peer, channel); });
            });
    connect(create_, &QPushButton::clicked, this, &CommunityPanel::create);
    connect(join_, &QPushButton::clicked, this, &CommunityPanel::join);
    connect(tree_, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* item) { select(item); });
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree_, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& position) {
        auto* item = tree_->itemAt(position);
        if (!item || item->parent() || !node_)
            return;
        const auto target = item->data(0, Qt::UserRole).toString();
        const auto model = node_->communities().value(target);
        if (target.isEmpty() || !model.isMember(node_->peerId()) ||
            model.owner == node_->peerId())
            return;
        QMenu menu(this);
        leaveAction_->setData(target);
        leaveAction_->setVisible(true);
        leaveAction_->setEnabled(true);
        menu.addAction(leaveAction_);
        menu.exec(tree_->viewport()->mapToGlobal(position));
        leaveAction_->setData({});
        refresh();
    });
    leaveAction_ = new QAction("Leave community…", this);
    leaveAction_->setObjectName("communityLeave");
    connect(leaveAction_, &QAction::triggered, this, [this] {
        const auto target = leaveAction_->data().toString();
        leaveAction_->setData({});
        leaveCommunity(target);
    });
    auto* conversation = new QWidget(split);
    auto* chat = new QVBoxLayout(conversation);
    chat->setContentsMargins(4, 0, 0, 0);
    auto* headingRow = new QHBoxLayout;
    heading_ = new QLabel("Your communities", conversation);
    heading_->setStyleSheet("font-size:17px; font-weight:700;");
    heading_->setTextFormat(Qt::PlainText);
    invite_ = button("Invite", "communityInvite", conversation);
    manage_ = button("Manage", "communityManage", conversation);
    headingRow->addWidget(heading_, 1);
    headingRow->addWidget(invite_);
    headingRow->addWidget(manage_);
    chat->addLayout(headingRow);
    topic_ = new QLabel("Create a community or join with an invite link or QR image.",
                        conversation);
    topic_->setTextFormat(Qt::PlainText);
    topic_->setWordWrap(true);
    topic_->setStyleSheet("color:#9aabb4;");
    chat->addWidget(topic_);
    connect(invite_, &QPushButton::clicked, this, &CommunityPanel::invite);
    connect(manage_, &QPushButton::clicked, this, &CommunityPanel::manage);
    auto* content = new QSplitter(conversation);
    auto* center = new QSplitter(Qt::Vertical, content);
    stage_ = new ScreenStage(center);
    transcript_ = new QTextBrowser(center);
    transcript_->setObjectName("communityTranscript");
    transcript_->setMinimumHeight(120);
    center->addWidget(stage_);
    center->addWidget(transcript_);
    center->setStretchFactor(0, 2);
    center->setStretchFactor(1, 1);
    members_ = new QListWidget(content);
    members_->setObjectName("communityMembers");
    members_->setMinimumWidth(190);
    members_->setContextMenuPolicy(Qt::CustomContextMenu);
    members_->setIconSize(QSize(32, 32));
    connect(members_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        showProfile(cid_, item->data(Qt::UserRole).toString(),
                    members_->viewport()->mapToGlobal(members_->visualItemRect(item).topLeft()));
    });
    connect(members_, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
        showProfile(cid_, item->data(Qt::UserRole).toString(),
                    members_->viewport()->mapToGlobal(members_->visualItemRect(item).topLeft()));
    });
    connect(members_, &QListWidget::customContextMenuRequested, this, [this](const QPoint& position) {
        auto* item = members_->itemAt(position);
        if (!item || !capture_.running() || cid_ != voiceCid_)
            return;
        const auto peer = item->data(Qt::UserRole).toString();
        const auto own = drawings_.find(node_->peerId());
        if (peer == node_->peerId() || own == drawings_.end())
            return;
        const bool banned = own->second->isBanned(peer);
        QMenu menu(this);
        auto* toggle = menu.addAction(banned ? "Unban from drawing on my stream"
                                            : "Ban from drawing on my stream");
        if (menu.exec(members_->viewport()->mapToGlobal(position)) == toggle) {
            const auto session = drawings_.find(node_->peerId());
            if (session != drawings_.end())
                session->second->setBanned(peer, !banned);
        }
    });
    content->addWidget(center);
    content->addWidget(members_);
    content->setStretchFactor(0, 1);
    content->setSizes({600, 190});
    chat->addWidget(content, 1);
    split->addWidget(sidebar);
    split->addWidget(conversation);
    split->setStretchFactor(1, 1);
    split->setSizes({260, 760});
    layout->addWidget(split, 1);
    voiceControls_ = new QWidget(this);
    voiceControls_->setObjectName("communityVoiceControls");
    auto* voiceRow = new QHBoxLayout(voiceControls_);
    voiceRow->setContentsMargins(10, 6, 10, 6);
    voiceStatus_ = new QLabel(voiceControls_);
    voiceStatus_->setObjectName("communityVoiceStatus");
    voiceStatus_->setTextFormat(Qt::PlainText);
    voice_ = button("Leave voice", "communityVoice", voiceControls_);
    mute_ = button("Mute", "communityMute", voiceControls_);
    deafen_ = button("Deafen", "communityDeafen", voiceControls_);
    share_ = button("Share screen", "communityShare", voiceControls_);
    mute_->setCheckable(true);
    deafen_->setCheckable(true);
    voiceRow->addWidget(voiceStatus_, 1);
    voiceRow->addWidget(voice_);
    voiceRow->addWidget(mute_);
    voiceRow->addWidget(deafen_);
    voiceRow->addWidget(share_);
    layout->addWidget(voiceControls_);
    voiceControls_->hide();
    connect(voice_, &QPushButton::clicked, this, &CommunityPanel::leaveVoice);
    connect(mute_, &QPushButton::toggled, this, &CommunityPanel::voiceState);
    connect(deafen_, &QPushButton::toggled, this, &CommunityPanel::voiceState);
    connect(share_, &QPushButton::clicked, this, &CommunityPanel::share);
    connect(stage_, &ScreenStage::watchRequested, this,
            [this](const QString& peer) { watch(peer, true); });
    connect(stage_, &ScreenStage::stopWatchingRequested, this,
            [this](const QString& peer) { watch(peer, false); });
    auto* compose = new QHBoxLayout;
    composer_ = new QLineEdit(this);
    composer_->setObjectName("communityComposer");
    composer_->setPlaceholderText("Message this channel");
    mentions_ = new MentionCompleter(composer_);
    send_ = button("Send", "communitySend", this);
    image_ = button("Image", "communityImage", this);
    gif_ = button("GIF", "communityGif", this);
    compose->addWidget(composer_, 1);
    compose->addWidget(image_);
    compose->addWidget(gif_);
    compose->addWidget(send_);
    layout->addLayout(compose);
    connect(send_, &QPushButton::clicked, this, &CommunityPanel::sendChat);
    connect(composer_, &QLineEdit::returnPressed, this, &CommunityPanel::sendChat);
    connect(image_, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getOpenFileName(
            this, "Send an image", {}, "Images (*.png *.jpg *.jpeg *.gif *.webp *.bmp)");
        if (!path.isEmpty())
            prepareUpload([path] { return prepareImageFile(path); });
    });
    status_ = new QLabel(this);
    status_->setObjectName("communityStatus");
    status_->setWordWrap(true);
    status_->hide();
    layout->addWidget(status_);
    addAction(notifications_->muteAction());
    status_->setTextFormat(Qt::PlainText);
    auto* network = new QNetworkAccessManager(this);
    auto* fetcher = new MediaFetcher(network, this);
    media_ = new ChatMedia(transcript_, fetcher, this);
    connect(transcript_, &QTextBrowser::anchorClicked, this, [this](const QUrl& link) {
        if (link.scheme() == "profile")
            showProfile(link.host(), link.path().mid(1), QCursor::pos());
    });
    gifSearch_ = new GifSearch(network, this);
    QSettings settings;
    auto customer = settings.value("gifs/klipyCustomerId").toString();
    if (customer.isEmpty()) {
        customer = freshId();
        settings.setValue("gifs/klipyCustomerId", customer);
    }
    gifSearch_->setKeys(DISQUISITION_GIPHY_API_KEY, DISQUISITION_KLIPY_API_KEY, customer);
    gifPicker_ = new GifPicker(gifSearch_, fetcher, this);
    connect(gif_, &QPushButton::clicked, this, [this] { gifPicker_->popup(gif_); });
    connect(gifPicker_, &GifPicker::chosen, this, [this](const QUrl& url) {
        composer_->setText(url.toString());
        sendChat();
    });
    gif_->setVisible(gifSearch_->hasKey(GifProvider::Giphy) ||
                     gifSearch_->hasKey(GifProvider::Klipy));
    setAcceptDrops(true);
}

void CommunityPanel::connectNode() {
    if (!node_) {
        if (!failure_.isEmpty())
            noteError(failure_);
        return;
    }
    if (online_) {
        leaveVoice();
        node_->stop();
        online_ = false;
        uploads_.clear();
        images_.clear();
        joiningCommunities_.clear();
        updateJoining();
        note("Disconnected. Saved communities remain available.");
    }
    else {
        QSettings settings;
        const auto port = settings.value("community/port", 0).toUInt();
        online_ =
            node_->start(name_->text().trimmed(), settings.value("community/host").toString(),
                         static_cast<quint16>(port),
                         settings.value("community/relay", "relay.disquisition.app").toString());
        if (online_) {
            settings.setValue("community/name", name_->text().trimmed());
            joiningCommunities_ = node_->pendingCommunities();
            updateJoining();
            openHistory();
            const QUrl endpoint(node_->endpoint());
            note(endpoint.scheme() == "relay"
                     ? "Using relay " + endpoint.authority() +
                           ". Communities reconnect automatically."
                     : "Listening for direct connections at " + endpoint.authority());
            if (!pendingLink_.isEmpty()) {
                const auto link = pendingLink_;
                pendingLink_.clear();
                acceptInvite(link);
            }
        }
    }
    refresh();
}

void CommunityPanel::settings() {
    QSettings saved;
    QDialog dialog(this);
    dialog.setObjectName("userSettings");
    dialog.setWindowTitle("User Settings");
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* body = new QWidget(&dialog);
    auto* columns = new QHBoxLayout(body);
    columns->setContentsMargins(0, 0, 0, 0);
    columns->setSpacing(0);
    auto* categories = new QListWidget(body);
    categories->setObjectName("userSettingsCategories");
    categories->setAccessibleName("Settings categories");
    categories->setFixedWidth(180);
    auto* pages = new QStackedWidget(body);
    pages->setObjectName("userSettingsPages");
    columns->addWidget(categories);
    columns->addWidget(pages, 1);
    layout->addWidget(body, 1);
    const auto page = [&](const QString& title, const QString& description) {
        categories->addItem(title);
        auto* scroll = new QScrollArea(pages);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto* content = new QWidget(scroll);
        auto* section = new QVBoxLayout(content);
        section->setContentsMargins(28, 24, 28, 24);
        section->setSpacing(16);
        auto* heading = new QLabel(title, content);
        heading->setObjectName("settingsHeading");
        section->addWidget(heading);
        auto* help = new QLabel(description, content);
        help->setObjectName("settingsDescription");
        help->setWordWrap(true);
        section->addWidget(help);
        auto* form = new QFormLayout;
        form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        form->setHorizontalSpacing(16);
        form->setVerticalSpacing(16);
        section->addLayout(form);
        section->addStretch();
        scroll->setWidget(content);
        pages->addWidget(scroll);
        return form;
    };
    auto* userForm = page("User", "Your profile is shared with members of your communities.");
    auto* showPenguins = new QCheckBox("Show desktop penguins for this community", &dialog);
    showPenguins->setObjectName("communityShowPenguins");
    showPenguins->setChecked(penguinsAction_->isChecked());
    showPenguins->setToolTip("Penguins appear above other apps and let clicks pass through.");
    auto* wander = new QCheckBox("Let penguins wander", &dialog);
    wander->setChecked(saved.value("penguins/wander", true).toBool());
    auto* idle = new QSpinBox(&dialog);
    idle->setRange(0, 1440);
    idle->setSuffix(" min");
    idle->setSpecialValueText("Never");
    idle->setValue(qBound(0, saved.value("penguins/idleMinutes", 10).toInt(), 1440));
    auto* spriteEditor = new PenguinSpriteEditor(PenguinSprites::fromSettings(saved), &dialog);
    spriteEditor->setObjectName("communitySpriteEditor");
    userForm->addRow(showPenguins);
    userForm->addRow(wander);
    userForm->addRow("Hide inactive penguins after", idle);
    userForm->addRow("Custom sprite", spriteEditor);
    const bool unlocked = saved.value("penguins/unlocked", false).toBool();
    for (QWidget* widget : QList<QWidget*>{showPenguins, wander, idle, spriteEditor})
        userForm->setRowVisible(widget, unlocked);
    wander->setEnabled(showPenguins->isChecked());
    idle->setEnabled(showPenguins->isChecked());
    connect(showPenguins, &QCheckBox::toggled, wander, &QWidget::setEnabled);
    connect(showPenguins, &QCheckBox::toggled, idle, &QWidget::setEnabled);
    auto* audioForm = page("Audio", "Choose your microphone, output device, and how you talk.");
    auto* notificationForm = page("Notifications",
                                  "Choose which messages and activity get your attention.");
    auto* connectionForm =
        page("Connection",
             online_ ? "Disconnect before changing your relay or direct connection."
                     : "Use a relay to connect, or leave Relay blank for direct connections.");
    auto* historyForm = page("Message History",
                             "Optionally save chat on this device. Missed messages are not "
                             "downloaded. Community metadata is always saved.");
    connect(categories, &QListWidget::currentRowChanged, pages, &QStackedWidget::setCurrentIndex);
    categories->setCurrentRow(0);

    const auto current = node_ ? node_->profile() : community::Profile{};
    QByteArray avatar = current.avatar;
    auto* displayName = new QLineEdit(current.name.isEmpty() ? name_->text() : current.name,
                                      &dialog);
    displayName->setObjectName("userDisplayName");
    displayName->setMaxLength(64);
    displayName->setPlaceholderText("How people see you");
    auto* bio = new QPlainTextEdit(current.bio, &dialog);
    bio->setObjectName("userBio");
    bio->setAccessibleName("Bio");
    bio->setPlaceholderText("A little about you");
    bio->setMinimumHeight(104);
    bio->setMaximumHeight(144);
    auto* count = new QLabel(&dialog);
    count->setObjectName("userBioCount");
    count->setAlignment(Qt::AlignRight);
    const auto updateCount = [=] {
        const auto length = bio->toPlainText().size();
        count->setText(QString::number(length) + " / " + QString::number(community::kMaxBioLength));
        count->setStyleSheet(length > community::kMaxBioLength ? "color:#ffb3b3;"
                                                               : "color:#a6b5bd;");
    };
    connect(bio, &QPlainTextEdit::textChanged, &dialog, updateCount);
    updateCount();
    auto* picture = new QWidget(&dialog);
    auto* pictureRow = new QHBoxLayout(picture);
    pictureRow->setContentsMargins(0, 0, 0, 0);
    pictureRow->setSpacing(16);
    auto* preview = new QLabel(picture);
    preview->setObjectName("userAvatarPreview");
    preview->setFixedSize(80, 80);
    preview->setStyleSheet("padding:0; background:transparent;");
    auto* pictureActions = new QVBoxLayout;
    auto* choose = button("Change picture", "userChooseAvatar", picture);
    auto* remove = button("Remove picture", "userRemoveAvatar", picture);
    pictureActions->addWidget(choose);
    pictureActions->addWidget(remove);
    pictureRow->addWidget(preview);
    pictureRow->addLayout(pictureActions);
    pictureRow->addStretch();
    const auto updatePreview = [&] {
        community::Member member;
        member.name = displayName->text();
        member.avatar = avatar;
        preview->setPixmap(profileAvatar(member, 80, devicePixelRatioF()));
        remove->setEnabled(!avatar.isEmpty());
    };
    connect(displayName, &QLineEdit::textChanged, &dialog, updatePreview);
    connect(remove, &QPushButton::clicked, &dialog, [&] {
        avatar.clear();
        updatePreview();
    });
    updatePreview();
    userForm->addRow("Profile picture", picture);
    userForm->addRow("Display name", displayName);
    userForm->addRow("Bio", bio);
    userForm->addRow(count);
    auto* footerWidget = new QWidget(&dialog);
    auto* footer = new QHBoxLayout(footerWidget);
    footer->setContentsMargins(20, 16, 24, 16);
    auto* error = new QLabel(footerWidget);
    error->setObjectName("userSettingsError");
    error->setTextFormat(Qt::PlainText);
    error->setWordWrap(true);
    error->setStyleSheet("color:#ffb3b3;");
    error->hide();
    footer->addWidget(error, 1);
    connect(choose, &QPushButton::clicked, &dialog, [&] {
        const auto path = QFileDialog::getOpenFileName(&dialog, "Choose a profile picture", {},
                                                       "Images (*.png *.jpg *.jpeg *.webp *.bmp)");
        if (path.isEmpty())
            return;
        const auto prepared = prepareImageFile(path);
        auto image = QImage::fromData(prepared.bytes);
        if (image.isNull()) {
            error->setText(prepared.error.isEmpty() ? "This image could not be opened."
                                                    : prepared.error);
            error->show();
            return;
        }
        const int side = std::min(image.width(), image.height());
        image = image.copy((image.width() - side) / 2, (image.height() - side) / 2, side, side)
                    .scaled(128, 128, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "PNG");
        if (bytes.size() > community::kMaxAvatarBytes) {
            bytes.clear();
            buffer.seek(0);
            image.save(&buffer, "JPEG", 85);
        }
        if (bytes.isEmpty() || bytes.size() > community::kMaxAvatarBytes) {
            error->setText("This picture could not be prepared. Try another image.");
            error->show();
            return;
        }
        avatar = bytes;
        error->hide();
        updatePreview();
    });
    auto relayAddress = saved.value("community/relay", "relay.disquisition.app").toString();
    if (relayAddress.startsWith("relay://"))
        relayAddress.remove(0, 8);
    auto* relay = new QLineEdit(relayAddress, &dialog);
    relay->setObjectName("communityRelayAddress");
    relay->setPlaceholderText("relay.disquisition.app, or leave blank for direct connections");
    auto* host = new QLineEdit(saved.value("community/host").toString(), &dialog);
    host->setPlaceholderText("Automatic LAN address");
    auto* port = new QSpinBox(&dialog);
    port->setRange(0, 65535);
    port->setSpecialValueText("Automatic");
    port->setValue(saved.value("community/port", 0).toInt());
    relay->setEnabled(!online_);
    host->setEnabled(!online_);
    port->setEnabled(!online_);
    connectionForm->addRow("Relay", relay);
    connectionForm->addRow("Direct advertised host", host);
    connectionForm->addRow("Direct listen port", port);
    auto* saving = new QCheckBox("Save messages received or sent by this client", &dialog);
    saving->setObjectName("communitySaveMessages");
    saving->setChecked(saved.value("messages/enabled", false).toBool());
    auto* path = new QLineEdit(
        saved.value("messages/file", QDir::homePath() + "/disquisition-messages.db").toString(),
        &dialog);
    path->setObjectName("communityMessageFile");
    auto* maximum = new QLineEdit(saved.value("messages/maximum").toString(), &dialog);
    maximum->setPlaceholderText("Unlimited");
    auto* linkedMedia = new QCheckBox("Show linked GIFs and images in chat", &dialog);
    linkedMedia->setObjectName("communityShowLinkedMedia");
    linkedMedia->setChecked(saved.value("gifs/showLinkedMedia", true).toBool());
    historyForm->addRow(linkedMedia);
    historyForm->addRow(saving);
    historyForm->addRow("SQLite file", path);
    historyForm->addRow("Maximum saved messages", maximum);
    path->setEnabled(saving->isChecked());
    maximum->setEnabled(saving->isChecked());
    connect(saving, &QCheckBox::toggled, path, &QWidget::setEnabled);
    connect(saving, &QCheckBox::toggled, maximum, &QWidget::setEnabled);
    auto* input = new QComboBox(&dialog);
    auto* output = new QComboBox(&dialog);
    input->setObjectName("communityInputDevice");
    output->setObjectName("communityOutputDevice");
    input->addItem("System default", "");
    output->addItem("System default", "");
    for (const auto& device : QMediaDevices::audioInputs())
        input->addItem(device.description(), QString::fromUtf8(device.id()));
    for (const auto& device : QMediaDevices::audioOutputs())
        output->addItem(device.description(), QString::fromUtf8(device.id()));
    input->setCurrentIndex(std::max(0, input->findData(saved.value("community/input", ""))));
    output->setCurrentIndex(std::max(0, output->findData(saved.value("community/output", ""))));
    audioForm->addRow("Microphone", input);
    audioForm->addRow("Audio output", output);
    auto* audioHelp = new QLabel(voiceCid_.isEmpty()
                                     ? "Audio devices apply the next time you join voice."
                                     : "Changing audio devices reconnects audio without leaving "
                                       "the call.",
                                 &dialog);
    audioHelp->setWordWrap(true);
    audioForm->addRow(audioHelp);
    auto* ptt = new QCheckBox("Hold a key to talk", &dialog);
    ptt->setObjectName("communityPushToTalk");
    ptt->setChecked(saved.value("voice/pushToTalk", false).toBool());
    auto* pttKey = new PushToTalkKeyButton(&dialog);
    pttKey->setObjectName("communityPushToTalkKey");
    pttKey->setKey(saved.contains("voice/pushToTalkKey")
                       ? std::optional<quint32>(saved.value("voice/pushToTalkKey").toUInt())
                       : std::nullopt, saved.value("voice/pushToTalkKeyName").toString());
    pttKey->setEnabled(ptt->isChecked());
    connect(ptt, &QCheckBox::toggled, pttKey, &QWidget::setEnabled);
    audioForm->addRow(ptt, pttKey);
    auto* pttHelp = new QLabel("Global push-to-talk needs Input Monitoring on macOS. "
                              "Restart after granting access. On Wayland it works while the app is focused.", &dialog);
    pttHelp->setWordWrap(true);
    audioForm->addRow(pttHelp);
    auto* muted = new QCheckBox("Mute all notifications", &dialog);
    muted->setObjectName("communityMuteNotifications");
    muted->setChecked(notifications_->settings().muted);
    auto* level = new QComboBox(&dialog);
    level->addItem("All messages", int(NotifyLevel::AllMessages));
    level->addItem("Only @mentions", int(NotifyLevel::MentionsOnly));
    level->setCurrentIndex(level->findData(int(notifications_->settings().level)));
    auto* suppress = new QCheckBox("Suppress @everyone and @here", &dialog);
    suppress->setChecked(notifications_->settings().suppressEveryone);
    auto* activity = new QCheckBox("Notify on joins and screen shares", &dialog);
    activity->setChecked(notifications_->settings().activity);
    notificationForm->addRow(muted);
    notificationForm->addRow("Notify for", level);
    notificationForm->addRow(suppress);
    notificationForm->addRow(activity);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel,
                                         &dialog);
    footer->addWidget(buttons);
    layout->addWidget(footerWidget);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        try {
            const auto sprites = spriteEditor->sprites();
            if (unlocked && sprites.enabled && !sprites.complete())
                throw std::runtime_error("Upload both a standing and walking image, or turn custom sprites off.");
            const community::Profile profile{displayName->text().trimmed(), bio->toPlainText(),
                                             avatar};
            community::Profile::fromJson(profile.toJson());
            if (ptt->isChecked() && !pttKey->key())
                throw std::runtime_error("Choose a push-to-talk key before enabling it.");
            const auto address = relay->text().trimmed();
            community::normalizeRelayEndpoint(address);
            bool validMaximum = false;
            const auto count = maximum->text().toLongLong(&validMaximum);
            if (!maximum->text().trimmed().isEmpty() && (!validMaximum || count <= 0))
                throw std::runtime_error("Maximum messages must be a positive number or blank.");
            if (saving->isChecked() && path->text().trimmed().isEmpty())
                throw std::runtime_error("Choose a message file.");
            std::unique_ptr<chat::RecentMessages> replacement;
            if (saving->isChecked())
                replacement = std::make_unique<chat::RecentMessages>(
                    path->text().trimmed().toStdString(), maximum->text().trimmed().isEmpty()
                                                              ? std::nullopt
                                                              : std::optional<std::int64_t>(count));
            if (!node_)
                throw std::runtime_error("The local identity is unavailable.");
            node_->setProfile(profile);
            name_->setText(profile.name);
            saved.setValue("community/name", profile.name);
            saved.setValue("voice/pushToTalk", ptt->isChecked());
            if (pttKey->key()) {
                saved.setValue("voice/pushToTalkKey", *pttKey->key());
                saved.setValue("voice/pushToTalkKeyName", pttKey->keyName());
            }
            notifications_->setSettings({muted->isChecked(), NotifyLevel(level->currentData().toInt()),
                                         suppress->isChecked(), activity->isChecked()});
            updatePushToTalk();
            const auto inputId = input->currentData().toString();
            const auto outputId = output->currentData().toString();
            const auto previousInput = saved.value("community/input").toString();
            const auto previousOutput = saved.value("community/output").toString();
            if (!voiceCid_.isEmpty() &&
                (inputId != previousInput || outputId != previousOutput)) {
                const bool silent = mute_->isChecked() || deafen_->isChecked() ||
                                    (ptt->isChecked() && !pushToTalk_.held());
                const bool deafened = deafen_->isChecked();
                if (!audio_.start(inputId, outputId)) {
                    if (!audio_.start(previousInput, previousOutput) && !audio_.start({}, {}))
                        throw std::runtime_error(
                            "The selected audio devices are unavailable, and the call could not "
                            "be restored.");
                    audio_.setMuted(silent);
                    audio_.setDeafened(deafened);
                    throw std::runtime_error(
                        "The selected audio devices are unavailable. The call is using other "
                        "devices.");
                }
                audio_.setMuted(silent);
                audio_.setDeafened(deafened);
            }
            voiceState();
            saved.setValue("community/relay", address);
            saved.setValue("community/host", host->text().trimmed());
            saved.setValue("community/port", port->value());
            saved.setValue("community/input", inputId);
            saved.setValue("community/output", outputId);
            saved.setValue("gifs/showLinkedMedia", linkedMedia->isChecked());
            saved.setValue("messages/enabled", saving->isChecked());
            saved.setValue("messages/file", path->text().trimmed());
            saved.setValue("messages/maximum", maximum->text().trimmed());
            if (unlocked) {
                sprites.save(saved);
                penguinsAction_->setChecked(showPenguins->isChecked());
                saved.setValue("penguins/wander", wander->isChecked());
                saved.setValue("penguins/idleMinutes", idle->value());
                penguins_->setWandering(wander->isChecked());
                penguins_->setIdleTimeoutMinutes(idle->value());
                if (online_)
                    for (const auto& community : node_->communities()) publishSprite(community.id);
                refreshPenguins();
            }
            history_ = std::move(replacement);
            historyLoaded_.clear();
            loadHistory(cid_, channel_);
            render();
            dialog.accept();
        }
        catch (const std::exception& exception) {
            error->setText(QString::fromUtf8(exception.what()));
            error->show();
        }
    });
    dialog.resize(820, 660);
    dialog.setMinimumSize(680, 520);
    dialog.exec();
}

void CommunityPanel::create() {
    bool ok = false;
    const auto name = QInputDialog::getText(this, "Create community", "Community name",
                                            QLineEdit::Normal, {}, &ok);
    if (ok && !name.trimmed().isEmpty())
        attempt([&] { selectCommunity(node_->createCommunity(name)); });
}

void CommunityPanel::showProfile(const QString& cid, const QString& peer, const QPoint& position) {
    if (!node_ || !node_->communities().value(cid).isMember(peer))
        return;
    if (auto* previous = findChild<QDialog*>("userProfileCard"))
        previous->close();
    const auto model = node_->communities().value(cid);
    const auto member = model.members.value(peer);
    auto* card = new QDialog(this, Qt::Popup);
    card->setObjectName("userProfileCard");
    card->setAttribute(Qt::WA_DeleteOnClose);
    card->setWindowTitle(member.name + "'s profile");
    card->setFixedWidth(340);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(16);
    auto* avatar = new QLabel(card);
    avatar->setObjectName("profileAvatar");
    avatar->setFixedSize(80, 80);
    avatar->setPixmap(profileAvatar(member, 80, devicePixelRatioF()));
    avatar->setStyleSheet("padding:0; background:transparent;");
    layout->addWidget(avatar);
    auto* displayName = new QLabel(member.name, card);
    displayName->setObjectName("profileDisplayName");
    displayName->setTextFormat(Qt::PlainText);
    displayName->setWordWrap(true);
    displayName->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(displayName);
    const auto role = peer == model.owner ? "Owner" : member.admin ? "Admin" : "Member";
    const auto state = online_ && node_->online(cid).contains(peer) ? "Online" : "Offline";
    auto* detail = new QLabel(QString(state) + " · " + role, card);
    detail->setObjectName("settingsDescription");
    layout->addWidget(detail);
    auto* bio = new QLabel(member.bio.isEmpty() ? "No bio yet." : member.bio, card);
    bio->setObjectName("profileBio");
    bio->setTextFormat(Qt::PlainText);
    bio->setWordWrap(true);
    bio->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto* scroll = new QScrollArea(card);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setWidget(bio);
    scroll->setFixedHeight(std::clamp(bio->heightForWidth(280) + 16, 56, 200));
    layout->addWidget(scroll);
    if (peer == node_->peerId()) {
        auto* edit = button("Edit profile", "profileEdit", card);
        layout->addWidget(edit);
        connect(edit, &QPushButton::clicked, this, [this, card] {
            card->close();
            settings();
        });
    }
    card->adjustSize();
    const auto* screen = QGuiApplication::screenAt(position);
    const auto bounds = (screen ? screen : QGuiApplication::primaryScreen())->availableGeometry();
    card->move(std::clamp(position.x(), bounds.left(),
                          std::max(bounds.left(), bounds.right() - card->width())),
               std::clamp(position.y(), bounds.top(),
                          std::max(bounds.top(), bounds.bottom() - card->height())));
    card->show();
}

void CommunityPanel::acceptInvite(const QString& link) {
    emit activated();
    if (!online_) {
        pendingLink_ = link;
        note("Enter your name and connect to accept this invite.");
        return;
    }
    attempt([&] {
        const auto invite = community::Invite::fromLink(link);
        joiningCommunities_.insert(invite.community);
        updateJoining();
        try {
            node_->joinCommunity(link);
        }
        catch (...) {
            joiningCommunities_.remove(invite.community);
            updateJoining();
            throw;
        }
        note("Looking for a community host. Retrying automatically if nobody is reachable.");
    });
}

void CommunityPanel::join() {
    QDialog dialog(this);
    dialog.setWindowTitle("Join a community");
    auto* layout = new QVBoxLayout(&dialog);
    auto* explanation = new QLabel("Paste an invite link, or open a QR code image.", &dialog);
    layout->addWidget(explanation);
    auto* link = new QPlainTextEdit(&dialog);
    link->setObjectName("communityInviteInput");
    link->setPlaceholderText("disquisition://join/...");
    layout->addWidget(link);
    auto* qr = button("Open QR image", "communityOpenQr", &dialog);
    layout->addWidget(qr);
    connect(qr, &QPushButton::clicked, &dialog, [&] {
        const auto path = QFileDialog::getOpenFileName(&dialog, "Open invite QR image", {},
                                                       "Images (*.png *.jpg *.jpeg *.webp *.bmp)");
        if (path.isEmpty())
            return;
        attempt([&] {
            const auto prepared = prepareImageFile(path);
            if (!prepared.error.isEmpty())
                throw std::runtime_error(prepared.error.toStdString());
            link->setPlainText(community::readInviteQr(QImage::fromData(prepared.bytes)));
        });
    });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    buttons->button(QDialogButtonBox::Ok)->setText("Join community");
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        attempt([&] {
            community::Invite::fromLink(link->toPlainText().trimmed());
            acceptInvite(link->toPlainText().trimmed());
            dialog.accept();
        });
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.resize(580, 250);
    dialog.exec();
}

void CommunityPanel::invite() {
    attempt([&] {
        const auto link = node_->invite(cid_);
        QDialog dialog(this);
        dialog.setWindowTitle("Invite to " + node_->communities()[cid_].name);
        auto* layout = new QVBoxLayout(&dialog);
        auto* code = new QPlainTextEdit(link, &dialog);
        code->setReadOnly(true);
        code->setMaximumHeight(100);
        layout->addWidget(code);
        const auto qr = community::inviteQr(link);
        auto* image = new QLabel(&dialog);
        image->setPixmap(
            QPixmap::fromImage(qr).scaled(360, 360, Qt::KeepAspectRatio, Qt::FastTransformation));
        image->setAlignment(Qt::AlignCenter);
        layout->addWidget(image);
        auto* actions = new QHBoxLayout;
        auto* copy = button("Copy invite link", "communityCopyInvite", &dialog);
        auto* save = button("Save QR image", "communitySaveQr", &dialog);
        actions->addWidget(copy);
        actions->addWidget(save);
        layout->addLayout(actions);
        connect(copy, &QPushButton::clicked, &dialog,
                [link] { QApplication::clipboard()->setText(link); });
        connect(save, &QPushButton::clicked, &dialog, [&] {
            const auto path = QFileDialog::getSaveFileName(&dialog, "Save invite QR",
                                                           "community-invite.png",
                                                           "PNG image (*.png)");
            if (!path.isEmpty() && !qr.save(path))
                noteError("Could not save QR image.");
        });
        auto* close = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
        layout->addWidget(close);
        connect(close, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        dialog.resize(580, 540);
        dialog.exec();
    });
}

void CommunityPanel::manage() {
    if (!node_ || !node_->communities().value(cid_).canAdmin(node_->peerId()))
        return;
    const auto cid = cid_;
    const auto model = node_->communities().value(cid);
    QDialog dialog(this);
    dialog.setWindowTitle("Manage " + model.name);
    dialog.setObjectName("communityManageDialog");
    auto* layout = new QVBoxLayout(&dialog);
    auto* content = new QHBoxLayout;
    auto* categories = new QListWidget(&dialog);
    categories->setObjectName("communityManageCategories");
    categories->setFixedWidth(185);
    categories->setSpacing(2);
    auto* tabs = new QStackedWidget(&dialog);
    tabs->setObjectName("communityManagePages");
    content->addWidget(categories);
    content->addWidget(tabs, 1);
    layout->addLayout(content, 1);
    connect(categories, &QListWidget::currentRowChanged, tabs, &QStackedWidget::setCurrentIndex);
    const auto addPage = [&](QWidget* page, const QString& title) {
        auto* heading = new QLabel(title, page);
        heading->setObjectName("settingsHeading");
        page->layout()->setContentsMargins(20, 12, 12, 12);
        if (auto* form = qobject_cast<QFormLayout*>(page->layout())) {
            form->insertRow(0, heading);
            form->setVerticalSpacing(14);
            form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        }
        else if (auto* vertical = qobject_cast<QVBoxLayout*>(page->layout()))
            vertical->insertWidget(0, heading);
        categories->addItem(title);
        tabs->addWidget(page);
    };
    auto* result = new QLabel(&dialog);
    result->setWordWrap(true);
    const auto apply = [&](const QString& kind, const QJsonObject& data) {
        try {
            node_->edit(cid, kind, data);
            result->setText("Saved. Updates are being distributed to connected members.");
            return true;
        }
        catch (const std::exception& error) {
            result->setText(QString::fromUtf8(error.what()));
            return false;
        }
    };
    auto* general = new QWidget(tabs);
    auto* form = new QFormLayout(general);
    auto* name = new QLineEdit(model.name, general);
    auto* saveName = button("Save name", "communitySaveName", general);
    form->addRow("Community name", name);
    form->addRow(saveName);
    connect(saveName, &QPushButton::clicked, &dialog,
            [&] { apply("rename", {{"name", name->text().trimmed()}}); });
    auto* icon = button("Choose community icon", "communityChooseIcon", general);
    form->addRow(icon);
    connect(icon, &QPushButton::clicked, &dialog, [&] {
        const auto path = QFileDialog::getOpenFileName(&dialog, "Community icon", {},
                                                       "Images (*.png *.jpg *.jpeg *.webp)");
        if (path.isEmpty())
            return;
        const auto prepared = prepareImageFile(path);
        if (!prepared.error.isEmpty()) {
            result->setText(prepared.error);
            return;
        }
        const auto image = QImage::fromData(prepared.bytes)
                               .scaled(128, 128, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "PNG");
        apply("icon", {{"bytes", QString::fromLatin1(bytes.toBase64())}});
    });
    auto* ownershipHelp =
        new QLabel("The creator keeps ownership. Admins can make changes independently. "
                   "Any member can host and accept existing valid invites.",
                   general);
    ownershipHelp->setWordWrap(true);
    form->addRow(ownershipHelp);
    if (model.owner == node_->peerId()) {
        auto* deletionHelp = new QLabel(
            "Deleting this community removes it for all members. This cannot be undone.", general);
        deletionHelp->setWordWrap(true);
        auto* remove = button("Delete community…", "communityDelete", general);
        remove->setStyleSheet("color:#ffb3b3; border-color:#e05252;");
        form->addRow(deletionHelp);
        form->addRow(remove);
        connect(remove, &QPushButton::clicked, &dialog, [this, &dialog, cid] {
            if (deleteCommunity(cid)) dialog.accept();
        });
    }
    addPage(general, "Community");

    auto* channelsPage = new QWidget(tabs);
    auto* channelLayout = new QFormLayout(channelsPage);
    auto* channels = new QComboBox(channelsPage);
    auto* channelName = new QLineEdit(channelsPage);
    auto* topic = new QLineEdit(channelsPage);
    auto* type = new QComboBox(channelsPage);
    type->addItem("Text", "text");
    type->addItem("Voice", "voice");
    const auto fillChannels = [&] {
        channels->clear();
        channels->addItem("New channel", "");
        for (const auto& item : node_->communities()[cid].channels)
            channels->addItem(item.name, item.id);
    };
    fillChannels();
    channelLayout->addRow("Channel", channels);
    channelLayout->addRow("Name", channelName);
    channelLayout->addRow("Type", type);
    channelLayout->addRow("Topic", topic);
    auto* saveChannel = button("Save channel", "communitySaveChannel", channelsPage);
    auto* removeChannel = button("Delete channel", "communityDeleteChannel", channelsPage);
    channelLayout->addRow(saveChannel);
    channelLayout->addRow(removeChannel);
    connect(channels, &QComboBox::currentIndexChanged, &dialog, [&] {
        const auto item = node_->communities()[cid].channels.value(
            channels->currentData().toString());
        channelName->setText(item.name);
        topic->setText(item.topic);
        type->setCurrentIndex(item.type == "voice" ? 1 : 0);
        removeChannel->setEnabled(!item.id.isEmpty());
        type->setEnabled(item.id.isEmpty());
    });
    removeChannel->setEnabled(false);
    connect(saveChannel, &QPushButton::clicked, &dialog, [&] {
        const auto selected = channels->currentData().toString();
        const auto id = selected.isEmpty() ? freshId() : selected;
        if (apply("channel", {{"id", id},
                              {"name", channelName->text().trimmed()},
                              {"topic", topic->text()},
                              {"type", type->currentData().toString()}})) {
            fillChannels();
            channels->setCurrentIndex(channels->findData(id));
        }
    });
    connect(removeChannel, &QPushButton::clicked, &dialog, [&] {
        if (apply("delete-channel", {{"id", channels->currentData().toString()}}))
            fillChannels();
    });
    addPage(channelsPage, "Channels");

    auto* people = new QWidget(tabs);
    auto* peopleForm = new QFormLayout(people);
    auto* member = new QComboBox(people);
    for (const auto& item : model.members)
        member->addItem(item.name + " · " + item.id.left(8), item.id);
    auto* admin = new QCheckBox("Administrator", people);
    auto* saveRole = button("Save role", "communitySaveRole", people);
    auto* remove = button("Remove and block member", "communityRemoveMember", people);
    peopleForm->addRow("Member", member);
    peopleForm->addRow(admin);
    peopleForm->addRow(saveRole);
    peopleForm->addRow(remove);
    const auto memberChanged = [&] {
        const auto peer = member->currentData().toString();
        const auto current = node_->communities()[cid];
        admin->setChecked(current.members.value(peer).admin);
        admin->setEnabled(node_->peerId() == current.owner && peer != current.owner);
        saveRole->setEnabled(admin->isEnabled());
        remove->setEnabled(peer != current.owner);
    };
    connect(member, &QComboBox::currentIndexChanged, &dialog, memberChanged);
    memberChanged();
    connect(saveRole, &QPushButton::clicked, &dialog, [&] {
        apply("admin",
              {{"peer", member->currentData().toString()}, {"enabled", admin->isChecked()}});
    });
    connect(remove, &QPushButton::clicked, &dialog, [&] {
        if (apply("kick", {{"peer", member->currentData().toString()}})) {
            member->removeItem(member->currentIndex());
        }
    });
    addPage(people, "Members");

    auto* invitesPage = new QWidget(tabs);
    auto* invitesForm = new QFormLayout(invitesPage);
    auto* invites = new QComboBox(invitesPage);
    for (const auto& hash : model.inviteIds())
        invites->addItem(hash.left(16) +
                             (model.revokedInvites.contains(hash) ? " · revoked" : " · active"),
                         hash);
    auto* revoke = button("Revoke invite", "communityRevokeInvite", invitesPage);
    revoke->setEnabled(invites->count() > 0);
    invitesForm->addRow("Invite ID", invites);
    invitesForm->addRow(revoke);
    auto* inviteExplanation = new QLabel(
        "Existing valid invites work while admins are offline. Revoking an invite prevents future "
        "admissions once peers receive the update; it does not remove existing members.",
        invitesPage);
    inviteExplanation->setWordWrap(true);
    invitesForm->addRow(inviteExplanation);
    connect(revoke, &QPushButton::clicked, &dialog, [&] {
        if (apply("revoke", {{"hash", invites->currentData().toString()}}))
            invites->setItemText(invites->currentIndex(),
                                 invites->currentData().toString().left(16) + " · revoked");
    });
    addPage(invitesPage, "Invites");

    auto* hostsPage = new QWidget(tabs);
    auto* hostsLayout = new QVBoxLayout(hostsPage);
    auto* hostHelp =
        new QLabel("Drag members into host priority order. If a member disconnects, the next "
                   "reachable member becomes the preferred host. This does not change permissions.",
                   hostsPage);
    hostHelp->setWordWrap(true);
    hostsLayout->addWidget(hostHelp);
    auto* priority = new QListWidget(hostsPage);
    priority->setDragDropMode(QAbstractItemView::InternalMove);
    auto ordered = model.priority;
    if (!ordered.contains(model.owner))
        ordered.prepend(model.owner);
    for (const auto& peer : model.members.keys())
        if (!ordered.contains(peer))
            ordered.append(peer);
    for (const auto& peer : ordered) {
        if (!model.members.contains(peer))
            continue;
        auto* item = new QListWidgetItem(model.members[peer].name + " · " + peer.left(8), priority);
        item->setData(Qt::UserRole, peer);
    }
    hostsLayout->addWidget(priority);
    auto* savePriority = button("Save host priority", "communitySavePriority", hostsPage);
    hostsLayout->addWidget(savePriority);
    connect(savePriority, &QPushButton::clicked, &dialog, [&] {
        QJsonArray peers;
        for (int i = 0; i < priority->count(); ++i)
            peers.append(priority->item(i)->data(Qt::UserRole).toString());
        apply("priority", {{"peers", peers}});
    });
    addPage(hostsPage, "Host priority");
    layout->addWidget(result);
    auto* close = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    layout->addWidget(close);
    connect(close, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    categories->setCurrentRow(0);
    dialog.resize(780, 500);
    dialog.exec();
}

void CommunityPanel::selectCommunity(const QString& cid) {
    selectedVoiceCid_.clear();
    selectedVoiceChannel_.clear();
    cid_ = cid;
    channel_.clear();
    if (node_->communities().contains(cid))
        for (const auto& item : orderedChannels(node_->communities()[cid]))
            if (item.type == "text") {
                channel_ = item.id;
                break;
            }
    loadHistory(cid_, channel_);
    refresh();
    render();
}

void CommunityPanel::select(QTreeWidgetItem* item) {
    if (!item)
        return;
    const auto cid = item->data(0, Qt::UserRole).toString();
    const auto channel = item->data(0, Qt::UserRole + 1).toString();
    if (channel.isEmpty()) {
        if (cid != cid_)
            selectCommunity(cid);
        return;
    }
    if (node_->communities().value(cid).channels.value(channel).type == "voice") {
        selectedVoiceCid_ = cid;
        selectedVoiceChannel_ = channel;
        refresh();
        return;
    }
    selectedVoiceCid_.clear();
    selectedVoiceChannel_.clear();
    cid_ = cid;
    channel_ = channel;
    unread_[key(cid_, channel_)] = 0;
    loadHistory(cid_, channel_);
    refresh();
    render();
}

QString CommunityPanel::memberName(const QString& cid, const QString& peer) const {
    if (!node_ || !node_->communities().contains(cid))
        return peer.left(8);
    return node_->communities()[cid].members.value(peer).name;
}

void CommunityPanel::refresh() {
    refreshPenguins();
    const bool usable = node_ != nullptr;
    if (usable &&
        !node_->communities().value(selectedVoiceCid_).channels.contains(selectedVoiceChannel_)) {
        selectedVoiceCid_.clear();
        selectedVoiceChannel_.clear();
    }
    QString currentVoiceCid, currentVoiceChannel;
    if (usable && online_) {
        for (const auto& model : node_->communities()) {
            const auto presence = node_->presence(model.id, node_->peerId());
            if (!presence.voiceChannel.isEmpty()) {
                currentVoiceCid = model.id;
                currentVoiceChannel = presence.voiceChannel;
                break;
            }
        }
    }
    if (!voiceCid_.isEmpty() && currentVoiceCid.isEmpty()) {
        // A metadata sync may briefly hide presence. Keep the call alive while
        // the retained membership and voice channel still exist.
        const auto model = node_->communities().value(voiceCid_);
        if (online_ && model.isMember(node_->peerId()) &&
            model.channels.value(voiceChannel_).type == "voice") {
            currentVoiceCid = voiceCid_;
            currentVoiceChannel = voiceChannel_;
            if (node_->presence(currentVoiceCid, node_->peerId()).voiceChannel.isEmpty())
                node_->setVoice(currentVoiceCid, currentVoiceChannel, mute_->isChecked(),
                                deafen_->isChecked(), capture_.running());
        }
        else {
            leaveVoice();
            return;
        }
    }
    voiceCid_ = currentVoiceCid;
    voiceChannel_ = currentVoiceChannel;
    if (!voiceCid_.isEmpty()) {
        const auto presence = node_->presence(voiceCid_, node_->peerId());
        QSignalBlocker mute(mute_), deafen(deafen_);
        deafen_->setChecked(presence.deafened);
        if (!presence.deafened && !QSettings().value("voice/pushToTalk", false).toBool())
            mute_->setChecked(presence.muted);
    }
    for (auto* action : {invite_, manage_, send_, image_, gif_, voice_, mute_, deafen_, share_})
        action->setEnabled(false);
    const bool composerFocused = composer_->hasFocus();
    composer_->setEnabled(false);
    connect_->setEnabled(usable);
    connect_->setText(online_ ? "Disconnect" : "Connect");
    name_->setEnabled(!online_);
    create_->setEnabled(usable && online_);
    join_->setEnabled(usable && online_);
    voiceControls_->setVisible(online_ && !voiceCid_.isEmpty());
    if (!usable) {
        if (!failure_.isEmpty())
            noteError(failure_);
        else
            note(QString());
        return;
    }
    if (!node_->communities().value(cid_).isMember(node_->peerId())) {
        cid_.clear();
        channel_.clear();
        selectedVoiceCid_.clear();
        selectedVoiceChannel_.clear();
    }
    if (cid_.isEmpty()) {
        for (const auto& model : node_->communities())
            if (model.isMember(node_->peerId())) { cid_ = model.id; break; }
        for (const auto& channel : orderedChannels(node_->communities()[cid_]))
            if (channel.type == "text") {
                channel_ = channel.id;
                break;
            }
        loadHistory(cid_, channel_);
        render();
    }
    QMap<QString, bool> expanded;
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        const auto* item = tree_->topLevelItem(i);
        expanded[item->data(0, Qt::UserRole).toString()] = item->isExpanded();
    }
    QSignalBlocker block(tree_);
    tree_->clear();
    for (const auto& model : node_->communities()) {
        if (!model.isMember(node_->peerId())) continue;
        auto* root = new QTreeWidgetItem(tree_, {model.name});
        root->setData(0, Qt::UserRole, model.id);
        root->setExpanded(expanded.value(model.id, true));
        auto font = root->font(0);
        font.setBold(true);
        root->setFont(0, font);
        if (!model.icon.isEmpty()) {
            ImageBytes image(model.icon);
            if (image.canRead() && image.size().width() <= 512 && image.size().height() <= 512)
                root->setIcon(0, QIcon(QPixmap::fromImage(image.read())));
        }
        for (const auto& channel : orderedChannels(model)) {
            const auto unread = unread_.value(key(model.id, channel.id));
            const auto count =
                channel.type == "voice"
                    ? std::count_if(model.members.begin(), model.members.end(),
                                    [&](const auto& member) {
                                        return node_->online(model.id).contains(member.id) &&
                                               node_->presence(model.id, member.id).voiceChannel ==
                                                   channel.id;
                                    })
                    : 0;
            auto label = (channel.type == "text" ? "# " : "") + channel.name;
            if (count)
                label += "  (" + QString::number(count) + ')';
            if (unread)
                label += "  [" + QString::number(unread) + ']';
            auto* child = new QTreeWidgetItem(root, {label});
            if (channel.type == "voice")
                child->setIcon(0, voiceChannelIcon(devicePixelRatioF()));
            child->setData(0, Qt::UserRole, model.id);
            child->setData(0, Qt::UserRole + 1, channel.id);
            if (channel.type == "voice")
                static_cast<CommunityTree*>(tree_)->configureVoiceChannel(
                    child, model.id, channel.id, online_ && model.canAdmin(node_->peerId()));
            const bool selectedVoice = model.id == selectedVoiceCid_ &&
                                       channel.id == selectedVoiceChannel_;
            const bool activeVoice = model.id == voiceCid_ && channel.id == voiceChannel_;
            if (selectedVoice ||
                (selectedVoiceChannel_.isEmpty() && model.id == cid_ && channel.id == channel_))
                tree_->setCurrentItem(child);
            if (channel.type != "voice")
                continue;
            child->setExpanded(true);
            if (activeVoice)
                child->setForeground(0, QColor("#9dccca"));
            for (const auto& peer : model.members) {
                const auto state = node_->presence(model.id, peer.id);
                if (!node_->online(model.id).contains(peer.id) || state.voiceChannel != channel.id)
                    continue;
                QStringList activity;
                if (state.muted)
                    activity.append("muted");
                if (state.deafened)
                    activity.append("deafened");
                if (state.sharing)
                    activity.append("sharing screen");
                auto label = peer.name;
                if (peer.id == node_->peerId())
                    label += " (you)";
                auto* participant = new QTreeWidgetItem(child, {label});
                participant->setFlags(Qt::ItemIsEnabled);
                participant->setData(0, Qt::UserRole + 2, peer.id);
                participant->setData(0, Qt::UserRole + 3, activity);
                participant->setData(0, Qt::UserRole, model.id);
                // Tree item padding is outside the widget's content rectangle.
                // Leave room for the avatar plus both the row and item insets.
                participant->setSizeHint(0, QSize(160, 48));
                auto* row = new QWidget(tree_);
                row->setAccessibleName(label + ", " + activity.join(", "));
                auto* line = new QHBoxLayout(row);
                line->setContentsMargins(0, 4, 4, 4);
                line->setSpacing(6);
                auto* profile = button(QString(label).replace('&', "&&"), "voiceMemberProfile",
                                       row);
                profile->setProperty("peer", peer.id);
                profile->setIcon(QIcon(profileAvatar(peer, 28, devicePixelRatioF())));
                profile->setIconSize(QSize(28, 28));
                profile->setMinimumHeight(32);
                profile->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
                profile->setToolTip("View " + peer.name + "'s profile");
                profile->setAccessibleName("View profile: " + label);
                line->addWidget(profile, 1);
                connect(profile, &QPushButton::clicked, this,
                        [this, profile, cid = model.id, id = peer.id] {
                            showProfile(cid, id,
                                        profile->mapToGlobal(QPoint(0, profile->height())));
                        });
                for (const auto& kind : activity) {
                    auto* indicator = new QLabel(row);
                    indicator->setObjectName("voiceActivity");
                    indicator->setPixmap(voiceIndicator(kind));
                    indicator->setToolTip(kind);
                    indicator->setAccessibleName(kind);
                    indicator->setFixedSize(16, 16);
                    line->addWidget(indicator);
                }
                participant->setSizeHint(0,
                                         QSize(160, std::max(48, row->sizeHint().height() + 12)));
                tree_->setItemWidget(participant, 0, row);
                paintSpeaking(row, speaking_.contains(peer.id));
                static_cast<CommunityTree*>(tree_)->configureVoiceParticipant(
                    participant, row, model.id, channel.id, peer.id,
                    online_ && model.canAdmin(node_->peerId()));
            }
            if (selectedVoice && !activeVoice && online_ && model.isMember(node_->peerId())) {
                auto* action = new QTreeWidgetItem(child);
                action->setFlags(Qt::ItemIsEnabled);
                auto* join = button(voiceCid_.isEmpty() ? "Join voice" : "Switch to this channel",
                                    "communityJoinVoice", tree_);
                join->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
                action->setSizeHint(0, QSize(160, 34));
                tree_->setItemWidget(action, 0, join);
                connect(join, &QPushButton::clicked, this, &CommunityPanel::joinVoice);
            }
        }
    }
    for (const auto& community : node_->communities()) {
        const auto online = online_ ? node_->online(community.id) : QSet<QString>{};
        QSet<QString> sharing;
        for (const auto& peer : online) {
            if (peer == node_->peerId()) continue;
            if (node_->presence(community.id, peer).sharing) sharing.insert(peer);
            if (previousOnline_.contains(community.id) && !previousOnline_[community.id].contains(peer))
                notifications_->activityOccurred(memberName(community.id, peer), "joined " + community.name);
        }
        for (const auto& peer : sharing - previousSharing_.value(community.id))
            notifications_->activityOccurred(memberName(community.id, peer), "started sharing in " + community.name);
        previousOnline_[community.id] = online;
        previousSharing_[community.id] = sharing;
    }
    const auto model = node_->communities().value(cid_);
    QString profiles = cid_;
    for (const auto& member : model.members)
        profiles += member.id + community::digest(member.name.toUtf8() + member.avatar);
    if (renderedProfiles_ != profiles) {
        renderedProfiles_ = profiles;
        const auto position = transcript_->verticalScrollBar()->value();
        const bool atBottom = position >= transcript_->verticalScrollBar()->maximum() - 4;
        render();
        if (!atBottom)
            transcript_->verticalScrollBar()->setValue(position);
    }
    QStringList mentionable = memberNames(cid_);
    if (model.isMember(node_->peerId()))
        mentionable.removeAll(model.members.value(node_->peerId()).name);
    mentions_->setMembers(mentionable);
    if (!model.id.isEmpty() && !model.channels.contains(channel_)) {
        channel_.clear();
        for (const auto& item : orderedChannels(model))
            if (item.type == "text") {
                channel_ = item.id;
                break;
            }
        if (!channel_.isEmpty()) {
            loadHistory(cid_, channel_);
            render();
        }
    }
    const auto channel = model.channels.value(channel_);
    const bool member = model.isMember(node_->peerId());
    const bool textChannel = member && channel.type == "text";
    heading_->setText(model.id.isEmpty() ? "Your communities"
                                         : model.name + " / " +
                                               (channel.type == "text" ? "#" : "") + channel.name);
    topic_->setText(channel.topic.isEmpty()
                        ? (model.id.isEmpty()
                               ? "Create a community or join with an invite link or QR image."
                               : QString::number(node_->online(cid_).size()) + " online · " +
                                     QString::number(model.members.size()) + " members · host " +
                                     memberName(cid_, node_->host(cid_)))
                        : channel.topic);
    const bool admin = model.canAdmin(node_->peerId());
    invite_->setEnabled(admin);
    manage_->setEnabled(admin);
    const bool canLeave = member && model.owner != node_->peerId();
    leaveAction_->setVisible(canLeave);
    leaveAction_->setEnabled(canLeave);
    composer_->setEnabled(textChannel);
    if (composerFocused && textChannel)
        composer_->setFocus(Qt::OtherFocusReason);
    send_->setEnabled(textChannel);
    image_->setEnabled(online_ && textChannel);
    gif_->setEnabled(online_ && textChannel);
    members_->clear();
    for (const auto& peer : model.members) {
        const bool present = node_->online(cid_).contains(peer.id);
        QString label = (present ? "+ " : "  ") + peer.name;
        if (peer.id == model.owner)
            label += " · owner";
        else if (peer.admin)
            label += " · admin";
        auto* item = new QListWidgetItem(label, members_);
        item->setIcon(QIcon(profileAvatar(peer, 32, devicePixelRatioF())));
        item->setSizeHint(QSize(160, 44));
        item->setData(Qt::UserRole, peer.id);
        item->setToolTip("View " + peer.name + "'s profile");
        item->setForeground(!present ? QColor("#858c96") :
            peer.id == node_->peerId() ? chatColor(messageColor_) :
                                         peerColors_.value(peer.id, chatColor("mint")));
    }
    const bool inVoice = !voiceCid_.isEmpty();
    voice_->setEnabled(inVoice);
    mute_->setEnabled(inVoice && !deafen_->isChecked());
    deafen_->setEnabled(inVoice);
    share_->setEnabled(inVoice);
    share_->setText(capture_.running() ? "Stop sharing" : "Share screen");
    if (inVoice) {
        const auto voiceModel = node_->communities().value(voiceCid_);
        if (!online_ || !voiceModel.isMember(node_->peerId()) ||
            !voiceModel.channels.contains(voiceChannel_) ||
            voiceModel.channels[voiceChannel_].type != "voice") {
            leaveVoice();
            return;
        }
        voiceStatus_->setText(voiceModel.name + " / " + voiceModel.channels[voiceChannel_].name);
        QSet<QString> active, streaming;
        // Presence is removed when the authenticated connection closes. A
        // metadata sync briefly clears readiness, but must not tear down video.
        for (const auto& peer : voiceModel.members.keys()) {
            const auto state = node_->presence(voiceCid_, peer);
            if (peer != node_->peerId() && state.voiceChannel == voiceChannel_) {
                active.insert(peer);
                if (state.muted) {
                    setSpeaking(peer, false);
                    quietAudioFrames_.remove(peer);
                }
                if (state.sharing)
                    streaming.insert(peer);
            }
        }
        for (const auto& peer : audioPeers_ - active) {
            audio_.remove(peer);
            setSpeaking(peer, false);
            quietAudioFrames_.remove(peer);
            lastAudioAt_.remove(peer);
            watchers_.remove(peer);
            for (const auto& [_, session] : drawings_)
                session->peerChanged(peer, false);
        }
        audioPeers_ = active;
        for (const auto& peer : streams_ - streaming) {
            stopDrawing(peer);
            stage_->removeStream(peer);
            // Preserve watch intent while a peer is disconnected. Explicitly
            // ending a share or leaving voice still removes the subscription.
            if (node_->online(voiceCid_).contains(peer) || !voiceModel.isMember(peer))
                watching_.remove(peer);
            lastScreenFrames_.remove(peer);
            decoders_.erase(peer);
            frames_.remove(peer);
        }
        for (const auto& peer : streaming - streams_) {
            stage_->addStream(peer, false, memberName(voiceCid_, peer));
            if (watching_.contains(peer))
                watch(peer, true);
        }
        streams_ = streaming;
        for (const auto& peer : streams_)
            stage_->setDisplayName(peer, memberName(voiceCid_, peer));
        if (capture_.running())
            stage_->setDisplayName(node_->peerId(), memberName(voiceCid_, node_->peerId()));
        QHash<QString, QString> names;
        for (const auto& member : node_->communities().value(voiceCid_).members)
            names[member.id] = member.name;
        for (const auto& [peer, session] : drawings_)
            if (auto* drawing = stage_->drawing(peer)) drawing->setAuthorNames(names);
    }
    else
        voiceStatus_->setText("Voice disconnected");
}

void CommunityPanel::openHistory() {
    history_.reset();
    historyLoaded_.clear();
    QSettings settings;
    if (!settings.value("messages/enabled", false).toBool())
        return;
    attempt([&] {
        const auto maximum = settings.value("messages/maximum").toString();
        const auto limit = maximum.isEmpty() ? std::nullopt
                                             : std::optional<std::int64_t>(maximum.toLongLong());
        history_ = std::make_unique<chat::RecentMessages>(
            settings.value("messages/file", QDir::homePath() + "/disquisition-messages.db")
                .toString()
                .toStdString(),
            limit);
        loadHistory(cid_, channel_);
        render();
    });
}

void CommunityPanel::loadHistory(const QString& cid, const QString& channel) {
    if (!history_ || cid.isEmpty() || channel.isEmpty() ||
        historyLoaded_.contains(key(cid, channel)))
        return;
    attempt([&] {
        const auto loaded = history_->recent(cid.toStdString(), channel.toStdString());
        auto& entries = entries_[key(cid, channel)];
        QSet<QString> existing;
        for (const auto& entry : entries)
            existing.insert(text(entry.message.messageId));
        QList<Entry> previous;
        for (const auto& message : loaded)
            if (!existing.contains(text(message.messageId)))
                previous.append({message, {}, false});
        previous.append(entries);
        entries = std::move(previous);
        historyLoaded_.insert(key(cid, channel));
    });
}

void CommunityPanel::append(const QString& cid, const QString& channel, Entry entry, bool persist) {
    entry.message.communityId = cid.toStdString();
    entry.message.channelId = channel.toStdString();
    if (persist && history_ && entry.image.isEmpty())
        attempt([&] { history_->append(entry.message); });
    auto& list = entries_[key(cid, channel)];
    if (list.isEmpty() && cid == cid_ && channel == channel_) {
        media_->clear();
        transcript_->clear();
    }
    if (!entry.image.isEmpty()) {
        entry.message.messageId = freshId().toStdString();
        cachedImageBytes_ += entry.image.size();
        cachedImages_.append({key(cid, channel), text(entry.message.messageId)});
    }
    list.append(entry);
    if (list.size() > 1000) {
        cachedImageBytes_ -= list.first().image.size();
        list.removeFirst();
    }
    bool rerender = false;
    while (cachedImages_.size() > 128 || cachedImageBytes_ > 24 * 1024 * 1024) {
        const auto oldest = cachedImages_.takeFirst();
        for (auto& cached : entries_[oldest.first]) {
            if (text(cached.message.messageId) != oldest.second || cached.image.isEmpty())
                continue;
            cachedImageBytes_ -= cached.image.size();
            cached.image.clear();
            cached.message.body = "[image no longer cached]";
            if (oldest.first == key(cid_, channel_))
                rerender = true;
            break;
        }
    }
    if (cid == cid_ && channel == channel_)
        showEntry(entry);
    else {
        if (!notifications_->settings().muted)
            ++unread_[key(cid, channel)];
        refresh();
    }
    if (rerender)
        render();
}

void CommunityPanel::showEntry(const Entry& entry) {
    if (entry.system) {
        transcript_->append("<p style='color:#b5b65f'><b>" + text(entry.message.sender).toHtmlEscaped() +
                            "</b> " + text(entry.message.body).toHtmlEscaped().replace('\n', "<br>") + "</p>");
        return;
    }
    const auto when = QDateTime::fromSecsSinceEpoch(entry.message.timestamp).toString("HH:mm");
    const auto body = text(entry.message.body);
    const auto cid = text(entry.message.communityId);
    const auto peer = text(entry.message.senderId);
    const auto model = node_ ? node_->communities().value(cid) : community::Model{};
    auto member = model.members.value(peer);
    const bool hasProfile = model.isMember(peer);
    if (!hasProfile)
        member.name = text(entry.message.sender);
    const auto imageUrl = QUrl("profile-avatar:" +
                               community::digest(member.name.toUtf8() + member.avatar));
    transcript_->document()->addResource(QTextDocument::ImageResource, imageUrl,
                                         profileAvatar(member, 32, devicePixelRatioF()).toImage());
    const auto color = chatColor(text(entry.message.color)).name();
    const auto href = "profile://" + cid + '/' + peer;
    const auto link = [&](const QString& content) {
        return hasProfile ? "<a href='" + href.toHtmlEscaped() +
                                "' style='color:" + color + ";text-decoration:none'>" + content + "</a>"
                          : "<span style='color:" + color + "'>" + content + "</span>";
    };
    const bool showMedia = QSettings().value("gifs/showLinkedMedia", true).toBool();
    const auto links = showMedia ? findMediaLinks(body) : QList<MediaLink>{};
    const bool onlyMedia = links.size() == 1 && body.trimmed() == links.first().text;
    const auto mentions = findMentions(body, memberNames(cid));
    const bool mentioned = peer != node_->peerId() &&
        mentionOf(mentions, memberName(cid, node_->peerId())) != Mentioned::No;
    QString html;
    qsizetype written = 0;
    for (const auto& mention : mentions) {
        if (mention.start < written) continue;
        html += body.mid(written, mention.start - written).toHtmlEscaped() +
                "<span style='background-color:#3e4372;color:#c9cdfb'>" +
                body.mid(mention.start, mention.length).toHtmlEscaped() + "</span>";
        written = mention.start + mention.length;
    }
    html += body.mid(written).toHtmlEscaped();
    if (onlyMedia) html.clear();
    transcript_->append(
        "<table width='100%' cellspacing='0' cellpadding='0' style='margin-top:12px'><tr>"
        "<td width='42' valign='top'" + QString(mentioned ? " bgcolor='#453d2c'" : "") + ">" +
        link("<img src='" + imageUrl.toString() + "' width='32' height='32'>") +
        "</td><td valign='top'" + (mentioned ? " bgcolor='#453d2c'" : "") +
        "><p style='margin:0 0 3px 0'>" +
        link("<b>" + member.name.toHtmlEscaped() + "</b>") +
        " <span style='color:#a6b5bd;font-size:11px'>" + when + "</span></p>" +
        "<p style='margin:0;color:#c5c8c6'>" + html.replace('\n', "<br>") +
        (entry.undelivered
             ? " <span style='color:#d4ae77'>[not delivered: no peers connected]</span>"
             : "") +
        "</p></td></tr></table>");
    if (!entry.image.isEmpty())
        media_->appendImage(entry.image);
    for (const auto& link : links)
        media_->appendRemote(link.url);
}

void CommunityPanel::render() {
    media_->clear();
    transcript_->clear();
    const auto entries = entries_.value(key(cid_, channel_));
    if (entries.isEmpty()) {
        const bool voice = node_ &&
                           node_->communities().value(cid_).channels.value(channel_).type ==
                               "voice";
        transcript_->setPlainText(
            voice ? "Join this voice channel to talk or share your screen."
                  : (cid_.isEmpty()
                         ? "Your communities live on member devices. Create one to get started."
                         : "No messages on this device yet. Messages are delivered live."));
    }
    else
        for (const auto& entry : entries)
            showEntry(entry);
}

void CommunityPanel::sendChat() {
    const auto body = composer_->text().trimmed();
    if (body.isEmpty()) return;
    if (body.startsWith('/')) {
        runCommand(body);
        return;
    }
    if (!online_) { noteError("Connect before sending messages."); return; }
    attempt([&] {
        const auto body = composer_->text().trimmed();
        const auto id = freshId();
        const auto stamp = QDateTime::currentSecsSinceEpoch();
        const int count =
            node_->sendPacket(cid_, channel_, "chat",
                              {{"id", id}, {"body", body}, {"color", messageColor_},
                               {"timestamp", double(stamp * 1000)}});
        if (entries_[key(cid_, channel_)].isEmpty()) {
            media_->clear();
            transcript_->clear();
        }
        append(cid_, channel_,
               {{stamp, name_->text().trimmed().toStdString(), body.toStdString(),
                 messageColor_.toStdString(),
                 cid_.toStdString(), channel_.toStdString(), id.toStdString(),
                 node_->peerId().toStdString()},
                {},
                count == 0});
        penguins_->showMessage(node_->peerId(), body);
        penguins_->setColor(node_->peerId(), chatColor(messageColor_));
        composer_->clear();
    });
}

void CommunityPanel::prepareUpload(std::function<PreparedImage()> prepare) {
    const auto cid = cid_, channel = channel_;
    imagePool_.start([this, cid, channel, prepare = std::move(prepare)] {
        PreparedImage image;
        try {
            image = prepare();
        }
        catch (const std::exception& error) {
            image.error = QString::fromUtf8(error.what());
        }
        QMetaObject::invokeMethod(
            this,
            [this, cid, channel, image = std::move(image)] { sendImage(image, cid, channel); },
            Qt::QueuedConnection);
    });
}

void CommunityPanel::sendImage(const PreparedImage& image, const QString& cid,
                               const QString& channel) {
    if (!online_ || !image.error.isEmpty() || image.bytes.isEmpty()) {
        if (!image.error.isEmpty())
            noteError(image.error);
        return;
    }
    if (!node_->communities().value(cid).isMember(node_->peerId()) ||
        node_->communities().value(cid).channels.value(channel).type != "text")
        return;
    if (uploads_.size() >= 4) {
        note("Wait for the current image uploads to finish.");
        return;
    }
    if (cid == cid_) penguins_->noteActivity(node_->peerId());
    const auto transfer = QRandomGenerator::global()->generate();
    uploads_.append({cid, channel, transfer, splitImage(image.bytes), 0, messageColor_});
    imageTimer_.start();
    append(cid, channel,
           {{QDateTime::currentSecsSinceEpoch(),
             name_->text().trimmed().toStdString(),
             "",
             messageColor_.toStdString(),
             cid.toStdString(),
             channel.toStdString(),
             {},
             node_->peerId().toStdString()},
            image.bytes,
            node_->online(cid).size() <= 1},
           false);
}

void CommunityPanel::receive(const QString& cid, const QString& channel, const QString& peer,
                             const QString& type, const QJsonObject& data) {
    const auto sender = memberName(cid, peer);
    if (type == "sprite") {
        const auto bytes = QByteArray::fromBase64(data.value("bytes").toString().toLatin1());
        const auto count = data.value("count").toInt();
        if (count == 0) {
            spriteImages_.remove(key(cid, peer));
            peerSprites_.remove(peer);
            refreshPenguins();
            return;
        }
        const auto image = spriteImages_.add(key(cid, peer), quint32(data.value("transfer").toDouble()),
            data.value("index").toInt(), count, bytes, QDateTime::currentMSecsSinceEpoch());
        if (image && image->size() <= 256 * 1024) {
            const auto sprites = PenguinSprites::fromWireData(*image);
            if (sprites.complete()) {
                peerSprites_[peer] = {sprites.standing, sprites.walking};
                refreshPenguins();
            }
        }
        return;
    }
    if (type == "chat" || type == "image") {
        peerColors_[peer] = chatColor(packetColor(data));
        if (cid == cid_) {
            penguins_->setColor(peer, peerColors_[peer]);
            for (int i = 0; i < members_->count(); ++i)
                if (members_->item(i)->data(Qt::UserRole).toString() == peer)
                    members_->item(i)->setForeground(peerColors_[peer]);
            if (type == "chat") penguins_->showMessage(peer, data.value("body").toString());
        }
    }
    if (type == "chat") {
        const auto body = data.value("body").toString();
        const auto links = findMediaLinks(body);
        const bool onlyMedia = links.size() == 1 && body.trimmed() == links.first().text;
        notifications_->messageReceived(sender, onlyMedia ? QStringLiteral("sent a GIF") : body,
            mentionOf(findMentions(body, memberNames(cid)), memberName(cid, node_->peerId())));
        if (cid == cid_ && channel == channel_ && entries_[key(cid, channel)].isEmpty()) {
            media_->clear();
            transcript_->clear();
        }
        append(cid, channel,
               {{qint64(data.value("timestamp").toDouble()) / 1000, sender.toStdString(),
                 data.value("body").toString().toStdString(), packetColor(data).toStdString(),
                 cid.toStdString(),
                 channel.toStdString(), data.value("id").toString().toStdString(),
                 peer.toStdString()},
                {},
                false});
    }
    else if (type == "audio" && cid == voiceCid_ && channel == voiceChannel_) {
        const auto pcm = QByteArray::fromBase64(data.value("bytes").toString().toLatin1());
        audio_.receive(peer, pcm);
        if (pcm.size() == 640) {
            lastAudioAt_[peer] = QDateTime::currentMSecsSinceEpoch();
            const auto* samples = reinterpret_cast<const unsigned char*>(pcm.constData());
            double sum = 0;
            for (int index = 0; index < pcm.size(); index += 2) {
                const auto sample =
                    static_cast<qint16>(samples[index] | (samples[index + 1] << 8));
                sum += static_cast<double>(sample) * sample;
            }
            const bool loud = std::sqrt(sum / 320) > kVoiceActivityLevel;
            const int quiet =
                loud ? 0 : std::min(quietAudioFrames_.value(peer) + 1, kVoiceActivityHoldFrames);
            quietAudioFrames_[peer] = quiet;
            setSpeaking(peer, quiet < kVoiceActivityHoldFrames);
        }
    }
    else if (type == "image") {
        const auto bytes = QByteArray::fromBase64(data.value("bytes").toString().toLatin1());
        const auto transfer = data.value("transfer").toDouble();
        const auto index = data.value("index").toInt(-1), count = data.value("count").toInt();
        if (transfer < 0 || transfer > UINT32_MAX || transfer != quint32(transfer) || index < 0 ||
            count <= 0 || count > int(chat::kMaxImageChunks) || index >= count ||
            bytes.size() > qsizetype(chat::kImageChunkSize))
            return;
        const auto image = images_.add(key(cid, channel) + '/' + peer, quint32(transfer), index,
                                       count, bytes, QDateTime::currentMSecsSinceEpoch());
        if (image && isDisplayableImage(*image)) {
            if (cid == cid_) penguins_->noteActivity(peer);
            notifications_->messageReceived(sender, "sent an image", Mentioned::No);
            append(cid, channel,
                   {{QDateTime::currentSecsSinceEpoch(),
                     sender.toStdString(),
                     "",
                     packetColor(data).toStdString(),
                     cid.toStdString(),
                     channel.toStdString(),
                     {},
                     peer.toStdString()},
                    *image,
                    false},
                   false);
        }
    }
    else if (type == "drawing" && cid == voiceCid_ && channel == voiceChannel_) {
        auto bytes = QByteArray::fromBase64(data.value("bytes").toString().toLatin1()).toStdString();
        chat::Message payload;
        if (chat::decode(bytes, payload) != chat::DecodeStatus::Ok || !bytes.empty() || payload.fields.empty())
            return;
        const auto session = drawings_.find(QString::fromStdString(payload.fields[0]));
        if (session != drawings_.end()) session->second->receive(peer, payload);
    }
    else if (type == "watch" && cid == voiceCid_ && channel == voiceChannel_) {
        if (data.value("enabled").toBool() && capture_.running()) {
            watchers_.insert(peer);
            capture_.requestKeyframe();
        }
        else
            watchers_.remove(peer);
    }
    else if (type == "screen" && watching_.contains(peer) && cid == voiceCid_ &&
             channel == voiceChannel_) {
        const auto frame = data.value("frame").toDouble(-1);
        const auto index = data.value("index").toInt(-1), count = data.value("count").toInt();
        const auto chunk = QByteArray::fromBase64(data.value("bytes").toString().toLatin1());
        if (frame < 0 || frame > UINT32_MAX || frame != quint32(frame) || index < 0 || count <= 0 ||
            count > kMaxScreenChunks || index >= count || chunk.size() > kScreenChunkBytes)
            return;
        QByteArray encoded;
        if (frames_.add(peer, quint32(frame), index, count, chunk, encoded)) {
            const auto image = decoders_[peer].decode(quint32(frame),
                                                      data.value("keyframe").toBool(), encoded);
            if (!image.isNull())
                lastScreenFrames_[peer] = QDateTime::currentMSecsSinceEpoch();
            if (!image.isNull())
                stage_->showFrame(peer, image, encoded.size());
        }
    }
}

void CommunityPanel::joinVoice() {
    if (!online_)
        return;
    const auto cid = selectedVoiceCid_, selectedChannel = selectedVoiceChannel_;
    if (node_->communities().value(cid).channels.value(selectedChannel).type != "voice")
        return;
    requestMicrophoneAccess(this, [this, cid, selectedChannel](bool granted) {
        if (!online_)
            return;
        if (!granted) {
            noteError("Microphone access is required to join voice.");
            return;
        }
        const auto model = node_->communities().value(cid);
        if (!model.isMember(node_->peerId()) ||
            model.channels.value(selectedChannel).type != "voice")
            return;
        leaveVoice();
        QSettings settings;
        if (!audio_.start(settings.value("community/input").toString(),
                          settings.value("community/output").toString())) {
            noteError("The selected audio devices are unavailable.");
            return;
        }
        voiceCid_ = cid;
        voiceChannel_ = selectedChannel;
        updatePushToTalk();
        voiceState();
        refresh();
    });
}

void CommunityPanel::voiceState() {
    if (voiceCid_.isEmpty() || !node_)
        return;
    const bool silent = mute_->isChecked() || deafen_->isChecked() ||
        (QSettings().value("voice/pushToTalk", false).toBool() && !pushToTalk_.held());
    audio_.setMuted(silent);
    audio_.setDeafened(deafen_->isChecked());
    attempt([&] {
        node_->setVoice(voiceCid_, voiceChannel_, silent,
                        deafen_->isChecked(), capture_.running());
    });
    refresh();
}

void CommunityPanel::leaveVoice() {
    const auto previous = voiceCid_;
    presenterOverlay_.reset();
    for (const auto& [_, session] : drawings_) session->stop();
    drawings_.clear();
    voiceCid_.clear();
    pushToTalk_.stop();
    voiceChannel_.clear();
    stopSharing();
    if (node_ && online_ && !previous.isEmpty())
        attempt([&] { node_->setVoice(previous, {}); });
    audio_.stop();
    audioPeers_.clear();
    speaking_.clear();
    quietAudioFrames_.clear();
    lastAudioAt_.clear();
    watchers_.clear();
    for (const auto& peer : streams_)
        stage_->removeStream(peer);
    streams_.clear();
    watching_.clear();
    lastScreenFrames_.clear();
    streamRetry_.stop();
    decoders_.clear();
    frames_ = {};
    {
        QSignalBlocker a(mute_), b(deafen_);
        mute_->setChecked(false);
        deafen_->setChecked(false);
    }
    refresh();
}

void CommunityPanel::share() {
    if (capture_.running()) {
        stopSharing();
        return;
    }
    if (voiceCid_.isEmpty() || !requestScreenRecordingAccess())
        return;
    SharePicker dialog(this);
    QPointer<SharePicker> guarded(&dialog);
    listShareSources([guarded](QList<ShareSource> sources) {
        if (guarded)
            guarded->setSources(sources);
    });
    if (dialog.exec() != QDialog::Accepted || !dialog.chosen())
        return;
    if (!capture_.start(*dialog.chosen())) {
        noteError("Could not start screen sharing.");
        return;
    }
    stage_->addStream(node_->peerId(), true, name_->text().trimmed());
    startDrawing(node_->peerId(), true);
    presenterOverlay_ = std::make_unique<PresenterOverlay>(*dialog.chosen(), stage_->drawing(node_->peerId()), this);
    voiceState();
}

void CommunityPanel::stopSharing() {
    const bool wasSharing = capture_.running();
    presenterOverlay_.reset();
    if (node_) stopDrawing(node_->peerId());
    capture_.stop();
    watchers_.clear();
    if (node_) {
        stage_->removeStream(node_->peerId());
        decoders_.erase(node_->peerId());
    }
    if (wasSharing)
        voiceState();
}

void CommunityPanel::watch(const QString& peer, bool enabled) {
    if (voiceCid_.isEmpty())
        return;
    attempt([&] {
        node_->sendPacket(voiceCid_, voiceChannel_, "watch",
                          {{"target", peer}, {"enabled", enabled}});
        if (enabled) {
            watching_.insert(peer);
            streamRetry_.start();
            startDrawing(peer, false);
        }
        else {
            stopDrawing(peer);
            watching_.remove(peer);
            lastScreenFrames_.remove(peer);
            decoders_.erase(peer);
            frames_.remove(peer);
            if (watching_.isEmpty())
                streamRetry_.stop();
        }
        stage_->setWatching(peer, enabled);
    });
}

void CommunityPanel::dragEnterEvent(QDragEnterEvent* event) {
    if (online_ && composer_->isEnabled() &&
        (event->mimeData()->hasImage() || event->mimeData()->hasUrls()))
        event->acceptProposedAction();
}
void CommunityPanel::dropEvent(QDropEvent* event) {
    if (!online_ || !composer_->isEnabled())
        return;
    if (event->mimeData()->hasImage()) {
        const auto image = qvariant_cast<QImage>(event->mimeData()->imageData());
        prepareUpload([image] { return prepareImage(image); });
    }
    else {
        int count = 0;
        for (const auto& url : event->mimeData()->urls())
            if (url.isLocalFile() && count++ < 4) {
                const auto path = url.toLocalFile();
                prepareUpload([path] { return prepareImageFile(path); });
            }
    }
    event->acceptProposedAction();
}

bool CommunityPanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == qApp && event->type() == QEvent::FileOpen) {
        const auto url = static_cast<QFileOpenEvent*>(event)->url();
        if (url.scheme() == "disquisition") {
            acceptInvite(url.toString());
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

QStringList CommunityPanel::memberNames(const QString& cid) const {
    QStringList result;
    if (node_)
        for (const auto& member : node_->communities().value(cid).members)
            result.append(member.name);
    return result;
}

void CommunityPanel::updatePushToTalk() {
    QSettings settings;
    if (!voiceCid_.isEmpty() && settings.value("voice/pushToTalk", false).toBool() &&
        settings.contains("voice/pushToTalkKey"))
        pushToTalk_.start(settings.value("voice/pushToTalkKey").toUInt());
    else
        pushToTalk_.stop();
}

void CommunityPanel::startDrawing(const QString& peer, bool owner) {
    stopDrawing(peer);
    auto* drawing = stage_->drawing(peer);
    if (!drawing) return;
    auto session = std::make_unique<ScreenDrawingSession>(node_->peerId(), peer, drawing, owner);
    QHash<QString, QString> names;
    for (const auto& member : node_->communities().value(voiceCid_).members)
        names[member.id] = member.name;
    drawing->setAuthorNames(names);
    drawing->setChatColor(chatColor(messageColor_));
    const auto cid = voiceCid_, channel = voiceChannel_;
    connect(session.get(), &ScreenDrawingSession::sendToPeer, this,
        [this, cid, channel](const QString& target, const chat::Message& payload) {
            if (!online_) return;
            attempt([&] {
                node_->sendPacket(cid, channel, "drawing",
                    {{"target", target}, {"bytes", QString::fromLatin1(
                        QByteArray::fromStdString(chat::encode(payload)).toBase64())}});
            });
        });
    auto* active = session.get();
    drawings_[peer] = std::move(session);
    if (!owner) active->start();
}

void CommunityPanel::stopDrawing(const QString& peer) {
    const auto session = drawings_.find(peer);
    if (session == drawings_.end()) return;
    session->second->stop();
    drawings_.erase(session);
}

void CommunityPanel::publishSprite(const QString& cid, const QString& target) {
    if (!online_ || !node_) return;
    // Sprites describe a community member and do not depend on its channels.
    const QString channel;
    QSettings settings;
    auto sprites = PenguinSprites::fromSettings(settings);
    if (!settings.value("penguins/unlocked", false).toBool()) sprites = {};
    const auto bytes = sprites.wireData();
    if (bytes.size() > 256 * 1024) return;
    const auto chunks = splitImage(bytes);
    const auto transfer = QRandomGenerator::global()->generate();
    attempt([&] {
        if (bytes.isEmpty()) {
            node_->sendPacket(cid, channel, "sprite", {{"target", target}, {"count", 0}});
            return;
        }
        for (int index = 0; index < chunks.size(); ++index)
            node_->sendPacket(cid, channel, "sprite", {{"target", target}, {"transfer", double(transfer)},
                {"index", index}, {"count", chunks.size()},
                {"bytes", QString::fromLatin1(chunks[index].toBase64())}});
    });
}

void CommunityPanel::refreshPenguins() {
    if (!penguins_ || !node_) return;
    if (!online_) {
        penguins_->setMembers({});
        spritePeers_.clear();
        peerSprites_.clear();
        peerColors_.clear();
        spriteImages_.clear();
        return;
    }
    QSet<QString> knownPeers;
    for (const auto& community : node_->communities()) {
        const auto online = node_->online(community.id);
        // Metadata reconciliation can briefly make a live link unready. Keep its
        // cached appearance while the authenticated identity is still a member.
        for (const auto& member : community.members) knownPeers.insert(member.id);
        for (const auto& peer : online - spritePeers_.value(community.id))
            if (peer != node_->peerId()) publishSprite(community.id, peer);
        spritePeers_[community.id] = online;
    }
    for (auto it = peerSprites_.begin(); it != peerSprites_.end();)
        if (!knownPeers.contains(it.key())) it = peerSprites_.erase(it); else ++it;
    for (auto it = peerColors_.begin(); it != peerColors_.end();)
        if (!knownPeers.contains(it.key())) it = peerColors_.erase(it); else ++it;
    const auto online = node_->online(cid_);
    penguins_->setMembers(online.values());
    QMap<QString, QString> names;
    for (const auto& peer : online) names[peer] = memberName(cid_, peer);
    penguins_->setDisplayNames(names);
    for (const auto& peer : online) {
        penguins_->setColor(peer, peer == node_->peerId() ? chatColor(messageColor_)
                                                        : peerColors_.value(peer, chatColor("mint")));
        const auto sprites = peerSprites_.value(peer);
        penguins_->setCustomSprites(peer, sprites.first, sprites.second);
    }
    QSettings settings;
    const auto sprites = PenguinSprites::fromSettings(settings);
    if (settings.value("penguins/unlocked", false).toBool() && sprites.enabled)
        penguins_->setCustomSprites(node_->peerId(), sprites.standing, sprites.walking);
}

void CommunityPanel::commandReply(const QString& title, const QString& body) {
    Entry entry;
    entry.system = true;
    entry.message.timestamp = QDateTime::currentSecsSinceEpoch();
    entry.message.sender = title.toStdString();
    entry.message.body = body.toStdString();
    append(cid_, channel_, std::move(entry), false);
}

void CommunityPanel::runCommand(const QString& command) {
    const auto parts = command.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    const auto name = parts.value(0).toLower();
    const auto argument = command.mid(parts.value(0).size()).trimmed();
    if (name == "/color") {
        if (argument.isEmpty()) {
            commandReply("Color", "Current color: " + messageColor_ +
                "\nColors: pink, mint, butter, periwinkle, lilac, aqua, peach. Use /color <name or 0-255>.");
        } else if (!validChatColor(argument.toLower())) {
            commandReply("Color", "Unknown or reserved color: " + argument + ". Try /color for available colors.");
            return;
        } else {
            messageColor_ = argument.toLower();
            if (node_) penguins_->setColor(node_->peerId(), chatColor(messageColor_));
            for (const auto& [peer, session] : drawings_)
                if (auto* drawing = stage_->drawing(peer)) drawing->setChatColor(chatColor(messageColor_));
            commandReply("Color", "You chose " + messageColor_);
            refresh();
        }
    } else if (name == "/help") {
        commandReply("Help", "/users  list online members\n/color <name or 0-255>  set your name color\n"
            "/voice  join the selected voice channel\n/leavevoice  leave voice\n"
            "/mute, /unmute, /deafen, /undeafen  control voice\n/gif [search]  choose a GIF\n"
            "/clear  clear this channel on this device\n/save  open User Settings\n"
            "/dnd  mute or unmute notifications\n/leave  leave this community\n"
            "/quit, /exit  close the app\n@name, @everyone and @here highlight mentions");
    } else if (name == "/users") {
        QStringList names;
        if (node_ && online_)
            for (const auto& peer : node_->online(cid_)) names.append(memberName(cid_, peer));
        names.sort(Qt::CaseInsensitive);
        commandReply("Users", names.isEmpty() ? "Nobody is online." : names.join(", "));
    } else if (name == "/clear") {
        const auto scope = key(cid_, channel_);
        for (const auto& entry : entries_.value(scope)) cachedImageBytes_ -= entry.image.size();
        entries_.remove(scope);
        cachedImages_.removeIf([&](const auto& cached) { return cached.first == scope; });
        render();
    } else if (name == "/gif") {
        if (!online_) commandReply("GIF", "Connect before sending a GIF.");
        else if (gif_->isHidden()) commandReply("GIF", "GIF search is not available in this build.");
        else gifPicker_->popup(gif_, argument);
    } else if (name == "/save" || name == "/settings") {
        settings();
    } else if (name == "/voice") {
        if (selectedVoiceChannel_.isEmpty()) commandReply("Voice", "Select a voice channel first.");
        else joinVoice();
    } else if (name == "/leavevoice") {
        leaveVoice();
    } else if (name == "/mute" || name == "/unmute" || name == "/deafen" || name == "/undeafen") {
        if (voiceCid_.isEmpty()) commandReply("Voice", "Join voice first.");
        else if (name == "/mute") mute_->setChecked(true);
        else if (name == "/deafen") deafen_->setChecked(true);
        else if (name == "/undeafen") deafen_->setChecked(false);
        else if (deafen_->isChecked()) commandReply("Voice", "Undeafen before unmuting.");
        else mute_->setChecked(false);
    } else if (name == "/dnd") {
        notifications_->muteAction()->trigger();
        commandReply("Notifications", notifications_->settings().muted ? "Muted" : "On");
    } else if (name == "/waddleon") {
        QSettings().setValue("penguins/unlocked", true);
        penguinsAction_->setVisible(true);
        penguinsAction_->setChecked(true);
        refreshPenguins();
        commandReply("Penguins", "Waddle on! Desktop penguins are now available in User Settings.");
    } else if (name == "/leave") {
        leaveCommunity();
    } else if (name == "/quit" || name == "/exit") {
        window()->close();
    } else {
        commandReply("Command", "Unknown command: " + name + ". Try /help.");
    }
    composer_->clear();
}

void CommunityPanel::leaveCommunity(const QString& target) {
    const auto cid = target.isEmpty() ? cid_ : target;
    if (!node_ || !node_->communities().value(cid).isMember(node_->peerId())) return;
    const auto model = node_->communities().value(cid);
    const bool selected = cid == cid_;
    if (model.owner == node_->peerId()) {
        commandReply("Community", "You own this community. Open Manage > Community > Delete community to remove it for everyone.");
        return;
    }
    if (QMessageBox::question(this, "Leave community", "Leave " + model.name +
        "? You will need a new invite to rejoin.", QMessageBox::Yes | QMessageBox::Cancel,
        QMessageBox::Cancel) != QMessageBox::Yes) return;
    attempt([&] {
        if (voiceCid_ == model.id) leaveVoice();
        node_->edit(model.id, "leave", {});
        refresh();
        if (selected)
            render();
        note("Left " + model.name + '.');
    });
}

bool CommunityPanel::deleteCommunity(const QString& cid) {
    if (!node_) return false;
    const auto model = node_->communities().value(cid);
    if (!model.isMember(node_->peerId()) || model.owner != node_->peerId()) return false;
    if (QMessageBox::question(this, "Delete community", "Delete " + model.name +
        " for all members? This cannot be undone.", QMessageBox::Yes | QMessageBox::Cancel,
        QMessageBox::Cancel) != QMessageBox::Yes) return false;
    bool deleted = false;
    attempt([&] {
        if (voiceCid_ == model.id) leaveVoice();
        node_->edit(model.id, "delete", {});
        refresh();
        render();
        note("Deleted " + model.name + '.');
        deleted = true;
    });
    return deleted;
}
