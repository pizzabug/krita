/*
 *  SPDX-FileCopyrightText: 2019 Dmitrii Utkin <loentar@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisClipboardUtil.h"

#include <QApplication>
#include <QClipboard>
#include <QMimeData>
#include <QMessageBox>
#include <QImage>
#include <QUrl>
#include <QTemporaryFile>
#include <QList>
#include <QSet>
#include <QPair>
#include <QDebug>
#include <QMenu>
#include <QFileInfo>

#include <KisPart.h>
#include <KisImportExportManager.h>
#include <KisMainWindow.h>
#include <KisMimeDatabase.h>
#include <kis_canvas2.h>
#include <KisViewManager.h>
#include <kis_image_manager.h>
#include "KisRemoteFileFetcher.h"
#include "kis_node_commands_adapter.h"
#include "kis_file_layer.h"
#include "KisReferenceImage.h"
#include "kis_coordinates_converter.h"
#include <KisDocument.h>

namespace KisClipboardUtil {

struct ClipboardImageFormat
{
    QSet<QString> mimeTypes;
    QString format;
};

bool clipboardHasUrls()
{
    return QApplication::clipboard()->mimeData()->hasUrls();
}

void clipboardHasUrlsAction(KisView *kisview, const QMimeData *data, const QPoint eventPos)
{
    if (data->hasUrls()) {
        QList<QUrl> urls = data->urls();
        if (urls.length() > 0) {

            QMenu popup;
            popup.setObjectName("drop_popup");

            QAction *insertAsNewLayer = new QAction(i18n("Insert as New Layer"), &popup);
            QAction *insertManyLayers = new QAction(i18n("Insert Many Layers"), &popup);

            QAction *insertAsNewFileLayer = new QAction(i18n("Insert as New File Layer"), &popup);
            QAction *insertManyFileLayers = new QAction(i18n("Insert Many File Layers"), &popup);

            QAction *openInNewDocument = new QAction(i18n("Open in New Document"), &popup);
            QAction *openManyDocuments = new QAction(i18n("Open Many Documents"), &popup);

            QAction *insertAsReferenceImage = new QAction(i18n("Insert as Reference Image"), &popup);
            QAction *insertAsReferenceImages = new QAction(i18n("Insert as Reference Images"), &popup);

            QAction *cancel = new QAction(i18n("Cancel"), &popup);

            popup.addAction(insertAsNewLayer);
            popup.addAction(insertAsNewFileLayer);
            popup.addAction(openInNewDocument);
            popup.addAction(insertAsReferenceImage);

            popup.addAction(insertManyLayers);
            popup.addAction(insertManyFileLayers);
            popup.addAction(openManyDocuments);
            popup.addAction(insertAsReferenceImages);

            insertAsNewLayer->setEnabled(kisview->image() && urls.count() == 1);
            insertAsNewFileLayer->setEnabled(kisview->image() && urls.count() == 1);
            openInNewDocument->setEnabled(urls.count() == 1);
            insertAsReferenceImage->setEnabled(kisview->image() && urls.count() == 1);

            insertManyLayers->setEnabled(kisview->image() && urls.count() > 1);
            insertManyFileLayers->setEnabled(kisview->image() && urls.count() > 1);
            openManyDocuments->setEnabled(urls.count() > 1);
            insertAsReferenceImages->setEnabled(kisview->image() && urls.count() > 1);

            popup.addSeparator();
            popup.addAction(cancel);

            QAction *action = popup.exec(QCursor::pos());
            if (action != 0 && action != cancel) {
                for (QUrl url : urls) {

                    QScopedPointer<QTemporaryFile> tmp(new QTemporaryFile());
                    tmp->setAutoRemove(true);

                    if (!url.isLocalFile()) {
                        // download the file and substitute the url
                        KisRemoteFileFetcher fetcher;

                        if (!fetcher.fetchFile(url, tmp.data())) {
                            qWarning() << "Fetching" << url << "failed";
                            continue;
                        }
                        url = url.fromLocalFile(tmp->fileName());
                    }

                    if (url.isLocalFile()) {
                        if (action == insertAsNewLayer || action == insertManyLayers) {
                            kisview->mainWindow()->viewManager()->imageManager()->importImage(url);
                            kisview->activateWindow();
                        }
                        else if (action == insertAsNewFileLayer || action == insertManyFileLayers) {
                            KisNodeCommandsAdapter adapter(kisview->mainWindow()->viewManager());
                            QFileInfo fileInfo(url.toLocalFile());

                            QString type = KisMimeDatabase::mimeTypeForFile(url.toLocalFile());
                            QStringList mimes =
                                KisImportExportManager::supportedMimeTypes(KisImportExportManager::Import);

                            if (!mimes.contains(type)) {
                                QString msg =
                                    KisImportExportErrorCode(ImportExportCodes::FileFormatNotSupported).errorMessage();
                                QMessageBox::warning(
                                    kisview, i18nc("@title:window", "Krita"),
                                    i18n("Could not open %2.\nReason: %1.", msg, url.toDisplayString()));
                                continue;
                            }

                            KisFileLayer *fileLayer = new KisFileLayer(kisview->image(), "", url.toLocalFile(),
                                                                       KisFileLayer::None, fileInfo.fileName(), OPACITY_OPAQUE_U8);

                            KisLayerSP above = kisview->mainWindow()->viewManager()->activeLayer();
                            KisNodeSP parent = above ? above->parent() : kisview->mainWindow()->viewManager()->image()->root();

                            adapter.addNode(fileLayer, parent, above);
                        }
                        else if (action == openInNewDocument || action == openManyDocuments) {
                            if (kisview->mainWindow()) {
                                kisview->mainWindow()->openDocument(url.toLocalFile(), KisMainWindow::None);
                            }
                        }
                        else if (action == insertAsReferenceImage || action == insertAsReferenceImages) {
                            auto *reference = KisReferenceImage::fromFile(url.toLocalFile(), *kisview->viewConverter(), kisview);

                            if (reference) {
                                const auto pos = kisview->canvasBase()->coordinatesConverter()->widgetToImage(eventPos);
                                reference->setPosition((*kisview->viewConverter()).imageToDocument(pos));
                                kisview->canvasBase()->referenceImagesDecoration()->addReferenceImage(reference);

                                KoToolManager::instance()->switchToolRequested("ToolReferenceImages");
                            }
                        }

                    }
                }
            }
        }
    }
}

QImage getImageFromClipboard()
{
    static const QList<ClipboardImageFormat> supportedFormats = {
            {{"image/png"}, "PNG"},
            {{"image/tiff"}, "TIFF"},
            {{"image/bmp", "image/x-bmp", "image/x-MS-bmp", "image/x-win-bitmap"}, "BMP"}
    };

    QClipboard *clipboard = QApplication::clipboard();

    QImage image;
    QSet<QString> clipboardMimeTypes;

    Q_FOREACH(const QString &format, clipboard->mimeData()->formats()) {
        clipboardMimeTypes << format;
    }

    Q_FOREACH (const ClipboardImageFormat &item, supportedFormats) {
        const QSet<QString> &intersection = item.mimeTypes & clipboardMimeTypes;
        if (intersection.isEmpty()) {
            continue;
        }

        const QString &format = *intersection.constBegin();
        const QByteArray &imageData = clipboard->mimeData()->data(format);
        if (imageData.isEmpty()) {
            continue;
        }

        if (image.loadFromData(imageData, item.format.toLatin1())) {
            break;
        }
    }

    if (image.isNull()) {
        image = clipboard->image();
    }

    return image;
}

KisPaintDeviceSP fetchImageByURL(const QUrl &originalUrl)
{
    KisPaintDeviceSP result;
    QUrl url = originalUrl;
    QScopedPointer<QTemporaryFile> tmp;

    if (!url.isLocalFile()) {
        tmp.reset(new QTemporaryFile());
        tmp->setAutoRemove(true);

        // download the file and substitute the url
        KisRemoteFileFetcher fetcher;

        if (!fetcher.fetchFile(url, tmp.data())) {
            qWarning() << "Fetching" << url << "failed";
            return result;
        }
        url = url.fromLocalFile(tmp->fileName());
    }

    if (url.isLocalFile()) {

        QFileInfo fileInfo(url.toLocalFile());

        QString type = KisMimeDatabase::mimeTypeForFile(url.toLocalFile());
        QStringList mimes =
            KisImportExportManager::supportedMimeTypes(KisImportExportManager::Import);

        if (!mimes.contains(type)) {
            QString msg =
                KisImportExportErrorCode(ImportExportCodes::FileFormatNotSupported).errorMessage();
            QMessageBox::warning(
                KisPart::instance()->currentMainwindow(), i18nc("@title:window", "Krita"),
                i18n("Could not open %2.\nReason: %1.", msg, url.toDisplayString()));
            return result;
        }

        QScopedPointer<KisDocument> doc(KisPart::instance()->createDocument());

        if (doc->importDocument(url.toLocalFile())) {
            result = new KisPaintDevice(*doc->image()->projection());
        } else {
            qWarning() << "Failed to import file" << url.toLocalFile();
        }
    }

    return result;
}
}
