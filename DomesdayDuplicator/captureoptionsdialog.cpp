// ...existing includes...

CaptureOptionsDialog::CaptureOptionsDialog(QWidget* parent)
    : QDialog(parent)
    , ui(new Ui::CaptureOptionsDialog)
{
    ui->setupUi(this);
    ui->captureFormatComboBox->clear();

    // Add FLAC 16-bit option to the capture mode selection
    ui->captureFormatComboBox->addItem(tr("16-bit FLAC Compressed"), QVariant::fromValue(0));
    ui->captureFormatComboBox->addItem(tr("test-16-bit-signed"), QVariant::fromValue(1));

    // ...existing code for other options...
}