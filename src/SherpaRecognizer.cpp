/***************************************************************************
 *   Copyright (C) 2026 by Mike Conley - mike.conley@stickmud.com          *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#include "SherpaRecognizer.h"

#include "SpeechAudioCapture.h"
#include "mudlet.h"

#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QVarLengthArray>
#include <QtMath>

#if defined(Q_OS_MACOS)
#include "MacMicrophonePermission.h"
#endif

// Vendored from sherpa-onnx v1.13.5 c-api.h. These layouts are the ABI the
// loaded library reads, so field order and types must not change; sherpa-onnx
// itself only ever appends fields, which initialize() defends against by
// passing the struct inside a larger zeroed block.

struct SherpaOnnxOnlineTransducerModelConfig
{
    const char* encoder;
    const char* decoder;
    const char* joiner;
};

struct SherpaOnnxOnlineParaformerModelConfig
{
    const char* encoder;
    const char* decoder;
};

struct SherpaOnnxOnlineZipformer2CtcModelConfig
{
    const char* model;
};

struct SherpaOnnxOnlineNemoCtcModelConfig
{
    const char* model;
};

struct SherpaOnnxOnlineToneCtcModelConfig
{
    const char* model;
};

struct SherpaOnnxOnlineModelConfig
{
    SherpaOnnxOnlineTransducerModelConfig transducer;
    SherpaOnnxOnlineParaformerModelConfig paraformer;
    SherpaOnnxOnlineZipformer2CtcModelConfig zipformer2_ctc;
    const char* tokens;
    qint32 num_threads;
    const char* provider;
    qint32 debug;
    const char* model_type;
    const char* modeling_unit;
    const char* bpe_vocab;
    const char* tokens_buf;
    qint32 tokens_buf_size;
    SherpaOnnxOnlineNemoCtcModelConfig nemo_ctc;
    SherpaOnnxOnlineToneCtcModelConfig t_one_ctc;
};

struct SherpaOnnxFeatureConfig
{
    qint32 sample_rate;
    qint32 feature_dim;
};

struct SherpaOnnxOnlineCtcFstDecoderConfig
{
    const char* graph;
    qint32 max_active;
};

struct SherpaOnnxHomophoneReplacerConfig
{
    const char* dict_dir;
    const char* lexicon;
    const char* rule_fsts;
};

struct SherpaOnnxOnlineRecognizerConfig
{
    SherpaOnnxFeatureConfig feat_config;
    SherpaOnnxOnlineModelConfig model_config;
    const char* decoding_method;
    qint32 max_active_paths;
    qint32 enable_endpoint;
    float rule1_min_trailing_silence;
    float rule2_min_trailing_silence;
    float rule3_min_utterance_length;
    const char* hotwords_file;
    float hotwords_score;
    SherpaOnnxOnlineCtcFstDecoderConfig ctc_fst_decoder_config;
    const char* rule_fsts;
    const char* rule_fars;
    float blank_penalty;
    const char* hotwords_buf;
    qint32 hotwords_buf_size;
    SherpaOnnxHomophoneReplacerConfig hr;
};

struct SherpaOnnxOnlineRecognizerResult
{
    const char* text;
    const char* tokens;
    const char* const* tokens_arr;
    float* timestamps;
    qint32 count;
    const char* json;
};

// Static member initialization
QLibrary SherpaRecognizer::sSherpaLibrary;
bool SherpaRecognizer::sLibraryLoaded = false;
bool SherpaRecognizer::sLibraryLoadAttempted = false;

SherpaRecognizer::create_recognizer_fn SherpaRecognizer::s_createOnlineRecognizer = nullptr;
SherpaRecognizer::destroy_recognizer_fn SherpaRecognizer::s_destroyOnlineRecognizer = nullptr;
SherpaRecognizer::create_stream_fn SherpaRecognizer::s_createOnlineStream = nullptr;
SherpaRecognizer::destroy_stream_fn SherpaRecognizer::s_destroyOnlineStream = nullptr;
SherpaRecognizer::accept_waveform_fn SherpaRecognizer::s_onlineStreamAcceptWaveform = nullptr;
SherpaRecognizer::is_ready_fn SherpaRecognizer::s_isOnlineStreamReady = nullptr;
SherpaRecognizer::decode_stream_fn SherpaRecognizer::s_decodeOnlineStream = nullptr;
SherpaRecognizer::get_result_fn SherpaRecognizer::s_getOnlineStreamResult = nullptr;
SherpaRecognizer::destroy_result_fn SherpaRecognizer::s_destroyOnlineRecognizerResult = nullptr;
SherpaRecognizer::is_endpoint_fn SherpaRecognizer::s_onlineStreamIsEndpoint = nullptr;
SherpaRecognizer::stream_reset_fn SherpaRecognizer::s_onlineStreamReset = nullptr;
SherpaRecognizer::input_finished_fn SherpaRecognizer::s_onlineStreamInputFinished = nullptr;
SherpaRecognizer::get_version_fn SherpaRecognizer::s_getVersionStr = nullptr;

SherpaRecognizer::SherpaRecognizer(QObject* parent)
: SpeechRecognizer(parent)
, mpCapture(new SpeechAudioCapture(this))
{
    connect(mpCapture, &SpeechAudioCapture::pcm, this, &SherpaRecognizer::slot_pcmReady);
    connect(mpCapture, &SpeechAudioCapture::captureError, this, &SherpaRecognizer::slot_captureError);
    // A silence timeout ends the utterance the way the user stopping would:
    // finalise and report, never discard
    connect(mpCapture, &SpeechAudioCapture::silenceTimedOut, this, &SherpaRecognizer::stopListening);
}

SherpaRecognizer::~SherpaRecognizer()
{
    // cancel() ends with setState(), which emits stateChanged(). Connections are
    // still live until ~QObject runs, so that would deliver a state change from a
    // half-destroyed object to slots that go on to query it.
    blockSignals(true);
    cancel();
    releaseSherpaResources();
}

void SherpaRecognizer::setSilenceTimeout(int msec)
{
    mpCapture->setSilenceTimeout(msec);
}

int SherpaRecognizer::silenceTimeout() const
{
    return mpCapture->silenceTimeout();
}

// The sherpa-onnx C-API library depends on onnxruntime, which release
// bundles place beside it. Loading those first lets the dynamic linker
// satisfy the dependency from a directory it would not search on its own.
static void preloadBundledDependencies(const QString& directory)
{
    const QDir dir(directory);
    const QStringList dependencies = dir.entryList({QStringLiteral("libonnxruntime*"), QStringLiteral("onnxruntime*")}, QDir::Files);
    for (const QString& dependency : dependencies) {
        QLibrary library(dir.filePath(dependency));
        // Exported globally so the c-api library's own linkage resolves
        // against the preloaded image; ~QLibrary does not unload, so the
        // mapping outlives this scope
        library.setLoadHints(QLibrary::ExportExternalSymbolsHint);
        library.load();
    }
}

bool SherpaRecognizer::loadSherpaLibrary()
{
    if (sLibraryLoadAttempted) {
        return sLibraryLoaded;
    }

    sLibraryLoadAttempted = true;

    // QLibrary automatically adds platform-specific prefix/suffix:
    // "sherpa-onnx-c-api" becomes libsherpa-onnx-c-api.dylib / .so, or
    // sherpa-onnx-c-api.dll on Windows
    sSherpaLibrary.setFileName(QStringLiteral("sherpa-onnx-c-api"));

    if (!sSherpaLibrary.load()) {
        // Try common installation paths
        for (const QString& path : librarySearchPaths()) {
            preloadBundledDependencies(QFileInfo(path).absolutePath());
            sSherpaLibrary.setFileName(path);
            if (sSherpaLibrary.load()) {
                break;
            }
        }
    }

    if (!sSherpaLibrary.isLoaded()) {
        qWarning() << "SherpaRecognizer: Failed to load sherpa-onnx library:" << sSherpaLibrary.errorString();
        return false;
    }

    // Resolve function pointers
    s_createOnlineRecognizer = reinterpret_cast<create_recognizer_fn>(sSherpaLibrary.resolve("SherpaOnnxCreateOnlineRecognizer"));
    s_destroyOnlineRecognizer = reinterpret_cast<destroy_recognizer_fn>(sSherpaLibrary.resolve("SherpaOnnxDestroyOnlineRecognizer"));
    s_createOnlineStream = reinterpret_cast<create_stream_fn>(sSherpaLibrary.resolve("SherpaOnnxCreateOnlineStream"));
    s_destroyOnlineStream = reinterpret_cast<destroy_stream_fn>(sSherpaLibrary.resolve("SherpaOnnxDestroyOnlineStream"));
    s_onlineStreamAcceptWaveform = reinterpret_cast<accept_waveform_fn>(sSherpaLibrary.resolve("SherpaOnnxOnlineStreamAcceptWaveform"));
    s_isOnlineStreamReady = reinterpret_cast<is_ready_fn>(sSherpaLibrary.resolve("SherpaOnnxIsOnlineStreamReady"));
    s_decodeOnlineStream = reinterpret_cast<decode_stream_fn>(sSherpaLibrary.resolve("SherpaOnnxDecodeOnlineStream"));
    s_getOnlineStreamResult = reinterpret_cast<get_result_fn>(sSherpaLibrary.resolve("SherpaOnnxGetOnlineStreamResult"));
    s_destroyOnlineRecognizerResult = reinterpret_cast<destroy_result_fn>(sSherpaLibrary.resolve("SherpaOnnxDestroyOnlineRecognizerResult"));
    s_onlineStreamIsEndpoint = reinterpret_cast<is_endpoint_fn>(sSherpaLibrary.resolve("SherpaOnnxOnlineStreamIsEndpoint"));
    s_onlineStreamReset = reinterpret_cast<stream_reset_fn>(sSherpaLibrary.resolve("SherpaOnnxOnlineStreamReset"));
    s_onlineStreamInputFinished = reinterpret_cast<input_finished_fn>(sSherpaLibrary.resolve("SherpaOnnxOnlineStreamInputFinished"));

    // Optional: only used for diagnostics
    s_getVersionStr = reinterpret_cast<get_version_fn>(sSherpaLibrary.resolve("SherpaOnnxGetVersionStr"));

    if (!s_createOnlineRecognizer || !s_destroyOnlineRecognizer || !s_createOnlineStream || !s_destroyOnlineStream || !s_onlineStreamAcceptWaveform || !s_isOnlineStreamReady || !s_decodeOnlineStream
        || !s_getOnlineStreamResult || !s_destroyOnlineRecognizerResult || !s_onlineStreamIsEndpoint || !s_onlineStreamReset || !s_onlineStreamInputFinished) {
        qWarning() << "SherpaRecognizer: Failed to resolve required sherpa-onnx functions";
        // Unloading on its own would leave the pointers that did resolve aiming
        // into a library that is no longer mapped
        resetLibraryLoadState();
        // resetLibraryLoadState() clears this to allow a fresh probe; a library
        // whose symbols are missing is not worth re-probing on every call
        sLibraryLoadAttempted = true;
        return false;
    }

    sLibraryLoaded = true;

    if (s_getVersionStr) {
        qInfo().noquote() << "SherpaRecognizer: Loaded sherpa-onnx" << QString::fromUtf8(s_getVersionStr());
    }

    return true;
}

bool SherpaRecognizer::isSherpaAvailable()
{
    if (!sLibraryLoadAttempted) {
        loadSherpaLibrary();
    }
    return sLibraryLoaded;
}

void SherpaRecognizer::resetLibraryLoadState()
{
    if (sSherpaLibrary.isLoaded()) {
        sSherpaLibrary.unload();
    }

    sLibraryLoaded = false;
    sLibraryLoadAttempted = false;

    s_createOnlineRecognizer = nullptr;
    s_destroyOnlineRecognizer = nullptr;
    s_createOnlineStream = nullptr;
    s_destroyOnlineStream = nullptr;
    s_onlineStreamAcceptWaveform = nullptr;
    s_isOnlineStreamReady = nullptr;
    s_decodeOnlineStream = nullptr;
    s_getOnlineStreamResult = nullptr;
    s_destroyOnlineRecognizerResult = nullptr;
    s_onlineStreamIsEndpoint = nullptr;
    s_onlineStreamReset = nullptr;
    s_onlineStreamInputFinished = nullptr;
    s_getVersionStr = nullptr;
}

QString SherpaRecognizer::userLibraryPath()
{
    return mudlet::getMudletPath(enums::mainDataItemPath, qsl("sherpa-onnx-lib"));
}

QStringList SherpaRecognizer::librarySearchPaths()
{
    QStringList paths;

#if defined(Q_OS_MACOS)
    paths << QDir(userLibraryPath()).filePath(qsl("libsherpa-onnx-c-api.dylib"));
    paths << QStringLiteral("/usr/local/lib/libsherpa-onnx-c-api.dylib") << QStringLiteral("/opt/homebrew/lib/libsherpa-onnx-c-api.dylib")
          << QCoreApplication::applicationDirPath() + QStringLiteral("/../Frameworks/libsherpa-onnx-c-api.dylib");
#elif defined(Q_OS_WIN)
    paths << QDir(userLibraryPath()).filePath(qsl("sherpa-onnx-c-api.dll"));
    paths << QCoreApplication::applicationDirPath() + QStringLiteral("/sherpa-onnx-c-api.dll");
#else
    paths << QDir(userLibraryPath()).filePath(qsl("libsherpa-onnx-c-api.so"));
    paths << QStringLiteral("/usr/lib/libsherpa-onnx-c-api.so") << QStringLiteral("/usr/local/lib/libsherpa-onnx-c-api.so") << QStringLiteral("/usr/lib/x86_64-linux-gnu/libsherpa-onnx-c-api.so");
#endif

    return paths;
}

bool SherpaRecognizer::isBackendAvailable() const
{
    return isSherpaAvailable();
}

QString SherpaRecognizer::backendVersion() const
{
    if (sLibraryLoaded && s_getVersionStr) {
        return QString::fromUtf8(s_getVersionStr());
    }
    return QString();
}

bool SherpaRecognizer::initialize(const QString& modelPath)
{
    if (!loadSherpaLibrary()) {
        emit errorOccurred(tr("sherpa-onnx library not available"));
        setState(State::Error);
        return false;
    }

    releaseSherpaResources();

    const QDir modelDir(modelPath);
    if (!modelDir.exists()) {
        emit errorOccurred(tr("Model path does not exist: %1").arg(modelPath));
        setState(State::Error);
        return false;
    }

    // Locate the transducer files. Quantised weights are preferred when a
    // model ships both, matching what the published packages recommend.
    auto pickModelFile = [&modelDir](const QString& stem) -> QString {
        const QStringList candidates = modelDir.entryList({stem + qsl("*.onnx")}, QDir::Files, QDir::Name);
        for (const QString& candidate : candidates) {
            if (candidate.contains(QLatin1String("int8"))) {
                return modelDir.filePath(candidate);
            }
        }
        return candidates.isEmpty() ? QString() : modelDir.filePath(candidates.first());
    };

    const QString encoderPath = pickModelFile(qsl("encoder"));
    const QString decoderPath = pickModelFile(qsl("decoder"));
    const QString joinerPath = pickModelFile(qsl("joiner"));
    const QString tokensPath = modelDir.exists(qsl("tokens.txt")) ? modelDir.filePath(qsl("tokens.txt")) : QString();

    if (encoderPath.isEmpty() || decoderPath.isEmpty() || joinerPath.isEmpty() || tokensPath.isEmpty()) {
        emit errorOccurred(tr("Not a sherpa-onnx streaming model (needs tokens.txt and encoder/decoder/joiner .onnx files): %1").arg(modelPath));
        setState(State::Error);
        return false;
    }

    mModelPath = modelPath;
    qInfo().noquote() << "SherpaRecognizer: Loading model from:" << modelPath;

    // The config crosses the ABI boundary by pointer. A library newer than the
    // vendored 1.13.5 layout may read fields appended after it, so the struct
    // sits at the front of a larger zeroed block and any such field reads as
    // zero, which sherpa-onnx replaces with its own default.
    alignas(std::max_align_t) char configBlock[4096] = {};
    static_assert(sizeof(SherpaOnnxOnlineRecognizerConfig) <= sizeof(configBlock));
    auto* config = reinterpret_cast<SherpaOnnxOnlineRecognizerConfig*>(configBlock);

    const QByteArray encoderUtf8 = encoderPath.toUtf8();
    const QByteArray decoderUtf8 = decoderPath.toUtf8();
    const QByteArray joinerUtf8 = joinerPath.toUtf8();
    const QByteArray tokensUtf8 = tokensPath.toUtf8();

    config->model_config.transducer.encoder = encoderUtf8.constData();
    config->model_config.transducer.decoder = decoderUtf8.constData();
    config->model_config.transducer.joiner = joinerUtf8.constData();
    config->model_config.tokens = tokensUtf8.constData();
    config->model_config.num_threads = 2;
    config->enable_endpoint = 1;

    // Endpoint rules by sensitivity; Default leaves the zeroes in place so the
    // library defaults (2.4s / 1.2s / 20s) apply. Rule 1 ends an utterance
    // after silence with no speech decoded, rule 2 after silence following
    // speech, rule 3 caps utterance length.
    switch (mSensitivity) {
    case Sensitivity::Short:
        config->rule1_min_trailing_silence = 1.5f;
        config->rule2_min_trailing_silence = 0.8f;
        config->rule3_min_utterance_length = 15.0f;
        break;
    case Sensitivity::Long:
        config->rule1_min_trailing_silence = 3.6f;
        config->rule2_min_trailing_silence = 2.0f;
        config->rule3_min_utterance_length = 30.0f;
        break;
    case Sensitivity::Default:
        break;
    }

    // Everything left zeroed takes the library default: 16kHz 80-dim
    // features, greedy_search decoding, CPU provider

    mRecognizer = s_createOnlineRecognizer(config);
    if (!mRecognizer) {
        emit errorOccurred(tr("Failed to load sherpa-onnx model from: %1").arg(modelPath));
        setState(State::Error);
        return false;
    }

    // Try to determine language from model path (convention: sherpa-onnx-nemotron-speech-streaming-en-0.6b-...)
    const QString dirName = modelDir.dirName();
    if (dirName.contains(QLatin1String("-en-")) || dirName.contains(QLatin1String("-en_"))) {
        mCurrentLanguage = QStringLiteral("en-US");
    } else if (dirName.contains(QLatin1String("-de-"))) {
        mCurrentLanguage = QStringLiteral("de-DE");
    } else if (dirName.contains(QLatin1String("-fr-"))) {
        mCurrentLanguage = QStringLiteral("fr-FR");
    } else if (dirName.contains(QLatin1String("-es-"))) {
        mCurrentLanguage = QStringLiteral("es-ES");
    } else {
        mCurrentLanguage = QStringLiteral("unknown");
    }

    setState(State::Ready);
    return true;
}

void SherpaRecognizer::startListening()
{
    if (mState != State::Ready) {
        // Every refusal but "already listening" reports why: startListening()
        // returns void, so silence here reads to the caller as a successful start
        if (mState == State::Uninitialized) {
            //: Shown when speech recognition is asked to listen before a language model is loaded
            emit errorOccurred(tr("Recognizer not initialized. Call initialize() first."));
        } else if (mState == State::Error) {
            //: Shown when speech recognition is asked to listen while it is in an error state
            emit errorOccurred(tr("Speech recognition is in an error state - reload the model before listening again."));
        } else if (mState == State::Processing) {
            //: Shown when speech recognition is asked to listen while still transcribing the previous phrase
            emit errorOccurred(tr("Speech recognition is still processing the previous phrase."));
        }
        return;
    }

    // Check microphone permission on macOS using native API
    // Qt's permission API requires proper app signing with entitlements,
    // which development builds don't have, so we use AVFoundation directly.
#if defined(Q_OS_MACOS)
    auto status = MacMicrophonePermission::checkStatus();

    switch (status) {
    case MacMicrophonePermission::AuthorizationStatus::NotDetermined: {
        // requestAccess() dispatches its callback to the main queue, so this runs
        // on the main thread already. Use QPointer to safely handle the case where
        // SherpaRecognizer is destroyed before the permission callback arrives.
        QPointer<SherpaRecognizer> weakThis = this;
        MacMicrophonePermission::requestAccess([weakThis](bool granted) {
            if (!weakThis) {
                return; // SherpaRecognizer was destroyed
            }
            if (granted) {
                weakThis->startListeningInternal();
            } else {
                qWarning() << "SherpaRecognizer: Microphone permission denied by user";
                emit weakThis->errorOccurred(QObject::tr("Microphone permission denied. Please grant microphone access in System Settings > Privacy & Security > Microphone."));
                weakThis->setState(State::Error);
            }
        });
        return;
    }
    case MacMicrophonePermission::AuthorizationStatus::Denied:
    case MacMicrophonePermission::AuthorizationStatus::Restricted:
        qWarning() << "SherpaRecognizer: Microphone permission denied or restricted";
        emit errorOccurred(tr("Microphone permission denied. Please grant microphone access in System Settings > Privacy & Security > Microphone."));
        setState(State::Error);
        return;
    case MacMicrophonePermission::AuthorizationStatus::Authorized:
        break;
    }
#endif

    startListeningInternal();
}

void SherpaRecognizer::startListeningInternal()
{
    if (!mRecognizer) {
        emit errorOccurred(tr("Failed to initialize speech recognition"));
        setState(State::Error);
        return;
    }

    // A fresh stream per session: the stream is the decoding state, so this is
    // what guarantees no residue from the previous session
    destroyStream();
    mStream = s_createOnlineStream(mRecognizer);
    if (!mStream) {
        qWarning() << "SherpaRecognizer: Failed to create recognition stream";
        emit errorOccurred(tr("Failed to initialize speech recognition"));
        setState(State::Error);
        return;
    }

    mLastPartialResult.clear();

    // mpCapture emits its own translated captureError before returning false,
    // which slot_captureError() has already turned into errorOccurred - only
    // the state transition is left to do here
    if (!mpCapture->start()) {
        destroyStream();
        setState(State::Error);
        return;
    }

    setState(State::Listening);
}

void SherpaRecognizer::stopListening()
{
    if (mState != State::Listening) {
        return;
    }

    setState(State::Processing);

    mpCapture->stop();

    if (mRecognizer && mStream) {
        // Flush: signal end-of-input, then decode whatever is buffered
        s_onlineStreamInputFinished(mStream);
        while (s_isOnlineStreamReady(mRecognizer, mStream)) {
            s_decodeOnlineStream(mRecognizer, mStream);
        }

        const SherpaOnnxOnlineRecognizerResult* result = s_getOnlineStreamResult(mRecognizer, mStream);
        if (result) {
            const QString text = result->text ? QString::fromUtf8(result->text).trimmed() : QString();
            if (!text.isEmpty()) {
                emit finalResult(text);
            }
            s_destroyOnlineRecognizerResult(result);
        }
    }

    destroyStream();
    setState(State::Ready);
}

void SherpaRecognizer::resetUtterance()
{
    if (mState != State::Listening) {
        return;
    }

    if (mRecognizer && mStream) {
        s_onlineStreamReset(mRecognizer, mStream);
    }

    // The phrase this was tracking is gone, so nothing should be compared against it
    mLastPartialResult.clear();
}

void SherpaRecognizer::cancel()
{
    if (mState != State::Listening && mState != State::Processing) {
        return;
    }

    // Stop audio capture and drop the stream without processing the remainder
    mpCapture->stop();
    destroyStream();

    setState(State::Ready);
}

void SherpaRecognizer::slot_pcmReady(const QByteArray& pcmData)
{
    if (!mRecognizer || !mStream || pcmData.isEmpty()) {
        return;
    }

    const float level = calculateAudioLevel(pcmData);
    emit audioLevelChanged(level);

    // sherpa-onnx consumes mono float samples in [-1, 1]
    const auto* samples = reinterpret_cast<const qint16*>(pcmData.constData());
    const int numSamples = pcmData.size() / static_cast<int>(sizeof(qint16));
    if (numSamples == 0) {
        return;
    }

    QVarLengthArray<float, 4096> floatSamples(numSamples);
    for (int i = 0; i < numSamples; ++i) {
        floatSamples[i] = static_cast<float>(samples[i]) / 32768.0f;
    }

    s_onlineStreamAcceptWaveform(mStream, SpeechAudioCapture::scmSampleRate, floatSamples.constData(), numSamples);
    while (s_isOnlineStreamReady(mRecognizer, mStream)) {
        s_decodeOnlineStream(mRecognizer, mStream);
    }

    const SherpaOnnxOnlineRecognizerResult* result = s_getOnlineStreamResult(mRecognizer, mStream);
    QString text;
    if (result) {
        text = result->text ? QString::fromUtf8(result->text).trimmed() : QString();
        s_destroyOnlineRecognizerResult(result);
    }

    if (s_onlineStreamIsEndpoint(mRecognizer, mStream)) {
        // The model's own endpointing closed the utterance: report it final and
        // keep listening for the next one on a reset stream
        if (!text.isEmpty()) {
#ifdef DEBUG_STT
            qDebug() << "SherpaRecognizer: Final result:" << text;
#endif
            emit finalResult(text);
        }
        s_onlineStreamReset(mRecognizer, mStream);
        mLastPartialResult.clear();
    } else if (!text.isEmpty() && text != mLastPartialResult) {
        emit partialResult(text);
        mLastPartialResult = text;
    }
}

void SherpaRecognizer::slot_captureError(const QString& message)
{
    // The capture component has already torn its device down; the recognizer
    // just surfaces the fault and leaves Listening
    emit errorOccurred(message);
    setState(State::Error);
}

float SherpaRecognizer::calculateAudioLevel(const QByteArray& data) const
{
    if (data.isEmpty()) {
        return 0.0f;
    }

    // Calculate RMS of 16-bit PCM samples
    const auto* samples = reinterpret_cast<const qint16*>(data.constData());
    const int numSamples = data.size() / sizeof(qint16);

    if (numSamples == 0) {
        return 0.0f;
    }

    qint64 sum = 0;
    for (int i = 0; i < numSamples; ++i) {
        sum += static_cast<qint64>(samples[i]) * samples[i];
    }

    const double rms = qSqrt(static_cast<double>(sum) / numSamples);

    // Normalize to 0.0-1.0 range (32767 is max for 16-bit signed)
    return static_cast<float>(qMin(rms / 32767.0, 1.0));
}

void SherpaRecognizer::destroyStream()
{
    if (mStream && s_destroyOnlineStream) {
        s_destroyOnlineStream(mStream);
        mStream = nullptr;
    }
}

void SherpaRecognizer::releaseSherpaResources()
{
    destroyStream();

    if (mRecognizer && s_destroyOnlineRecognizer) {
        s_destroyOnlineRecognizer(mRecognizer);
        mRecognizer = nullptr;
    }
}

void SherpaRecognizer::releaseResources()
{
    releaseSherpaResources();
    setState(State::Uninitialized);
}

void SherpaRecognizer::setState(State newState)
{
    if (mState != newState) {
        mState = newState;
        emit stateChanged(newState);
    }
}

void SherpaRecognizer::setSensitivity(Sensitivity sensitivity)
{
    if (mSensitivity == sensitivity) {
        return;
    }

    mSensitivity = sensitivity;

    // The endpoint rules are baked into the recognizer at creation, so a
    // loaded model is reloaded for the change to take effect. Only when idle:
    // a listening session keeps the rules it started with.
    if (mState == State::Ready && !mModelPath.isEmpty()) {
        initialize(mModelPath);
    }
}

QStringList SherpaRecognizer::availableLanguages() const
{
    // Derived from what is actually installed rather than a fixed list: model
    // names carry their language following the release naming convention
    QStringList languages;
    for (const QString& model : getInstalledModels()) {
        QString code;
        if (model.contains(QLatin1String("-en-")) || model.contains(QLatin1String("-en_"))) {
            code = QStringLiteral("en-US");
        } else if (model.contains(QLatin1String("-de-"))) {
            code = QStringLiteral("de-DE");
        } else if (model.contains(QLatin1String("-fr-"))) {
            code = QStringLiteral("fr-FR");
        } else if (model.contains(QLatin1String("-es-"))) {
            code = QStringLiteral("es-ES");
        } else {
            continue;
        }
        if (!languages.contains(code)) {
            languages.append(code);
        }
    }
    return languages;
}

bool SherpaRecognizer::setLanguage(const QString& languageCode)
{
    if (mCurrentLanguage == languageCode) {
        return true;
    }

    const QString modelPath = findModelPathForLanguage(languageCode);
    if (modelPath.isEmpty()) {
        emit errorOccurred(tr("No installed model found for language: %1").arg(languageCode));
        return false;
    }

    // initialize() already emits errorOccurred and sets state on failure, and
    // updates mCurrentLanguage from the model path on success
    return initialize(modelPath);
}

QString SherpaRecognizer::findModelPathForLanguage(const QString& languageCode) const
{
    const QString langPart = languageCode.left(2).toLower();

    for (const QString& model : getInstalledModels()) {
        if (model.contains(QLatin1Char('-') + langPart + QLatin1Char('-'), Qt::CaseInsensitive) || model.contains(QLatin1Char('-') + langPart + QLatin1Char('_'), Qt::CaseInsensitive)) {
            return QDir(modelsDirectoryPath()).filePath(model);
        }
    }

    return QString();
}

QString SherpaRecognizer::modelsDirectoryPath()
{
    return mudlet::getMudletPath(enums::mainDataItemPath, qsl("sherpa-models"));
}

QStringList SherpaRecognizer::getInstalledModels()
{
    const QDir modelsDir(modelsDirectoryPath());
    QStringList models;
    for (const QString& entry : modelsDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        if (looksLikeModelDir(modelsDir.filePath(entry))) {
            models.append(entry);
        }
    }
    return models;
}

QString SherpaRecognizer::defaultModelPath()
{
    const QStringList models = getInstalledModels();
    if (models.isEmpty()) {
        return QString();
    }
    return QDir(modelsDirectoryPath()).filePath(models.first());
}

bool SherpaRecognizer::looksLikeModelDir(const QString& modelPath)
{
    const QDir dir(modelPath);
    if (!dir.exists() || !dir.exists(qsl("tokens.txt"))) {
        return false;
    }
    return !dir.entryList({qsl("encoder*.onnx")}, QDir::Files).isEmpty() && !dir.entryList({qsl("decoder*.onnx")}, QDir::Files).isEmpty()
           && !dir.entryList({qsl("joiner*.onnx")}, QDir::Files).isEmpty();
}
