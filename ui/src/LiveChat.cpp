#include "LiveChat.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDebug>
#include <QDesktopServices>
#include <QEvent>
#include <QFileInfo>
#include <QHash>
#include <QHBoxLayout>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QNetworkReply>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QStandardPaths>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <limits>

struct ChatPart {
    bool isEmote = false;
    QString text;
    QString url;
};
Q_DECLARE_METATYPE(ChatPart)
Q_DECLARE_METATYPE(QList<ChatPart>)

namespace {
constexpr int historyLimit = 200;
constexpr int imageLimit = 128 * 1024;
constexpr int emoteImageLimit = 512 * 1024;
constexpr int RoleAvatarUrl = Qt::UserRole;
constexpr int RolePlatform = Qt::UserRole + 1;
constexpr int RoleNick = Qt::UserRole + 2;
constexpr int RoleBody = Qt::UserRole + 3;
constexpr int RoleColor = Qt::UserRole + 4;
constexpr int RoleParts = Qt::UserRole + 5;

QString formatViewerCount(qint64 count)
{
    QString s = QString::number(std::max<qint64>(0, count));
    for (int i = s.size() - 3; i > 0; i -= 3)
        s.insert(i, QLatin1Char(' '));
    return s;
}

QString normalizeAccount(QString raw)
{
    raw = raw.trimmed();
    while (raw.startsWith('@')) raw.remove(0, 1);
    return raw.trimmed().left(64);
}

QString normalizeKickAccount(QString raw)
{
    raw = raw.trimmed();
    const int idx = raw.indexOf(QStringLiteral("kick.com/"), 0, Qt::CaseInsensitive);
    if (idx >= 0) raw = raw.mid(idx + 9);
    while (raw.startsWith('@') || raw.startsWith('/')) raw.remove(0, 1);
    const int slash = raw.indexOf('/');
    if (slash >= 0) raw = raw.left(slash);
    const int q = raw.indexOf('?');
    if (q >= 0) raw = raw.left(q);
    return raw.trimmed().left(32);
}

bool avatarHost(const QString &host)
{
    for (const auto *domain : {"tiktokcdn.com", "tiktokcdn-us.com", "tiktokcdn-eu.com", "tiktok.com", "ibyteimg.com", "byteoversea.com", "muscdn.com"}) {
        if (host == QLatin1StringView(domain) || host.endsWith(QStringLiteral(".") + QLatin1StringView(domain))) return true;
    }
    return false;
}

bool emoteHost(const QString &host)
{
    for (const auto *domain : {"files.kick.com", "static-cdn.jtvnw.net", "cdn.7tv.app", "cdn.betterttv.net", "cdn.frankerfacez.com"}) {
        if (host == QLatin1StringView(domain) || host.endsWith(QStringLiteral(".") + QLatin1StringView(domain))) return true;
    }
    return false;
}

int layoutMessageBody(QPainter *p,
                      const QRect &rect,
                      const QFont &font,
                      const QString &body,
                      const QList<ChatPart> &parts,
                      const QCache<QString, QPixmap> *emotes,
                      int emoteH)
{
    const QFontMetrics fm(font);
    const int availW = std::max(40, rect.width());
    if (parts.isEmpty()) {
        if (p) {
            p->setFont(font);
            p->drawText(rect, Qt::TextWordWrap | Qt::TextWrapAnywhere | Qt::AlignLeft | Qt::AlignTop, body);
        }
        return fm.boundingRect(QRect(0, 0, availW, 4000), Qt::TextWordWrap | Qt::TextWrapAnywhere, body).height();
    }

    struct DrawTok {
        bool isEmote = false;
        QString text;
        QPixmap pm;
        int w = 0;
        int h = 0;
    };
    QList<DrawTok> toks;
    toks.reserve(parts.size() * 2);

    const int textH = fm.height();
    const int spaceW = std::max(3, fm.horizontalAdvance(QLatin1Char(' ')));

    auto pushWordChunk = [&](const QString &word) {
        if (word.isEmpty()) return;
        const int ww = fm.horizontalAdvance(word);
        if (ww <= availW) {
            toks.append({false, word, {}, ww, textH});
            return;
        }
        // Break overly long unbreakable tokens to match TextWrapAnywhere
        int start = 0;
        while (start < word.size()) {
            int len = 1;
            int chunkW = fm.horizontalAdvance(word.mid(start, 1));
            while (start + len < word.size()) {
                const int nextW = fm.horizontalAdvance(word.mid(start, len + 1));
                if (nextW > availW) break;
                chunkW = nextW;
                ++len;
            }
            toks.append({false, word.mid(start, len), {}, chunkW, textH});
            start += len;
        }
    };

    for (const auto &part : parts) {
        if (part.isEmote) {
            const QPixmap *pm = (emotes && !part.url.isEmpty()) ? emotes->object(part.url) : nullptr;
            if (pm && !pm->isNull() && pm->height() > 0) {
                const qreal aspect = qreal(pm->width()) / qreal(pm->height());
                const int ew = std::clamp(int(std::lround(emoteH * aspect)), emoteH / 2, std::min(availW, emoteH * 3));
                toks.append({true, part.text, *pm, ew + 2, emoteH});
            } else {
                pushWordChunk(part.text);
            }
            continue;
        }
        // Split text preserving spaces as separate breakable tokens
        int i = 0;
        const int n = part.text.size();
        while (i < n) {
            if (part.text[i].isSpace()) {
                while (i < n && part.text[i].isSpace()) ++i;
                toks.append({false, QStringLiteral(" "), {}, spaceW, textH});
            } else {
                const int s = i;
                while (i < n && !part.text[i].isSpace()) ++i;
                pushWordChunk(part.text.mid(s, i - s));
            }
        }
    }

    struct LineItem {
        int tokIdx;
        int x;
    };
    QList<QList<LineItem>> lines;
    QList<int> lineHeights;

    QList<LineItem> curLine;
    int curX = 0;
    int curH = textH;

    for (int i = 0; i < toks.size(); ++i) {
        const auto &t = toks[i];
        const bool isSpace = !t.isEmote && t.text == QLatin1StringView(" ");
        if (isSpace && curX == 0) continue; // drop leading space on wrapped lines
        if (curX > 0 && curX + t.w > availW) {
            lines.append(curLine);
            lineHeights.append(curH);
            curLine.clear();
            curX = 0;
            curH = textH;
            if (isSpace) continue;
        }
        curLine.append({i, curX});
        curX += t.w;
        curH = std::max(curH, t.h);
    }
    if (!curLine.isEmpty()) {
        lines.append(curLine);
        lineHeights.append(curH);
    }

    const int lineGap = 2;
    int totalH = 0;
    if (p) p->setFont(font);
    for (int li = 0; li < lines.size(); ++li) {
        const int lh = lineHeights[li];
        const int y = rect.top() + totalH;
        if (p) {
            for (const auto &item : lines[li]) {
                const auto &t = toks[item.tokIdx];
                if (t.isEmote) {
                    const int drawW = std::max(1, t.w - 2);
                    const QRect dst(rect.left() + item.x + 1, y + (lh - t.h) / 2, drawW, t.h);
                    p->drawPixmap(dst, t.pm);
                } else {
                    const QRect dst(rect.left() + item.x, y, t.w + 2, lh);
                    p->drawText(dst, Qt::AlignLeft | Qt::AlignVCenter, t.text);
                }
            }
        }
        totalH += lh + (li + 1 < lines.size() ? lineGap : 0);
    }
    return std::max(textH, totalH);
}

QPixmap makePlatformBadge(const QString &platform, int sz = 16)
{
    const int px = sz * 2;
    QPixmap pm(px, px);
    pm.setDevicePixelRatio(2.0);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
    const QRectF r(0.5, 0.5, sz - 1.0, sz - 1.0);
    if (platform == QStringLiteral("kick")) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(83, 252, 24)); // Kick #53FC18
        p.drawRoundedRect(r, sz * 0.24, sz * 0.24);
        QFont f(QStringLiteral("Segoe UI"));
        f.setPixelSize(int(sz * 0.72));
        f.setWeight(QFont::Black);
        p.setFont(f);
        p.setPen(QColor(10, 14, 10));
        p.drawText(r, Qt::AlignCenter, QStringLiteral("K"));
    } else {
        p.setPen(QPen(QColor(255, 255, 255, 38), 0.75));
        p.setBrush(QColor(18, 19, 26));
        p.drawRoundedRect(r, sz * 0.24, sz * 0.24);
        QFont f(QStringLiteral("Segoe UI"));
        f.setPixelSize(int(sz * 0.74));
        f.setWeight(QFont::Black);
        p.setFont(f);
        p.setPen(QColor(37, 244, 238));
        p.drawText(r.translated(-0.7, -0.5), Qt::AlignCenter, QStringLiteral("T"));
        p.setPen(QColor(254, 44, 85));
        p.drawText(r.translated(0.7, 0.5), Qt::AlignCenter, QStringLiteral("T"));
        p.setPen(Qt::white);
        p.drawText(r, Qt::AlignCenter, QStringLiteral("T"));
    }
    return pm;
}

const QPixmap &platformBadgeCached(const QString &platform, int sz = 16)
{
    static QHash<QString, QPixmap> cache;
    const int clamped = std::clamp(sz, 12, 40);
    const QString key = (platform == QStringLiteral("kick") ? QStringLiteral("k:") : QStringLiteral("t:")) + QString::number(clamped);
    auto it = cache.find(key);
    if (it == cache.end()) it = cache.insert(key, makePlatformBadge(platform, clamped));
    return it.value();
}
}

class ChatMessageDelegate final : public QStyledItemDelegate
{
public:
    explicit ChatMessageDelegate(QListWidget *list, const QCache<QString, QPixmap> *emotes)
        : QStyledItemDelegate(list), m_list(list), m_emotes(emotes) {}

    void setScale(qreal scale) { m_scale = std::clamp(scale, 0.75, 2.20); }
    qreal scale() const { return m_scale; }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const QString body = index.data(RoleBody).toString();
        const auto parts = index.data(RoleParts).value<QList<ChatPart>>();
        const QIcon avatar = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
        const bool hasAvatar = !avatar.isNull();

        const int pxFont = std::clamp(int(std::lround(13.0 * m_scale)), 10, 32);
        const int avSz = std::clamp(int(std::lround(24.0 * m_scale)), 18, 52);
        const int badgeSz = std::clamp(int(std::lround(16.0 * m_scale)), 12, 34);
        const int emoteH = std::clamp(int(std::lround(pxFont * 1.55)), 16, 48);
        const int padX = std::max(6, int(std::lround(8.0 * m_scale)));
        const int padY = std::max(4, int(std::lround(5.0 * m_scale)));
        const int avGap = std::max(5, int(std::lround(6.0 * m_scale)));
        const int lineGap = std::max(2, int(std::lround(2.0 * m_scale)));

        const int viewW = (m_list && m_list->viewport()->width() > 40)
            ? m_list->viewport()->width()
            : std::max(160, option.rect.width());
        const int leftOff = padX + (hasAvatar ? avSz + avGap : 0);
        const int availW = std::max(60, viewW - leftOff - padX);

        QFont bodyF = option.font;
        bodyF.setPixelSize(pxFont);
        QFont nickF = bodyF;
        nickF.setBold(true);
        const QFontMetrics nfm(nickF);
        const int headH = std::max(badgeSz, nfm.height());
        const int bodyH = layoutMessageBody(nullptr, QRect(0, 0, availW, 4000), bodyF, body, parts, m_emotes, emoteH);
        const int contentH = padY + headH + lineGap + bodyH + padY + 2;
        const int minAvatarH = hasAvatar ? (padY * 2 + avSz + 2) : (padY * 2 + headH + 4);
        return {viewW, std::max(minAvatarH, contentH)};
    }

    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        p->save();
        p->setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
        if (option.state & QStyle::State_Selected)
            p->fillRect(option.rect, QColor(255, 255, 255, 16));

        const QString platform = index.data(RolePlatform).toString();
        const QString nick = index.data(RoleNick).toString();
        const QString body = index.data(RoleBody).toString();
        const auto parts = index.data(RoleParts).value<QList<ChatPart>>();
        const QString colorHex = index.data(RoleColor).toString();
        const QIcon avatar = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
        const bool hasAvatar = !avatar.isNull();

        const int pxFont = std::clamp(int(std::lround(13.0 * m_scale)), 10, 32);
        const int avSz = std::clamp(int(std::lround(24.0 * m_scale)), 18, 52);
        const int badgeSz = std::clamp(int(std::lround(16.0 * m_scale)), 12, 34);
        const int emoteH = std::clamp(int(std::lround(pxFont * 1.55)), 16, 48);
        const int padX = std::max(6, int(std::lround(8.0 * m_scale)));
        const int padY = std::max(4, int(std::lround(5.0 * m_scale)));
        const int avGap = std::max(5, int(std::lround(6.0 * m_scale)));
        const int badgeGap = std::max(4, int(std::lround(5.0 * m_scale)));
        const int lineGap = std::max(2, int(std::lround(2.0 * m_scale)));

        int x = option.rect.left() + padX;
        const int top = option.rect.top() + padY;
        if (hasAvatar) {
            const QRect avRect(x, top + 1, avSz, avSz);
            QPainterPath clip;
            clip.addEllipse(avRect);
            p->save();
            p->setClipPath(clip);
            avatar.paint(p, avRect);
            p->restore();
            x += avSz + avGap;
        }

        const int availW = std::max(60, option.rect.right() - x - padX);
        QFont bodyF = option.font;
        bodyF.setPixelSize(pxFont);
        QFont nickF = bodyF;
        nickF.setBold(true);
        const QFontMetrics nfm(nickF);
        const int headH = std::max(badgeSz, nfm.height());

        const QPixmap &badge = platformBadgeCached(platform, badgeSz);
        p->drawPixmap(x, top + (headH - badgeSz) / 2, badge);

        p->setFont(nickF);
        QColor nickCol = (platform == QStringLiteral("kick")) ? QColor(83, 252, 24) : QColor(120, 210, 255);
        if (!colorHex.isEmpty()) {
            const QColor parsed(colorHex);
            if (parsed.isValid() && parsed.lightness() > 55) nickCol = parsed;
        }
        p->setPen(nickCol);
        const int nickX = x + badgeSz + badgeGap;
        const int nickW = std::max(40, availW - badgeSz - badgeGap);
        p->drawText(QRect(nickX, top, nickW, headH), Qt::AlignVCenter | Qt::AlignLeft, nfm.elidedText(nick, Qt::ElideRight, nickW));

        p->setFont(bodyF);
        p->setPen(QColor(232, 234, 242));
        const int bodyTop = top + headH + lineGap;
        const QRect bodyRect(x, bodyTop, availW, std::max(pxFont + 4, option.rect.bottom() - bodyTop - padY + 2));
        layoutMessageBody(p, bodyRect, bodyF, body, parts, m_emotes, emoteH);
        p->restore();
    }

private:
    QListWidget *m_list = nullptr;
    const QCache<QString, QPixmap> *m_emotes = nullptr;
    qreal m_scale = 1.0;
};

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

        // Dedicated horizontal container strip separated above the chat list
        const QRectF strip(0.5, 0.5, width() - 1.0, height() - 1.0);
        painter.setPen(QPen(QColor(255, 255, 255, 24), 1.0));
        painter.setBrush(QColor(18, 20, 28, 210));
        painter.drawRoundedRect(strip, 6, 6);

        const qreal pad = 4.0;
        const qreal gap = 5.0;
        const qreal innerW = std::max(60.0, width() - pad * 2.0);
        const qreal cardW = (innerW - gap * 2.0) / 3.0;
        const qreal step = cardW + gap;

        painter.setClipRect(QRectF(pad, pad, innerW, height() - pad * 2.0));
        if (m_turn.state() != QAbstractAnimation::Running) {
            for (int i = 0; i < m_rows.size(); ++i) paintCard(painter, m_rows[i], pad + i * step, cardW);
            return;
        }
        const qreal phase = m_turn.currentValue().toDouble();
        const qreal fade = std::min(1.0, phase / 0.38);
        const qreal offset = step * QEasingCurve(QEasingCurve::OutCubic).valueForProgress(std::max(0.0, (phase - 0.38) / 0.62));
        if (fade < 1.0 && !m_rows.isEmpty()) {
            painter.setOpacity(1.0 - fade);
            paintCard(painter, m_rows.front(), pad, cardW);
            painter.setOpacity(1.0);
        }
        for (int i = 1; i < m_rows.size(); ++i) paintCard(painter, m_rows[i], pad + i * step - offset, cardW);
        if (!m_waiting.isEmpty()) paintCard(painter, m_waiting.head(), pad + 3.0 * step - offset, cardW);
    }

private:
    static constexpr int barHeight = 48;
    QList<Row> m_rows;
    QQueue<Row> m_waiting;
    QVariantAnimation m_turn;

    void changed()
    {
        const int h = m_rows.isEmpty() ? 0 : barHeight;
        if (minimumHeight() != h || maximumHeight() != h) {
            setFixedHeight(h);
            setVisible(h > 0);
        }
        QStringList descriptions;
        for (const auto &row : m_rows) descriptions.append(row.nick + ": " + row.text);
        setAccessibleDescription(descriptions.join('\n'));
        update();
    }

    void paintCard(QPainter &painter, const Row &row, qreal left, qreal cardW) const
    {
        const QRectF card(left, 4.0, std::max(24.0, cardW), barHeight - 8.0);
        painter.setPen(QPen(QColor(255, 196, 61, 95), 1.0));
        painter.setBrush(QColor(28, 25, 38, 235));
        painter.drawRoundedRect(card, 5, 5);
        const qreal x = card.left() + 7.0;
        const int available = std::max(0, int(card.width()) - 14);
        QFont nickF = painter.font();
        nickF.setPixelSize(11);
        nickF.setBold(true);
        painter.setFont(nickF);
        const QFontMetrics nfm(nickF);
        painter.setPen(QColor(255, 214, 102));
        painter.drawText(QRectF(x, card.top() + 3.0, available, 16.0), Qt::AlignVCenter | Qt::AlignLeft,
                         nfm.elidedText(row.nick, Qt::ElideRight, available));
        QFont giftF = painter.font();
        giftF.setPixelSize(12);
        giftF.setBold(false);
        painter.setFont(giftF);
        const QFontMetrics gfm(giftF);
        painter.setPen(QColor(242, 244, 250));
        painter.drawText(QRectF(x, card.top() + 19.0, available, 18.0), Qt::AlignVCenter | Qt::AlignLeft,
                         gfm.elidedText(row.text, Qt::ElideRight, available));
    }
};

LiveChat::LiveChat(QWidget *parent) : QWidget(parent)
{
    setMinimumWidth(200);

    // Row 1: TikTok LIVE username
    auto *accRow = new QHBoxLayout;
    accRow->setSpacing(5);
    auto *ttBadge = new QLabel(this);
    ttBadge->setPixmap(platformBadgeCached(QStringLiteral("tiktok"), 16));
    ttBadge->setToolTip("TikTok LIVE");
    m_accountEdit = new QLineEdit(this);
    m_accountEdit->setMinimumWidth(0);
    m_accountEdit->setPlaceholderText("tiktok_username");
    m_accountEdit->setToolTip("Enter a TikTok username (without @) and press Enter");
    auto *connectBtn = new QPushButton("Connect", this);
    connectBtn->setMinimumWidth(0);
    accRow->addWidget(ttBadge);
    accRow->addWidget(m_accountEdit, 1);
    accRow->addWidget(connectBtn);

    // Row 2: Kick.com channel
    auto *kickRow = new QHBoxLayout;
    kickRow->setSpacing(5);
    auto *kickBadge = new QLabel(this);
    kickBadge->setPixmap(platformBadgeCached(QStringLiteral("kick"), 16));
    kickBadge->setToolTip("Kick.com Live Chat");
    m_kickAccountEdit = new QLineEdit(this);
    m_kickAccountEdit->setMinimumWidth(0);
    m_kickAccountEdit->setPlaceholderText("kick_channel (e.g. zayosh)");
    m_kickAccountEdit->setToolTip("Enter a Kick.com channel name or URL and press Enter");
    auto *kickConnectBtn = new QPushButton("Connect", this);
    kickConnectBtn->setMinimumWidth(0);
    kickRow->addWidget(kickBadge);
    kickRow->addWidget(m_kickAccountEdit, 1);
    kickRow->addWidget(kickConnectBtn);

    m_status = new QLabel("Enter a TikTok or Kick channel above to connect.", this);
    m_status->setWordWrap(true);
    m_viewers = new QLabel(this);
    m_viewers->setObjectName("liveChatViewers");
    m_viewers->setAlignment(Qt::AlignCenter);
    m_viewers->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    updateViewerCount();
    auto *statusRow = new QHBoxLayout;
    statusRow->setSpacing(6);
    statusRow->addWidget(m_status, 1);
    statusRow->addWidget(m_viewers, 0, Qt::AlignVCenter);
    m_taps = new QLabel("Likes: —", this);
    m_taps->setObjectName("liveChatTaps");
    m_taps->setMinimumHeight(26);
    m_taps->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    m_gifts = new GiftNotifications(this);
    m_messages = new QListWidget(this);
    m_messages->setObjectName("liveChatMessages");
    m_delegate = new ChatMessageDelegate(m_messages, &m_emotes);
    m_messages->setItemDelegate(m_delegate);
    m_messages->setWordWrap(true);
    m_messages->setResizeMode(QListView::Adjust);
    m_messages->setTextElideMode(Qt::ElideNone);
    m_messages->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_messages->installEventFilter(this);
    m_messages->viewport()->installEventFilter(this);
    installEventFilter(this);
    m_connection = new QPushButton("Pause", this);
    auto *refresh = new QPushButton("Refresh", this);
    refresh->setToolTip("Reconnect TikTok and Kick chat immediately");
    m_autoScrollBtn = new QPushButton(this);
    m_autoScrollBtn->setCheckable(true);
    m_autoScrollBtn->setToolTip("Automatically scroll to the newest chat messages");
    auto *open = new QPushButton("Open LIVE", this);
    m_ttsSkipBtn = new QPushButton("Skip TTS", this);
    m_ttsSkipBtn->setToolTip("Skip the currently speaking TTS message");
    m_ttsMuteBtn = new QPushButton("Mute TTS", this);
    m_ttsMuteBtn->setCheckable(true);
    m_ttsMuteBtn->setToolTip("Mute / unmute built-in TTS audio");
    auto *actions = new QHBoxLayout;
    actions->setSpacing(5);
    actions->addWidget(m_connection);
    actions->addWidget(refresh);
    actions->addWidget(m_autoScrollBtn);
    actions->addWidget(open);
    actions->addWidget(m_ttsSkipBtn);
    actions->addWidget(m_ttsMuteBtn);

    auto *zoomOut = new QPushButton("-", this);
    zoomOut->setFixedSize(28, 26);
    zoomOut->setStyleSheet("padding: 0px; font-weight: bold; font-size: 14px;");
    zoomOut->setToolTip("Smaller chat text (Ctrl + Scroll down)");
    m_zoomReset = new QPushButton("100%", this);
    m_zoomReset->setMinimumSize(46, 26);
    m_zoomReset->setStyleSheet("padding: 0px 6px;");
    m_zoomReset->setToolTip("Reset chat zoom to 100% (Ctrl + 0)");
    auto *zoomIn = new QPushButton("+", this);
    zoomIn->setFixedSize(28, 26);
    zoomIn->setStyleSheet("padding: 0px; font-weight: bold; font-size: 14px;");
    zoomIn->setToolTip("Larger chat text (Ctrl + Scroll up)");
    auto *bottomBar = new QHBoxLayout;
    bottomBar->setSpacing(4);
    bottomBar->addWidget(zoomOut);
    bottomBar->addWidget(m_zoomReset);
    bottomBar->addWidget(zoomIn);
    bottomBar->addStretch(1);
    bottomBar->addWidget(m_taps);

    m_giftTotal = new QLabel(this);
    m_giftTotal->setStyleSheet("background: rgba(255, 214, 102, 18); border: 1px solid rgba(255, 196, 61, 65); border-radius: 5px; padding: 3px 8px; color: #ffd666; font-weight: 600;");
    m_giftTotal->setToolTip("Total TikTok gifts and diamonds received during this LIVE");
    m_giftEarned = new QLabel(this);
    m_giftEarned->setStyleSheet("background: rgba(74, 222, 128, 18); border: 1px solid rgba(74, 222, 128, 65); border-radius: 5px; padding: 3px 8px; color: #4ade80; font-weight: 600;");
    m_giftEarned->setToolTip("Estimated TikTok LIVE creator earnings from gifts (~$0.005 per diamond / 50% creator share)");
    updateGiftStats();
    auto *giftStatsBar = new QHBoxLayout;
    giftStatsBar->setSpacing(6);
    giftStatsBar->addWidget(m_giftTotal);
    giftStatsBar->addStretch(1);
    giftStatsBar->addWidget(m_giftEarned);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(5);
    layout->addLayout(accRow);
    layout->addLayout(kickRow);
    layout->addLayout(statusRow);
    layout->addWidget(m_gifts);
    layout->addWidget(m_messages, 1);
    layout->addLayout(actions);
    layout->addLayout(giftStatsBar);
    layout->addLayout(bottomBar);

    connect(zoomOut, &QPushButton::clicked, this, [this] { setChatScale(m_chatScale - 0.10); });
    connect(zoomIn, &QPushButton::clicked, this, [this] { setChatScale(m_chatScale + 0.10); });
    connect(m_zoomReset, &QPushButton::clicked, this, [this] { setChatScale(1.0); });
    const qreal savedScale = QSettings("TikStream", "UI2").value("chatScale", 1.0).toDouble();
    setChatScale(savedScale);
    const bool savedAutoScroll = QSettings("TikStream", "UI2").value("chatAutoScroll", true).toBool();
    setAutoScroll(savedAutoScroll);
    connect(m_autoScrollBtn, &QPushButton::toggled, this, &LiveChat::setAutoScroll);

    connect(m_accountEdit, &QLineEdit::returnPressed, this, &LiveChat::applyAccountFromInput);
    connect(connectBtn, &QPushButton::clicked, this, &LiveChat::applyAccountFromInput);
    connect(m_kickAccountEdit, &QLineEdit::returnPressed, this, &LiveChat::applyKickAccountFromInput);
    connect(kickConnectBtn, &QPushButton::clicked, this, &LiveChat::applyKickAccountFromInput);

    connect(open, &QPushButton::clicked, this, [this] {
        if (!m_account.isEmpty())
            QDesktopServices::openUrl(QUrl(QStringLiteral("https://www.tiktok.com/@%1/live").arg(m_account)));
        else if (!m_kickAccount.isEmpty())
            QDesktopServices::openUrl(QUrl(QStringLiteral("https://kick.com/%1").arg(m_kickAccount)));
    });
    connect(m_ttsSkipBtn, &QPushButton::clicked, this, &LiveChat::ttsSkipRequested);
    connect(m_ttsMuteBtn, &QPushButton::toggled, this, [this](bool muted) {
        m_ttsMuteBtn->setText(muted ? QStringLiteral("TTS Muted") : QStringLiteral("Mute TTS"));
        m_ttsMuteBtn->setStyleSheet(muted
            ? QStringLiteral("background: rgba(239, 68, 68, 45); border: 1px solid rgba(239, 68, 68, 130); color: #fca5a5;")
            : QString());
        emit ttsMuteToggled(muted);
    });
    connect(refresh, &QPushButton::clicked, this, [this] {
        m_paused = false;
        m_messages->clear();
        m_ttViewers = -1;
        m_kickViewers = -1;
        updateViewerCount();
        notifyOverlay();
        stopProcess();
        stopKickProcess();
        start();
        startKick();
    });
    connect(m_connection, &QPushButton::clicked, this, [this] {
        const bool anyRunning = m_process.state() != QProcess::NotRunning || m_kickProcess.state() != QProcess::NotRunning;
        if (anyRunning) {
            m_paused = true;
            stopProcess();
            stopKickProcess();
            m_ttViewers = -1;
            m_kickViewers = -1;
            updateViewerCount();
            if (!m_account.isEmpty()) m_ttStatus = QStringLiteral("TikTok: paused");
            if (!m_kickAccount.isEmpty()) m_kickStatus = QStringLiteral("Kick: paused");
            m_connection->setText("Resume");
            updateCombinedStatus();
            return;
        }
        m_paused = false;
        start();
        startKick();
    });

    connect(&m_process, &QProcess::readyReadStandardOutput, this, &LiveChat::readEvents);
    connect(&m_process, &QProcess::readyReadStandardError, this, [this] {
        const QString err = QString::fromUtf8(m_process.readAllStandardError()).trimmed();
        if (!err.isEmpty()) qWarning().noquote() << "TikTokChat:" << err.left(300);
    });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            setPlatformViewers(QStringLiteral("tiktok"), -1);
            m_ttStatus = QStringLiteral("TikTok: failed to start");
            updateCombinedStatus();
        }
    });
    connect(&m_process, &QProcess::finished, this, [this] {
        setPlatformViewers(QStringLiteral("tiktok"), -1);
        m_ttStatus = m_paused ? QStringLiteral("TikTok: paused") : QStringLiteral("TikTok: ended");
        updateCombinedStatus();
    });

    connect(&m_kickProcess, &QProcess::readyReadStandardOutput, this, &LiveChat::readKickEvents);
    connect(&m_kickProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            setPlatformViewers(QStringLiteral("kick"), -1);
            m_kickStatus = QStringLiteral("Kick: failed to start");
            updateCombinedStatus();
        }
    });
    connect(&m_kickProcess, &QProcess::finished, this, [this] {
        setPlatformViewers(QStringLiteral("kick"), -1);
        m_kickStatus = m_paused ? QStringLiteral("Kick: paused") : QStringLiteral("Kick: ended");
        updateCombinedStatus();
    });
}

LiveChat::~LiveChat()
{
    m_process.disconnect(this);
    if (m_process.state() != QProcess::NotRunning) {
        m_process.kill();
        m_process.waitForFinished(500);
    }
    m_kickProcess.disconnect(this);
    if (m_kickProcess.state() != QProcess::NotRunning) {
        m_kickProcess.kill();
        m_kickProcess.waitForFinished(500);
    }
}

void LiveChat::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (!m_started && !m_paused) {
        m_started = true;
        if (!m_account.isEmpty()) start();
        if (!m_kickAccount.isEmpty()) startKick();
    }
}

bool LiveChat::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::Wheel) {
        auto *we = static_cast<QWheelEvent *>(event);
        if (we->modifiers() & Qt::ControlModifier) {
            const int dy = we->angleDelta().y();
            if (dy != 0) setChatScale(m_chatScale + (dy > 0 ? 0.10 : -0.10));
            we->accept();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void LiveChat::keyPressEvent(QKeyEvent *event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        if (event->key() == Qt::Key_Plus || event->key() == Qt::Key_Equal) { setChatScale(m_chatScale + 0.10); event->accept(); return; }
        if (event->key() == Qt::Key_Minus) { setChatScale(m_chatScale - 0.10); event->accept(); return; }
        if (event->key() == Qt::Key_0) { setChatScale(1.0); event->accept(); return; }
    }
    QWidget::keyPressEvent(event);
}

void LiveChat::setChatScale(qreal scale)
{
    const qreal clamped = std::round(std::clamp(scale, 0.75, 2.20) * 20.0) / 20.0;
    m_chatScale = clamped;
    if (m_delegate) m_delegate->setScale(m_chatScale);
    if (m_zoomReset) m_zoomReset->setText(QStringLiteral("%1%").arg(int(std::lround(m_chatScale * 100.0))));
    if (m_messages) {
        m_messages->doItemsLayout();
        m_messages->viewport()->update();
        if (m_autoScroll) m_messages->scrollToBottom();
    }
    if (!QCoreApplication::arguments().contains("--test")) {
        QSettings qs("TikStream", "UI2");
        qs.setValue("chatScale", m_chatScale);
    }
}

void LiveChat::setAutoScroll(bool enabled)
{
    m_autoScroll = enabled;
    if (m_autoScrollBtn) {
        QSignalBlocker blocked(m_autoScrollBtn);
        m_autoScrollBtn->setChecked(m_autoScroll);
        m_autoScrollBtn->setText(m_autoScroll ? QStringLiteral("Auto-scroll: ON") : QStringLiteral("Auto-scroll: OFF"));
        m_autoScrollBtn->setStyleSheet(m_autoScroll
            ? QStringLiteral("background: rgba(74, 222, 128, 34); border: 1px solid rgba(74, 222, 128, 120); color: #86efac;")
            : QString());
    }
    if (m_autoScroll && m_messages) m_messages->scrollToBottom();
    if (!QCoreApplication::arguments().contains("--test")) {
        QSettings qs("TikStream", "UI2");
        qs.setValue("chatAutoScroll", m_autoScroll);
    }
}

void LiveChat::updateGiftStats()
{
    const QLocale en(QLocale::English);
    if (m_giftTotal) {
        m_giftTotal->setText(QStringLiteral("🎁 Gifts: %1  (%2 💎)")
            .arg(en.toString(m_totalGifts), en.toString(m_totalDiamonds)));
    }
    if (m_giftEarned) {
        const double usd = double(m_totalDiamonds) * 0.005;
        m_giftEarned->setText(QStringLiteral("Earned: ~$%1").arg(QString::number(usd, 'f', 2)));
    }
}

QList<QPair<QString, qint64>> LiveChat::topGifters(int limit) const
{
    QList<QPair<QString, qint64>> list;
    list.reserve(m_gifterDiamonds.size());
    for (auto it = m_gifterDiamonds.cbegin(); it != m_gifterDiamonds.cend(); ++it)
        list.append({it.key(), it.value()});
    std::sort(list.begin(), list.end(), [](const auto &a, const auto &b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    if (limit > 0 && list.size() > limit) list.resize(limit);
    return list;
}

void LiveChat::resetSessionStats()
{
    m_totalGifts = 0;
    m_totalDiamonds = 0;
    m_totalTaps = 0;
    m_totalFollows = 0;
    m_totalChatMessages = 0;
    m_gifterDiamonds.clear();
    updateGiftStats();
    emit statsChanged();
}

int LiveChat::viewerCount() const
{
    const bool hasTt = m_ttViewers >= 0;
    const bool hasKick = m_kickViewers >= 0;
    if (!hasTt && !hasKick) return -1;
    const qint64 total = (hasTt ? m_ttViewers : 0) + (hasKick ? m_kickViewers : 0);
    return int(std::min<qint64>(total, std::numeric_limits<int>::max()));
}

void LiveChat::handleViewerEvent(const QJsonObject &event, const QString &defaultPlatform)
{
    const QString platform = event["platform"].toString(defaultPlatform).trimmed().toLower();
    const auto countVal = event["count"];
    const qint64 count = countVal.isDouble() ? countVal.toInteger(-1) : -1;
    setPlatformViewers(platform.isEmpty() ? defaultPlatform : platform, count >= 0 ? count : -1);
}

void LiveChat::setPlatformViewers(const QString &platform, qint64 count)
{
    const qint64 normalized = count >= 0 ? count : -1;
    if (platform == QLatin1StringView("kick")) {
        if (m_kickViewers == normalized) return;
        m_kickViewers = normalized;
    } else {
        if (m_ttViewers == normalized) return;
        m_ttViewers = normalized;
    }
    updateViewerCount();
}

void LiveChat::updateViewerCount()
{
    const bool hasTt = m_ttViewers >= 0;
    const bool hasKick = m_kickViewers >= 0;
    const int total = viewerCount();
    const bool active = total >= 0;

    if (m_viewers) {
        m_viewers->setText(active
            ? QStringLiteral("Viewers: %1").arg(formatViewerCount(total))
            : QStringLiteral("Viewers: —"));
        if (hasTt && hasKick) {
            m_viewers->setToolTip(QStringLiteral("Live viewers: %1 (TikTok: %2 · Kick: %3)")
                .arg(formatViewerCount(total), formatViewerCount(m_ttViewers), formatViewerCount(m_kickViewers)));
        } else if (hasTt) {
            m_viewers->setToolTip(QStringLiteral("TikTok LIVE viewers: %1").arg(formatViewerCount(m_ttViewers)));
        } else if (hasKick) {
            m_viewers->setToolTip(QStringLiteral("Kick LIVE viewers: %1").arg(formatViewerCount(m_kickViewers)));
        } else {
            m_viewers->setToolTip(QStringLiteral("Current LIVE viewer count"));
        }
        if (!m_viewers->property("active").isValid() || m_viewers->property("active").toBool() != active) {
            m_viewers->setProperty("active", active);
            if (auto *st = m_viewers->style()) {
                st->unpolish(m_viewers);
                st->polish(m_viewers);
            }
        }
    }

    if (m_lastEmittedViewers != total) {
        m_lastEmittedViewers = total;
        emit viewerCountChanged(total);
    }
}

void LiveChat::ensureStarted()
{
    if (!m_started && !m_paused) {
        m_started = true;
        if (!m_account.isEmpty()) start();
        if (!m_kickAccount.isEmpty()) startKick();
    }
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

void LiveChat::updateCombinedStatus()
{
    QStringList parts;
    if (!m_ttStatus.isEmpty()) parts << m_ttStatus;
    if (!m_kickStatus.isEmpty()) parts << m_kickStatus;
    if (parts.isEmpty()) m_status->setText("Enter a TikTok or Kick channel above to connect.");
    else m_status->setText(parts.join(QStringLiteral("  |  ")));
    const bool anyRunning = m_process.state() != QProcess::NotRunning || m_kickProcess.state() != QProcess::NotRunning;
    m_connection->setText(anyRunning ? QStringLiteral("Pause") : (m_paused ? QStringLiteral("Resume") : QStringLiteral("Connect")));
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
        struct Msg { QString platform, nick, text, color; QIcon icon; QList<ChatPart> parts; };
        QList<Msg> msgs;
        const int count = m_messages->count();
        for (int i = std::max(0, count - 10); i < count; ++i) {
            const auto *it = m_messages->item(i);
            msgs.append({
                it->data(RolePlatform).toString(),
                it->data(RoleNick).toString(),
                it->data(RoleBody).toString(),
                it->data(RoleColor).toString(),
                it->icon(),
                it->data(RoleParts).value<QList<ChatPart>>(),
            });
        }
        if (m_demoPreview && msgs.isEmpty()) {
            msgs.append({"tiktok", "alex_gg", "Let's get the win today! 🔥", "", QIcon(), {}});
            msgs.append({"kick", "zayosh_fan", "Crisp quality and smooth 60 FPS!", "", QIcon(), {}});
            msgs.append({"tiktok", "mark99", "Shoutout from the stream!", "", QIcon(), {}});
        }

        QFont nickF("Segoe UI", 14, QFont::Bold);
        QFont bodyF("Segoe UI", 15, QFont::Normal);
        const QFontMetrics nfm(nickF);
        int curBottom = bottomY;
        for (int i = msgs.size() - 1; i >= 0; --i) {
            const auto &m = msgs[i];
            const bool hasIcon = !m.icon.isNull();
            const int iconSz = hasIcon ? 32 : 0;
            const int leftOff = 12 + (hasIcon ? iconSz + 10 : 0);
            const int textW = std::max(80, availW - leftOff - 12);
            const int bodyH = layoutMessageBody(nullptr, QRect(0, 0, textW, 200), bodyF, m.text, m.parts, &m_emotes, 26);
            const int headH = std::max(20, nfm.height());
            const int cardH = std::max(hasIcon ? 50 : 44, 8 + headH + 2 + bodyH + 10);
            if (curBottom - cardH < topY) break;
            const QRectF card(pad, curBottom - cardH, availW, cardH);
            p.setPen(QPen(QColor(255, 255, 255, 36), 1.0));
            p.setBrush(QColor(15, 17, 24, 185));
            p.drawRoundedRect(card, 12, 12);
            if (hasIcon) {
                const QRect ir(pad + 12, int(card.top()) + (cardH - iconSz) / 2, iconSz, iconSz);
                m.icon.paint(&p, ir);
            }
            const QPixmap &badge = platformBadgeCached(m.platform, 20);
            p.drawPixmap(pad + leftOff, int(card.top()) + 8 + (headH - 20) / 2, badge);
            p.setFont(nickF);
            QColor nickCol = (m.platform == QStringLiteral("kick")) ? QColor(83, 252, 24) : QColor(120, 210, 255);
            if (!m.color.isEmpty()) {
                const QColor parsed(m.color);
                if (parsed.isValid() && parsed.lightness() > 55) nickCol = parsed;
            }
            p.setPen(nickCol);
            p.drawText(QRectF(pad + leftOff + 26, card.top() + 8, std::max(40, textW - 26), headH),
                       Qt::AlignVCenter, nfm.elidedText(m.nick, Qt::ElideRight, std::max(40, textW - 26)));
            p.setFont(bodyF);
            p.setPen(QColor(245, 247, 252));
            layoutMessageBody(&p, QRect(pad + leftOff, int(card.top()) + 10 + headH, textW, bodyH + 4),
                              bodyF, m.text, m.parts, &m_emotes, 26);
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

void LiveChat::stopKickProcess()
{
    if (m_kickProcess.state() == QProcess::NotRunning) return;
    QSignalBlocker blocked(&m_kickProcess);
    m_kickProcess.kill();
    m_kickProcess.waitForFinished(500);
}

void LiveChat::setAccount(const QString &username)
{
    const QString norm = normalizeAccount(username);
    if (m_accountEdit && m_accountEdit->text() != norm) m_accountEdit->setText(norm);
    if (m_account == norm) return;
    m_account = norm;
    m_gifts->clear();
    m_totalGifts = 0;
    m_totalDiamonds = 0;
    updateGiftStats();
    m_tapsText.clear();
    m_taps->setText("Likes: —");
    setPlatformViewers(QStringLiteral("tiktok"), -1);
    notifyOverlay();
    stopProcess();
    if (m_account.isEmpty()) {
        m_ttStatus.clear();
        updateCombinedStatus();
        return;
    }
    if (m_started || isVisible()) start();
    else {
        m_ttStatus = QStringLiteral("TikTok: @%1 (waiting)").arg(m_account);
        updateCombinedStatus();
    }
}

void LiveChat::setKickAccount(const QString &channel)
{
    const QString norm = normalizeKickAccount(channel);
    if (m_kickAccountEdit && m_kickAccountEdit->text() != norm) m_kickAccountEdit->setText(norm);
    if (m_kickAccount == norm) return;
    m_kickAccount = norm;
    setPlatformViewers(QStringLiteral("kick"), -1);
    stopKickProcess();
    if (m_kickAccount.isEmpty()) {
        m_kickStatus.clear();
        updateCombinedStatus();
        return;
    }
    if (m_started || isVisible()) startKick();
    else {
        m_kickStatus = QStringLiteral("Kick: %1 (waiting)").arg(m_kickAccount);
        updateCombinedStatus();
    }
}

void LiveChat::applyAccountFromInput()
{
    const QString norm = normalizeAccount(m_accountEdit->text());
    m_accountEdit->setText(norm);
    const bool changed = (norm != m_account);
    m_account = norm;
    if (changed) {
        m_gifts->clear();
        m_totalGifts = 0;
        m_totalDiamonds = 0;
        updateGiftStats();
        m_tapsText.clear();
        m_taps->setText("Likes: —");
        notifyOverlay();
        emit accountChanged(m_account);
    }
    setPlatformViewers(QStringLiteral("tiktok"), -1);
    stopProcess();
    m_paused = false;
    m_started = true;
    start();
}

void LiveChat::applyKickAccountFromInput()
{
    const QString norm = normalizeKickAccount(m_kickAccountEdit->text());
    m_kickAccountEdit->setText(norm);
    const bool changed = (norm != m_kickAccount);
    m_kickAccount = norm;
    if (changed) emit kickAccountChanged(m_kickAccount);
    setPlatformViewers(QStringLiteral("kick"), -1);
    stopKickProcess();
    m_paused = false;
    m_started = true;
    startKick();
}

void LiveChat::start()
{
    setPlatformViewers(QStringLiteral("tiktok"), -1);
    if (m_account.isEmpty()) {
        m_ttStatus.clear();
        updateCombinedStatus();
        return;
    }
    m_started = true;
    m_paused = false;
    m_input.clear();
    const auto script = QCoreApplication::applicationDirPath() + "/chat/live.mjs";
    const auto bundledNode = QCoreApplication::applicationDirPath() + "/chat/node.exe";
    const auto node = QFileInfo::exists(bundledNode) ? bundledNode : QStandardPaths::findExecutable("node");
    if (node.isEmpty() || !QFileInfo::exists(script) || !QFileInfo::exists(QFileInfo(script).path() + "/node_modules/tiktok-live-connector/package.json")) {
        m_ttStatus = QStringLiteral("TikTok: module missing");
        updateCombinedStatus();
        return;
    }
    m_ttStatus = QStringLiteral("TikTok: connecting @%1…").arg(m_account);
    m_process.start(node, {script, m_account});
    updateCombinedStatus();
}

void LiveChat::startKick()
{
    setPlatformViewers(QStringLiteral("kick"), -1);
    if (m_kickAccount.isEmpty()) {
        m_kickStatus.clear();
        updateCombinedStatus();
        return;
    }
    m_started = true;
    m_paused = false;
    m_kickInput.clear();
    const auto script = QCoreApplication::applicationDirPath() + "/chat/kick.mjs";
    const auto bundledNode = QCoreApplication::applicationDirPath() + "/chat/node.exe";
    const auto node = QFileInfo::exists(bundledNode) ? bundledNode : QStandardPaths::findExecutable("node");
    if (node.isEmpty() || !QFileInfo::exists(script)) {
        m_kickStatus = QStringLiteral("Kick: script missing");
        updateCombinedStatus();
        return;
    }
    m_kickStatus = QStringLiteral("Kick: connecting %1…").arg(m_kickAccount);
    m_kickProcess.setStandardErrorFile(QProcess::nullDevice());
    m_kickProcess.start(node, {script, m_kickAccount});
    updateCombinedStatus();
}

void LiveChat::readEvents()
{
    m_input += m_process.readAllStandardOutput();
    qsizetype consumed = 0;
    for (qsizetype end; (end = m_input.indexOf('\n', consumed)) >= 0; consumed = end + 1) {
        if (end - consumed > 512 * 1024) continue;
        const auto event = QJsonDocument::fromJson(m_input.mid(consumed, end - consumed)).object();
        if (event["type"] == "messages") appendMessages(event["messages"].toArray());
        if (event["type"] == "clearGifts") {
            m_gifts->clear();
            m_totalGifts = 0;
            m_totalDiamonds = 0;
            updateGiftStats();
            notifyOverlay();
        }
        if (event["type"] == "giftStats") {
            m_totalGifts = std::max<qint64>(0, event["count"].toInteger(0));
            m_totalDiamonds = std::max<qint64>(0, event["diamonds"].toInteger(0));
            updateGiftStats();
            emit statsChanged();
        }
        if (event["type"] == "taps") {
            const auto count = event["count"].toInteger(-1);
            if (count < 0) { m_totalTaps = 0; m_tapsText.clear(); m_taps->setText("Likes: —"); }
            else {
                m_totalTaps = count;
                m_tapsText = QString(event["partial"].toBool() ? "Received likes: %1" : "Likes: %1")
                                 .arg(QLocale(QLocale::English).toString(count));
                m_taps->setText(m_tapsText);
            }
            notifyOverlay();
            emit statsChanged();
        }
        if (event["type"] == "viewers") {
            handleViewerEvent(event, QStringLiteral("tiktok"));
            continue;
        }
        if (event["type"] != "status") continue;
        const auto state = event["state"].toString();
        if (state != "connected") setPlatformViewers(QStringLiteral("tiktok"), -1);
        const auto codeVal = event["code"];
        const QString codeText = codeVal.isDouble()
            ? QStringLiteral(" (%1)").arg(codeVal.toInteger())
            : (codeVal.isString() && !codeVal.toString().isEmpty()
                ? QStringLiteral(" (%1)").arg(codeVal.toString().left(24))
                : QString());
        if (state == "connecting") m_ttStatus = QStringLiteral("TikTok: connecting @%1…").arg(m_account);
        else if (state == "connected") m_ttStatus = QStringLiteral("TikTok: LIVE · @%1").arg(m_account);
        else if (state == "offline") m_ttStatus = QStringLiteral("TikTok: @%1 offline").arg(m_account);
        else if (state == "disconnected") m_ttStatus = QStringLiteral("TikTok: reconnecting%1").arg(codeText);
        else if (state == "rate_limited") m_ttStatus = QStringLiteral("TikTok: rate-limited%1").arg(codeText);
        else if (state == "sign_error") m_ttStatus = QStringLiteral("TikTok: sign error%1").arg(codeText);
        else if (state == "ws_error") m_ttStatus = QStringLiteral("TikTok: WS error%1").arg(codeText);
        else if (state == "room_error") m_ttStatus = QStringLiteral("TikTok: room error%1").arg(codeText);
        else if (state == "unavailable") m_ttStatus = QStringLiteral("TikTok: unavailable%1").arg(codeText);
        updateCombinedStatus();
    }
    m_input.remove(0, consumed);
    if (m_input.size() > 512 * 1024) m_input.clear();
}

void LiveChat::readKickEvents()
{
    m_kickInput += m_kickProcess.readAllStandardOutput();
    qsizetype consumed = 0;
    for (qsizetype end; (end = m_kickInput.indexOf('\n', consumed)) >= 0; consumed = end + 1) {
        if (end - consumed > 512 * 1024) continue;
        const auto event = QJsonDocument::fromJson(m_kickInput.mid(consumed, end - consumed)).object();
        if (event["type"] == "messages") appendMessages(event["messages"].toArray());
        if (event["type"] == "viewers") {
            handleViewerEvent(event, QStringLiteral("kick"));
            continue;
        }
        if (event["type"] != "status") continue;
        const auto state = event["state"].toString();
        if (state != "connected") setPlatformViewers(QStringLiteral("kick"), -1);
        const auto codeVal = event["code"];
        const QString codeText = codeVal.isDouble()
            ? QStringLiteral(" (%1)").arg(codeVal.toInteger())
            : (codeVal.isString() && !codeVal.toString().isEmpty()
                ? QStringLiteral(" (%1)").arg(codeVal.toString().left(24))
                : QString());
        if (state == "connecting") m_kickStatus = QStringLiteral("Kick: connecting %1…").arg(m_kickAccount);
        else if (state == "connected") m_kickStatus = QStringLiteral("Kick: LIVE · %1").arg(m_kickAccount);
        else if (state == "not_found") m_kickStatus = QStringLiteral("Kick: '%1' not found").arg(m_kickAccount);
        else if (state == "disconnected") m_kickStatus = QStringLiteral("Kick: reconnecting%1").arg(codeText);
        else if (state == "ws_error") m_kickStatus = QStringLiteral("Kick: WS error%1").arg(codeText);
        else if (state == "unavailable") m_kickStatus = QStringLiteral("Kick: unavailable%1").arg(codeText);
        updateCombinedStatus();
    }
    m_kickInput.remove(0, consumed);
    if (m_kickInput.size() > 512 * 1024) m_kickInput.clear();
}

void LiveChat::appendMessages(const QJsonArray &messages)
{
    bool chatChanged = false;
    bool giftChanged = false;
    for (const auto &value : messages) {
        const auto message = value.toObject();
        const auto platform = message["platform"].toString(QStringLiteral("tiktok"));
        const auto nick = message["nick"].toString().left(100);
        const auto text = message["text"].toString().left(500);
        const auto url = message["avatar"].toString().left(1500);
        const auto color = message["color"].toString().left(16);
        if (nick.isEmpty() || text.isEmpty()) continue;
        if (message["kind"] == "gift") {
            const auto sender = message["senderId"].toString().left(160);
            const QString giftName = message["giftName"].toString();
            const int giftCount = std::max(1, message["giftCount"].toInt(1));
            const int diamonds = std::max(0, message["diamonds"].toInt(0));
            if (diamonds > 0) m_gifterDiamonds[nick] += diamonds;
            else if (!m_gifterDiamonds.contains(nick)) m_gifterDiamonds[nick] += giftCount;
            m_gifts->add(sender.isEmpty() ? nick : sender, nick, text);
            emit alertTriggered("gift", nick, text);
            emit giftReceived(nick, giftName, giftCount, diamonds, text);
            giftChanged = true;
            continue;
        }
        if (message["kind"] == "follow") {
            ++m_totalFollows;
            emit alertTriggered("follow", nick, "followed the channel!");
            emit followReceived(nick);
            emit statsChanged();
            continue;
        }
        ++m_totalChatMessages;
        emit chatMessageReceived(platform, nick, text);
        if (!chatChanged) {
            m_messages->setUpdatesEnabled(false);
            chatChanged = true;
        }
        if (m_messages->count() == historyLimit) delete m_messages->takeItem(0);
        auto *item = new QListWidgetItem(m_messages);
        item->setData(RolePlatform, platform);
        item->setData(RoleNick, nick);
        item->setData(RoleBody, text);
        item->setData(RoleColor, color);
        item->setData(RoleAvatarUrl, url);
        if (message.contains("parts")) {
            const auto arr = message.value("parts").toArray();
            QList<ChatPart> parts;
            parts.reserve(arr.size());
            for (const auto &pv : arr) {
                const auto po = pv.toObject();
                const QString kind = po.value("t").toString();
                if (kind == QLatin1StringView("e")) {
                    const QString eUrl = po.value("url").toString().left(512);
                    const QString eName = po.value("name").toString().left(64);
                    parts.append({true, eName, eUrl});
                    if (!eUrl.isEmpty() && !m_emotes.contains(eUrl)) requestEmote(eUrl);
                } else {
                    const QString s = po.value("v").toString();
                    if (!s.isEmpty()) parts.append({false, s, {}});
                }
            }
            if (!parts.isEmpty()) item->setData(RoleParts, QVariant::fromValue(parts));
        }
        if (!url.isEmpty()) {
            if (const auto *cached = m_avatars.object(url)) item->setIcon(*cached);
            else requestAvatar(url);
        }
    }
    if (chatChanged) {
        m_messages->setUpdatesEnabled(true);
        if (m_autoScroll) m_messages->scrollToBottom();
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
            if (item->data(RoleAvatarUrl).toString() == address) item->setIcon(icon);
        }
        notifyOverlay();
    });
}

void LiveChat::requestEmote(const QString &address)
{
    if (m_emotes.contains(address) || m_emoteRequests.contains(address) || m_emoteRequests.size() >= historyLimit) return;
    const QUrl url(address, QUrl::StrictMode);
    if (url.scheme() != "https" || !url.userInfo().isEmpty() || !emoteHost(url.host())) return;
    m_emoteRequests.insert(address);
    if (m_emoteRequests.size() <= 8) startEmote(address);
    else m_emoteQueue.enqueue(address);
}

void LiveChat::startEmote(const QString &address)
{
    QNetworkRequest request{QUrl(address)};
    request.setTransferTimeout(5000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    auto *reply = m_network.get(request);
    reply->setReadBufferSize(emoteImageLimit + 1);
    connect(reply, &QNetworkReply::readyRead, this, [reply] {
        if (reply->bytesAvailable() > emoteImageLimit) reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, address] {
        m_emoteRequests.remove(address);
        reply->deleteLater();
        if (!m_emoteQueue.isEmpty()) startEmote(m_emoteQueue.dequeue());
        if (reply->error() != QNetworkReply::NoError || reply->bytesAvailable() > emoteImageLimit) return;
        QBuffer imageData;
        imageData.setData(reply->readAll());
        imageData.open(QIODevice::ReadOnly);
        QImageReader reader(&imageData);
        reader.setAutoTransform(true);
        const auto size = reader.size();
        if (!size.isValid() || size.width() > 1024 || size.height() > 1024) return;
        if (size.height() > 64) {
            const int targetW = std::max(1, int(std::lround(qreal(size.width()) * 64.0 / qreal(size.height()))));
            reader.setScaledSize(QSize(std::min(256, targetW), 64));
        }
        const auto pixmap = QPixmap::fromImage(reader.read());
        if (pixmap.isNull()) return;
        m_emotes.insert(address, new QPixmap(pixmap));
        if (m_messages) {
            m_messages->doItemsLayout();
            m_messages->viewport()->update();
            if (m_autoScroll) m_messages->scrollToBottom();
        }
        notifyOverlay();
    });
}
