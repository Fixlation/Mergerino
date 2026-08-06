// SPDX-License-Identifier: MIT

#include "providers/seventv/SeventvBrowserAuth.hpp"

#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <functional>
#include <utility>
#include <vector>

namespace chatterino {

namespace {

const QUrl SEVENTV_LOGIN_URL(QStringLiteral("https://7tv.app/login"));
constexpr qint64 AUTH_TIMEOUT_MS = 5 * 60 * 1000;
constexpr qint64 LEVELDB_LOG_BLOCK_SIZE = 32768;

struct TokenRecord {
    bool seen = false;
    quint64 sequence = 0;
    QString token;
};

struct TokenCandidate {
    QString token;
    qint64 issuedAt = 0;
};

struct ScanResult {
    QString token;
};

quint32 readLittle32(const QByteArray &data, qsizetype offset)
{
    if (offset < 0 || offset + 4 > data.size())
        return 0;
    const auto *bytes = reinterpret_cast<const uchar *>(data.constData() + offset);
    return static_cast<quint32>(bytes[0]) |
           (static_cast<quint32>(bytes[1]) << 8) |
           (static_cast<quint32>(bytes[2]) << 16) |
           (static_cast<quint32>(bytes[3]) << 24);
}

quint64 readLittle64(const QByteArray &data, qsizetype offset)
{
    if (offset < 0 || offset + 8 > data.size())
        return 0;
    quint64 value = 0;
    for (int index = 7; index >= 0; --index)
    {
        value = (value << 8) |
                static_cast<uchar>(data.at(offset + index));
    }
    return value;
}

bool readVarint(const QByteArray &data, qsizetype &offset, quint64 &value)
{
    value = 0;
    for (int shift = 0; shift <= 63 && offset < data.size(); shift += 7)
    {
        const auto byte = static_cast<uchar>(data.at(offset++));
        if (shift == 63 && (byte & 0xfe) != 0)
            return false;
        value |= static_cast<quint64>(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0)
            return true;
    }
    return false;
}

bool copySnappyBytes(QByteArray &output, quint64 offset, quint64 length,
                     quint64 expectedSize)
{
    if (offset == 0 || offset > static_cast<quint64>(output.size()) ||
        length > expectedSize - static_cast<quint64>(output.size()))
    {
        return false;
    }
    for (quint64 index = 0; index < length; ++index)
    {
        output.append(output.at(output.size() - static_cast<qsizetype>(offset)));
    }
    return true;
}

bool decompressSnappy(const QByteArray &input, QByteArray &output)
{
    qsizetype position = 0;
    quint64 expectedSize = 0;
    if (!readVarint(input, position, expectedSize) ||
        expectedSize > 64ULL * 1024ULL * 1024ULL)
    {
        return false;
    }
    output.clear();
    output.reserve(static_cast<qsizetype>(expectedSize));

    while (position < input.size() &&
           static_cast<quint64>(output.size()) < expectedSize)
    {
        const auto tag = static_cast<uchar>(input.at(position++));
        const auto type = tag & 0x03;
        if (type == 0)
        {
            quint64 length = tag >> 2;
            if (length < 60)
            {
                length += 1;
            }
            else
            {
                const auto byteCount = static_cast<int>(length - 59);
                if (byteCount < 1 || byteCount > 4 ||
                    position + byteCount > input.size())
                {
                    return false;
                }
                length = 0;
                for (int index = 0; index < byteCount; ++index)
                {
                    length |= static_cast<quint64>(static_cast<uchar>(
                                  input.at(position++)))
                              << (index * 8);
                }
                length += 1;
            }
            if (length > expectedSize - static_cast<quint64>(output.size()) ||
                length > static_cast<quint64>(input.size() - position))
            {
                return false;
            }
            output.append(input.constData() + position,
                          static_cast<qsizetype>(length));
            position += static_cast<qsizetype>(length);
            continue;
        }

        quint64 length = 0;
        quint64 copyOffset = 0;
        if (type == 1)
        {
            if (position >= input.size())
                return false;
            length = 4 + ((tag >> 2) & 0x07);
            copyOffset = (static_cast<quint64>(tag & 0xe0) << 3) |
                         static_cast<uchar>(input.at(position++));
        }
        else if (type == 2)
        {
            if (position + 2 > input.size())
                return false;
            length = 1 + (tag >> 2);
            copyOffset = static_cast<uchar>(input.at(position)) |
                         (static_cast<quint64>(static_cast<uchar>(
                              input.at(position + 1)))
                          << 8);
            position += 2;
        }
        else
        {
            if (position + 4 > input.size())
                return false;
            length = 1 + (tag >> 2);
            copyOffset = readLittle32(input, position);
            position += 4;
        }
        if (!copySnappyBytes(output, copyOffset, length, expectedSize))
            return false;
    }
    return static_cast<quint64>(output.size()) == expectedSize;
}

using BlockVisitor =
    std::function<void(const QByteArray &, const QByteArray &)>;

bool visitLevelDBBlock(const QByteArray &block, const BlockVisitor &visitor)
{
    if (block.size() < 4)
        return false;
    const auto restartCount = readLittle32(block, block.size() - 4);
    const auto restartBytes = static_cast<quint64>(restartCount) * 4ULL + 4ULL;
    if (restartBytes > static_cast<quint64>(block.size()))
        return false;
    const auto entriesEnd =
        block.size() - static_cast<qsizetype>(restartBytes);

    qsizetype position = 0;
    QByteArray previousKey;
    while (position < entriesEnd)
    {
        quint64 shared = 0;
        quint64 unshared = 0;
        quint64 valueLength = 0;
        if (!readVarint(block, position, shared) ||
            !readVarint(block, position, unshared) ||
            !readVarint(block, position, valueLength) ||
            shared > static_cast<quint64>(previousKey.size()) ||
            unshared > static_cast<quint64>(entriesEnd - position))
        {
            return false;
        }
        QByteArray key = previousKey.left(static_cast<qsizetype>(shared));
        key.append(block.constData() + position,
                   static_cast<qsizetype>(unshared));
        position += static_cast<qsizetype>(unshared);
        if (valueLength > static_cast<quint64>(entriesEnd - position))
            return false;
        const QByteArray value(block.constData() + position,
                               static_cast<qsizetype>(valueLength));
        position += static_cast<qsizetype>(valueLength);
        visitor(key, value);
        previousKey = std::move(key);
    }
    return position == entriesEnd;
}

bool readTableBlock(const QByteArray &table, quint64 offset, quint64 size,
                    QByteArray &block)
{
    if (offset > static_cast<quint64>(table.size()) ||
        size > static_cast<quint64>(table.size()) - offset ||
        size + 5 > static_cast<quint64>(table.size()) - offset)
    {
        return false;
    }
    const auto compressed = table.mid(static_cast<qsizetype>(offset),
                                      static_cast<qsizetype>(size));
    const auto compression =
        static_cast<uchar>(table.at(static_cast<qsizetype>(offset + size)));
    if (compression == 0)
    {
        block = compressed;
        return true;
    }
    if (compression == 1)
        return decompressSnappy(compressed, block);
    return false;
}

bool decodeBlockHandle(const QByteArray &data, quint64 &offset, quint64 &size)
{
    qsizetype position = 0;
    return readVarint(data, position, offset) &&
           readVarint(data, position, size);
}

QString jwtFromBytes(const QByteArray &value)
{
    for (qsizetype start = 0; start + 16 < value.size(); ++start)
    {
        if (value.at(start) != 'e' || value.at(start + 1) != 'y' ||
            value.at(start + 2) != 'J')
        {
            continue;
        }
        qsizetype end = start;
        int dots = 0;
        while (end < value.size() && end - start <= 8192)
        {
            const auto character = value.at(end);
            const bool allowed =
                (character >= 'A' && character <= 'Z') ||
                (character >= 'a' && character <= 'z') ||
                (character >= '0' && character <= '9') || character == '-' ||
                character == '_' || character == '.';
            if (!allowed)
                break;
            if (character == '.')
                ++dots;
            ++end;
        }
        if (dots == 2 && end - start >= 40)
            return QString::fromLatin1(value.mid(start, end - start));
    }
    return {};
}

qint64 usableJwtIssuedAt(const QString &token)
{
    const auto parts = token.toLatin1().split('.');
    if (parts.size() != 3 || parts[0].isEmpty() || parts[1].isEmpty() ||
        parts[2].isEmpty())
    {
        return 0;
    }
    const auto payload = QJsonDocument::fromJson(
        QByteArray::fromBase64(parts[1], QByteArray::Base64UrlEncoding |
                                            QByteArray::AbortOnBase64DecodingErrors))
                             .object();
    const auto expiration =
        payload.value(QStringLiteral("exp")).toVariant().toLongLong();
    if (payload.value(QStringLiteral("iss")).toString() !=
            QStringLiteral("7tv.io") ||
        payload.value(QStringLiteral("sub")).toString().isEmpty() ||
        expiration <= QDateTime::currentSecsSinceEpoch())
    {
        return 0;
    }
    const auto issuedAt =
        payload.value(QStringLiteral("iat")).toVariant().toLongLong();
    return std::max<qint64>(issuedAt, 1);
}

void considerCandidate(TokenCandidate &best, const QString &token)
{
    const auto issuedAt = usableJwtIssuedAt(token);
    if (issuedAt > best.issuedAt)
        best = {token, issuedAt};
}

void scanTokenBytes(const QByteArray &bytes, TokenCandidate &best)
{
    qsizetype position = 0;
    while ((position = bytes.indexOf("eyJ", position)) >= 0)
    {
        considerCandidate(best, jwtFromBytes(bytes.mid(position, 8193)));
        position += 3;
    }
}

bool isSevenTVTokenKey(const QByteArray &key)
{
    return key.contains("7tv.app") && key.contains("7tv-token");
}

void considerTokenRecord(TokenRecord &record, const QByteArray &key,
                         const QByteArray &value, quint64 sequence,
                         bool deleted)
{
    if (!isSevenTVTokenKey(key) || (record.seen && sequence < record.sequence))
        return;
    record.seen = true;
    record.sequence = sequence;
    record.token = deleted ? QString{} : jwtFromBytes(value);
}

void scanLevelDBTable(const QString &path, TokenRecord &record)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return;
    const auto table = file.readAll();
    if (table.size() < 48)
        return;

    qsizetype footerPosition = table.size() - 48;
    quint64 ignoredOffset = 0;
    quint64 ignoredSize = 0;
    quint64 indexOffset = 0;
    quint64 indexSize = 0;
    if (!readVarint(table, footerPosition, ignoredOffset) ||
        !readVarint(table, footerPosition, ignoredSize) ||
        !readVarint(table, footerPosition, indexOffset) ||
        !readVarint(table, footerPosition, indexSize))
    {
        return;
    }

    QByteArray indexBlock;
    if (!readTableBlock(table, indexOffset, indexSize, indexBlock))
        return;
    std::vector<std::pair<quint64, quint64>> handles;
    visitLevelDBBlock(indexBlock, [&handles](const QByteArray &,
                                             const QByteArray &value) {
        quint64 offset = 0;
        quint64 size = 0;
        if (decodeBlockHandle(value, offset, size))
            handles.emplace_back(offset, size);
    });

    for (const auto &[offset, size] : handles)
    {
        QByteArray dataBlock;
        if (!readTableBlock(table, offset, size, dataBlock))
            continue;
        visitLevelDBBlock(
            dataBlock, [&record](const QByteArray &internalKey,
                                 const QByteArray &value) {
                if (internalKey.size() < 8)
                    return;
                const auto trailer =
                    readLittle64(internalKey, internalKey.size() - 8);
                const auto sequence = trailer >> 8;
                const auto type = static_cast<uchar>(trailer & 0xff);
                considerTokenRecord(record, internalKey.left(
                                                internalKey.size() - 8),
                                    value, sequence, type == 0);
            });
    }
}

void scanWriteBatch(const QByteArray &batch, TokenRecord &record)
{
    if (batch.size() < 12)
        return;
    const auto baseSequence = readLittle64(batch, 0);
    const auto count = readLittle32(batch, 8);
    qsizetype position = 12;
    for (quint32 index = 0; index < count && position < batch.size(); ++index)
    {
        const auto type = static_cast<uchar>(batch.at(position++));
        quint64 keyLength = 0;
        if (!readVarint(batch, position, keyLength) ||
            keyLength > static_cast<quint64>(batch.size() - position))
        {
            return;
        }
        const QByteArray key(batch.constData() + position,
                             static_cast<qsizetype>(keyLength));
        position += static_cast<qsizetype>(keyLength);
        QByteArray value;
        if (type == 1)
        {
            quint64 valueLength = 0;
            if (!readVarint(batch, position, valueLength) ||
                valueLength > static_cast<quint64>(batch.size() - position))
            {
                return;
            }
            value = QByteArray(batch.constData() + position,
                               static_cast<qsizetype>(valueLength));
            position += static_cast<qsizetype>(valueLength);
        }
        else if (type != 0)
        {
            return;
        }
        considerTokenRecord(record, key, value, baseSequence + index,
                            type == 0);
    }
}

void scanLevelDBLog(const QString &path, TokenRecord &record)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return;
    const auto log = file.readAll();
    QByteArray fragmented;
    qsizetype position = 0;
    while (position + 7 <= log.size())
    {
        const auto blockRemaining =
            LEVELDB_LOG_BLOCK_SIZE - (position % LEVELDB_LOG_BLOCK_SIZE);
        if (blockRemaining < 7)
        {
            position += blockRemaining;
            continue;
        }
        const auto length = static_cast<quint16>(
            static_cast<uchar>(log.at(position + 4)) |
            (static_cast<quint16>(static_cast<uchar>(log.at(position + 5)))
             << 8));
        const auto type = static_cast<uchar>(log.at(position + 6));
        position += 7;
        if (length == 0 && type == 0)
        {
            position += blockRemaining - 7;
            continue;
        }
        if (length > log.size() - position || length > blockRemaining - 7)
            break;
        const QByteArray payload(log.constData() + position, length);
        position += length;
        if (type == 1)
        {
            scanWriteBatch(payload, record);
        }
        else if (type == 2)
        {
            fragmented = payload;
        }
        else if (type == 3 && !fragmented.isEmpty())
        {
            fragmented.append(payload);
        }
        else if (type == 4 && !fragmented.isEmpty())
        {
            fragmented.append(payload);
            scanWriteBatch(fragmented, record);
            fragmented.clear();
        }
    }
}

void scanLevelDBTableTokens(const QString &path, TokenCandidate &best)
{
    constexpr quint64 LEVELDB_TABLE_MAGIC = 0xdb4775248b80fb57ULL;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return;
    const auto table = file.readAll();
    if (table.size() < 48 ||
        readLittle64(table, table.size() - 8) != LEVELDB_TABLE_MAGIC)
    {
        return;
    }

    const auto footer = table.mid(table.size() - 48, 40);
    qsizetype footerPosition = 0;
    quint64 ignoredOffset = 0;
    quint64 ignoredSize = 0;
    quint64 indexOffset = 0;
    quint64 indexSize = 0;
    if (!readVarint(footer, footerPosition, ignoredOffset) ||
        !readVarint(footer, footerPosition, ignoredSize) ||
        !readVarint(footer, footerPosition, indexOffset) ||
        !readVarint(footer, footerPosition, indexSize))
    {
        return;
    }

    QByteArray indexBlock;
    if (!readTableBlock(table, indexOffset, indexSize, indexBlock))
        return;

    std::vector<std::pair<quint64, quint64>> handles;
    if (!visitLevelDBBlock(
            indexBlock, [&handles](const QByteArray &, const QByteArray &value) {
                quint64 offset = 0;
                quint64 size = 0;
                if (decodeBlockHandle(value, offset, size))
                    handles.emplace_back(offset, size);
            }))
    {
        return;
    }

    for (const auto &[offset, size] : handles)
    {
        QByteArray dataBlock;
        if (readTableBlock(table, offset, size, dataBlock))
            scanTokenBytes(dataBlock, best);
    }
}

void scanRawFileTokens(const QString &path, TokenCandidate &best)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return;

    QByteArray overlap;
    while (!file.atEnd())
    {
        const auto chunk = file.read(1024 * 1024);
        if (chunk.isEmpty())
            break;
        const auto bytes = overlap + chunk;
        scanTokenBytes(bytes, best);
        overlap = bytes.right(8192);
    }
}

QString scanChromiumDatabase(const QString &path)
{
    TokenCandidate best;
    const auto files = QDir(path).entryInfoList(
        {QStringLiteral("*.ldb"), QStringLiteral("*.sst"),
         QStringLiteral("*.log")},
        QDir::Files, QDir::Time);
    for (const auto &file : files)
    {
        if (file.suffix().compare(QStringLiteral("log"),
                                  Qt::CaseInsensitive) != 0)
        {
            scanLevelDBTableTokens(file.absoluteFilePath(), best);
        }
        scanRawFileTokens(file.absoluteFilePath(), best);
    }
    return best.token;
}

void addChromiumProfiles(const QString &root, QStringList &databases)
{
    if (root.isEmpty() || !QDir(root).exists())
        return;
    const auto addProfileDatabases = [&databases](const QString &profilePath) {
        const auto indexedDB =
            QDir(profilePath).filePath(QStringLiteral(
                "IndexedDB/https_7tv.app_0.indexeddb.leveldb"));
        if (QDir(indexedDB).exists())
        {
            databases.push_back(indexedDB);
        }

        const auto localStorage = QDir(profilePath).filePath(
            QStringLiteral("Local Storage/leveldb"));
        if (QDir(localStorage).exists())
        {
            databases.push_back(localStorage);
        }
    };
    addProfileDatabases(root);
    const auto profiles = QDir(root).entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time);
    for (const auto &profile : profiles)
    {
        const auto name = profile.fileName();
        if (name != QStringLiteral("Default") &&
            !name.startsWith(QStringLiteral("Profile ")) &&
            name != QStringLiteral("Guest Profile"))
        {
            continue;
        }
        addProfileDatabases(profile.absoluteFilePath());
    }
}

QString defaultBrowserProgID()
{
#ifdef Q_OS_WIN
    QSettings settings(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\Shell\\Associations\\UrlAssociations\\https\\UserChoice"),
        QSettings::NativeFormat);
    return settings.value(QStringLiteral("ProgId")).toString().toLower();
#else
    return {};
#endif
}

QStringList chromiumDatabasesForDefaultBrowser()
{
    const auto local = qEnvironmentVariable("LOCALAPPDATA");
    const auto roaming = qEnvironmentVariable("APPDATA");
    const auto progID = defaultBrowserProgID();
    QStringList roots;
    const auto addKnownRoot = [&roots](const QString &path) {
        if (!path.isEmpty() && !roots.contains(path))
            roots.push_back(path);
    };
    if (progID.contains(QStringLiteral("brave")))
        addKnownRoot(local + QStringLiteral("/BraveSoftware/Brave-Browser/User Data"));
    else if (progID.contains(QStringLiteral("msedge")))
        addKnownRoot(local + QStringLiteral("/Microsoft/Edge/User Data"));
    else if (progID.contains(QStringLiteral("vivaldi")))
        addKnownRoot(local + QStringLiteral("/Vivaldi/User Data"));
    else if (progID.contains(QStringLiteral("opera")))
    {
        addKnownRoot(roaming + QStringLiteral("/Opera Software/Opera Stable"));
        addKnownRoot(roaming + QStringLiteral("/Opera Software/Opera GX Stable"));
    }
    else if (progID.contains(QStringLiteral("chrome")))
        addKnownRoot(local + QStringLiteral("/Google/Chrome/User Data"));

    if (roots.isEmpty() && !progID.contains(QStringLiteral("firefox")))
    {
        addKnownRoot(local + QStringLiteral("/BraveSoftware/Brave-Browser/User Data"));
        addKnownRoot(local + QStringLiteral("/Google/Chrome/User Data"));
        addKnownRoot(local + QStringLiteral("/Microsoft/Edge/User Data"));
        addKnownRoot(local + QStringLiteral("/Vivaldi/User Data"));
        addKnownRoot(roaming + QStringLiteral("/Opera Software/Opera Stable"));
        addKnownRoot(roaming + QStringLiteral("/Opera Software/Opera GX Stable"));
    }

    QStringList databases;
    for (const auto &root : roots)
        addChromiumProfiles(root, databases);
    databases.removeDuplicates();
    return databases;
}

void scanFirefox(TokenCandidate &best)
{
    const auto profilesRoot =
        qEnvironmentVariable("APPDATA") + QStringLiteral("/Mozilla/Firefox/Profiles");
    if (!QDir(profilesRoot).exists())
        return;
    QDirIterator iterator(
        profilesRoot,
        {QStringLiteral("data.sqlite"), QStringLiteral("data.sqlite-wal")},
        QDir::Files, QDirIterator::Subdirectories);
    while (iterator.hasNext())
    {
        const auto path = iterator.next();
        const auto normalized = QDir::fromNativeSeparators(path);
        if (!normalized.contains(
                QStringLiteral("/storage/default/https+++7tv.app")))
        {
            continue;
        }
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const auto bytes = file.readAll();
        qsizetype position = 0;
        while ((position = bytes.indexOf("eyJ", position)) >= 0)
        {
            const auto token = jwtFromBytes(bytes.mid(position, 8192));
            considerCandidate(best, token);
            position += 3;
        }
    }
}

ScanResult scanBrowserStorage(const QStringList &databases,
                              bool scanFirefoxStorage)
{
    TokenCandidate best;
    for (const auto &database : databases)
        considerCandidate(best, scanChromiumDatabase(database));
    if (scanFirefoxStorage)
        scanFirefox(best);
    return {best.token};
}

}  // namespace

class SeventvBrowserAuth::Impl
{
public:
    Impl()
    {
        this->pollTimer_.setInterval(750);
        this->timeoutTimer_.setSingleShot(true);
        QObject::connect(&this->pollTimer_, &QTimer::timeout,
                         [this] { this->beginScan(); });
        QObject::connect(&this->timeoutTimer_, &QTimer::timeout, [this] {
            this->fail(QStringLiteral(
                "7TV sign-in timed out. Click Connect account to try again."));
        });
        QObject::connect(&this->watcher_,
                         &QFutureWatcher<ScanResult>::finished,
                         [this] { this->scanFinished(); });
    }

    ~Impl()
    {
        this->cancel(false);
        this->watcher_.waitForFinished();
    }

    bool isRunning() const
    {
        return this->active_;
    }

    void start(SeventvBrowserAuth::SuccessCallback onSuccess,
               SeventvBrowserAuth::ErrorCallback onError)
    {
        if (this->active_)
        {
            this->cancel(false);
        }

        this->active_ = true;
        ++this->generation_;
        this->onSuccess_ = std::move(onSuccess);
        this->onError_ = std::move(onError);
        this->databases_ = chromiumDatabasesForDefaultBrowser();
        this->scanFirefox_ =
            defaultBrowserProgID().contains(QStringLiteral("firefox"));
        this->loginPageOpened_ = false;
        this->timeoutTimer_.start(AUTH_TIMEOUT_MS);
        this->beginScan();
    }

    void cancel(bool notify = true)
    {
        if (!this->active_)
            return;
        this->active_ = false;
        ++this->generation_;
        this->pollTimer_.stop();
        this->timeoutTimer_.stop();
        auto onError = std::exchange(this->onError_, {});
        this->onSuccess_ = {};
        if (notify && onError)
            onError(QStringLiteral("7TV sign-in was cancelled."));
    }

private:
    void beginScan()
    {
        if (!this->active_ || this->watcher_.isRunning())
            return;
        this->pollTimer_.stop();
        this->scanGeneration_ = this->generation_;
        const auto databases = this->databases_;
        const auto scanFirefoxStorage = this->scanFirefox_;
        this->watcher_.setFuture(QtConcurrent::run(
            [databases, scanFirefoxStorage] {
                return scanBrowserStorage(databases, scanFirefoxStorage);
            }));
    }

    void scanFinished()
    {
        if (!this->active_)
            return;
        if (this->scanGeneration_ != this->generation_)
        {
            QTimer::singleShot(0, &this->pollTimer_,
                               [this] { this->beginScan(); });
            return;
        }
        const auto result = this->watcher_.result();
        if (!result.token.isEmpty() && usableJwtIssuedAt(result.token) > 0)
        {
            this->succeed(result.token);
            return;
        }
        if (!this->loginPageOpened_)
        {
            if (!QDesktopServices::openUrl(SEVENTV_LOGIN_URL))
            {
                this->fail(QStringLiteral(
                    "Mergerino could not open the 7TV sign-in page in your default browser."));
                return;
            }
            this->loginPageOpened_ = true;
        }
        this->pollTimer_.start();
    }

    void succeed(const QString &token)
    {
        if (!this->active_)
            return;
        this->active_ = false;
        ++this->generation_;
        this->pollTimer_.stop();
        this->timeoutTimer_.stop();
        auto onSuccess = std::exchange(this->onSuccess_, {});
        this->onError_ = {};
        if (onSuccess)
            onSuccess(token);
    }

    void fail(const QString &error)
    {
        if (!this->active_)
            return;
        this->active_ = false;
        ++this->generation_;
        this->pollTimer_.stop();
        this->timeoutTimer_.stop();
        auto onError = std::exchange(this->onError_, {});
        this->onSuccess_ = {};
        if (onError)
            onError(error);
    }

    bool active_ = false;
    bool scanFirefox_ = false;
    bool loginPageOpened_ = false;
    quint64 generation_ = 0;
    quint64 scanGeneration_ = 0;
    QStringList databases_;
    QTimer pollTimer_;
    QTimer timeoutTimer_;
    QFutureWatcher<ScanResult> watcher_;
    SeventvBrowserAuth::SuccessCallback onSuccess_;
    SeventvBrowserAuth::ErrorCallback onError_;
};

SeventvBrowserAuth &SeventvBrowserAuth::instance()
{
    static SeventvBrowserAuth auth;
    return auth;
}

SeventvBrowserAuth::SeventvBrowserAuth()
    : impl_(std::make_unique<Impl>())
{
}

SeventvBrowserAuth::~SeventvBrowserAuth() = default;

bool SeventvBrowserAuth::isRunning() const
{
    return this->impl_->isRunning();
}

void SeventvBrowserAuth::start(SuccessCallback onSuccess,
                               ErrorCallback onError)
{
    this->impl_->start(std::move(onSuccess), std::move(onError));
}

void SeventvBrowserAuth::cancel()
{
    this->impl_->cancel();
}

}  // namespace chatterino
