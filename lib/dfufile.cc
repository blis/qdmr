#include "dfufile.hh"
#include <QFile>
#include <QtEndian>
#include "crc32.hh"


typedef struct __attribute((packed)) {
  uint8_t signature[5];      ///< File signature = "DfuSe"
  uint8_t version;           ///< File version = 0x01
  uint32_t image_size;       ///< Total file size in big-endian
  uint8_t n_targets;         ///< Number of images.
} file_prefix_t;

typedef struct __attribute((packed)) {
  uint16_t device_id;        ///< Device id in little endian
  uint16_t product_id;       ///< Product id in little endian
  uint16_t vendor_id;        ///< Vendor id in little endian
  uint8_t DFUlo;             ///< Fixed 0x1A
  uint8_t DFUhi;             ///< Fixed 0x01
  uint8_t signature[3];      ///< Fixed "UFD", {0x44, 0x46, 0x55}
  uint8_t size;              ///< Suffix size, fixed 16
  uint32_t crc;              ///< CRC over compltete file excluding the CRC in little endian.
} file_suffix_t;

typedef struct __attribute((packed)) {
  uint8_t signature[6];      ///< Target signature, fixed "Target"
  uint8_t alternate_setting; ///< Alternate setting for image.
  uint32_t is_named;         ///< Bool, if targed is named;
  uint8_t name[255];         ///< Target name (0-padded?).
  uint32_t size;             ///< Size of complete image excl. prefix in big endian.
  uint32_t n_elements;       ///< Number of elements in image in big endian.
} image_prefix_t;

typedef struct __attribute((packed)) {
  uint32_t address;          ///< Target address of element in big endian.
  uint32_t size;             ///< Element size in big endian;
} element_prefix_t;


/* ********************************************************************************************* *
 * Implementation of DFUFile
 * ********************************************************************************************* */
DFUFile::DFUFile(QObject *parent)
  : QObject(parent)
{
  // pass...
}

const QString &
DFUFile::errorMessage() const {
  return _errorMessage;
}

uint32_t
DFUFile::size() const {
  uint32_t size = sizeof(file_prefix_t);
  foreach (const Image &i, _images) {
    size += i.size();
  }
  return size+sizeof(file_suffix_t);
}

int
DFUFile::numImages() const {
  return _images.size();
}

const DFUFile::Image &
DFUFile::image(int i) const {
  return _images[i];
}

DFUFile::Image &
DFUFile::image(int i) {
  return _images[i];
}

void
DFUFile::addImage(const QString &name, uint8_t altSettings) {
  _images.append(Image(name, altSettings));
}

void
DFUFile::addImage(const Image &img) {
  _images.append(img);
}

void
DFUFile::remImage(int i) {
  _images.remove(i);
}

bool
DFUFile::read(const QString &filename) {
  QFile file(filename);

  if (! file.open(QIODevice::ReadOnly)) {
    _errorMessage = tr("Cannot read DFU file '%1': %2").arg(filename).arg(file.errorString());
    return false;
  }

  if (! read(file)) {
    file.close();
    return false;
  }

  return true;
}

bool
DFUFile::read(QFile &file)
{
  CRC32 crc;

  _images.clear();

  file_prefix_t prefix;
  if (sizeof(file_prefix_t) != file.read((char *)&prefix, sizeof(file_prefix_t))) {
    _errorMessage = tr("Cannot read DFU file '%1': Cannot read prefix.").arg(file.fileName());
    return false;
  }

  // update crc
  crc.update((const uint8_t *)&prefix, sizeof(file_prefix_t));

  if (memcmp(prefix.signature, "DfuSe", 5)) {
    _errorMessage = tr("Cannot read DFU file '%1': Invalid DFU file signature. Not a DFU file?").arg(file.fileName());
    return false;
  }

  uint32_t filesize = qFromLittleEndian(prefix.image_size);
  uint8_t  n_images = prefix.n_targets;

  for (uint8_t i=0; i<n_images; i++) {
    Image img;
    if (! img.read(file, crc, _errorMessage))
      return false;
    _images.append(img);
  }

  file_suffix_t suffix;
  if (sizeof(file_suffix_t) != file.read((char *)&suffix, sizeof(suffix))) {
    _errorMessage = tr("Cannot read DFU file '%1': Cannot read suffix: %2.")
        .arg(file.fileName()).arg(file.errorString());
    return false;
  }

  // Update CRC with suffix excl. CRC itself
  crc.update((const uint8_t *) &suffix, sizeof(file_suffix_t)-4);

  if (filesize != (size()-sizeof(file_suffix_t))) {
    _errorMessage = tr("Cannot read DFU file '%1': Filesize %2 does not match declared content %3.")
        .arg(file.fileName()).arg(size()-sizeof(file_suffix_t)).arg(filesize);
    return false;
  }

  if (memcmp(suffix.signature, "UFD", 3)) {
    _errorMessage = tr("Cannot read DFU file '%1': Invalid suffix signature.").arg(file.fileName());
    return false;
  }

  if (crc.get() != suffix.crc) {
    _errorMessage = tr("Cannot read DFU file '%1': Invalid checksum.").arg(file.fileName());
    return false;
  }
  return true;
}

bool
DFUFile::write(const QString &filename) {
  QFile file(filename);
  if (! file.open(QIODevice::WriteOnly)) {
    _errorMessage = tr("Cannot create DFU file '%1': %2").arg(filename).arg(file.errorString());
    return false;
  }

  bool res = write(file);
  file.close();

  return res;
}

bool
DFUFile::write(QFile &file) {
  file_prefix_t prefix;
  memcpy(prefix.signature, "DfuSe", 5);
  prefix.version = 0x01;
  prefix.image_size = qToLittleEndian(size()-sizeof(file_suffix_t));
  prefix.n_targets = _images.size();

  if (sizeof(file_prefix_t) != file.write((char *)&prefix, sizeof(file_prefix_t))) {
    _errorMessage = tr("Cannot write DFU prefix to '%1': %2")
        .arg(file.fileName()).arg(file.errorString());
    return false;
  }

  CRC32 crc;
  crc.update((uint8_t *)&prefix, sizeof(file_prefix_t));

  foreach (const Image &i, _images) {
    if (! i.write(file, crc, _errorMessage))
      return false;
  }

  file_suffix_t suffix;
  suffix.device_id = qToLittleEndian((uint16_t)0xffff);
  suffix.product_id = qToLittleEndian((uint16_t)0xffff);
  suffix.vendor_id = qToLittleEndian((uint16_t)0xffff);
  suffix.DFUlo = 0x1a;
  suffix.DFUhi = 0x01;
  memcpy(suffix.signature, "UFD", 3);
  suffix.size = 16;

  crc.update((uint8_t *) &suffix, sizeof(file_suffix_t)-4);
  suffix.crc = qToLittleEndian(crc.get());

  if (sizeof(file_suffix_t) != file.write((char *)&suffix, sizeof(file_suffix_t))) {
    _errorMessage = tr("Cannot write DFU suffix to '%1': %2")
        .arg(file.fileName()).arg(file.errorString());
    return false;
  }

  return true;
}

unsigned char *
DFUFile::data(uint32_t offset, uint32_t img) {
  // Search for element that contains address
  for  (int i=0; i<image(img).numElements(); i++) {
    if ((offset >= image(img).element(i).address()) &&
        (offset < (image(img).element(i).address()+image(img).element(i).data().size())))
    {
      return (unsigned char *)(image(img).element(i).data().data()+
                               (offset-image(img).element(i).address()));
    }
  }
  return nullptr;
}

const unsigned char *
DFUFile::data(uint32_t offset, uint32_t img) const {
  // Search for element that contains address
  for  (int i=0; i<image(img).numElements(); i++) {
    if ((offset >= image(img).element(i).address()) &&
        (offset < (image(img).element(i).address()+image(img).element(i).data().size())))
    {
      return (const unsigned char *)(image(img).element(i).data().data()+
                                     (offset-image(img).element(i).address()));
    }
  }
  return nullptr;
}

void
DFUFile::dump(QTextStream &stream) const {
  stream << "DFU file with " << _images.size() << " images:" << endl;
  foreach (const Image &i, _images) {
    i.dump(stream);
  }
}


/* ********************************************************************************************* *
 * Implementation of DFUFile::Element
 * ********************************************************************************************* */
DFUFile::Element::Element()
  : _address(0), _data()
{
  // pass...
}

DFUFile::Element::Element(uint32_t addr, uint32_t size)
  : _address(addr), _data(size, 0x00)
{
  // pass...
}

DFUFile::Element::Element(const Element &other)
  : _address(other._address), _data(other._data)
{
  // pass...
}

DFUFile::Element &
DFUFile::Element::operator=(const Element &other) {
  _address = other._address;
  _data = other._data;
  return *this;
}

uint32_t
DFUFile::Element::size() const {
  return sizeof(element_prefix_t) + _data.size();
}

uint32_t
DFUFile::Element::address() const {
  return _address;
}

void
DFUFile::Element::setAddress(uint32_t addr) {
  _address = addr;
}

bool
DFUFile::Element::isAligned(uint blocksize) const {
  return (0 == (_address % blocksize)) && (0 == (_data.size() % blocksize));
}

const QByteArray &
DFUFile::Element::data() const {
  return _data;
}

QByteArray &
DFUFile::Element::data() {
  return _data;
}

bool
DFUFile::Element::read(QFile &file, CRC32 &crc, QString &errorMessage)
{
  // Read Element prefix:
  element_prefix_t prefix;
  if (sizeof(element_prefix_t) != file.read((char *)&prefix, sizeof(element_prefix_t))) {
    errorMessage = tr("Cannot read DFU file '%1': Cannot read element prefix: %2").arg(file.fileName()).arg(file.errorString());
    return false;
  }

  crc.update((const uint8_t *) &prefix, sizeof(element_prefix_t));

  _address = qFromLittleEndian(prefix.address);
  uint32_t size = qFromLittleEndian(prefix.size);

  _data.clear();
  _data = file.read(size);

  if (size != uint32_t(_data.size())) {
    errorMessage = tr("Cannot read DFU file '%1': Cannot read element data: %2").arg(file.fileName()).arg(file.errorString());
    return false;
  }

  crc.update(_data);

  return true;
}

bool
DFUFile::Element::write(QFile &file, CRC32 &crc, QString &errorMessage) const {
  element_prefix_t prefix;
  prefix.address = qToLittleEndian(_address);
  prefix.size = qToLittleEndian(uint32_t(_data.size()));

  crc.update((uint8_t *) &prefix, sizeof(element_prefix_t));

  if (sizeof(element_prefix_t) != file.write((const char *)&prefix, sizeof(element_prefix_t))) {
    errorMessage = tr("Cannot write element prefix to file '%1': %2")
        .arg(file.fileName()).arg(file.errorString());
    return false;
  }

  crc.update(_data);

  if (_data.size() != file.write(_data)) {
    errorMessage = tr("Cannot write element data to file '%1': %2")
        .arg(file.fileName()).arg(file.errorString());
    return false;
  }

  return true;
}

void
DFUFile::Element::dump(QTextStream &stream) const {
  stream << "  Element @ 0x" << hex << _address << ", size=0x" << hex << _data.size() << endl;
  int nrow = _data.size()/16;
  uint8_t last_line[16]; memset(last_line, 0, 16);
  bool skipping = false;
  for (int i=0; i<nrow; i++) {
    if ((i>0) && (0==memcmp(last_line, _data.constData()+i*16, 16)) && skipping)
      continue;
    if ((i>0) && (0==memcmp(last_line, _data.constData()+i*16, 16))) {
      skipping = true;
      stream << qSetFieldWidth(8) << right << "*" << qSetFieldWidth(1) << endl;
      continue;
    }
    memcpy(last_line, _data.constData()+i*16, 16);
    skipping = false;
    stream << qSetFieldWidth(8) << right << hex << (_address+i*16)
           << qSetFieldWidth(1) << "  ";
    for (int j=(i*16); j<(i*16+8); j++) {
      stream << qSetFieldWidth(2) << right << hex << uint8_t(_data.at(j))
             << qSetFieldWidth(1) << " ";
    }
    stream << " ";
    for (int j=(i*16+8); j<(i*16+16); j++) {
      stream << qSetFieldWidth(2) << right << hex << uint8_t(_data.at(j))
             << qSetFieldWidth(1) << " ";
    }
    stream << " |";
    for (int j=(i*16); j<(i*16+16); j++) {
      char c = _data.at(j);
      if ((c>=32) && (c<127))
        stream << c;
      else
        stream << ".";
    }
    stream << "|" << endl;
  }
}


/* ********************************************************************************************* *
 * Implementation of DFUFile::Image
 * ********************************************************************************************* */
DFUFile::Image::Image()
  : _alternate_settings(0), _name(), _elements()
{
  // pass...
}

DFUFile::Image::Image(const QString &name, uint8_t altSettings)
  : _alternate_settings(altSettings), _name(name), _elements()
{
  // pass...
}

DFUFile::Image::Image(const Image &other)
  : _alternate_settings(other._alternate_settings), _name(other._name), _elements(other._elements)
{
  // pass...
}

DFUFile::Image &
DFUFile::Image::operator=(const Image &other) {
  _alternate_settings = other._alternate_settings;
  _name = other._name;
  _elements = other._elements;
  return *this;
}

uint32_t
DFUFile::Image::size() const {
  uint32_t size = sizeof(image_prefix_t);
  foreach (const Element &e, _elements)
    size += e.size();
  return size;
}

uint8_t
DFUFile::Image::alternateSettings() const {
  return _alternate_settings;
}

void
DFUFile::Image::setAlternateSettings(uint8_t s) {
  _alternate_settings = s;
}

bool
DFUFile::Image::isNamed() const {
  return ! _name.isEmpty();
}

const QString &
DFUFile::Image::name() const {
  return _name;
}

void
DFUFile::Image::setName(const QString &name) {
  _name = name;
}

int
DFUFile::Image::numElements() const {
  return _elements.size();
}

const DFUFile::Element &
DFUFile::Image::element(int i) const {
  return _elements[i];
}

DFUFile::Element &
DFUFile::Image::element(int i) {
  return _elements[i];
}

void
DFUFile::Image::addElement(uint32_t addr, uint32_t size, int index) {
  if ((0 > index) || (_elements.size() <= index))
    _elements.append(Element(addr, size));
  else
    _elements.insert(index, Element(addr, size));
}

void
DFUFile::Image::addElement(const Element &element) {
  _elements.append(element);
}

void
DFUFile::Image::remElement(int i) {
  _elements.remove(i);
}

bool
DFUFile::Image::read(QFile &file, CRC32 &crc, QString &errorMessage)
{
  image_prefix_t prefix;
  if (sizeof(image_prefix_t) != file.read((char *)&prefix, sizeof(image_prefix_t))) {
    errorMessage = tr("Cannot read DFU file '%1': Cannot read image: %2").arg(file.fileName()).arg(file.errorString());
    return false;
  }

  crc.update((const uint8_t *) &prefix, sizeof(image_prefix_t));

  if (memcmp(prefix.signature, "Target", 6)) {
    errorMessage = tr("Cannot read DFU file '%1': Invalid image signature value.").arg(file.fileName());
    return false;
  }

  _alternate_settings = prefix.alternate_setting;
  if (0x01 ==qFromLittleEndian(prefix.is_named)) {
    char tmp[256]; tmp[255]=0;
    memcpy(tmp, prefix.name, 255);
    _name = tmp;
  }

  uint32_t size = qFromLittleEndian(prefix.size);
  uint32_t n_elements = qFromLittleEndian(prefix.n_elements);
  for (uint32_t i=0; i<n_elements; i++) {
    Element element;
    if (! element.read(file, crc, errorMessage))
      return false;
    _elements.append(element);
  }

  // verify size:
  if (size != (this->size()-sizeof(image_prefix_t))) {
    errorMessage = tr("Cannot read DFU file '%1': Invalid image size %2b specified, expected %3b.")
        .arg(file.fileName()).arg(size).arg(this->size()-sizeof(image_prefix_t));
    return false;
  }
  return true;
}

bool
DFUFile::Image::write(QFile &file, CRC32 &crc, QString &errorMessage) const {
  image_prefix_t prefix;
  memcpy(prefix.signature, "Target", 6);
  prefix.alternate_setting = _alternate_settings;
  prefix.is_named = qToLittleEndian(uint32_t(_name.isEmpty() ? 0 : 1));
  memset(prefix.name, 0, 255);
  if (! _name.isEmpty())
    memcpy(prefix.name, _name.toLocal8Bit().constData(), std::min(255, _name.size()));
  prefix.size = qToLittleEndian(uint32_t(size()-sizeof(image_prefix_t)));
  prefix.n_elements = qToLittleEndian(uint32_t(_elements.size()));

  crc.update((uint8_t *)&prefix, sizeof(image_prefix_t));

  if (sizeof(image_prefix_t) != file.write((char *)&prefix, sizeof(image_prefix_t))) {
    errorMessage = tr("Cannot write image prefix to '%1': %2.")
        .arg(file.fileName()).arg(file.errorString());
    return false;
  }

  foreach (const Element &e, _elements) {
    if (! e.write(file, crc, errorMessage))
      return false;
  }

  return true;
}

void
DFUFile::Image::sort() {
  std::stable_sort(_elements.begin(), _elements.end(),
                   [](const Element &first, const Element &second) {
                     return first.address()<second.address();
                   });
}

void
DFUFile::Image::dump(QTextStream &stream) const {
  stream << " Image";
  if (_name.isEmpty())
    stream << ", target not named";
  else
    stream << ", target '" << _name << "'";
  stream << ", #elements=" << _elements.size() << ":" << endl;
  foreach (const Element &e, _elements) {
    e.dump(stream);
  }
}

