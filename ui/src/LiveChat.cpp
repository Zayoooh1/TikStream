#include "LiveChat.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QNetworkReply>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <algorithm>

namespace {
constexpr int historyLimit = 200;
constexpr int imageLimit = 128 * 1024;

QString normalizeAccount(QString raw)
{
    raw = raw.trimmed();
    while (raw.startsWith('@')) raw.remove(0, 1);
    return raw.trimmed().left(64);
}

bool avatarHost(const QString &host)
{
    for (const auto *domain : {"tiktokcdn.com", "tiktokcdn-us.com", "tiktokcdn-eu.com", "tiktok.com", "ibyteimg.com", "byteoversea.com", "muscdn.com"}) {
        if (host == QLatin1StringView(domain) || host.endsWith(QStringLiteral(".") + QLatin1StringView(domain))) return true;
    }
    return false;
}
}

class GiftNotifications final : public QWidget
{
public:
    explicit GiftNotifications(QWidget *parent) : QWidget(parent)
    {
        setObjectName("liveChatGifts");
        setAccessibleName("LIVE Gifts");
        hide();
        m_turn.setDuration(240);
        m_turn.setStartValue(0.0);
        m_turn.setEndValue(1.0);
        connect(&m_turn, &QVariantAnimation::valueChanged, this, [this] { update(); });
        connect(&m_turn, &QVariantAnimation::finished, this, [this] {
            m_rows.removeFirst();
            m_rows.append(m_waiting.dequeue());
            changed();
            if (!m_waiting.isEmpty()) m_turn.start();
        });
    }

    struct Row { QString sender, nick, text; };
    const QList<Row> &rows() const { return m_rows; }

    void clear()
    {
        m_turn.stop();
        m_rows.clear();
        m_waiting.clear();
        changed();
    }

    void add(const QString &sender, const QString &nick, const QString &text)
    {
        const Row incoming{sender, nick, text};
        const bool moving = m_turn.state() == QAbstractAnimation::Running;
        // An entry that is currently fading out can reappear at the bottom as a new notification.
        for (int i = moving ? 1 : 0; i < m_rows.size(); ++i) {
            if (m_rows[i].sender != sender) continue;
            m_rows[i] = incoming;
            changed();
            return;
        }
        for (auto &row : m_waiting) {
            if (row.sender != sender) continue;
            row = incoming;
            update();
            return;
        }
        if (m_rows.size() < 3) {
            m_rows.append(incoming);
            changed();
            return;
        }
        if (m_waiting.size() == 8) m_waiting.removeAt(1);
        m_waiting.enqueue(incoming);
        if (!moving) m_turn.start();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        if (m_turn.state() != QAbstractAnimation::Running) {
            for (int i = 0; i < m_rows.size(); ++i) paintRow(painter, m_rows[i], i * rowHeight);
            return;
        }
        const qreal phase = m_turn.currentValue().toDouble();
        const qreal fade = std::min(1.0, phase / 0.38);
        const qreal offset = rowHeight * QEasingCurve(QEasingCurve::OutCubic).valueForProgress(std::max(0.0, (phase - 0.38) / 0.62));
        if (fade < 1.0) {
            painter.setOpacity(1.0 - fade);
            paintRow(painter, m_rows.front(), 0);
            painter.setOpacity(1.0);
        }
        for (int i = 1; i < m_rows.size(); ++i) paintRow(painter, m_rows[i], i * rowHeight - offset);
        paintRow(painter, m_waiting.head(), 3 * rowHeight - offset);
    }

private:
    static constexpr int rowHeight = 44;
    QList<Row> m_rows;
    QQueue<Row> m_waiting;
    QVariantAnimation m_turn;

    void changed()
    {
        const int height = int(m_rows.size()) * rowHeight;
        if (minimumHeight() != height) {
            setFixedHeight(height);
            setVisible(height > 0);
        }
        QStringList descriptions;
        for (const auto &row : m_rows) descriptions.append(row.nick + ": " + row.text);
        setAccessibleDescription(descriptions.join('\n'));
        update();
    }

    void paintRow(QPainter &painter, const Row &row, qreal top) const
    {
        const int cardWidth = std::min(width(), 300);
        const QRectF card(width() - cardWidth, top + 2, cardWidth, rowHeight - 4);
        painter.setPen(Qt::NoPen);
        painter.setBrush(palette().base());
        painter.drawRoundedRect(card, 6, 6);
        const qreal x = card.left() + 8;
        const int available = std::max(0, cardWidth - 16);
        const auto fm = painter.fontMetrics();
        painter.setPen(palette().buttonText().color());
        painter.drawText(QRectF(x, top + 4, available, 17), Qt::AlignVCenter, fm.elidedText(row.nick, Qt::ElideRight, available));
        painter.setPen(palette().text().color());
        painter.drawText(QRectF(x, top + 21, available, 18), Qt::AlignVCenter, fm.elidedText(row.text, Qt::ElideRight, available));
    }
};

LiveChat::LiveChat(QWidget *parent) : QWidget(parent)
{
    setMinimumWidth(200);
    auto *accRow = new QHBoxLayout;
    accRow->setSpacing(4);
    auto *atLabel = new QLabel("@", this);
    m_accountEdit = new QLineEdit(this);
    m_accountEdit->setPlaceholderText("tiktok_username");
    m_accountEdit->setToolTip("Enter a TikTok username (without @) and press Enter");
    auto *connectBtn = new QPushButton("Connect", this);
    accRow->addWidget(atLabel);
    accRow->addWidget(m_accountEdit, 1);
    accRow->addWidget(connectBtn);

    m_status = new QLabel("Enter a TikTok @username above to connect to LIVE chat.", this);
    m_status->setWordWrap(true);
    m_taps = new QLabel("Likes: —", this);
    m_taps->setObjectName("liveChatTaps");
    m_gifts = new GiftNotifications(this);
    m_messages = new QListWidget(this);
    m_messages->setObjectName("liveChatMessages");
    m_messages->setWordWrap(true);
    m_messages->setResizeMode(QListView::Adjust);
    m_messages->setTextElideMode(Qt::ElideNone);
    m_messages->setIconSize({26, 26});
    m_messages->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_connection = new QPushButton("Connect", this);
    auto *refresh = new QPushButton("Refresh", this);
    refresh->setToolTip("Check immediately if the LIVE stream is active (without waiting 30 s)");
    auto *open = new QPushButton("Open LIVE", this);
    auto *actions = new QHBoxLayout;
    actions->addWidget(m_connection);
    actions->addWidget(refresh);
    actions->addWidget(open);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->addLayout(accRow);
    layout->addWidget(m_status);
    layout->addWidget(m_gifts);
    layout->addWidget(m_messages, 1);
    layout->addLayout(actions);
    layout->addWidget(m_taps, 0, Qt::AlignRight);

    connect(m_accountEdit, &QLineEdit::returnPressed, this, &LiveChat::applyAccountFromInput);
    connect(connectBtn, &QPushButton::clicked, this, &LiveChat::applyAccountFromInput);
    connect(open, &QPushButton::clicked, this, [this] {
        if (m_account.isEmpty()) return;
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://www.tiktok.com/@%1/live").arg(m_account)));
    });
    connect(refresh, &QPushButton::clicked, this, [this] {
        m_paused = false;
        stopProcess();
        start();
    });
    connect(m_connection, &QPushButton::clicked, this, [this] {
        if (m_process.state() != QProcess::NotRunning) {
            m_paused = true;
            m_process.kill();
            return;
        }
        start();
    });
    connect(&m_process, &QProcess::readyReadStandardOutput, this, &LiveChat::readEvents);
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            m_status->setText("Failed to start chat connection.");
            m_connection->setText("Reconnect");
        }
    });
    connect(&m_process, &QProcess::finished, this, [this] {
        m_status->setText(m_paused ? "Chat paused." : "Chat connection ended.");
        m_connection->setText(m_paused ? "Resume" : "Reconnect");
    });
}

LiveChat::~LiveChat()
{
    m_process.disconnect(this);
    if (m_process.state() != QProcess::NotRunning) {
        m_process.kill();
        m_process.waitForFinished(500);
    }
}

void LiveChat::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (!m_started) start();
}

void LiveChat::ensureStarted()
{
    if (!m_started && !m_paused) start();
}

void LiveChat::setDemoPreview(bool enabled)
{
    if (m_demoPreview == enabled) return;
    m_demoPreview = enabled;
    notifyOverlay();
}

void LiveChat::notifyOverlay()
{
    ++m_overlayRevision;
    emit overlayChanged();
}

QImage LiveChat::renderOverlay(int width, int height, bool showChat, bool showGifts, bool showTaps) const
{
    QImage img(width, height, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);

    const int pad = 12;
    const int availW = width - pad * 2;
    int topY = pad;
    int bottomY = height - pad;

    // 1. Likes counter in bottom-right corner
    const QString tapsLabel = m_demoPreview && m_tapsText.isEmpty() ? QStringLiteral("Likes: 12,450") : m_tapsText;
    if (showTaps && !tapsLabel.isEmpty()) {
        QFont tf("Segoe UI", 20, QFont::Bold);
        p.setFont(tf);
        const QFontMetrics fm(tf);
        const QString text = QStringLiteral("❤ ") + tapsLabel;
        const int pillW = std::min(availW, fm.horizontalAdvance(text) + 32);
        const int pillH = 46;
        const QRectF pill(width - pad - pillW, bottomY - pillH, pillW, pillH);
        p.setPen(QPen(QColor(255, 255, 255, 50), 1.0));
        p.setBrush(QColor(18, 18, 24, 195));
        p.drawRoundedRect(pill, 23, 23);
        p.setPen(QColor(255, 92, 138));
        p.drawText(pill, Qt::AlignCenter, fm.elidedText(text, Qt::ElideRight, pillW - 18));
        bottomY = int(pill.top()) - 10;
    }

    // 2. Gifts in top-right corner (max 3 cards)
    QList<GiftNotifications::Row> giftRows = m_gifts->rows();
    if (m_demoPreview && giftRows.isEmpty()) {
        giftRows.append({"demo1", "alex_gg", "🎁 Rose ×5"});
        giftRows.append({"demo2", "maya_live", "🎁 Heart ×2"});
    }
    if (showGifts && !giftRows.isEmpty()) {
        QFont nickF("Segoe UI", 14, QFont::Bold);
        QFont giftF("Segoe UI", 15, QFont::DemiBold);
        const int cardW = std::min(availW, 380);
        const int cardH = 58;
        for (int i = 0; i < giftRows.size() && i < 3; ++i) {
            if (topY + cardH > bottomY) break;
            const QRectF card(width - pad - cardW, topY, cardW, cardH);
            p.setPen(QPen(QColor(255, 196, 61, 160), 1.5));
            p.setBrush(QColor(24, 20, 32, 210));
            p.drawRoundedRect(card, 12, 12);
            const int tx = int(card.left()) + 14;
            const int tw = cardW - 28;
            p.setFont(nickF);
            p.setPen(QColor(255, 214, 102));
            p.drawText(QRectF(tx, topY + 6, tw, 22), Qt::AlignVCenter, p.fontMetrics().elidedText(giftRows[i].nick, Qt::ElideRight, tw));
            p.setFont(giftF);
            p.setPen(Qt::white);
            p.drawText(QRectF(tx, topY + 28, tw, 24), Qt::AlignVCenter, p.fontMetrics().elidedText(giftRows[i].text, Qt::ElideRight, tw));
            topY += cardH + 8;
        }
        topY += 4;
    }

    // 3. Chat messages from bottom to top (most recent visible)
    if (showChat && bottomY > topY + 40) {
        struct Msg { QString nick, text; QIcon icon; };
        QList<Msg> msgs;
        const int count = m_messages->count();
        for (int i = std::max(0, count - 10); i < count; ++i) {
            const auto *it = m_messages->item(i);
            const QString raw = it->text();
            const int nl = raw.indexOf('\n');
            msgs.append({nl >= 0 ? raw.left(nl) : raw, nl >= 0 ? raw.mid(nl + 1) : QString(), it->icon()});
        }
        if (m_demoPreview && msgs.isEmpty()) {
            msgs.append({"alex_gg", "Let's get the win today! 🔥", QIcon()});
            msgs.append({"sarah_tv", "Crisp quality and smooth 60 FPS!", QIcon()});
            msgs.append({"mark99", "Shoutout from the stream!", QIcon()});
        }

        QFont nickF("Segoe UI", 14, QFont::Bold);
        QFont bodyF("Segoe UI", 15, QFont::Normal);
        const QFontMetrics nfm(nickF);
        const QFontMetrics bfm(bodyF);
        int curBottom = bottomY;
        for (int i = msgs.size() - 1; i >= 0; --i) {
            const auto &m = msgs[i];
            const bool hasIcon = !m.icon.isNull();
            const int iconSz = hasIcon ? 32 : 0;
            const int leftOff = 12 + (hasIcon ? iconSz + 10 : 0);
            const int textW = std::max(80, availW - leftOff - 12);
            const QRect bodyRect = bfm.boundingRect(QRect(0, 0, textW, 200), Qt::TextWordWrap, m.text);
            const int cardH = std::max(hasIcon ? 50 : 44, 8 + nfm.height() + 2 + bodyRect.height() + 10);
            if (curBottom - cardH < topY) break;
            const QRectF card(pad, curBottom - cardH, availW, cardH);
            p.setPen(QPen(QColor(255, 255, 255, 36), 1.0));
            p.setBrush(QColor(15, 17, 24, 185));
            p.drawRoundedRect(card, 12, 12);
            if (hasIcon) {
                const QRect ir(pad + 12, int(card.top()) + (cardH - iconSz) / 2, iconSz, iconSz);
                m.icon.paint(&p, ir);
            }
            p.setFont(nickF);
            p.setPen(QColor(120, 210, 255));
            p.drawText(QRectF(pad + leftOff, card.top() + 8, textW, nfm.height()), Qt::AlignVCenter, nfm.elidedText(m.nick, Qt::ElideRight, textW));
            p.setFont(bodyF);
            p.setPen(QColor(245, 247, 252));
            p.drawText(QRectF(pad + leftOff, card.top() + 10 + nfm.height(), textW, bodyRect.height() + 4), Qt::TextWordWrap, m.text);
            curBottom = int(card.top()) - 8;
        }
    }
    return img;
}

void LiveChat::stopProcess()
{
    if (m_process.state() == QProcess::NotRunning) return;
    QSignalBlocker blocked(&m_process);
    m_process.kill();
    m_process.waitForFinished(500);
}

void LiveChat::setAccount(const QString &username)
{
    const QString norm = normalizeAccount(username);
    if (m_accountEdit && m_accountEdit->text() != norm) m_accountEdit->setText(norm);
    if (m_account == norm) return;
    m_account = norm;
    m_messages->clear();
    m_gifts->clear();
    m_tapsText.clear();
    m_taps->setText("Likes: —");
    notifyOverlay();
    stopProcess();
    if (m_account.isEmpty()) {
        m_started = false;
        m_status->setText("Enter a TikTok @username above to connect to LIVE chat.");
        m_connection->setText("Connect");
        return;
    }
    if (m_started || isVisible()) start();
    else m_status->setText(QStringLiteral("@%1 · waiting to connect").arg(m_account));
}

void LiveChat::applyAccountFromInput()
{
    const QString norm = normalizeAccount(m_accountEdit->text());
    m_accountEdit->setText(norm);
    const bool changed = (norm != m_account);
    m_account = norm;
    if (changed) {
        m_messages->clear();
        m_gifts->clear();
        m_tapsText.clear();
        m_taps->setText("Likes: —");
        notifyOverlay();
        emit accountChanged(m_account);
    }
    stopProcess();
    m_paused = false;
    start();
}

void LiveChat::start()
{
    if (m_account.isEmpty()) {
        m_status->setText("Enter a TikTok @username above to connect to LIVE chat.");
        m_connection->setText("Connect");
        return;
    }
    m_started = true;
    m_paused = false;
    m_input.clear();
    const auto script = QCoreApplication::applicationDirPath() + "/chat/live.mjs";
    const auto bundledNode = QCoreApplication::applicationDirPath() + "/chat/node.exe";
    const auto node = QFileInfo::exists(bundledNode) ? bundledNode : QStandardPaths::findExecutable("node");
    if (node.isEmpty() || !QFileInfo::exists(script) || !QFileInfo::exists(QFileInfo(script).path() + "/node_modules/tiktok-live-connector/package.json")) {
        m_status->setText("Node.js or TikTok chat module is missing.");
        m_connection->setText("Reconnect");
        return;
    }
    m_status->setText(QStringLiteral("Connecting to @%1…").arg(m_account));
    m_connection->setText("Pause");
    m_process.setStandardErrorFile(QProcess::nullDevice());
    m_process.start(node, {script, m_account});
}

void LiveChat::readEvents()
{
    m_input += m_process.readAllStandardOutput();
    qsizetype consumed = 0;
    for (qsizetype end; (end = m_input.indexOf('\n', consumed)) >= 0; consumed = end + 1) {
        if (end - consumed > 512 * 1024) continue;
        const auto event = QJsonDocument::fromJson(m_input.mid(consumed, end - consumed)).object();
        if (event["type"] == "messages") appendMessages(event["messages"].toArray());
        if (event["type"] == "clearGifts") { m_gifts->clear(); notifyOverlay(); }
        if (event["type"] == "taps") {
            const auto count = event["count"].toInteger(-1);
            if (count < 0) { m_tapsText.clear(); m_taps->setText("Likes: —"); }
            else {
                m_tapsText = QString(event["partial"].toBool() ? "Received likes: %1" : "Likes: %1")
                                 .arg(QLocale(QLocale::English).toString(count));
                m_taps->setText(m_tapsText);
            }
            notifyOverlay();
        }
        if (event["type"] != "status") continue;
        const auto state = event["state"].toString();
        const auto codeVal = event["code"];
        const QString codeText = codeVal.isDouble()
            ? QStringLiteral(" (code %1)").arg(codeVal.toInteger())
            : (codeVal.isString() && !codeVal.toString().isEmpty()
                ? QStringLiteral(" (%1)").arg(codeVal.toString().left(24))
                : QString());
        if (state == "connecting") m_status->setText(QStringLiteral("Connecting to @%1…").arg(m_account));
        else if (state == "connected") m_status->setText(QStringLiteral("LIVE · @%1").arg(m_account));
        else if (state == "offline") m_status->setText(QStringLiteral("@%1 is offline. Retrying in 30 s.").arg(m_account));
        else if (state == "disconnected") m_status->setText(QStringLiteral("WebSocket connection lost%1. Retrying in 30 s.").arg(codeText));
        else if (state == "rate_limited") m_status->setText(QStringLiteral("Signing service rate-limited%1. Retrying in 30 s.").arg(codeText));
        else if (state == "sign_error") m_status->setText(QStringLiteral("Chat signing service error%1. Retrying in 30 s.").arg(codeText));
        else if (state == "ws_error") m_status->setText(QStringLiteral("WebSocket connection error%1. Retrying in 30 s.").arg(codeText));
        else if (state == "room_error") m_status->setText(QStringLiteral("Cannot access LIVE room%1. Retrying in 30 s.").arg(codeText));
        else if (state == "unavailable") m_status->setText(QStringLiteral("TikTok chat is unavailable%1. Retrying in 30 s.").arg(codeText));
    }
    m_input.remove(0, consumed);
    if (m_input.size() > 512 * 1024) m_input.clear();
}

void LiveChat::appendMessages(const QJsonArray &messages)
{
    const auto *scroll = m_messages->verticalScrollBar();
    const bool follow = scroll->value() >= scroll->maximum() - 2;
    bool chatChanged = false;
    bool giftChanged = false;
    for (const auto &value : messages) {
        const auto message = value.toObject();
        const auto nick = message["nick"].toString().left(100);
        const auto text = message["text"].toString().left(500);
        const auto url = message["avatar"].toString().left(1500);
        if (nick.isEmpty() || text.isEmpty()) continue;
        if (message["kind"] == "gift") {
            const auto sender = message["senderId"].toString().left(160);
            m_gifts->add(sender.isEmpty() ? nick : sender, nick, text);
            emit alertTriggered("gift", nick, text);
            giftChanged = true;
            continue;
        }
        if (message["kind"] == "follow") {
            emit alertTriggered("follow", nick, "followed the channel!");
            continue;
        }
        if (!chatChanged) {
            m_messages->setUpdatesEnabled(false);
            chatChanged = true;
        }
        if (m_messages->count() == historyLimit) delete m_messages->takeItem(0);
        auto *item = new QListWidgetItem(nick + "\n" + text, m_messages);
        item->setData(Qt::UserRole, url);
        if (const auto *cached = m_avatars.object(url)) item->setIcon(*cached);
        else requestAvatar(url);
    }
    if (chatChanged) {
        m_messages->setUpdatesEnabled(true);
        if (follow) m_messages->scrollToBottom();
    }
    if (chatChanged || giftChanged) notifyOverlay();
}

void LiveChat::requestAvatar(const QString &address)
{
    if (m_requests.size() >= historyLimit || m_requests.contains(address)) return;
    const QUrl url(address, QUrl::StrictMode);
    if (url.scheme() != "https" || !url.userInfo().isEmpty() || !avatarHost(url.host())) return;
    m_requests.insert(address);
    if (m_requests.size() <= 6) startAvatar(address);
    else m_avatarQueue.enqueue(address);
}

void LiveChat::startAvatar(const QString &address)
{
    QNetworkRequest request{QUrl(address)};
    request.setTransferTimeout(5000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    auto *reply = m_network.get(request);
    reply->setReadBufferSize(imageLimit + 1);
    connect(reply, &QNetworkReply::readyRead, this, [reply] {
        if (reply->bytesAvailable() > imageLimit) reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, address] {
        m_requests.remove(address);
        reply->deleteLater();
        if (!m_avatarQueue.isEmpty()) startAvatar(m_avatarQueue.dequeue());
        if (reply->error() != QNetworkReply::NoError || reply->bytesAvailable() > imageLimit) return;
        QBuffer imageData;
        imageData.setData(reply->readAll());
        imageData.open(QIODevice::ReadOnly);
        QImageReader reader(&imageData);
        const auto size = reader.size();
        if (!size.isValid() || size.width() > 1024 || size.height() > 1024) return;
        reader.setScaledSize(size.scaled(32, 32, Qt::KeepAspectRatio));
        const auto pixmap = QPixmap::fromImage(reader.read());
        if (pixmap.isNull()) return;
        const QIcon icon(pixmap);
        m_avatars.insert(address, new QIcon(icon));
        for (int i = m_messages->count() - 1; i >= 0; --i) {
            auto *item = m_messages->item(i);
            if (item->data(Qt::UserRole).toString() == address) item->setIcon(icon);
        }
        notifyOverlay();
    });
}
