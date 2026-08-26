/************************************************************************

    configurationdialog.cpp

    Capture application for the Domesday Duplicator
    DomesdayDuplicator - LaserDisc RF sampler
    Copyright (C) 2018-2019 Simon Inns

    This file is part of Domesday Duplicator.

    Domesday Duplicator is free software: you can redistribute it and/or
    modify it under the terms of the GNU General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

    Email: simon.inns@gmail.com

************************************************************************/

#include "configurationdialog.h"
#include "ui_configurationdialog.h"
#include "AudioResampler.h"
#include "DddFrontEndGain.h"
#include <algorithm>
#include <QProcess>
#include <QFile>
#include <QFileInfo>
#include <QCoreApplication>
#include <QDir>
#include <QRegularExpression>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSizePolicy>
#include <QDebug>
#include <vector>

namespace
{
QString flacProgramPath()
{
#ifdef _WIN32
    const QString executableName = QStringLiteral("flac.exe");
#else
    const QString executableName = QStringLiteral("flac");
#endif
    const QString bundledPath =
        QDir(QCoreApplication::applicationDirPath()).filePath(executableName);
    if (QFileInfo(bundledPath).isFile())
        return bundledPath;
    return executableName;
}
}

ConfigurationDialog::ConfigurationDialog(QWidget *parent) :
    QDialog(parent)
{
    ui.reset(new Ui::ConfigurationDialog());
    ui->setupUi(this);

    // Build the captureFormatComboBox
    ui->captureFormatComboBox->clear();
    ui->captureFormatComboBox->addItem("Native 16-bit FLAC (real-time)", Configuration::CaptureFormat::flacDirect);
    ui->captureFormatComboBox->addItem("16-bit Signed Raw", Configuration::CaptureFormat::sixteenBitSigned);
    ui->captureFormatComboBox->addItem("10-bit Packed Unsigned", Configuration::CaptureFormat::tenBitPacked);

    // Firmware 3.1 performs the 20 MSPS filtering and 2:1 decimation in the
    // FPGA.  This is a hardware capture-rate choice for every output format.
    ui->sampleRateComboBox->clear();
    ui->sampleRateComboBox->addItem("40 MSPS (LaserDisc / full bandwidth)", 40000);
    ui->sampleRateComboBox->addItem("20 MSPS (tape / FPGA 2:1 decimation)", 20000);
    const QString sampleRateToolTip = tr(
            "Selects the DDD 3.1 hardware sample rate. 20 MSPS uses the FPGA "
            "half-band filter and 2:1 decimator; it is not host-side resampling.");
    ui->sampleRateLabel->setToolTip(sampleRateToolTip);
    ui->sampleRateComboBox->setToolTip(sampleRateToolTip);

    // SW401 is physical and cannot be read or moved by the application.  The
    // setting records the observed switch position for calibration/metadata.
    ui->frontEndGainComboBox->clear();
    ui->frontEndGainComboBox->addItem(
            tr("Not declared - levels remain in converter codes"),
            static_cast<int>(DddFrontEndGain::UndeclaredSwitchPattern));

    std::vector<int> gainPatterns;
    gainPatterns.reserve(DddFrontEndGain::MaximumSwitchPattern);
    for (int pattern = 1; pattern <= DddFrontEndGain::MaximumSwitchPattern; ++pattern)
        gainPatterns.push_back(pattern);
    std::sort(gainPatterns.begin(), gainPatterns.end(), [](int first, int second) {
        return DddFrontEndGain::Gain(first) > DddFrontEndGain::Gain(second);
    });

    for (const int pattern : gainPatterns) {
        ui->frontEndGainComboBox->addItem(
                QString::fromUtf8(DddFrontEndGain::Description(pattern)),
                pattern);
    }

    const QString gainToolTip = tr(
            "This only declares the physical SW401 DIP-switch position for "
            "calibration and capture metadata (1 = ON). Software cannot adjust "
            "the hardware gain; move SW401 on the DDD board to change it.");
    ui->frontEndGainLabel->setToolTip(gainToolTip);
    ui->frontEndGainComboBox->setToolTip(gainToolTip);

    // Build the flacOutputFormatComboBox
    ui->flacOutputFormatComboBox->clear();
    ui->flacOutputFormatComboBox->addItem(".flac - Native real-time FLAC", 0);
    ui->flacOutputFormatComboBox->addItem(".ldf - Legacy post-capture compression", 1);

    // Build the flacCompressionLevelComboBox
    ui->flacCompressionLevelComboBox->clear();
    ui->flacCompressionLevelComboBox->addItem("0 - Fast (largest files)", 0);
    ui->flacCompressionLevelComboBox->addItem("1 - Fast", 1);
    ui->flacCompressionLevelComboBox->addItem("2 - Fast", 2);
    ui->flacCompressionLevelComboBox->addItem("3 - Fast", 3);
    ui->flacCompressionLevelComboBox->addItem("4 - Fast", 4);
    ui->flacCompressionLevelComboBox->addItem("5 - Balanced", 5);
    ui->flacCompressionLevelComboBox->addItem("6 - High", 6);
    ui->flacCompressionLevelComboBox->addItem("7 - High", 7);
    ui->flacCompressionLevelComboBox->addItem("8 - Default / best compression", 8);

    // Build the diskBufferQueueSizeComboBox
    ui->diskBufferQueueSizeComboBox->clear();
    ui->diskBufferQueueSizeComboBox->addItem("64MB", 64 * 1024 * 1024);
    ui->diskBufferQueueSizeComboBox->addItem("128MB", 128 * 1024 * 1024);
    ui->diskBufferQueueSizeComboBox->addItem("256MB", 256 * 1024 * 1024);
    ui->diskBufferQueueSizeComboBox->addItem("512MB", 512 * 1024 * 1024);

    // Build the serialSpeedComboBox
    ui->serialSpeedComboBox->clear();
    ui->serialSpeedComboBox->addItem("Auto", Configuration::SerialSpeeds::autoDetect);
    ui->serialSpeedComboBox->addItem("9600", Configuration::SerialSpeeds::bps9600);
    ui->serialSpeedComboBox->addItem("4800", Configuration::SerialSpeeds::bps4800);
    ui->serialSpeedComboBox->addItem("2400", Configuration::SerialSpeeds::bps2400);
    ui->serialSpeedComboBox->addItem("1200", Configuration::SerialSpeeds::bps1200);
    
    // Build the themeComboBox
    ui->themeComboBox->clear();
    ui->themeComboBox->addItem("Auto (System Default)", 0);
    ui->themeComboBox->addItem("Light Theme", 1);
    ui->themeComboBox->addItem("Dark Theme", 2);

    // If we're running on Linux, disable Windows-specific options.
#ifndef _WIN32
    ui->useWinUsb->setChecked(false);
    ui->useWinUsb->setEnabled(false);
    ui->useAsyncFileIo->setChecked(false);
    ui->useAsyncFileIo->setEnabled(false);
#endif

    // Connect signals for capture format and sample rate selection to update control visibility
    connect(ui->captureFormatComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ConfigurationDialog::onCaptureFormatChanged);
    connect(ui->sampleRateComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ConfigurationDialog::onSampleRateChanged);

    // Build the Audio tab programmatically
    buildAudioTab();

    // Build the SDR HiFi tab programmatically
    buildSdrTab();

#ifdef __APPLE__
    // macOS native controls (QLineEdit, QComboBox, QPushButton) have a larger natural
    // height than on Windows/Linux. The dialog's fixed geometry from the .ui file is
    // too short, causing the layout to distribute leftover space to input rows.
    setMinimumHeight(390);
    resize(width(), 420);
#endif
}

ConfigurationDialog::~ConfigurationDialog()
{
}

void ConfigurationDialog::buildAudioTab()
{
    // Create the Audio tab page
    QWidget* audioPage = new QWidget();
    QVBoxLayout* vbox = new QVBoxLayout(audioPage);

    // Enable checkbox
    audioCaptureCheckBox = new QCheckBox(tr("Enable audio capture (fmedia)"), audioPage);
    vbox->addWidget(audioCaptureCheckBox);

    // fmedia path row
    QHBoxLayout* pathRow = new QHBoxLayout();
    pathRow->addWidget(new QLabel(tr("fmedia path:"), audioPage));
    fmediaPathLineEdit = new QLineEdit(audioPage);
    fmediaPathLineEdit->setPlaceholderText(tr("Leave blank to auto-detect"));
    pathRow->addWidget(fmediaPathLineEdit);
    fmediaPathBrowseBtn = new QPushButton(tr("Browse"), audioPage);
    fmediaPathBrowseBtn->setMinimumWidth(80);
    pathRow->addWidget(fmediaPathBrowseBtn);
    vbox->addLayout(pathRow);

    // Device row
    QHBoxLayout* devRow = new QHBoxLayout();
    devRow->addWidget(new QLabel(tr("Capture device:"), audioPage));
    audioDeviceComboBox = new QComboBox(audioPage);
    audioDeviceComboBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    devRow->addWidget(audioDeviceComboBox);
    audioDeviceRefreshBtn = new QPushButton(tr("Refresh"), audioPage);
    devRow->addWidget(audioDeviceRefreshBtn);
    vbox->addLayout(devRow);

    // Info label
    vbox->addWidget(new QLabel(tr("Audio saved as capture_name_audio.flac in capture directory."), audioPage));

    vbox->addStretch();

    // Add tab to tab widget
    ui->tabWidget->addTab(audioPage, tr("Audio"));

    // Connect buttons
    connect(fmediaPathBrowseBtn, &QPushButton::clicked, this, [this]() {
#ifdef _WIN32
        QString filter = tr("Executable (*.exe);;All files (*)");
        QString title  = tr("Select fmedia.exe");
#else
        QString filter = tr("All files (*)");
        QString title  = tr("Select fmedia");
#endif
        QString path = QFileDialog::getOpenFileName(this, title,
            fmediaPathLineEdit->text(), filter);
        if (!path.isEmpty()) fmediaPathLineEdit->setText(path);
    });
    connect(audioDeviceRefreshBtn, &QPushButton::clicked, this, [this]() {
        refreshAudioDevices();
    });
}

void ConfigurationDialog::refreshAudioDevices()
{
    QString fmediaExe = fmediaPathLineEdit->text().trimmed();
    if (fmediaExe.isEmpty()) {
#ifdef _WIN32
        if (QFileInfo::exists("C:/Program Files/fmedia/fmedia.exe"))
            fmediaExe = "C:/Program Files/fmedia/fmedia.exe";
        else
#endif
            fmediaExe = "fmedia";
    }

    audioDeviceComboBox->clear();

    QProcess proc;
    proc.start(fmediaExe, QStringList() << "--list-dev");
    if (!proc.waitForFinished(5000)) {
        audioDeviceComboBox->addItem(tr("fmedia not found — check path"), -1);
        return;
    }

    QString output = proc.readAllStandardOutput() + proc.readAllStandardError();
    bool inCapture = false;
    QRegularExpression re("^device #(\\d+): (.+)$");
    for (const QString& line : output.split('\n')) {
        QString t = line.trimmed();
        if (t.startsWith("Capture:")) { inCapture = true; continue; }
        if (!inCapture) continue;
        auto m = re.match(t);
        if (m.hasMatch()) {
            int devNum = m.captured(1).toInt();
            QString name = m.captured(2).trimmed();
            name.remove(QRegularExpression("\\s*-\\s*Default\\s*$", QRegularExpression::CaseInsensitiveOption));
            audioDeviceComboBox->addItem(QString("#%1: %2").arg(devNum).arg(name), devNum);
        }
    }

    if (audioDeviceComboBox->count() == 0)
        audioDeviceComboBox->addItem(tr("No capture devices found"), -1);
}

void ConfigurationDialog::updateDeviceList(const std::vector<std::string>& deviceList)
{
    // Build the captureFormatComboBox
    ui->preferredDeviceComboBox->clear();
    for (const auto& devicePath : deviceList)
    {
        ui->preferredDeviceComboBox->addItem(devicePath.c_str(), devicePath.c_str());
    }
}

// Load the configuration settings into the UI widgets
void ConfigurationDialog::loadConfiguration(const Configuration& configuration)
{
    // Read the configuration and set up the widgets

    // Capture
    ui->captureDirectoryLineEdit->setText(configuration.getCaptureDirectory());
    
    // Handle capture format and sample rate loading
    Configuration::CaptureFormat configFormat = configuration.getCaptureFormat();
    int storedSampleRateKHz = configuration.getSampleRate(); // kHz value

    // Migrate combined legacy raw formats to native signed 16-bit.  Firmware
    // 3.1 supports 20 MSPS as its lowest hardware rate.
    if (configFormat == Configuration::CaptureFormat::sixteenBitSigned_Half) {
        ui->captureFormatComboBox->setCurrentIndex(ui->captureFormatComboBox->findData(static_cast<unsigned int>(Configuration::CaptureFormat::sixteenBitSigned)));
        storedSampleRateKHz = 20000;
    } else if (configFormat == Configuration::CaptureFormat::sixteenBitSigned_Quarter) {
        ui->captureFormatComboBox->setCurrentIndex(ui->captureFormatComboBox->findData(static_cast<unsigned int>(Configuration::CaptureFormat::sixteenBitSigned)));
        storedSampleRateKHz = 20000;
    } else if (configFormat == Configuration::CaptureFormat::ldfCompressed) {
        // LDF is represented by the FLAC base format plus its output-extension
        // selector.  Looking up ldfCompressed in the base-format combo leaves
        // the combo at -1 and previously broke the remainder of this page.
        ui->captureFormatComboBox->setCurrentIndex(ui->captureFormatComboBox->findData(static_cast<unsigned int>(Configuration::CaptureFormat::flacDirect)));
    } else {
        ui->captureFormatComboBox->setCurrentIndex(ui->captureFormatComboBox->findData(static_cast<unsigned int>(configFormat)));
    }

    // Update FLAC-only control visibility after the base-format migration.
    onCaptureFormatChanged(ui->captureFormatComboBox->currentIndex());

    int sampleRateItemIndex = ui->sampleRateComboBox->findData(storedSampleRateKHz);
    if (sampleRateItemIndex < 0) sampleRateItemIndex = 0;  // fallback to first item
    ui->sampleRateComboBox->setCurrentIndex(sampleRateItemIndex);
    ui->flacCompressionLevelComboBox->setCurrentIndex(ui->flacCompressionLevelComboBox->findData(configuration.getFlacCompressionLevel()));
    const int flacOutputFormat = (configFormat == Configuration::CaptureFormat::ldfCompressed)
            ? 1
            : configuration.getFlacOutputFormat();
    ui->flacOutputFormatComboBox->setCurrentIndex(
            ui->flacOutputFormatComboBox->findData(flacOutputFormat));
    ui->frontEndGainComboBox->setCurrentIndex(
            ui->frontEndGainComboBox->findData(configuration.getFrontEndGainSwitches()));

    // USB
    ui->vendorIdLineEdit->setText(QString::number(configuration.getUsbVid()));
    ui->productIdLineEdit->setText(QString::number(configuration.getUsbPid()));
    ui->preferredDeviceComboBox->setCurrentText(configuration.getUsbPreferredDevice());
    ui->diskBufferQueueSizeComboBox->setCurrentIndex(ui->diskBufferQueueSizeComboBox->findData((qulonglong)configuration.getDiskBufferQueueSize()));
    ui->useSmallUsbTransferQueue->setChecked(configuration.getUseSmallUsbTransferQueue());
    ui->useSmallUsbTransfers->setChecked(configuration.getUseSmallUsbTransfers());
#ifdef _WIN32
    ui->useWinUsb->setChecked(configuration.getUseWinUsb());
    ui->useAsyncFileIo->setChecked(configuration.getUseAsyncFileIo());
#endif

    // Player Integration

    // Build the serialDeviceComboBox
    ui->serialDeviceComboBox->clear();
    const auto infos = QSerialPortInfo::availablePorts();

    // Add additional "None" option to allow de-selection of COM port
    ui->serialDeviceComboBox->addItem(QString(tr("None")), QString(tr("None")));

    bool configuredSerialDevicePresent = false;
    for (const QSerialPortInfo &info : infos) {
        ui->serialDeviceComboBox->addItem(info.portName(), info.portName());

        // Is this the currently configured serial device?
        if (info.portName() == configuration.getSerialDevice())
                configuredSerialDevicePresent = true;
    }

    // Select the currently configured device (or default to 'none' if the device is not set)
    if (!configuredSerialDevicePresent) {
        // No device is present in the configuration or the configured device is no longer available - set to none
        ui->serialDeviceComboBox->setCurrentIndex(0);
    } else {
        // Set to the configured device
        int index = ui->serialDeviceComboBox->findData(configuration.getSerialDevice());
        ui->serialDeviceComboBox->setCurrentIndex(index);
    }

    // Select the currently configured serial speed
    ui->serialSpeedComboBox->setCurrentIndex(ui->serialSpeedComboBox->findData(static_cast<unsigned int>(configuration.getSerialSpeed())));

    // Keylock flag
    ui->keyLockCheckBox->setChecked(configuration.getKeyLock());

    // Advanced naming
    ui->perSideNotesCheckBox->setChecked(configuration.getPerSideNotesEnabled());
    ui->perSideMintCheckBox->setChecked(configuration.getPerSideMintEnabled());

    // Amplitude
    ui->amplitudeLabelCheckBox->setChecked(configuration.getAmplitudeLabelEnabled());
    ui->amplitudeChartCheckBox->setChecked(configuration.getAmplitudeChartEnabled());
    
    // Theme
    ui->themeComboBox->setCurrentIndex(configuration.getThemeStyle());

    // Audio capture
    audioCaptureCheckBox->setChecked(configuration.getAudioCaptureEnabled());
    fmediaPathLineEdit->setText(configuration.getFmediaPath());
    // Populate device list and restore saved selection
    refreshAudioDevices();
    int savedDeviceIndex = configuration.getAudioCaptureDeviceIndex();
    for (int i = 0; i < audioDeviceComboBox->count(); ++i) {
        if (audioDeviceComboBox->itemData(i).toInt() == savedDeviceIndex) {
            audioDeviceComboBox->setCurrentIndex(i);
            break;
        }
    }

    // SDR HiFi
    sdrEnabledCheckBox->setChecked(configuration.getSdrEnabled());
    sdrPythonPathEdit->setText(configuration.getSdrPythonPath());
    sdrScriptPathEdit->setText(configuration.getSdrScriptPath());
    sdrSystemComboBox->setCurrentIndex(configuration.getSdrSystem() == "NTSC" ? 1 : 0);
    sdrGainSpinBox->setValue(configuration.getSdrGain());
    sdrStartDelaySpinBox->setValue(configuration.getSdrStartDelayMs() / 1000);

    // Update FLAC control visibility based on selected format
    onCaptureFormatChanged(ui->captureFormatComboBox->currentIndex());
}

// Save the configuration settings from the UI widgets
void ConfigurationDialog::saveConfiguration(Configuration& configuration)
{
    qDebug() << "ConfigurationDialog::saveConfiguration(): Saving configuration";

    // Capture
    configuration.setCaptureDirectory(ui->captureDirectoryLineEdit->text());
    
    // Capture format and hardware sample rate are independent in firmware 3.1.
    Configuration::CaptureFormat baseFormat = static_cast<Configuration::CaptureFormat>(ui->captureFormatComboBox->itemData(ui->captureFormatComboBox->currentIndex()).toInt());
    int sampleRateKHz = ui->sampleRateComboBox->currentData().toInt(); // stored as kHz
    int flacOutputFormat = ui->flacOutputFormatComboBox->currentData().toInt();

    Configuration::CaptureFormat finalFormat = baseFormat;

    // For FLAC: choose ldfCompressed vs flacDirect based on output format dropdown
    if (baseFormat == Configuration::CaptureFormat::flacDirect) {
        if (flacOutputFormat == 1) {
            finalFormat = Configuration::CaptureFormat::ldfCompressed;
        }
        // else stay as flacDirect; sample rate stored separately as kHz
    }

    configuration.setCaptureFormat(finalFormat);
    configuration.setFlacCompressionLevel(ui->flacCompressionLevelComboBox->itemData(ui->flacCompressionLevelComboBox->currentIndex()).toInt());
    configuration.setFlacOutputFormat(flacOutputFormat);
    configuration.setSampleRate(sampleRateKHz); // store actual kHz
    configuration.setFrontEndGainSwitches(ui->frontEndGainComboBox->currentData().toInt());

    // USB
    configuration.setUsbVid(static_cast<quint16>(ui->vendorIdLineEdit->text().toInt()));
    configuration.setUsbPid(static_cast<quint16>(ui->productIdLineEdit->text().toInt()));
    configuration.setUsbPreferredDevice(ui->preferredDeviceComboBox->currentText());
    configuration.setDiskBufferQueueSize((size_t)ui->diskBufferQueueSizeComboBox->itemData(ui->diskBufferQueueSizeComboBox->currentIndex()).toULongLong());
    configuration.setUseSmallUsbTransferQueue(ui->useSmallUsbTransferQueue->isChecked());
    configuration.setUseSmallUsbTransfers(ui->useSmallUsbTransfers->isChecked());
    configuration.setUseWinUsb(ui->useWinUsb->isChecked());
    configuration.setUseAsyncFileIo(ui->useAsyncFileIo->isChecked());

    // Player integration - serial device
    configuration.setSerialDevice(ui->serialDeviceComboBox->currentText());

    // Player integration - Serial speed
    configuration.setSerialSpeed(static_cast<Configuration::SerialSpeeds>(ui->serialSpeedComboBox->itemData(ui->serialSpeedComboBox->currentIndex()).toInt()));

    // KeyLock
    configuration.setKeyLock(ui->keyLockCheckBox->isChecked());

    // Advanced naming
    configuration.setPerSideNotesEnabled(ui->perSideNotesCheckBox->isChecked());
    configuration.setPerSideMintEnabled(ui->perSideMintCheckBox->isChecked());

    // Amplitude
    configuration.setAmplitudeLabelEnabled(ui->amplitudeLabelCheckBox->isChecked());
    configuration.setAmplitudeChartEnabled(ui->amplitudeChartCheckBox->isChecked());
    
    // Theme
    configuration.setThemeStyle(ui->themeComboBox->currentIndex());

    // Audio capture
    configuration.setAudioCaptureEnabled(audioCaptureCheckBox->isChecked());
    configuration.setFmediaPath(fmediaPathLineEdit->text());
    configuration.setAudioCaptureDeviceIndex(audioDeviceComboBox->currentData().toInt());

    // SDR HiFi
    configuration.setSdrEnabled(sdrEnabledCheckBox->isChecked());
    configuration.setSdrPythonPath(sdrPythonPathEdit->text());
    configuration.setSdrScriptPath(sdrScriptPathEdit->text());
    configuration.setSdrSystem(sdrSystemComboBox->currentData().toString());
    configuration.setSdrGain(sdrGainSpinBox->value());
    configuration.setSdrStartDelayMs(sdrStartDelaySpinBox->value() * 1000);

    // Save the configuration to disk
    configuration.writeConfiguration();
}

void ConfigurationDialog::buildSdrTab()
{
    QWidget* page = new QWidget();
    QVBoxLayout* vbox = new QVBoxLayout(page);

    // Enable checkbox
    sdrEnabledCheckBox = new QCheckBox(tr("Enable SDR HiFi capture (GNURadio)"), page);
    vbox->addWidget(sdrEnabledCheckBox);

    // Python path row
    QHBoxLayout* pyRow = new QHBoxLayout();
    pyRow->addWidget(new QLabel(tr("Python path:"), page));
    sdrPythonPathEdit = new QLineEdit(page);
    sdrPythonPathEdit->setPlaceholderText(tr("Leave blank to use C:\\ProgramData\\radioconda\\python.exe"));
    pyRow->addWidget(sdrPythonPathEdit);
    sdrPythonBrowseBtn = new QPushButton(tr("Browse"), page);
    sdrPythonBrowseBtn->setMinimumWidth(80);
    pyRow->addWidget(sdrPythonBrowseBtn);
    vbox->addLayout(pyRow);

    // Script path row
    QHBoxLayout* scriptRow = new QHBoxLayout();
    scriptRow->addWidget(new QLabel(tr("Script path:"), page));
    sdrScriptPathEdit = new QLineEdit(page);
    sdrScriptPathEdit->setPlaceholderText(tr("Path to rtlsdr_to_flac.py"));
    scriptRow->addWidget(sdrScriptPathEdit);
    sdrScriptBrowseBtn = new QPushButton(tr("Browse"), page);
    sdrScriptBrowseBtn->setMinimumWidth(80);
    scriptRow->addWidget(sdrScriptBrowseBtn);
    vbox->addLayout(scriptRow);

    // System + Gain row
    QHBoxLayout* optRow = new QHBoxLayout();
    optRow->addWidget(new QLabel(tr("TV System:"), page));
    sdrSystemComboBox = new QComboBox(page);
    sdrSystemComboBox->addItem("PAL",  QString("PAL"));
    sdrSystemComboBox->addItem("NTSC", QString("NTSC"));
    optRow->addWidget(sdrSystemComboBox);
    optRow->addSpacing(20);
    optRow->addWidget(new QLabel(tr("Gain (0=auto):"), page));
    sdrGainSpinBox = new QSpinBox(page);
    sdrGainSpinBox->setRange(0, 50);
    sdrGainSpinBox->setValue(0);
    optRow->addWidget(sdrGainSpinBox);
    optRow->addStretch();
    vbox->addLayout(optRow);

    // Start delay row
    QHBoxLayout* delayRow = new QHBoxLayout();
    delayRow->addWidget(new QLabel(tr("Start delay (seconds):"), page));
    sdrStartDelaySpinBox = new QSpinBox(page);
    sdrStartDelaySpinBox->setRange(0, 30);
    sdrStartDelaySpinBox->setValue(4);
    sdrStartDelaySpinBox->setToolTip(tr("Delay before starting SDR capture after RF capture begins.\nIncrease if you get USB buffer underflow errors."));
    delayRow->addWidget(sdrStartDelaySpinBox);
    delayRow->addStretch();
    vbox->addLayout(delayRow);

    vbox->addWidget(new QLabel(tr("Output saved as capture_name_hifi.flac and capture_name_hifi.u8"), page));
    vbox->addStretch();

    ui->tabWidget->addTab(page, tr("SDR HiFi"));

    // Connect browse buttons
    connect(sdrPythonBrowseBtn, &QPushButton::clicked, this, [this]() {
        QString path = QFileDialog::getOpenFileName(this, tr("Select python.exe"),
            sdrPythonPathEdit->text(), tr("Executable (*.exe);;All files (*)"));
        if (!path.isEmpty()) sdrPythonPathEdit->setText(path);
    });
    connect(sdrScriptBrowseBtn, &QPushButton::clicked, this, [this]() {
        QString path = QFileDialog::getOpenFileName(this, tr("Select GNURadio script"),
            sdrScriptPathEdit->text(), tr("Python script (*.py);;All files (*)"));
        if (!path.isEmpty()) sdrScriptPathEdit->setText(path);
    });
}

// Browse for capture directory button clicked
void ConfigurationDialog::on_captureDirectoryPushButton_clicked()
{
    QString captureDirectoryPath;

    captureDirectoryPath = QFileDialog::getExistingDirectory(this, tr("Select capture directory"), ui->captureDirectoryLineEdit->text());

    if (captureDirectoryPath.isEmpty()) {
        qDebug() << "ConfigurationDialog::on_captureDirectoryPushButton_clicked(): QFileDialog::getExistingDirectory returned empty directory path";
    } else {
        ui->captureDirectoryLineEdit->setText(captureDirectoryPath);
    }
}

// Save configuration clicked
void ConfigurationDialog::on_buttonBox_accepted()
{
    qDebug() << "ConfigurationDialog::on_buttonBox_accepted(): Configuration changed";

    // Emit a configuration changed signal
    emit configurationChanged();
}

// Cancel configuration clicked
void ConfigurationDialog::on_buttonBox_rejected()
{
    qDebug() << "ConfigurationDialog::on_buttonBox_rejected(): Ignoring configuration changes";
}

// Any button clicked
void ConfigurationDialog::on_buttonBox_clicked(QAbstractButton *button)
{
    // Check for restore defaults button
    if (button == ui->buttonBox->button(QDialogButtonBox::RestoreDefaults)) {
        qDebug() << "ConfigurationDialog::on_buttonBox_clicked(): Restore defaults clicked";

        Configuration defaultConfig(this);
        defaultConfig.setDefault();
        loadConfiguration(defaultConfig);
    }
}

void ConfigurationDialog::encodeToFlac(const QString& inputFilePath, const QString& outputFilePath)
{
    QProcess flacProcess;

    // Set up the FLAC encoder command
    QStringList arguments;
    arguments << "-8"  // Compression level 8
              << "--endian=little"  // Little-endian input
              << "--sign=signed"  // Signed input
              << "--channels=1"  // Mono
              << "--bps=16"  // 16 bits per sample
              << "--sample-rate=40000"  // Sample rate
              << "--threads=8"  // Default to 8 threads
              << "-o" << outputFilePath  // Output file
              << "-";  // Read from stdin

    flacProcess.setProgram(flacProgramPath());
    flacProcess.setArguments(arguments);

    // Open the input file
    QFile inputFile(inputFilePath);
    if (!inputFile.open(QIODevice::ReadOnly)) {
        qWarning() << "Failed to open input file:" << inputFilePath;
        return;
    }

    // Start the FLAC process
    flacProcess.start();

    // Write the input file data to the FLAC process
    while (!inputFile.atEnd()) {
        QByteArray buffer = inputFile.read(4096);
        flacProcess.write(buffer);
    }

    inputFile.close();
    flacProcess.closeWriteChannel();

    // Wait for the process to finish
    if (!flacProcess.waitForFinished()) {
        qWarning() << "FLAC encoding process failed:" << flacProcess.errorString();
        return;
    }

    qDebug() << "FLAC encoding completed successfully. Output file:" << outputFilePath;
}

bool ConfigurationDialog::encodeToLdf(const QString& inputFilePath, const QString& outputFilePath,
    int sampleRateKHz, bool testMode, int frontEndGainSwitches, const QString& gatewareVersion)
{
    QProcess flacProcess;
    const bool outputExistedBefore = QFileInfo::exists(outputFilePath);
    const auto removePartialOutput = [&]()
    {
        if (!outputExistedBefore && QFileInfo::exists(outputFilePath))
            QFile::remove(outputFilePath);
    };

    // The FPGA has already produced the selected 40/20 MSPS stream. LDF is a
    // post-capture container conversion only; applying host resampling here would
    // change the time base a second time.
    const int effectiveSampleRate = sampleRateKHz == 20000 ? 20000 : 40000;
    const int hardwareDecimation = effectiveSampleRate == 20000 ? 2 : 1;

    // Set up the FLAC encoder command for LDF format (ld-compress compatible)
    // LDF uses FLAC compression with specific settings to match ld-compress output
    QStringList arguments;
    arguments << "-8"  // Use level 8 for LDF (maximum compression for ld-compress compatibility)
              << "--endian=little"  // Little-endian input
              << "--sign=signed"  // Signed input
              << "--channels=1"  // Mono
              << "--bps=16"  // 16 bits per sample
              << QString("--sample-rate=%1").arg(effectiveSampleRate)
              << "--force-raw-format"
              << "--tag=ENCODER=DomesdayDuplicator-2.1"
              << "--tag=DDD_VERSION=2.1"
              << QString("--tag=DDD_SAMPLE_RATE_HZ=%1").arg(effectiveSampleRate * 1000)
              << QString("--tag=DDD_DECIMATION=%1").arg(hardwareDecimation)
              << QString("--tag=DDD_TEST_MODE=%1").arg(testMode ? "true" : "false")
              << "--exhaustive-model-search"  // Better compression matching ld-compress
              << "--qlp-coeff-precision-search"  // Optimize coefficient precision
              << "-j" << "8"  // FLAC 1.5 multi-threading support
              << "-o" << outputFilePath  // Output file
              << "-";  // Read from stdin
    const std::string gainDescription = DddFrontEndGain::Description(frontEndGainSwitches);
    if (!gainDescription.empty())
        arguments.insert(arguments.size() - 3,
            QString("--tag=DDD_FRONT_END_GAIN=%1").arg(QString::fromUtf8(gainDescription)));
    if (!gatewareVersion.isEmpty())
        arguments.insert(arguments.size() - 3,
            QString("--tag=DDD_GATEWARE_VERSION=%1").arg(gatewareVersion));

    const QString flacProgram = flacProgramPath();
    flacProcess.setProgram(flacProgram);
    flacProcess.setArguments(arguments);

    // Open the input file
    QFile inputFile(inputFilePath);
    if (!inputFile.open(QIODevice::ReadOnly)) {
        qWarning() << "Failed to open input file for LDF encoding:" << inputFilePath;
        return false;
    }
    inputFile.close();

    // Let QProcess connect the file directly to the child's stdin. QProcess::write()
    // only confirms that data entered its user-space queue, so streaming a full-side
    // raw capture through write() can otherwise buffer hundreds of gigabytes in RAM
    // when level-8 FLAC encoding is slower than sequential disk reads.
    flacProcess.setStandardInputFile(inputFilePath);

    qDebug() << "Starting LDF encoding for native" << effectiveSampleRate << "kHz-labelled RF data...";

    // Start the FLAC process
    flacProcess.start();
    if (!flacProcess.waitForStarted()) {
        qWarning() << "Failed to start FLAC process for LDF encoding:" << flacProcess.errorString();
        removePartialOutput();
        return false;
    }

    // A full-side level-8 encode can legitimately take longer than five minutes.
    // Keep the UI responsive and wait for the encoder's real completion instead
    // of timing out and risking the only raw capture.
    while (flacProcess.state() != QProcess::NotRunning)
    {
        flacProcess.waitForFinished(100);
        QCoreApplication::processEvents();
    }

    if (flacProcess.exitStatus() != QProcess::NormalExit || flacProcess.exitCode() != 0) {
        qWarning() << "LDF encoding process failed with exit code:" << flacProcess.exitCode();
        qWarning() << "Standard error:" << flacProcess.readAllStandardError();
        removePartialOutput();
        return false;
    }

    // Do not delete the raw source merely because flac created a file. Verify
    // the complete stream first; partial FLAC files also begin with fLaC.
    QProcess verifier;
    verifier.setProgram(flacProgram);
    verifier.setArguments({"-t", "--silent", outputFilePath});
    verifier.start();
    if (!verifier.waitForStarted())
    {
        qWarning() << "Failed to start FLAC verification for LDF:" << verifier.errorString();
        removePartialOutput();
        return false;
    }
    while (verifier.state() != QProcess::NotRunning)
    {
        verifier.waitForFinished(100);
        QCoreApplication::processEvents();
    }
    if (verifier.exitStatus() != QProcess::NormalExit || verifier.exitCode() != 0)
    {
        qWarning() << "LDF verification failed:" << verifier.readAllStandardError();
        removePartialOutput();
        return false;
    }

    qDebug() << "LDF encoding completed successfully. Output file:" << outputFilePath;
    return true;
}

void ConfigurationDialog::encodeToFlacDirect(const QString& inputFilePath, const QString& outputFilePath, int compressionLevel, int downsampleFactor)
{
    QProcess flacProcess;

    // Calculate the effective sample rate after downsampling
    int baseSampleRate = 40000;  // 40 kHz base sample rate
    int effectiveSampleRate = baseSampleRate / downsampleFactor;

    // Set up the FLAC encoder command for direct FLAC output (like misrc_capture)
    // Direct FLAC uses standard settings optimized for speed and compatibility
    QStringList arguments;
    arguments << QString("-%1").arg(compressionLevel)  // Use configurable compression level
              << "--endian=little"  // Little-endian input
              << "--sign=signed"  // Signed input
              << "--channels=1"  // Mono
              << "--bps=16"  // 16 bits per sample
              << QString("--sample-rate=%1").arg(effectiveSampleRate)  // Correct sample rate for downsampled data
              << "--threads=8"  // Multi-threading support
              << "--no-seektable"  // Skip seektable for streaming compatibility
              << "-o" << outputFilePath  // Output file
              << "-";  // Read from stdin

    flacProcess.setProgram(flacProgramPath());
    flacProcess.setArguments(arguments);

    // Open the input file
    QFile inputFile(inputFilePath);
    if (!inputFile.open(QIODevice::ReadOnly)) {
        qWarning() << "Failed to open input file for direct FLAC encoding:" << inputFilePath;
        return;
    }

    qDebug() << "Starting direct FLAC encoding with" << downsampleFactor << "x downsampling...";

    // Initialize resampler if downsampling is required
    std::unique_ptr<AudioResampler> resampler;
    if (downsampleFactor > 1) {
        resampler = std::make_unique<AudioResampler>();
        if (!resampler->initialize(baseSampleRate, effectiveSampleRate)) {
            qWarning() << "Failed to initialize audio resampler";
            inputFile.close();
            return;
        }
    }

    // Start the FLAC process
    flacProcess.start();
    if (!flacProcess.waitForStarted()) {
        qWarning() << "Failed to start FLAC process for direct encoding:" << flacProcess.errorString();
        inputFile.close();
        return;
    }

    // Process the input file data with resampling if needed
    const qint64 inputBufferSize = 32768;  // Input buffer size (16-bit samples)
    std::vector<int16_t> inputBuffer(inputBufferSize / 2);  // Buffer for 16-bit samples
    std::vector<int16_t> outputBuffer;
    
    if (resampler) {
        // Pre-allocate output buffer for resampling
        int expectedOutputSize = resampler->getExpectedOutputSampleCount(inputBufferSize / 2);
        outputBuffer.resize(expectedOutputSize + 1024); // Add some extra space for safety
    }

    while (!inputFile.atEnd()) {
        QByteArray rawData = inputFile.read(inputBufferSize);
        if (rawData.isEmpty()) break;
        
        // Convert QByteArray to 16-bit signed samples
        int sampleCount = rawData.size() / 2;
        memcpy(inputBuffer.data(), rawData.data(), rawData.size());
        
        QByteArray outputData;
        
        if (resampler) {
            // Perform resampling
            int resampledSamples = resampler->resample(inputBuffer.data(), sampleCount, 
                                                      outputBuffer.data(), outputBuffer.size());
            if (resampledSamples < 0) {
                qWarning() << "Resampling failed";
                break;
            }
            
            // Convert resampled data back to QByteArray
            outputData = QByteArray((const char*)outputBuffer.data(), resampledSamples * 2);
        } else {
            // No resampling needed, use original data
            outputData = rawData;
        }
        
        // Write to FLAC process
        qint64 written = flacProcess.write(outputData);
        if (written != outputData.size()) {
            qWarning() << "Failed to write complete buffer to FLAC process";
            break;
        }
        
        // Allow for real-time processing
        QCoreApplication::processEvents();
    }

    inputFile.close();
    flacProcess.closeWriteChannel();

    // Wait for the process to finish with reasonable timeout
    if (!flacProcess.waitForFinished(180000)) {  // 3 minute timeout
        qWarning() << "Direct FLAC encoding process timed out or failed:" << flacProcess.errorString();
        flacProcess.kill();
        return;
    }

    if (flacProcess.exitCode() != 0) {
        qWarning() << "Direct FLAC encoding process failed with exit code:" << flacProcess.exitCode();
        qWarning() << "Standard error:" << flacProcess.readAllStandardError();
        return;
    }

    qDebug() << "FLAC Direct encoding completed successfully. Output file:" << outputFilePath;
}

void ConfigurationDialog::downsampleRawFile(const QString& inputFilePath, const QString& outputFilePath, int downsampleFactor)
{
    // Open the input file
    QFile inputFile(inputFilePath);
    if (!inputFile.open(QIODevice::ReadOnly)) {
        qWarning() << "Failed to open input file for downsampling:" << inputFilePath;
        return;
    }
    
    // Open the output file
    QFile outputFile(outputFilePath);
    if (!outputFile.open(QIODevice::WriteOnly)) {
        qWarning() << "Failed to open output file for downsampling:" << outputFilePath;
        inputFile.close();
        return;
    }
    
    qDebug() << "Starting raw file downsampling with factor" << downsampleFactor;
    
    // Initialize resampler
    AudioResampler resampler;
    int baseSampleRate = 40000;  // 40 kHz base sample rate
    int effectiveSampleRate = baseSampleRate / downsampleFactor;
    
    if (!resampler.initialize(baseSampleRate, effectiveSampleRate)) {
        qWarning() << "Failed to initialize audio resampler for raw file downsampling";
        inputFile.close();
        outputFile.close();
        return;
    }
    
    // Process the file in chunks
    const qint64 inputBufferSize = 32768;  // Input buffer size (16-bit samples)
    std::vector<int16_t> inputBuffer(inputBufferSize / 2);  // Buffer for 16-bit samples
    std::vector<int16_t> outputBuffer;
    
    // Pre-allocate output buffer for resampling
    int expectedOutputSize = resampler.getExpectedOutputSampleCount(inputBufferSize / 2);
    outputBuffer.resize(expectedOutputSize + 1024); // Add some extra space for safety
    
    while (!inputFile.atEnd()) {
        QByteArray rawData = inputFile.read(inputBufferSize);
        if (rawData.isEmpty()) break;
        
        // Convert QByteArray to 16-bit signed samples
        int sampleCount = rawData.size() / 2;
        memcpy(inputBuffer.data(), rawData.data(), rawData.size());
        
        // Perform resampling
        int resampledSamples = resampler.resample(inputBuffer.data(), sampleCount, 
                                                 outputBuffer.data(), outputBuffer.size());
        if (resampledSamples < 0) {
            qWarning() << "Resampling failed during raw file processing";
            break;
        }
        
        // Convert resampled data back to QByteArray and write to output
        QByteArray outputData((const char*)outputBuffer.data(), resampledSamples * 2);
        qint64 written = outputFile.write(outputData);
        if (written != outputData.size()) {
            qWarning() << "Failed to write complete buffer to output file";
            break;
        }
    }
    
    inputFile.close();
    outputFile.close();
    
    qDebug() << "Raw file downsampling completed successfully. Output file:" << outputFilePath;
}

void ConfigurationDialog::onCaptureFormatChanged(int index)
{
    // Get the selected capture format
    Configuration::CaptureFormat selectedFormat = static_cast<Configuration::CaptureFormat>(
        ui->captureFormatComboBox->itemData(index).toInt());
    
    // Show FLAC-related controls only for FLAC format
    bool showFlacControls = (selectedFormat == Configuration::CaptureFormat::flacDirect);

    ui->flacCompressionLabel->setVisible(showFlacControls);
    ui->flacCompressionLevelComboBox->setVisible(showFlacControls);
    ui->flacOutputFormatLabel->setVisible(showFlacControls);
    ui->flacOutputFormatComboBox->setVisible(showFlacControls);

    // The hardware sample-rate choice applies to every output format.
    ui->sampleRateLabel->setVisible(true);
    ui->sampleRateComboBox->setVisible(true);
}

void ConfigurationDialog::onSampleRateChanged(int index)
{
    // This method can be used for any future sample rate-specific logic
    Q_UNUSED(index);
}
