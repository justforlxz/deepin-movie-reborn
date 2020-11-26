/*
 * (c) 2017, Deepin Technology Co., Ltd. <support@deepin.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * is provided AS IS, WITHOUT ANY WARRANTY; without even the implied
 * warranty of MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, and
 * NON-INFRINGEMENT.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link the code of portions of this program with the
 * OpenSSL library under certain conditions as described in each
 * individual source file, and distribute linked combinations
 * including the two.
 * You must obey the GNU General Public License in all respects
 * for all of the code used other than OpenSSL.  If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so.  If you
 * do not wish to do so, delete this exception statement from your
 * version.  If you delete this exception statement from all source
 * files in the program, then also delete it here.
 */
#include "playlist_model.h"
#include "player_engine.h"
#include "utils.h"
#ifndef _LIBDMR_
#include "dmr_settings.h"
#endif
#include "dvd_utils.h"

#include <random>
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/avutil.h>
}

typedef int (*mvideo_avformat_open_input)(AVFormatContext **ps, const char *url, AVInputFormat *fmt, AVDictionary **options);
typedef int (*mvideo_avformat_find_stream_info)(AVFormatContext *ic, AVDictionary **options);
typedef int (*mvideo_av_find_best_stream)(AVFormatContext *ic, enum AVMediaType type, int wanted_stream_nb, int related_stream, AVCodec **decoder_ret, int flags);
typedef AVCodec *(*mvideo_avcodec_find_decoder)(enum AVCodecID id);
typedef void (*mvideo_av_dump_format)(AVFormatContext *ic, int index, const char *url, int is_output);
typedef void (*mvideo_avformat_close_input)(AVFormatContext **s);
typedef AVDictionaryEntry *(*mvideo_av_dict_get)(const AVDictionary *m, const char *key, const AVDictionaryEntry *prev, int flags);


mvideo_avformat_open_input g_mvideo_avformat_open_input = nullptr;
mvideo_avformat_find_stream_info g_mvideo_avformat_find_stream_info = nullptr;
mvideo_av_find_best_stream g_mvideo_av_find_best_stream = nullptr;
mvideo_avcodec_find_decoder g_mvideo_avcodec_find_decoder = nullptr;
mvideo_av_dump_format g_mvideo_av_dump_format = nullptr;
mvideo_avformat_close_input g_mvideo_avformat_close_input = nullptr;
mvideo_av_dict_get g_mvideo_av_dict_get = nullptr;

static bool check_wayland()
{
    //此处在wayland下也是直接return false
    return false;
//    auto e = QProcessEnvironment::systemEnvironment();
//    QString XDG_SESSION_TYPE = e.value(QStringLiteral("XDG_SESSION_TYPE"));
//    QString WAYLAND_DISPLAY = e.value(QStringLiteral("WAYLAND_DISPLAY"));

//    if (XDG_SESSION_TYPE == QLatin1String("wayland") || WAYLAND_DISPLAY.contains(QLatin1String("wayland"), Qt::CaseInsensitive))
//        return true;
//    else {
//        return false;
//    }
}

namespace dmr {
QDebug operator<<(QDebug debug, const struct MovieInfo &mi)
{
    debug << "MovieInfo{"
          << mi.valid
          << mi.title
          << mi.fileType
          << mi.resolution
          << mi.filePath
          << mi.creation
          << mi.raw_rotate
          << mi.fileSize
          << mi.duration
          << mi.width
          << mi.height
          << mi.vCodecID
          << mi.vCodeRate
          << mi.fps
          << mi.proportion
          << mi.aCodeID
          << mi.aCodeRate
          << mi.aDigit
          << mi.channels
          << mi.sampling
          << "}";
    return debug;
}

QDataStream &operator<< (QDataStream &st, const MovieInfo &mi)
{
    st << mi.valid;
    st << mi.title;
    st << mi.fileType;
    st << mi.resolution;
    st << mi.filePath;
    st << mi.creation;
    st << mi.raw_rotate;
    st << mi.fileSize;
    st << mi.duration;
    st << mi.width;
    st << mi.height;
    st << mi.vCodecID;
    st << mi.vCodeRate;
    st << mi.fps;
    st << mi.proportion;
    st << mi.aCodeID;
    st << mi.aCodeRate;
    st << mi.aDigit;
    st << mi.channels;
    st << mi.sampling;
    return st;
}

QDataStream &operator>> (QDataStream &st, MovieInfo &mi)
{
    st >> mi.valid;
    st >> mi.title;
    st >> mi.fileType;
    st >> mi.resolution;
    st >> mi.filePath;
    st >> mi.creation;
    st >> mi.raw_rotate;
    st >> mi.fileSize;
    st >> mi.duration;
    st >> mi.width;
    st >> mi.height;
    st >> mi.vCodecID;
    st >> mi.vCodeRate;
    st >> mi.fps;
    st >> mi.proportion;
    st >> mi.aCodeID;
    st >> mi.aCodeRate;
    st >> mi.aDigit;
    st >> mi.channels;
    st >> mi.sampling;
    return st;
}

static class PersistentManager *_persistentManager = nullptr;

static QString hashUrl(const QUrl &url)
{
    return QString(QCryptographicHash::hash(url.toEncoded(), QCryptographicHash::Sha256).toHex());
}

//TODO: clean cache periodically
class PersistentManager: public QObject
{
    Q_OBJECT
public:
    static PersistentManager &get()
    {
        if (!_persistentManager) {
            _persistentManager = new PersistentManager;
        }
        return *_persistentManager;
    }

    struct CacheInfo {
        struct MovieInfo mi;
        QPixmap thumb;
        QPixmap thumb_dark;
        bool mi_valid {false};
        bool thumb_valid {false};
//        char m_padding [6];//占位符
    };

    CacheInfo loadFromCache(const QUrl &url)
    {
        auto h = hashUrl(url);
        CacheInfo ci;

        {
            auto filename = QString("%1/%2").arg(_cacheInfoPath).arg(h);
            QFile f(filename);
            if (!f.exists()) return ci;

            if (f.open(QIODevice::ReadOnly)) {
                QDataStream ds(&f);
                ds >> ci.mi;
                ci.mi_valid = ci.mi.valid;
            } else {
                qWarning() << f.errorString();
            }
            f.close();
        }

        if (ci.mi_valid) {
            auto filename = QString("%1/%2").arg(_pixmapCachePath).arg(h);
            QFile f(filename);
            if (!f.exists()) return ci;

            if (f.open(QIODevice::ReadOnly)) {
                QDataStream ds(&f);
                ds >> ci.thumb;
                ds >> ci.thumb_dark;
                ci.thumb.setDevicePixelRatio(qApp->devicePixelRatio());
                ci.thumb_dark.setDevicePixelRatio(qApp->devicePixelRatio());
                if (DGuiApplicationHelper::DarkType == DGuiApplicationHelper::instance()->themeType()) {
                    ci.thumb_valid = !ci.thumb_dark.isNull();
                } else {
                    ci.thumb_valid = !ci.thumb.isNull();
                }
            } else {
                qWarning() << f.errorString();
            }
            f.close();
        }

        return ci;
    }

    void save(const PlayItemInfo &pif)
    {
        auto h = hashUrl(pif.url);

        bool mi_saved = false;

        {
            auto filename = QString("%1/%2").arg(_cacheInfoPath).arg(h);
            QFile f(filename);
            if (f.open(QIODevice::WriteOnly)) {
                QDataStream ds(&f);
                ds << pif.mi;
                mi_saved = true;
                qDebug() << "cache" << pif.url << "->" << h;
            } else {
                qWarning() << f.errorString();
            }
            f.close();
        }

        if (mi_saved) {
            auto filename = QString("%1/%2").arg(_pixmapCachePath).arg(h);
            QFile f(filename);
            if (f.open(QIODevice::WriteOnly)) {
                QDataStream ds(&f);
                ds << pif.thumbnail;
                ds << pif.thumbnail_dark;
            } else {
                qWarning() << f.errorString();
            }
            f.close();
        }
    }

    /*bool cacheExists(const QUrl &url)
    {
        auto h = hashUrl(url);
        auto filename = QString("%1/%2").arg(_cacheInfoPath).arg(h);
        return QFile::exists(filename);
    }*/

private:
    PersistentManager()
    {
        auto tmpl = QString("%1/%2/%3/%4")
                    .arg(QStandardPaths::writableLocation(QStandardPaths::ConfigLocation))
                    .arg(qApp->organizationName())
                    .arg(qApp->applicationName());
        {
            _cacheInfoPath = tmpl.arg("cacheinfo");
            QDir d;
            d.mkpath(_cacheInfoPath);
        }
        {
            _pixmapCachePath = tmpl.arg("thumbs");
            QDir d;
            d.mkpath(_pixmapCachePath);
        }
    }

    QString _pixmapCachePath;
    QString _cacheInfoPath;

};

struct MovieInfo PlaylistModel::parseFromFile(const QFileInfo &fi, bool *ok)
{
    struct MovieInfo mi;
    mi.valid = false;
    AVFormatContext *av_ctx = nullptr;
    int stream_id = -1;
    AVCodecParameters *video_dec_ctx = nullptr;
    AVCodecParameters *audio_dec_ctx = nullptr;
//    AVStream *av_stream = nullptr;

    if (!fi.exists()) {
        if (ok) *ok = false;
        return mi;
    }

    auto ret = g_mvideo_avformat_open_input(&av_ctx, fi.filePath().toUtf8().constData(), nullptr, nullptr);
    if (ret < 0) {
        qWarning() << "avformat: could not open input";
        if (ok) *ok = false;
        return mi;
    }

    if (g_mvideo_avformat_find_stream_info(av_ctx, nullptr) < 0) {
        qWarning() << "av_find_stream_info failed";
        if (ok) *ok = false;
        return mi;
    }

    if (av_ctx->nb_streams == 0) {
        if (ok) *ok = false;
        return mi;
    }
//    if (open_codec_context(&stream_id, &dec_ctx, av_ctx, AVMEDIA_TYPE_VIDEO) < 0) {
//        if (open_codec_context(&stream_id, &dec_ctx, av_ctx, AVMEDIA_TYPE_AUDIO) < 0) {
//            if (ok) *ok = false;
//            return mi;
//        }
//    }

    int videoRet = -1;
    int audioRet = -1;
    AVStream *videoStream = nullptr;
    AVStream *audioStream = nullptr;
//    AVCodec *dec = nullptr;
    //AVDictionary *opts = nullptr;
    videoRet = g_mvideo_av_find_best_stream(av_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    audioRet = g_mvideo_av_find_best_stream(av_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (videoRet < 0 && audioRet < 0) {
//        qWarning() << "Could not find " << av_get_media_type_string(type)
//                   << " stream in input file";
        if (ok) *ok = false;
        return mi;
    }

    //AVCodecParameters *dec_ctx = nullptr;
    if (videoRet >= 0) {
        int video_stream_index = -1;
        video_stream_index = videoRet;
        videoStream = av_ctx->streams[video_stream_index];
        video_dec_ctx = videoStream->codecpar;

        mi.width = video_dec_ctx->width;
        mi.height = video_dec_ctx->height;
        mi.vCodecID = video_dec_ctx->codec_id;
        mi.vCodeRate = video_dec_ctx->bit_rate;

        if (videoStream->r_frame_rate.den != 0) {
            mi.fps = videoStream->r_frame_rate.num / videoStream->r_frame_rate.den;
        } else {
            mi.fps = 0;
        }
        if (mi.height != 0) {
            mi.proportion = static_cast<float>(mi.width) / static_cast<float>(mi.height);
        } else {
            mi.proportion = 0;
        }
    }
    if (audioRet >= 0) {
        int audio_stream_index = -1;
        audio_stream_index = audioRet;
        audioStream = av_ctx->streams[audio_stream_index];
        audio_dec_ctx = audioStream->codecpar;

        mi.aCodeID = audio_dec_ctx->codec_id;
        mi.aCodeRate = audio_dec_ctx->bit_rate;
        mi.aDigit = audio_dec_ctx->format;
        mi.channels = audio_dec_ctx->channels;
        mi.sampling = audio_dec_ctx->sample_rate;
    }
//    dec = g_mvideo_avcodec_find_decoder((video_dec_ctx)->codec_id);
//    stream_id = video_stream_index;

    g_mvideo_av_dump_format(av_ctx, 0, fi.fileName().toUtf8().constData(), 0);

//    for (int i = 0; i < av_ctx->nb_streams; i++) {
//        av_stream = av_ctx->streams[i];
//        if (av_stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
//            break;
//        }
//    }
    auto duration = av_ctx->duration == AV_NOPTS_VALUE ? 0 : av_ctx->duration;
    duration = duration + (duration <= INT64_MAX - 5000 ? 5000 : 0);
    mi.duration = duration / AV_TIME_BASE;
    mi.resolution = QString("%1x%2").arg(mi.width).arg(mi.height);
    mi.title = fi.fileName(); //FIXME this
    mi.filePath = fi.canonicalFilePath();
    mi.creation = fi.created().toString();
    mi.fileSize = fi.size();
    mi.fileType = fi.suffix();

//    if (open_codec_context(&stream_id, &dec_ctx, av_ctx, AVMEDIA_TYPE_AUDIO) < 0) {
//        if (open_codec_context(&stream_id, &dec_ctx, av_ctx, AVMEDIA_TYPE_VIDEO) < 0) {
//            if (ok) *ok = false;
//            return mi;
//        }
//    }

    AVDictionaryEntry *tag = nullptr;
    while ((tag = g_mvideo_av_dict_get(av_ctx->metadata, "", tag, AV_DICT_IGNORE_SUFFIX)) != nullptr) {
        if (tag->key && strcmp(tag->key, "creation_time") == 0) {
            auto dt = QDateTime::fromString(tag->value, Qt::ISODate);
            mi.creation = dt.toString();
            qDebug() << __func__ << dt.toString();
            break;
        }
        qDebug() << "tag:" << tag->key << tag->value;
    }

    AVStream *pTempStream = nullptr;
    if (videoRet >= 0) {
        pTempStream = av_ctx->streams[videoRet];
    } else if (audioRet >= 0) {
        pTempStream = av_ctx->streams[audioRet];
    }

    if(nullptr != pTempStream)
    {
        while ((tag = g_mvideo_av_dict_get(pTempStream->metadata, "", tag, AV_DICT_IGNORE_SUFFIX)) != nullptr) {
            if (tag->key && strcmp(tag->key, "rotate") == 0) {
                mi.raw_rotate = QString(tag->value).toInt();
                auto vr = (mi.raw_rotate + 360) % 360;
                if (vr == 90 || vr == 270) {
                    auto tmp = mi.height;
                    mi.height = mi.width;
                    mi.width = tmp;
                }
                break;
            }
            qDebug() << "tag:" << tag->key << tag->value;
        }
    }

    g_mvideo_avformat_close_input(&av_ctx);
    mi.valid = true;

    if (ok) *ok = true;
    return mi;
}

bool PlayItemInfo::refresh()
{
    if (url.isLocalFile()) {
        //FIXME: it seems that info.exists always gets refreshed
        auto o = this->info.exists();
        auto sz = this->info.size();

        this->info.refresh();
        this->valid = this->info.exists();

        return (o != this->info.exists()) || sz != this->info.size();
    }
    return false;
}

void PlaylistModel::slotStateChanged()
{
    PlayerEngine *e = dynamic_cast<PlayerEngine *>(sender());
    if(!e) return;
    qDebug() << "model" << "_userRequestingItem" << _userRequestingItem << "state" << e->state();
    switch (e->state()) {
    case PlayerEngine::Playing: {
        auto &pif = currentInfo();
        if (!pif.url.isLocalFile() && !pif.loaded) {
            pif.mi.width = e->videoSize().width();
            pif.mi.height = e->videoSize().height();
            pif.mi.duration = e->duration();
            pif.loaded = true;
            emit itemInfoUpdated(_current);
        }
        break;
    }
    case PlayerEngine::Paused:
        break;

    case PlayerEngine::Idle:
        if (!_userRequestingItem) {
            stop();
            playNext(false);
        }
        break;
    }
}

PlaylistModel::PlaylistModel(PlayerEngine *e)
    : _engine(e)
{
    m_pdataMutex = new QMutex();
    m_ploadThread = nullptr;
    m_brunning = false;
    //initThumb();
    //m_video_thumbnailer->thumbnail_size = 400 * qApp->devicePixelRatio();
    //av_register_all();

    _playlistFile = QString("%1/%2/%3/playlist")
                    .arg(QStandardPaths::writableLocation(QStandardPaths::ConfigLocation))
                    .arg(qApp->organizationName())
                    .arg(qApp->applicationName());

	connect(e, &PlayerEngine::stateChanged, this, &PlaylistModel::slotStateChanged);


//    _jobWatcher = new QFutureWatcher<PlayItemInfo>();
//    connect(_jobWatcher, &QFutureWatcher<PlayItemInfo>::finished,
//            this, &PlaylistModel::onAsyncAppendFinished);

    stop();
    //loadPlaylist();
#ifdef _LIBDMR_
    initThumb();
    initFFmpeg();
#endif
#ifndef _LIBDMR_
    if (Settings::get().isSet(Settings::ResumeFromLast)) {
        int restore_pos = Settings::get().internalOption("playlist_pos").toInt();
        _last = restore_pos;
    }
#endif
}

QString PlaylistModel::libPath(const QString &strlib)
{
    QDir  dir;
    QString path  = QLibraryInfo::location(QLibraryInfo::LibrariesPath);
    dir.setPath(path);
    QStringList list = dir.entryList(QStringList() << (strlib + "*"), QDir::NoDotAndDotDot | QDir::Files); //filter name with strlib
    if (list.contains(strlib)) {
        return strlib;
    } else {
        list.sort();
    }

    Q_ASSERT(list.size() > 0);
    return list.last();
}

void PlaylistModel::initThumb()
{
    QLibrary library(libPath("libffmpegthumbnailer.so"));
    m_mvideo_thumbnailer = (mvideo_thumbnailer) library.resolve( "video_thumbnailer_create");
    m_mvideo_thumbnailer_destroy = (mvideo_thumbnailer_destroy) library.resolve( "video_thumbnailer_destroy");
    m_mvideo_thumbnailer_create_image_data = (mvideo_thumbnailer_create_image_data) library.resolve( "video_thumbnailer_create_image_data");
    m_mvideo_thumbnailer_destroy_image_data = (mvideo_thumbnailer_destroy_image_data) library.resolve( "video_thumbnailer_destroy_image_data");
    m_mvideo_thumbnailer_generate_thumbnail_to_buffer = (mvideo_thumbnailer_generate_thumbnail_to_buffer) library.resolve( "video_thumbnailer_generate_thumbnail_to_buffer");
    if (m_mvideo_thumbnailer == nullptr || m_mvideo_thumbnailer_destroy == nullptr
            || m_mvideo_thumbnailer_create_image_data == nullptr || m_mvideo_thumbnailer_destroy_image_data == nullptr
            || m_mvideo_thumbnailer_generate_thumbnail_to_buffer == nullptr )

    {
        return;
    }
    m_video_thumbnailer = m_mvideo_thumbnailer();
    m_image_data = m_mvideo_thumbnailer_create_image_data();
    m_video_thumbnailer->thumbnail_size = 400 * qApp->devicePixelRatio();
}

void PlaylistModel::initFFmpeg()
{
    QLibrary avcodecLibrary(libPath("libavcodec.so"));
    QLibrary avformatLibrary(libPath("libavformat.so"));
    QLibrary avutilLibrary(libPath("libavutil.so"));

    g_mvideo_avformat_open_input = (mvideo_avformat_open_input) avformatLibrary.resolve("avformat_open_input");
    g_mvideo_avformat_find_stream_info = (mvideo_avformat_find_stream_info) avformatLibrary.resolve("avformat_find_stream_info");
    g_mvideo_av_find_best_stream = (mvideo_av_find_best_stream) avformatLibrary.resolve("av_find_best_stream");
    g_mvideo_av_dump_format = (mvideo_av_dump_format) avformatLibrary.resolve("av_dump_format");
    g_mvideo_avformat_close_input = (mvideo_avformat_close_input) avformatLibrary.resolve("avformat_close_input");

    g_mvideo_av_dict_get = (mvideo_av_dict_get) avutilLibrary.resolve("av_dict_get");

    g_mvideo_avcodec_find_decoder = (mvideo_avcodec_find_decoder) avcodecLibrary.resolve("avcodec_find_decoder");
    m_initFFmpeg = true;
}

PlaylistModel::~PlaylistModel()
{
    qDebug() << __func__;
    //delete _jobWatcher;

    delete m_pdataMutex;

#ifndef _LIBDMR_
    if (Settings::get().isSet(Settings::ClearWhenQuit)) {
        clearPlaylist();
    } else {
        //persistently save current playlist
        savePlaylist();
    }
#endif
    if (utils::check_wayland_env() && m_getThumanbil) {
        if (m_getThumanbil->isRunning()) {
            m_getThumanbil->stop();
        }
        m_getThumanbil->wait();
        delete m_getThumanbil;
        m_getThumanbil = nullptr;
    }
}

qint64 PlaylistModel::getUrlFileTotalSize(QUrl url, int tryTimes) const
{
    qint64 size = -1;

    if (tryTimes <= 0) {
        tryTimes = 1;
    }

    do {
        QNetworkAccessManager manager;
        // 事件循环，等待请求文件头信息结束;
        QEventLoop loop;
        // 超时，结束事件循环;
        QTimer timer;

        //发出请求，获取文件地址的头部信息;
        QNetworkReply *reply = manager.head(QNetworkRequest(QUrl(url)));//QNetworkRequest(url)
        if (!reply)
            continue;

        QObject::connect(reply, SIGNAL(finished()), &loop, SLOT(quit()));
        QObject::connect(&timer, SIGNAL(timeout()), &loop, SLOT(quit()));

        timer.start(5000);
        loop.exec();

        if (reply->error() != QNetworkReply::NoError) {
            qDebug() << reply->errorString();
            continue;
        }
        QVariant var = reply->header(QNetworkRequest::ContentLengthHeader);
        size = var.toLongLong();
        reply->deleteLater();
//        qDebug() << reply->hasRawHeader("Content-Encoding ");
//        qDebug() << reply->hasRawHeader("Content-Language");
//        qDebug() << reply->hasRawHeader("Content-Length");
//        qDebug() << reply->hasRawHeader("Content-Type");
//        qDebug() << reply->hasRawHeader("Last-Modified");
//        qDebug() << reply->hasRawHeader("Expires");

        break;


    } while (tryTimes--);



    return size;
}

void PlaylistModel::clearPlaylist()
{
    QSettings cfg(_playlistFile, QSettings::NativeFormat);
    cfg.beginGroup("playlist");
    cfg.remove("");
    cfg.endGroup();
}

void PlaylistModel::savePlaylist()
{
    QSettings cfg(_playlistFile, QSettings::NativeFormat);
    cfg.beginGroup("playlist");
    cfg.remove("");

    for (int i = 0; i < count(); ++i) {
        const auto &pif = _infos[i];
        cfg.setValue(QString::number(i), pif.url);
        qDebug() << "save " << pif.url;
    }
    cfg.endGroup();
    cfg.sync();
}

void PlaylistModel::loadPlaylist()
{
    initThumb();
    initFFmpeg();
    QList<QUrl> urls;

    QSettings cfg(_playlistFile, QSettings::NativeFormat);
    cfg.beginGroup("playlist");
    auto keys = cfg.childKeys();
    for (int i = 0; i < keys.size(); ++i) {
        auto url = cfg.value(QString::number(i)).toUrl();
        if (indexOf(url) >= 0) continue;

        if (url.isLocalFile()) {
            urls.append(url);

        } else {
            auto pif = calculatePlayInfo(url, QFileInfo());
            _infos.append(pif);
        }
    }
    cfg.endGroup();

    if (urls.size() == 0) {
        _firstLoad = false;
        reshuffle();
        emit countChanged();
        return;
    }

    //QTimer::singleShot(0, [ = ]() {
    delayedAppendAsync(urls);
    //});
}


/*PlaylistModel::PlayMode PlaylistModel::playMode() const
{
    return _playMode;
}*/

void PlaylistModel::setPlayMode(PlaylistModel::PlayMode pm)
{
    if (_playMode != pm) {
        _playMode = pm;
        reshuffle();
        emit playModeChanged(pm);
    }
}

void PlaylistModel::reshuffle()
{
    if (_playMode != PlayMode::ShufflePlay || _infos.size() == 0) {
        return;
    }

    _shufflePlayed = 0;
    _playOrder.clear();
    for (int i = 0, sz = _infos.size(); i < sz; ++i) {
        _playOrder.append(i);
    }

    std::random_device rd;
    std::mt19937 g(rd());

    std::shuffle(_playOrder.begin(), _playOrder.end(), g);
    qDebug() << _playOrder;
}

void PlaylistModel::clear()
{
    _infos.clear();
    _engine->stop();
    _engine->waitLastEnd();

    _current = -1;
    _last = -1;
    emit emptied();
    emit currentChanged();
    emit countChanged();
}

void PlaylistModel::remove(int pos)
{
    if (pos < 0 || pos >= count()) return;

    _userRequestingItem = true;

    m_loadFile.removeOne(_infos[pos].url);
    _infos.removeAt(pos);
    reshuffle();

    _last = _current;
    if (_engine->state() != PlayerEngine::Idle) {
        if (_current == pos) {
            _last = _current;
            _current = -1;
            _engine->waitLastEnd();

        } else if (pos < _current) {
            _current--;
            _last = _current;
        }
    } else {
        if (_current == pos) {
            _last = _current;
            _current = -1;
            _engine->waitLastEnd();
        }
    }

    if (_last >= count())
        _last = -1;

    emit itemRemoved(pos);
    if (_last != _current)
        emit currentChanged();
    emit countChanged();


    qDebug() << _last << _current;
    _userRequestingItem = false;
    savePlaylist();
}

void PlaylistModel::stop()
{
    _current = -1;
    emit currentChanged();
}

void PlaylistModel::tryPlayCurrent(bool next)
{
    qDebug() << __func__;
    auto &pif = _infos[_current];
    if (pif.refresh()) {
        qDebug() << pif.url.fileName() << "changed";
    }
    emit itemInfoUpdated(_current);
    if (pif.valid) {
        if(!utils::check_wayland_env()) {
            _engine->requestPlay(_current);
            emit currentChanged();
        } else {
        //本地视频单个循环/列表循环，小于1s视频/无法解码视频，不播放，直接播放下一个
          if ( (pif.mi.duration <= 1 || pif.thumbnail.isNull()) && pif.url.isLocalFile()) {
              if (1 == count() || _playMode == PlayMode::SingleLoop || _playMode == PlayMode::SinglePlay) {
                  qWarning() << "return for video is cannot play and loop play!";
                  return;
              }
              if (_current < count() - 1) {
                  _current++;
                  _last = _current;
              } else {
                  _current = 0;
              }
          }
          _hasNormalVideo = false;
          for (auto info : _infos) {
              if ((info.valid && info.mi.duration > 1 && !info.thumbnail.isNull()) || !pif.url.isLocalFile()) {
                  _hasNormalVideo = true;
                  break;
              }
          }
          if (_hasNormalVideo) {
              _engine->requestPlay(_current);
              emit currentChanged();
          }
        }
    } else {
        _current = -1;
        bool canPlay = false;
        //循环播放时，无效文件播放闪退
        if (_playMode == PlayMode::SingleLoop) {
            if ((_last < count() - 1) && next) {
                _last++;
            } else if ((_last > 0) && !next) {
                _last--;
            } else if (next) {
                _last = 0;
            } else if (!next) {
                _last = count() - 1;
            }
        }
        for (auto info : _infos) {
            if (info.valid) {
                canPlay = true;
                break;
            }
        }
        if (canPlay) {
            emit currentChanged();
            if (next) playNext(false);
            else playPrev(false);
        }
    }
}

void PlaylistModel::clearLoad()
{
    m_loadFile.clear();
}

void PlaylistModel::playNext(bool fromUser)
{
    if (count() == 0) return;
    qDebug() << "playmode" << _playMode << "fromUser" << fromUser
             << "last" << _last << "current" << _current;

    _userRequestingItem = fromUser;

    switch (_playMode) {
    case SinglePlay:
        if (fromUser) {
            if (_last + 1 >= count()) {
                _last = -1;
            }
            _engine->waitLastEnd();
            _current = _last + 1;
            _last = _current;
            tryPlayCurrent(true);
        }
        break;

    case SingleLoop:
        if (fromUser) {
            if (_engine->state() == PlayerEngine::Idle) {
                _last = _last == -1 ? 0 : _last;
                _current = _last;
                tryPlayCurrent(true);

            } else {
                if (_last + 1 >= count()) {
                    _last = -1;
                }
                _engine->waitLastEnd();
                _current = _last + 1;
                _last = _current;
                tryPlayCurrent(true);
            }
        } else {
            if (_engine->state() == PlayerEngine::Idle) {
                _last = _last < 0 ? 0 : _last;
                _current = _last;
                tryPlayCurrent(true);
            } else {
                // replay current
                tryPlayCurrent(true);
            }
        }
        break;

    case ShufflePlay: {
        if (_shufflePlayed >= _playOrder.size()) {
            _shufflePlayed = 0;
            reshuffle();
        }
        _shufflePlayed++;
        qDebug() << "shuffle next " << _shufflePlayed - 1;
        _engine->waitLastEnd();
        _last = _current = _playOrder[_shufflePlayed - 1];
        tryPlayCurrent(true);
        break;
    }

    case OrderPlay:
        _last++;
        if (_last == count()) {
            if (fromUser)
                _last = 0;
            else {
                _last--;
                break;
            }
        }

        _engine->waitLastEnd();
        _current = _last;
        tryPlayCurrent(true);
        break;

    case ListLoop:
        _last++;
        if (_last == count()) {
            _loopCount++;
            _last = 0;
        }

        _engine->waitLastEnd();
        _current = _last;
        tryPlayCurrent(true);
        break;
    }

    _userRequestingItem = false;
}

void PlaylistModel::playPrev(bool fromUser)
{
    if (count() == 0) return;
    qDebug() << "playmode" << _playMode << "fromUser" << fromUser
             << "last" << _last << "current" << _current;

    _userRequestingItem = fromUser;

    switch (_playMode) {
    case SinglePlay:
        if (fromUser) {
            if (_last - 1 < 0) {
                _last = count();
            }
            _engine->waitLastEnd();
            _current = _last - 1;
            _last = _current;
            tryPlayCurrent(false);
        }
        break;

    case SingleLoop:
        if (fromUser) {
            if (_engine->state() == PlayerEngine::Idle) {
                _last = _last == -1 ? 0 : _last;
                _current = _last;
                tryPlayCurrent(false);
            } else {
                if (_last - 1 < 0) {
                    _last = count();
                }
                _engine->waitLastEnd();
                _current = _last - 1;
                _last = _current;
                tryPlayCurrent(false);
            }
        } else {
            if (_engine->state() == PlayerEngine::Idle) {
                _last = _last < 0 ? 0 : _last;
                _current = _last;
                tryPlayCurrent(false);
            } else {
                // replay current
                tryPlayCurrent(false);
            }
        }
        break;

    case ShufflePlay: { // this must comes from user
        if (_shufflePlayed <= 1) {
            reshuffle();
            _shufflePlayed = _playOrder.size();
        }
        _shufflePlayed--;
        qDebug() << "shuffle prev " << _shufflePlayed - 1;
        _engine->waitLastEnd();
        _last = _current = _playOrder[_shufflePlayed - 1];
        tryPlayCurrent(false);
        break;
    }

    case OrderPlay:
        _last--;
        if (_last < 0) {
            _last = count() - 1;
        }

        _engine->waitLastEnd();
        _current = _last;
        tryPlayCurrent(false);
        break;

    case ListLoop:
        _last--;
        if (_last < 0) {
            _loopCount++;
            _last = count() - 1;
        }

        _engine->waitLastEnd();
        _current = _last;
        tryPlayCurrent(false);
        break;
    }

    _userRequestingItem = false;

}

static QDebug operator<<(QDebug s, const QFileInfoList &v)
{
    std::for_each(v.begin(), v.end(), [&](const QFileInfo & fi) {
        s << fi.fileName();
    });
    return s;
}

void PlaylistModel::appendSingle(const QUrl &url)
{
    qDebug() << __func__;
    if (indexOf(url) >= 0) return;

    if (url.isLocalFile()) {
        QFileInfo fi(url.toLocalFile());
        if (!fi.exists()) return;
        auto pif = calculatePlayInfo(url, fi);
        if (!pif.valid) return;
        _infos.append(pif);

#ifndef _LIBDMR_
        if (Settings::get().isSet(Settings::AutoSearchSimilar)) {
            auto fil = utils::FindSimilarFiles(fi);
            qDebug() << "auto search similar files" << fil;
            std::for_each(fil.begin(), fil.end(), [ = ](const QFileInfo & fi) {
                auto url = QUrl::fromLocalFile(fi.absoluteFilePath());
                if (indexOf(url) < 0 && _engine->isPlayableFile(fi.fileName())) {
                    auto playitem_info = calculatePlayInfo(url, fi);
                    if (playitem_info.valid)
                        _infos.append(playitem_info);
                }
            });
        }
#endif
    } else {
        auto pif = calculatePlayInfo(url, QFileInfo(), true);
        _infos.append(pif);
    }
}

void PlaylistModel::collectionJob(const QList<QUrl> &urls, QList<QUrl> &inputUrls)
{
    for (const auto &url : urls) {
        int aa = indexOf(url);
        if (m_loadFile.contains(url))
            continue;
        if (!url.isValid() || indexOf(url) >= 0 || !url.isLocalFile() || _urlsInJob.contains(url.toLocalFile()))
            continue;

        m_loadFile.append(url);
        qDebug() << __func__ << _infos.size() << "index is" << aa << url;
        QFileInfo fi(url.toLocalFile());
        if (!_firstLoad && (!fi.exists() || !fi.isFile())) continue;

        _pendingJob.append(qMakePair(url, fi));
        _urlsInJob.insert(url.toLocalFile());
        inputUrls.append(url);
        qDebug() << "append " << url.fileName();

#ifndef _LIBDMR_
        if (!_firstLoad && Settings::get().isSet(Settings::AutoSearchSimilar)) {
            auto fil = utils::FindSimilarFiles(fi);
            qDebug() << "auto search similar files" << fil;
            for (const QFileInfo &fileinfo : fil) {
                if (fileinfo.isFile()) {
                    auto file_url = QUrl::fromLocalFile(fileinfo.absoluteFilePath());

                    if (!_urlsInJob.contains(file_url.toLocalFile()) && indexOf(file_url) < 0 &&
                            _engine->isPlayableFile(fileinfo.fileName())) {
                        _pendingJob.append(qMakePair(file_url, fileinfo));
                        _urlsInJob.insert(file_url.toLocalFile());
                        inputUrls.append(file_url);
                        //handleAsyncAppendResults(QList<PlayItemInfo>()<<calculatePlayInfo(url,fi));
                    }
                }
            }
        }
#endif
    }

    qDebug() << "input size" << urls.size() << "output size" << _urlsInJob.size()
             << "_pendingJob: " << _pendingJob.size();
}

void PlaylistModel::appendAsync(const QList<QUrl> &urls)
{
    if (!m_initFFmpeg) {
        initThumb();
        initFFmpeg();
    }
    if (check_wayland()) {
        if (m_ploadThread == nullptr) {
            m_ploadThread = new LoadThread(this, urls);
            connect(m_ploadThread, &QThread::finished, this, &PlaylistModel::deleteThread);
        }
        if (!m_ploadThread->isRunning()) {
            m_ploadThread->start();
            m_brunning = m_ploadThread->isRunning();
        }
    } else {
        //QTimer::singleShot(10, [ = ]() {
        delayedAppendAsync(urls);
        //});
    }
}

void PlaylistModel::deleteThread()
{
    if (check_wayland()) {
        if (m_ploadThread == nullptr)
            return ;
        if (m_ploadThread->isRunning()) {
            m_ploadThread->wait();
        }
        delete m_ploadThread;
        m_ploadThread = nullptr;
        m_brunning = false;
    }
}

void PlaylistModel::delayedAppendAsync(const QList<QUrl> &urls)
{
    if (_pendingJob.size() > 0) {
        //TODO: may be automatically schedule later
        qWarning() << "there is a pending append going on, enqueue";
        m_pdataMutex->lock();
        _pendingAppendReq.enqueue(urls);
        m_pdataMutex->unlock();
        return;
    }

    QList<QUrl> t_urls;
    m_pdataMutex->lock();
    collectionJob(urls, t_urls);
    m_pdataMutex->unlock();

    if (!_pendingJob.size()) return;

    struct MapFunctor {
        PlaylistModel *_model = nullptr;
        using result_type = PlayItemInfo;
        explicit MapFunctor(PlaylistModel *model): _model(model) {}

        struct PlayItemInfo operator()(const AppendJob &a)
        {
            qDebug() << "mapping " << a.first.fileName();
            return _model->calculatePlayInfo(a.first, a.second);
        }
    };

    if (check_wayland()) {
        m_pdataMutex->lock();
        PlayItemInfoList pil;
        for (const auto &a : _pendingJob) {
            qDebug() << "sync mapping " << a.first.fileName();
            pil.append(calculatePlayInfo(a.first, a.second));
            if (m_ploadThread && m_ploadThread->isRunning()) {
                m_ploadThread->msleep(10);
            }
        }
        _pendingJob.clear();
        _urlsInJob.clear();

        m_pdataMutex->unlock();

        handleAsyncAppendResults(pil);
    } else {
        qDebug() << "not wayland";
        if (QThread::idealThreadCount() > 1) {
//            auto future = QtConcurrent::mapped(_pendingJob, MapFunctor(this));
//            _jobWatcher->setFuture(future);
            if (!m_getThumanbil) {
                m_getThumanbil = new GetThumanbil(this, t_urls);
                connect(m_getThumanbil, &GetThumanbil::finished, this, &PlaylistModel::onAsyncFinished);
                connect(m_getThumanbil, &GetThumanbil::updateItem, this, &PlaylistModel::onAsyncUpdate, Qt::BlockingQueuedConnection);
                m_isLoadRunning = true;
                m_getThumanbil->start();
            } else {
                if (m_isLoadRunning) {
                    m_tempList.append(t_urls);
                } else {
                    m_getThumanbil->setUrls(t_urls);
                    m_getThumanbil->start();
                }
            }
            _pendingJob.clear();
            _urlsInJob.clear();
        } else {
            PlayItemInfoList pil;
            for (const auto &a : _pendingJob) {
                qDebug() << "sync mapping " << a.first.fileName();
                pil.append(calculatePlayInfo(a.first, a.second));
                if (m_ploadThread && m_ploadThread->isRunning()) {
                    m_ploadThread->msleep(10);
                }
            }
            _pendingJob.clear();
            _urlsInJob.clear();
            handleAsyncAppendResults(pil);
        }
    }

}

static QList<PlayItemInfo> &SortSimilarFiles(QList<PlayItemInfo> &fil)
{
    //sort names by digits inside, take care of such a possible:
    //S01N04, S02N05, S01N12, S02N04, etc...
    struct {
        bool operator()(const PlayItemInfo &fi1, const PlayItemInfo &fi2) const
        {
            if (!fi1.valid)
                return true;
            if (!fi2.valid)
                return false;

            QString fileName1 = fi1.url.fileName();
            QString fileName2 = fi2.url.fileName();

            if (utils::IsNamesSimilar(fileName1, fileName2)) {
                return utils::CompareNames(fileName1, fileName2);
            }
            return fileName1.localeAwareCompare(fileName2) < 0;
        }
    } SortByDigits;
    std::sort(fil.begin(), fil.end(), SortByDigits);

    return fil;
}

/*not used yet*/
/*void PlaylistModel::onAsyncAppendFinished()
{
    qDebug() << __func__;
//    auto f = _jobWatcher->future();
    _pendingJob.clear();
    _urlsInJob.clear();

    //auto fil = f.results();
    //handleAsyncAppendResults(fil);
}*/

void PlaylistModel::onAsyncFinished()
{
//    QList<PlayItemInfo> fil = m_getThumanbil->getInfoList();
//    qDebug() << __func__ << "size" << fil.size() << "info size" << _infos.size();
//    for (int i = 0; i < fil.size();i++) {
//        if (indexOf(fil[i].url) >= 0) {
//            fil.removeAt(i);
//        }
//    }
    m_isLoadRunning = false;
    //qDebug() << fil.size();
    m_getThumanbil->clearItem();
    //handleAsyncAppendResults(fil);
    if (!m_tempList.isEmpty()) {
        m_getThumanbil->setUrls(m_tempList);
        m_tempList.clear();
        m_isLoadRunning = true;
        m_getThumanbil->start();
    }
}

void PlaylistModel::onAsyncUpdate(PlayItemInfo fil)
{
    QList<PlayItemInfo> fils;
    fils.append(fil);
    if (!_firstLoad) {
        //since _infos are modified only at the same thread, the lock is not necessary
        auto last = std::remove_if(fils.begin(), fils.end(), [](const PlayItemInfo & pif) {
            return !pif.mi.valid;
        });
        fils.erase(last, fils.end());
    }

    if (!_firstLoad)
        _infos += SortSimilarFiles(fils);
    else
        _infos += fil;
    reshuffle();
    _firstLoad = false;
    emit itemsAppended();
    emit countChanged();
    _firstLoad = false;
    emit asyncAppendFinished(fils);

    if (_pendingAppendReq.size()) {
        auto job = _pendingAppendReq.dequeue();
        delayedAppendAsync(job);
    }
    savePlaylist();
}

void PlaylistModel::handleAsyncAppendResults(QList<PlayItemInfo> &fil)
{
    qDebug() << __func__ << fil.size();
    if (!fil.size())
        return;
    if (!_firstLoad) {
        //since _infos are modified only at the same thread, the lock is not necessary
        auto last = std::remove_if(fil.begin(), fil.end(), [](const PlayItemInfo & pif) {
            return !pif.mi.valid;
        });
        fil.erase(last, fil.end());
    }

    qDebug() << "collected items" << fil.count();
    if (fil.size()) {
        if (!_firstLoad)
            _infos += SortSimilarFiles(fil);
        else
            _infos += fil;
        reshuffle();
        _firstLoad = false;
        emit itemsAppended();
        emit countChanged();
    }
    _firstLoad = false;
    emit asyncAppendFinished(fil);

    QTimer::singleShot(0, [&]() {
        if (_pendingAppendReq.size()) {
            auto job = _pendingAppendReq.dequeue();
            delayedAppendAsync(job);
        }
    });
    savePlaylist();
}

/*bool PlaylistModel::hasPendingAppends()
{
    return _pendingAppendReq.size() > 0 || _pendingJob.size() > 0;
}*/

//TODO: what if loadfile failed
void PlaylistModel::append(const QUrl &url)
{
    if (!url.isValid()) return;

    appendSingle(url);
    reshuffle();
    emit itemsAppended();
    emit countChanged();
}

void PlaylistModel::changeCurrent(int pos)
{
    qDebug() << __func__ << pos;
    if (pos < 0 || pos >= count()) return;
    auto mi = items().at(pos).mi;
    if (mi.fileType == "webm") {
        auto pif = calculatePlayInfo(items().at(pos).url, items().at(pos).info);
        items().removeAt(pos);
        items().insert(pos, pif);
        emit updateDuration();
    } else {
        if (_current == pos) {
            return;
        }
    }

    _userRequestingItem = true;

    _engine->waitLastEnd();
    _current = pos;
    _last = _current;
    tryPlayCurrent(true);
    _userRequestingItem = false;
    emit currentChanged();
}

void PlaylistModel::switchPosition(int src, int target)
{
    //Q_ASSERT_X(0, "playlist", "not implemented");
    Q_ASSERT (src < _infos.size() && target < _infos.size());
    _infos.move(src, target);

    int min = qMin(src, target);
    int max = qMax(src, target);
    if (_current >= min && _current <= max) {
        if (_current == src) {
            _current = target;
            _last = _current;
        } else {
            if (src < target) {
                _current--;
                _last = _current;
            } else if (src > target) {
                _current++;
                _last = _current;
            }
        }
        emit currentChanged();
    }
}

PlayItemInfo &PlaylistModel::currentInfo()
{
    //Q_ASSERT (_infos.size() > 0 && _current >= 0);
    Q_ASSERT (_infos.size() > 0);

    if (_current >= 0)
        return _infos[_current];
    if (_last >= 0)
        return _infos[_last];
    return _infos[0];
}

const PlayItemInfo &PlaylistModel::currentInfo() const
{
    Q_ASSERT (_infos.size() > 0 && _current >= 0);
    return _infos[_current];
}

int PlaylistModel::count() const
{
    return _infos.count();
}

int PlaylistModel::current() const
{
    return _current;
}

bool PlaylistModel::getthreadstate()
{
    if (m_ploadThread) {
        m_brunning = m_ploadThread->isRunning();
    } else {
        m_brunning = false;
    }
    return m_brunning;
}

//获取音乐缩略图
bool PlaylistModel::getMusicPix(const QFileInfo &fi, QPixmap &rImg)
{

    AVFormatContext *av_ctx = nullptr;
    //AVCodecContext *dec_ctx = nullptr;

    if (!fi.exists()) {
        return false;
    }

#ifdef __x86_64__
    QString path = "/usr/lib/x86_64-linux-gnu/";
#elif __mips__
    QString path = "/usr/lib/mips64el-linux-gnuabi64/";
#elif __aarch64__
    QString path = "/usr/lib/aarch64-linux-gnu/";
#elif __sw_64__
    QString path = "/usr/lib/sw_64-linux-gnu/";
#else
    QString path = "/usr/lib/i386-linux-gnu/";
#endif
    QLibrary library(libPath("libavformat.so"));
    mvideo_avformat_open_input g_mvideo_avformat_open_input = (mvideo_avformat_open_input) library.resolve("avformat_open_input");
    mvideo_avformat_find_stream_info g_mvideo_avformat_find_stream_info = (mvideo_avformat_find_stream_info) library.resolve("avformat_find_stream_info");

    auto ret = g_mvideo_avformat_open_input(&av_ctx, fi.filePath().toUtf8().constData(), nullptr, nullptr);
    if (ret < 0) {
        qWarning() << "avformat: could not open input";
        return false;
    }

    if (g_mvideo_avformat_find_stream_info(av_ctx, nullptr) < 0) {
        qWarning() << "av_find_stream_info failed";
        return false;
    }

    // read the format headers  comment by thx , 这里会导致一些音乐 奔溃
    //if (av_ctx->iformat->read_header(av_ctx) < 0) {
    //    printf("No header format");
    //    return false;
    //}

    for (unsigned int i = 0; i < av_ctx->nb_streams; i++) {
        if (av_ctx->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC) {
            AVPacket pkt = av_ctx->streams[i]->attached_pic;
            //使用QImage读取完整图片数据（注意，图片数据是为解析的文件数据，需要用QImage::fromdata来解析读取）
            //rImg = QImage::fromData((uchar *)pkt.data, pkt.size);
            return rImg.loadFromData(static_cast<uchar *>(pkt.data), static_cast<uint>(pkt.size));
        }
    }
    return false;
}

struct PlayItemInfo PlaylistModel::calculatePlayInfo(const QUrl &url, const QFileInfo &fi, bool isDvd)
{
    bool ok = false;
    struct MovieInfo mi;
    auto ci = PersistentManager::get().loadFromCache(url);
//    if (ci.mi_valid && url.isLocalFile()) {
//        mi = ci.mi;
//        ok = true;
//        qDebug() << "load cached MovieInfo" << mi;
//    } else {

        mi = parseFromFile(fi, &ok);
        if (isDvd && url.scheme().startsWith("dvd")) {
            QString dev = url.path();
            if (dev.isEmpty()) dev = "/dev/sr0";
#ifdef heyi
            dmr::dvd::RetrieveDvdThread::get()->startDvd(dev);
#endif
//            mi.title = dmr::dvd::RetrieveDVDTitle(dev);
//            if (mi.title.isEmpty()) {
//              mi.title = "DVD";
//            }
//            mi.valid = true;
        } else if (!url.isLocalFile()) {
            QString msg = url.fileName();
            if (msg != "sr0" || msg != "cdrom") {
                if (msg.isEmpty()) msg = url.path();
                mi.title = msg;
                mi.valid = true;
            }
        } else {
            mi.title = fi.fileName();
        }
    //}

    QPixmap pm;
    QPixmap dark_pm;
    if (ci.thumb_valid) {
        pm = ci.thumb;
        dark_pm = ci.thumb_dark;

        qDebug() << "load cached thumb" << url;
    } else if (ok) {
        try {
            //如果打开的是音乐就读取音乐缩略图
            bool isMusic = false;
            foreach (QString sf, _engine->audio_filetypes) {
                if (sf.right(sf.size() - 2) == mi.fileType) {
                    isMusic = true;
                }
            }

            if(_engine->state() != dmr::PlayerEngine::Idle && _engine->videoSize().width()<0)   //如果没有视频流，就当做音乐播放
            {
                isMusic = true;
            }

            if (isMusic == false) {
                m_mvideo_thumbnailer_generate_thumbnail_to_buffer(m_video_thumbnailer, fi.canonicalFilePath().toUtf8().data(),  m_image_data);
                auto img = QImage::fromData(m_image_data->image_data_ptr, static_cast<int>(m_image_data->image_data_size), "png");
                pm = QPixmap::fromImage(img);
                dark_pm = pm;
            } else {
                if (getMusicPix(fi, pm) == false) {
                    pm.load(":/resources/icons/music-light.svg");
                }
                if (getMusicPix(fi, dark_pm) == false) {
                    dark_pm.load(":/resources/icons/music-dark.svg");
                }
            }
            pm.setDevicePixelRatio(qApp->devicePixelRatio());
            dark_pm.setDevicePixelRatio(qApp->devicePixelRatio());
        } catch (const std::logic_error &) {
        }
    }

    PlayItemInfo pif { fi.exists() || !url.isLocalFile(), ok, url, fi, pm, dark_pm, mi };

    if (ok && url.isLocalFile() && (!ci.mi_valid || !ci.thumb_valid)) {
        PersistentManager::get().save(pif);
    }
    if (!url.isLocalFile() && !url.scheme().startsWith("dvd")) {
        pif.mi.filePath = pif.url.path();

        pif.mi.width = _engine->_current->width();
        pif.mi.height = _engine->_current->height();
        pif.mi.resolution = QString::number(_engine->_current->width()) + "x"
                            + QString::number(_engine->_current->height());

        pif.mi.duration = _engine->_current->duration();
        auto suffix = pif.mi.title.mid(pif.mi.title.lastIndexOf('.'));
        suffix.replace(QString("."), QString(""));
        pif.mi.fileType = suffix;
        pif.mi.fileSize = getUrlFileTotalSize(url, 3);
        pif.mi.filePath = url.toDisplayString();
    }
    return pif;
}

int PlaylistModel::indexOf(const QUrl &url)
{
    auto p = std::find_if(_infos.begin(), _infos.end(), [&](const PlayItemInfo & pif) {
        return pif.url == url;
    });

    if (p == _infos.end()) return -1;
    return static_cast<int>(std::distance(_infos.begin(), p));
}


LoadThread::LoadThread(PlaylistModel *model, const QList<QUrl> &urls):_urls(urls)
{
    _pModel = nullptr;
    _pModel = model;
//    _urls = urls;
}
LoadThread::~LoadThread()
{
    _pModel = nullptr;
}

void LoadThread::run()
{
    if (_pModel) {
        _pModel->delayedAppendAsync(_urls);
    }
}
#ifdef _LIBDMR_
static int open_codec_context(int *stream_idx,
                              AVCodecParameters **dec_ctx, AVFormatContext *fmt_ctx, enum AVMediaType type)
{
    int ret, stream_index;
    AVStream *st;
    AVCodec *dec = NULL;
    AVDictionary *opts = NULL;
    ret = av_find_best_stream(fmt_ctx, type, -1, -1, NULL, 0);
    if (ret < 0) {
        qWarning() << "Could not find " << av_get_media_type_string(type)
                   << " stream in input file";
        return ret;
    }

    stream_index = ret;
    st = fmt_ctx->streams[stream_index];
#if LIBAVFORMAT_VERSION_MAJOR >= 57
    *dec_ctx = st->codecpar;
    dec = avcodec_find_decoder((*dec_ctx)->codec_id);
#else
    /* find decoder for the stream */
    dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) {
        fprintf(stderr, "Failed to find %s codec\n",
                av_get_media_type_string(type));
        return AVERROR(EINVAL);
    }
    /* Allocate a codec context for the decoder */
    *dec_ctx = avcodec_alloc_context3(dec);
    if (!*dec_ctx) {
        fprintf(stderr, "Failed to allocate the %s codec context\n",
                av_get_media_type_string(type));
        return AVERROR(ENOMEM);
    }
    /* Copy codec parameters from input stream to output codec context */
    if ((ret = avcodec_parameters_to_context(*dec_ctx, st->codecpar)) < 0) {
        fprintf(stderr, "Failed to copy %s codec parameters to decoder context\n",
                av_get_media_type_string(type));
        return ret;
    }
#endif

    *stream_idx = stream_index;
    return 0;
}
MovieInfo MovieInfo::parseFromFile(const QFileInfo &fi, bool *ok)
{
    struct MovieInfo mi;
    mi.valid = false;
    AVFormatContext *av_ctx = NULL;
    int stream_id = -1;
    AVCodecParameters *dec_ctx = NULL;
    AVStream *av_stream = nullptr;

    if (!fi.exists()) {
        if (ok) *ok = false;
        return mi;
    }

    auto ret = avformat_open_input(&av_ctx, fi.filePath().toUtf8().constData(), NULL, NULL);
    if (ret < 0) {
        qWarning() << "avformat: could not open input";
        if (ok) *ok = false;
        return mi;
    }

    if (avformat_find_stream_info(av_ctx, NULL) < 0) {
        qWarning() << "av_find_stream_info failed";
        if (ok) *ok = false;
        return mi;
    }

    if (av_ctx->nb_streams == 0) {
        if (ok) *ok = false;
        return mi;
    }
    if (open_codec_context(&stream_id, &dec_ctx, av_ctx, AVMEDIA_TYPE_VIDEO) < 0) {
        if (open_codec_context(&stream_id, &dec_ctx, av_ctx, AVMEDIA_TYPE_AUDIO) < 0) {
            if (ok) *ok = false;
            return mi;
        }
    }

    for (int i = 0; i < av_ctx->nb_streams; i++) {
        av_stream = av_ctx->streams[i];
        if (av_stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            break;
        }
    }

    av_dump_format(av_ctx, 0, fi.fileName().toUtf8().constData(), 0);

    mi.width = dec_ctx->width;
    mi.height = dec_ctx->height;
    auto duration = av_ctx->duration == AV_NOPTS_VALUE ? 0 : av_ctx->duration;
    duration = duration + (duration <= INT64_MAX - 5000 ? 5000 : 0);
    mi.duration = duration / AV_TIME_BASE;
    mi.resolution = QString("%1x%2").arg(mi.width).arg(mi.height);
    mi.title = fi.fileName(); //FIXME this
    mi.filePath = fi.canonicalFilePath();
    mi.creation = fi.created().toString();
    mi.fileSize = fi.size();
    mi.fileType = fi.suffix();

    mi.vCodecID = dec_ctx->codec_id;
    mi.vCodeRate = dec_ctx->bit_rate;
    if (av_stream->r_frame_rate.den != 0) {
        mi.fps = av_stream->r_frame_rate.num / av_stream->r_frame_rate.den;
    } else {
        mi.fps = 0;
    }
    if (mi.height != 0) {
        mi.proportion = mi.width / mi.height;
    } else {
        mi.proportion = 0;
    }

    if (open_codec_context(&stream_id, &dec_ctx, av_ctx, AVMEDIA_TYPE_AUDIO) < 0) {
        if (open_codec_context(&stream_id, &dec_ctx, av_ctx, AVMEDIA_TYPE_VIDEO) < 0) {
            if (ok) *ok = false;
            return mi;
        }
    }


    mi.aCodeID = dec_ctx->codec_id;
    mi.aCodeRate = dec_ctx->bit_rate;
    mi.aDigit = dec_ctx->format;
    mi.channels = dec_ctx->channels;
    mi.sampling = dec_ctx->sample_rate;

    AVDictionaryEntry *tag = NULL;
    while ((tag = av_dict_get(av_ctx->metadata, "", tag, AV_DICT_IGNORE_SUFFIX)) != NULL) {
        if (tag->key && strcmp(tag->key, "creation_time") == 0) {
            auto dt = QDateTime::fromString(tag->value, Qt::ISODate);
            mi.creation = dt.toString();
            qDebug() << __func__ << dt.toString();
            break;
        }
        qDebug() << "tag:" << tag->key << tag->value;
    }

    tag = NULL;
    AVStream *st = av_ctx->streams[stream_id];
    while ((tag = av_dict_get(st->metadata, "", tag, AV_DICT_IGNORE_SUFFIX)) != NULL) {
        if (tag->key && strcmp(tag->key, "rotate") == 0) {
            mi.raw_rotate = QString(tag->value).toInt();
            auto vr = (mi.raw_rotate + 360) % 360;
            if (vr == 90 || vr == 270) {
                auto tmp = mi.height;
                mi.height = mi.width;
                mi.width = tmp;
            }
            break;
        }
        qDebug() << "tag:" << tag->key << tag->value;
    }


    avformat_close_input(&av_ctx);
    mi.valid = true;

    if (ok) *ok = true;
    return mi;
}
//#else
//MovieInfo MovieInfo::parseFromFile(const QFileInfo &fi, bool *ok)
//{
//    MovieInfo info;
//    return info;
//}
#endif
}

#include "playlist_model.moc"

