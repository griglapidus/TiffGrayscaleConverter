#include "TiffConverter.h"
#include <immintrin.h>
#include <tiffio.h>
#include <tiffiop.h>
#include <QFileInfo>
#include <QDir>
#include <QFile>
#include <QDesktopServices>
#include <QUrl>
#include <QMessageBox>
#include <omp.h>
#include <QDebug>

extern QMap<int, QString> PhotometricNames;

void Convert1Bit(TIFF *inTiffImage, TIFF *outTiffImage, int nWidth, int nHeight, int targetValue, bool negative)
{
    QDateTime start = QDateTime::currentDateTime();
    unsigned allocatedSize256In = (nWidth + 255) / 256;
    unsigned allocatedSize256Out = allocatedSize256In * 2;

    const __m256i swapOddEvenBytesMask = _mm256_setr_epi8(
        1, 0, 3, 2, 5, 4, 7, 6,
        9, 8, 11, 10, 13, 12, 15, 14,
        1, 0, 3, 2, 5, 4, 7, 6,
        9, 8, 11, 10, 13, 12, 15, 14
        );

    quint64 exendedMask = 0x5555555555555555; // 0b0101....

    std::vector<__m256i> inLineData(allocatedSize256In);
    std::vector<__m256i> outLineData(allocatedSize256Out);
    std::vector<__m256i> emptyData(allocatedSize256Out);

    std::vector<__m256i> &leftData = targetValue == 1 ? emptyData : outLineData;
    std::vector<__m256i> &rightData = targetValue == 2 ? emptyData : outLineData;

    __m256i negativeMask = _mm256_set1_epi8(0xff);
    for (int i = 0; i < nHeight; i++)
    {
        TIFFReadScanline(inTiffImage,(qint8*)inLineData.data(), i);
        quint32 *in = (quint32*)inLineData.data();
        quint64 *out = (quint64*)outLineData.data();
#pragma omp parallel for
        for (int j = 0; j < inLineData.size() * 8; ++j) {
            out[j] = _pdep_u64((uint64_t)in[j], exendedMask);
            }
#pragma omp parallel for
        for (int j = 0; j < outLineData.size(); ++j) {
            outLineData[j] = _mm256_or_si256(_mm256_slli_epi64(leftData[j], 1), rightData[j]);
            outLineData[j] = _mm256_shuffle_epi8(outLineData[j], swapOddEvenBytesMask); // It's needed to swap odd and even bytes due to extension

            if(negative) {
                outLineData[j] = _mm256_xor_si256(outLineData[j], negativeMask);
        }
        }
        TIFFWriteScanline(outTiffImage, outLineData.data(), i, 0);
    }
    QDateTime end = QDateTime::currentDateTime();
    qint64 duration = start.msecsTo(end);
    qDebug() << "Convert1Bit" << inTiffImage->tif_name << start << "duration" << duration << "msec";
}

void Convert2Bit(TIFF *inTiffImage, TIFF *outTiffImage, int nWidth, int nHeight, bool negative)
{
    QDateTime start = QDateTime::currentDateTime();
    unsigned allocatedSize256In = (nWidth + 127) / 128;

    std::vector<__m256i> inLineData(allocatedSize256In);

    __m256i negativeMask = _mm256_set1_epi8(0xff);
    for (int i = 0; i < nHeight; i++)
    {
        TIFFReadScanline(inTiffImage,(qint8*)inLineData.data(), i);
#pragma omp parallel for
        for (int j = 0; j < inLineData.size(); ++j) {
            if(negative) {
                inLineData[j] = _mm256_xor_si256(inLineData[j], negativeMask);
            }
        }
        TIFFWriteScanline(outTiffImage, inLineData.data(), i, 0);
    }
    QDateTime end = QDateTime::currentDateTime();
    qint64 duration = start.msecsTo(end);
    qDebug() << "Convert2Bit" << inTiffImage->tif_name << start << "duration" << duration << "msec";
}

void Convert8Bit(TIFF *inTiffImage, TIFF *outTiffImage, int nWidth, int nHeight, bool negative)
{
    QDateTime start = QDateTime::currentDateTime();
    unsigned allocatedSize256In = (nWidth + 31)/32 * 4;
    unsigned allocatedSize256Out = allocatedSize256In / 4;

    QVector<__m256i> inLineData(allocatedSize256In);
    QVector<__m256i> outLineData(allocatedSize256Out);
    QVector<__m256i> copyMasks = {_mm256_set1_epi8(0xc0),_mm256_set1_epi8(0x30),_mm256_set1_epi8(0x0c),_mm256_set1_epi8(0x03)};

    __m256i negativeMask = _mm256_set1_epi8(0xff);
    QVector<__m256i> pixels(4);
    auto pixelsBytes = reinterpret_cast<quint8(&)[4][32]>(pixels[0]);
    for (int row = 0; row < nHeight; row++)
    {
        TIFFReadScanline(inTiffImage,(qint8*)inLineData.data(), row);
#pragma omp parallel for
        for(int i = 0; i < allocatedSize256Out; ++i) {
            quint8 *chankData = (quint8*)&inLineData[i*4];
            for(int j = 0; j < 4; ++j) {
                for(int pixel = 0; pixel < 32; ++pixel) {
                    pixelsBytes[j][pixel] = chankData[pixel * 4 + j];
                }
                if(j > 0) {
                    pixels[j] = _mm256_srli_epi64(pixels[j], j * 2);
                }

                pixels[j] = _mm256_and_si256(pixels[j], copyMasks[j]);
            }
            outLineData[i] = _mm256_or_si256(_mm256_or_si256(pixels[0], pixels[1]), _mm256_or_si256(pixels[2], pixels[3]));
            if(negative) {
                outLineData[i] = _mm256_xor_si256(outLineData[i], negativeMask);
            }
        }

        TIFFWriteScanline(outTiffImage, outLineData.data(), row, 0);
    }
    QDateTime end = QDateTime::currentDateTime();
    qint64 duration = start.msecsTo(end);
    qDebug() << "Convert8Bit" << inTiffImage->tif_name << start << "duration" << duration << "msec";
}

TiffConverter::TiffConverter(QObject *parent)
    : QObject{parent}
{}

void TiffConverter::ConvertTiff(QStringList inFiles, QString outputFolder, int targetValue, bool negative, bool openOutput)
{
    m_stopFlag = false;
    if(!inFiles.size()){
        return;
    }
    int count = inFiles.size();
    std::atomic<int> doneCounter = 0;
    omp_lock_t ompSync;
    omp_init_lock( &ompSync );
    QStringList outputFolders;
#pragma omp parallel for shared(doneCounter)
    for(int i = 0; i < inFiles.size(); ++i) {
        if(m_stopFlag)
            continue;
        QString file = inFiles[i];
        QFileInfo info(file);
        QString fileName = info.fileName();
        QString outputFilePath;
        if(outputFolder.size()) {
            QDir dir(outputFolder);
            outputFolders.append(dir.absolutePath());
            outputFilePath = dir.absoluteFilePath(fileName);
        } else {
            QDir dir(info.absolutePath());
            dir.mkdir("Output_2Bit");
            dir.cd("Output_2Bit");
            outputFolders.append(dir.absolutePath());
            outputFilePath = dir.absoluteFilePath(fileName);
        }
        QFile fFile(outputFilePath);
        if (fFile.exists())
            fFile.remove();
        ConvertTiff(file, outputFilePath, targetValue, negative);
        omp_set_lock( &ompSync );
        doneCounter++;
        emit progressSignal(100 * doneCounter / count);
        omp_unset_lock( &ompSync );
    }
    if(openOutput) {
        outputFolders.removeDuplicates();
        for(auto& outputFolder: outputFolders) {
            QDesktopServices::openUrl(QUrl(outputFolder));
        }
    }
    emit finished();
}

void TiffConverter::stopProcess()
{
    m_stopFlag = true;
}

void TiffConverter::ConvertTiff(QString inFile, QString outFile, int targetValue, bool negative)
{
    TIFF* inTiffImage = TIFFOpen(inFile.toLocal8Bit(), "r");
    if (inTiffImage == NULL)
        return;

    int nWidth = 0, nHeight = 0, nRowsPerStrip = 0;
    quint16 nInBpp, nInSpp, nInPlanConf, nInPhotomrtric, nInComp;
    nInBpp = nInSpp = nInPlanConf = nInPhotomrtric = nInComp = 0;
    float fXRes = 0, fYRes = 0;
    // Configure the tiff parameters (use 8 bits palette configuration)
    TIFFGetField(inTiffImage, TIFFTAG_IMAGEWIDTH, &nWidth);
    TIFFGetField(inTiffImage, TIFFTAG_IMAGELENGTH, &nHeight);
    TIFFGetField(inTiffImage, TIFFTAG_XRESOLUTION, &fXRes);
    TIFFGetField(inTiffImage, TIFFTAG_YRESOLUTION, &fYRes);
    TIFFGetField(inTiffImage, TIFFTAG_SAMPLESPERPIXEL, &nInSpp);
    TIFFGetField(inTiffImage, TIFFTAG_BITSPERSAMPLE, &nInBpp);
    TIFFGetField(inTiffImage, TIFFTAG_PLANARCONFIG, &nInPlanConf);
    TIFFGetField(inTiffImage, TIFFTAG_PHOTOMETRIC, &nInPhotomrtric);
    TIFFGetField(inTiffImage, TIFFTAG_COMPRESSION, &nInComp);
    TIFFGetField(inTiffImage, TIFFTAG_ROWSPERSTRIP, &nRowsPerStrip);

    if(nInBpp != 1 && nInBpp != 8 && nInBpp != 2) {
        emit showMsg(QString("Unsupported sample per pixel image format: %1 in File: %2").arg(nInBpp).arg(inFile));
        return;
    }

    if(nInPhotomrtric == 1) {
        negative = !negative;
        nInPhotomrtric = 0;
    }

    if(nInComp == 1) {
        nInComp = 32946; // COMPRESSION_DEFLATE
    }

    TIFF* outTiffImage = TIFFOpen(outFile.toLocal8Bit(), "w+");
    if (outTiffImage == NULL)
        return;

    if(m_xRes > 0) fXRes = m_xRes;
    if(m_yRes > 0) fYRes = m_yRes;

    TIFFSetField(outTiffImage, TIFFTAG_IMAGEWIDTH, nWidth);
    TIFFSetField(outTiffImage, TIFFTAG_IMAGELENGTH, nHeight);
    TIFFSetField(outTiffImage, TIFFTAG_XRESOLUTION, fXRes);
    TIFFSetField(outTiffImage, TIFFTAG_YRESOLUTION, fYRes);
    TIFFSetField(outTiffImage, TIFFTAG_SAMPLESPERPIXEL, nInSpp);
    TIFFSetField(outTiffImage, TIFFTAG_BITSPERSAMPLE, 2);
    TIFFSetField(outTiffImage, TIFFTAG_PLANARCONFIG, nInPlanConf);
    TIFFSetField(outTiffImage, TIFFTAG_PHOTOMETRIC, nInPhotomrtric);
    TIFFSetField(outTiffImage, TIFFTAG_COMPRESSION, nInComp);
    TIFFSetField(outTiffImage, TIFFTAG_ROWSPERSTRIP, nRowsPerStrip);

    switch (nInBpp) {
    case 1:{
        Convert1Bit(inTiffImage, outTiffImage, nWidth, nHeight, targetValue, negative);
    }break;
    case 2:{
        Convert2Bit(inTiffImage, outTiffImage, nWidth, nHeight, negative);
    }break;
    case 8:{
        Convert8Bit(inTiffImage, outTiffImage, nWidth, nHeight, negative);
    }break;
    default:
        break;
    }

    TIFFClose(inTiffImage);
    inTiffImage = NULL;
    TIFFClose(outTiffImage);
    outTiffImage = NULL;
}

void TiffConverter::setRes(double newXRes, double newYRes)
{
    m_xRes = newXRes;
    m_yRes = newYRes;
}
