#include "community/qr.h"

#include <BitMatrix.h>
#include <MultiFormatWriter.h>
#include <ReadBarcode.h>

#include <stdexcept>

#include "community/model.h"

namespace community {
QImage inviteQr(const QString& link) {
    Invite::fromLink(link);
    const auto bits = ZXing::MultiFormatWriter(ZXing::BarcodeFormat::QRCode)
                          .setMargin(4)
                          .setEccLevel(1)
                          .encode(link.toStdString(), 512, 512);
    QImage image(bits.width(), bits.height(), QImage::Format_Grayscale8);
    for (int y = 0; y < bits.height(); ++y) {
        auto* row = image.scanLine(y);
        for (int x = 0; x < bits.width(); ++x)
            row[x] = bits.get(x, y) ? 0 : 255;
    }
    return image;
}

QString readInviteQr(const QImage& image) {
    if (image.isNull() || qint64(image.width()) * image.height() > 16 * 1024 * 1024)
        throw std::runtime_error("QR image is empty or too large");
    const auto gray = image.convertToFormat(QImage::Format_Grayscale8);
    const auto barcode =
        ZXing::ReadBarcode(ZXing::ImageView(gray.constBits(), gray.width(), gray.height(),
                                            ZXing::ImageFormat::Lum, gray.bytesPerLine()),
                           ZXing::ReaderOptions().setFormats(ZXing::BarcodeFormat::QRCode));
    if (!barcode.isValid())
        throw std::runtime_error("no readable invite QR code in this image");
    const auto link = QString::fromStdString(barcode.text());
    Invite::fromLink(link);
    return link;
}
} // namespace community
