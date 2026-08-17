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

#include "SpeechRecognizerFactory.h"
#include "SherpaRecognizer.h"
#include "SpeechRecognizer.h"
#include "VoskRecognizer.h"

#include <QDir>

SpeechRecognizer* SpeechRecognizerFactory::create(Backend backend, QObject* parent)
{
    // Handle Auto selection - pick the first available backend
    if (backend == Backend::Auto) {
        const auto backends = availableBackends();
        if (backends.isEmpty()) {
            return nullptr;
        }
        backend = backends.first();
    }

    switch (backend) {
    case Backend::Vosk:
        if (!VoskRecognizer::isVoskAvailable()) {
            qWarning() << "SpeechRecognizerFactory: Vosk backend requested but not available";
            return nullptr;
        }
        return new VoskRecognizer(parent);

    case Backend::Sherpa:
        if (!SherpaRecognizer::isSherpaAvailable()) {
            qWarning() << "SpeechRecognizerFactory: sherpa-onnx backend requested but not available";
            return nullptr;
        }
        return new SherpaRecognizer(parent);

    case Backend::Whisper:
        // Future: return new WhisperRecognizer(parent);
        return nullptr;

    case Backend::Platform:
        // Future: return platform-specific implementation
        return nullptr;

    case Backend::Auto:
        // Already handled above, but needed for compiler warning
        return nullptr;
    }

    return nullptr;
}

QList<SpeechRecognizerFactory::Backend> SpeechRecognizerFactory::availableBackends()
{
    QList<Backend> backends;

    if (VoskRecognizer::isVoskAvailable()) {
        backends.append(Backend::Vosk);
    }

    if (SherpaRecognizer::isSherpaAvailable()) {
        backends.append(Backend::Sherpa);
    }

    // Future: Check for Whisper availability
    // Future: Check for platform API availability

    return backends;
}

bool SpeechRecognizerFactory::isBackendAvailable(Backend backend)
{
    switch (backend) {
    case Backend::Vosk:
        return VoskRecognizer::isVoskAvailable();

    case Backend::Sherpa:
        return SherpaRecognizer::isSherpaAvailable();

    case Backend::Whisper:
        return false; // Not yet implemented

    case Backend::Platform:
        return false; // Not yet implemented

    case Backend::Auto:
        return !availableBackends().isEmpty();
    }

    return false;
}

QString SpeechRecognizerFactory::backendDisplayName(Backend backend)
{
    switch (backend) {
    case Backend::Vosk:
        return tr("Vosk (Offline)");
    case Backend::Sherpa:
        return tr("sherpa-onnx (Offline)");
    case Backend::Whisper:
        return tr("Whisper (Offline)");
    case Backend::Platform:
#if defined(Q_OS_MACOS)
        return tr("macOS Speech Recognition");
#elif defined(Q_OS_WIN)
        return tr("Windows Speech Recognition");
#else
        return tr("Platform Speech Recognition");
#endif
    case Backend::Auto:
        return tr("Automatic");
    }

    return tr("Unknown");
}

QString SpeechRecognizerFactory::backendIdentifier(Backend backend)
{
    switch (backend) {
    case Backend::Vosk:
        return QStringLiteral("vosk");
    case Backend::Sherpa:
        return QStringLiteral("sherpa");
    case Backend::Whisper:
        return QStringLiteral("whisper");
    case Backend::Platform:
        return QStringLiteral("platform");
    case Backend::Auto:
        return QStringLiteral("auto");
    }

    return QStringLiteral("auto");
}

SpeechRecognizerFactory::Backend SpeechRecognizerFactory::backendFromIdentifier(const QString& identifier)
{
    if (identifier == QLatin1String("vosk")) {
        return Backend::Vosk;
    }
    if (identifier == QLatin1String("sherpa")) {
        return Backend::Sherpa;
    }
    if (identifier == QLatin1String("whisper")) {
        return Backend::Whisper;
    }
    if (identifier == QLatin1String("platform")) {
        return Backend::Platform;
    }

    return Backend::Auto;
}

QString SpeechRecognizerFactory::defaultModelPath(Backend backend)
{
    // Handle Auto selection - the first available backend that actually has a
    // model installed wins, so an engine whose library is present but whose
    // models are not does not shadow one that is ready to run
    if (backend == Backend::Auto) {
        for (const Backend candidate : availableBackends()) {
            // Existence-checked: Vosk's default is a conventional path that is
            // reported whether or not a model is installed there
            const QString path = defaultModelPath(candidate);
            if (!path.isEmpty() && QDir(path).exists()) {
                return path;
            }
        }
        return QString();
    }

    switch (backend) {
    case Backend::Vosk:
        return VoskRecognizer::defaultModelPath();

    case Backend::Sherpa:
        return SherpaRecognizer::defaultModelPath();

    case Backend::Whisper:
        // Future: return WhisperRecognizer::defaultModelPath();
        return QString();

    case Backend::Platform:
        // Platform APIs typically don't use model paths
        return QString();

    case Backend::Auto:
        // Already handled above
        return QString();
    }

    return QString();
}

SpeechRecognizerFactory::Backend SpeechRecognizerFactory::backendForModelDir(const QString& modelPath)
{
    if (SherpaRecognizer::looksLikeModelDir(modelPath)) {
        return Backend::Sherpa;
    }

    // Vosk/Kaldi models carry their acoustic model in an "am" subdirectory
    const QDir modelDir(modelPath);
    if (modelDir.exists(QStringLiteral("am")) || modelDir.exists(QStringLiteral("conf"))) {
        return Backend::Vosk;
    }

    return Backend::Auto;
}
