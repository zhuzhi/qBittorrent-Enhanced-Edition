#include <QDebug>

#include "torrentmodel.h"
#include "torrentpersistentdata.h"
#include "qbtsession.h"

using namespace libtorrent;

TorrentModelItem::TorrentModelItem(const QTorrentHandle &h)
{
  m_torrent = h;
  m_name = TorrentPersistentData::getName(h.hash());
  if(m_name.isEmpty()) m_name = h.name();
  m_addedTime = TorrentPersistentData::getAddedDate(h.hash());
  m_seedTime = TorrentPersistentData::getSeedDate(h.hash());
  m_label = TorrentPersistentData::getLabel(h.hash());
}

TorrentModelItem::State TorrentModelItem::state() const
{
  try {
    // Pause or Queued
    if(m_torrent.is_paused()) {
      m_icon = QIcon(":/Icons/skin/paused.png");
      m_fgColor = QColor("red");
      return m_torrent.is_seed() ? STATE_PAUSED_UP : STATE_PAUSED_DL;
    }
    if(m_torrent.is_queued()) {
      m_icon = QIcon(":/Icons/skin/queued.png");
      m_fgColor = QColor("grey");
      return m_torrent.is_seed() ? STATE_QUEUED_UP : STATE_QUEUED_DL;
    }
    // Other states
    switch(m_torrent.state()) {
    case torrent_status::allocating:
    case torrent_status::downloading_metadata:
    case torrent_status::downloading: {
      if(m_torrent.download_payload_rate() > 0) {
        m_icon = QIcon(":/Icons/skin/downloading.png");
        m_fgColor = QColor("green");
        return STATE_DOWNLOADING;
      } else {
        m_icon = QIcon(":/Icons/skin/stalledDL.png");
        m_fgColor = QColor("grey");
        return STATE_STALLED_DL;
      }
    }
    case torrent_status::finished:
    case torrent_status::seeding:
      if(m_torrent.upload_payload_rate() > 0) {
        m_icon = QIcon(":/Icons/skin/uploading.png");
        m_fgColor = QColor("orange");
        return STATE_SEEDING;
      } else {
        m_icon = QIcon(":/Icons/skin/stalledUP.png");
        m_fgColor = QColor("grey");
        return STATE_STALLED_UP;
      }
    case torrent_status::queued_for_checking:
    case torrent_status::checking_resume_data:
    case torrent_status::checking_files:
      m_icon = QIcon(":/Icons/skin/checking.png");
      m_fgColor = QColor("grey");
      return m_torrent.is_seed() ? STATE_CHECKING_UP : STATE_CHECKING_DL;
    default:
      m_icon = QIcon(":/Icons/skin/error.png");
      m_fgColor = QColor("red");
      return STATE_INVALID;
    }
  } catch(invalid_handle&) {
    m_icon = QIcon(":/Icons/skin/error.png");
    m_fgColor = QColor("red");
    return STATE_INVALID;
  }
}

bool TorrentModelItem::setData(int column, const QVariant &value, int role)
{
  qDebug() << Q_FUNC_INFO << column << value;
  if(role != Qt::DisplayRole) return false;
  // Label and Name columns can be edited
  switch(column) {
  case TR_NAME:
    m_name = value.toString();
    TorrentPersistentData::saveName(m_torrent.hash(), m_name);
    return true;
  case TR_LABEL: {
    QString new_label = value.toString();
    if(m_label != new_label) {
      QString old_label = m_label;
      m_label = new_label;
      TorrentPersistentData::saveLabel(m_torrent.hash(), new_label);
      emit labelChanged(old_label, new_label);
    }
    return true;
  }
  default:
    break;
  }
  return false;
}

QVariant TorrentModelItem::data(int column, int role) const
{
  if(role == Qt::DecorationRole && column == TR_NAME) {
    return m_icon;
  }
  if(role == Qt::ForegroundRole) {
    return m_fgColor;
  }
  if(role != Qt::DisplayRole) return QVariant();
  switch(column) {
  case TR_NAME:
    return m_name;
  case TR_PRIORITY:
    return m_torrent.queue_position();
  case TR_SIZE:
    return static_cast<qlonglong>(m_torrent.actual_size());
  case TR_PROGRESS:
    return m_torrent.progress();
  case TR_STATUS:
    return state();
  case TR_SEEDS: {
    // XXX: Probably a better way to do this
    qulonglong seeds = m_torrent.num_seeds()*1000000;
    if(m_torrent.num_complete() >= m_torrent.num_seeds())
      seeds += m_torrent.num_complete()*10;
    else
      seeds += 1;
    return seeds;
  }
  case TR_PEERS: {
    qulonglong peers = (m_torrent.num_peers()-m_torrent.num_seeds())*1000000;
    if(m_torrent.num_incomplete() >= (m_torrent.num_peers()-m_torrent.num_seeds()))
      peers += m_torrent.num_incomplete()*10;
    else
      peers += 1;
    return peers;
  }
  case TR_DLSPEED:
    return m_torrent.download_payload_rate();
  case TR_UPSPEED:
    return m_torrent.upload_payload_rate();
  case TR_ETA: {
    // XXX: Is this correct?
    if(m_torrent.is_seed() || m_torrent.is_paused() || m_torrent.is_queued()) return MAX_ETA;
    return QBtSession::instance()->getETA(m_torrent.hash());
  }
  case TR_RATIO:
    return QBtSession::instance()->getRealRatio(m_torrent.hash());
  case TR_LABEL:
    return m_label;
  case TR_ADD_DATE:
    return m_addedTime;
  case TR_SEED_DATE:
    return m_seedTime;
  case TR_TRACKER:
    return m_torrent.current_tracker();
  case TR_DLLIMIT:
    return m_torrent.download_limit();
  case TR_UPLIMIT:
    return m_torrent.upload_limit();
  default:
    return QVariant();
  }
}

// TORRENT MODEL

TorrentModel::TorrentModel(QObject *parent) :
  QAbstractListModel(parent), m_refreshInterval(2000)
{
}

void TorrentModel::populate() {
  // Load the torrents
  std::vector<torrent_handle> torrents = QBtSession::instance()->getSession()->get_torrents();
  std::vector<torrent_handle>::const_iterator it;
  for(it = torrents.begin(); it != torrents.end(); it++) {
    addTorrent(QTorrentHandle(*it));
  }
  // Refresh timer
  connect(&m_refreshTimer, SIGNAL(timeout()), SLOT(forceModelRefresh()));
  m_refreshTimer.start(m_refreshInterval);
  // Listen for torrent changes
  connect(QBtSession::instance(), SIGNAL(addedTorrent(QTorrentHandle)), SLOT(addTorrent(QTorrentHandle)));
  connect(QBtSession::instance(), SIGNAL(torrentAboutToBeRemoved(QTorrentHandle)), SLOT(handleTorrentAboutToBeRemoved(QTorrentHandle)));
  connect(QBtSession::instance(), SIGNAL(deletedTorrent(QString)), SLOT(removeTorrent(QString)));
  connect(QBtSession::instance(), SIGNAL(finishedTorrent(QTorrentHandle&)), SLOT(handleTorrentUpdate(QTorrentHandle&)));
  connect(QBtSession::instance(), SIGNAL(metadataReceived(QTorrentHandle&)), SLOT(handleTorrentUpdate(QTorrentHandle&)));
  connect(QBtSession::instance(), SIGNAL(resumedTorrent(QTorrentHandle&)), SLOT(handleTorrentUpdate(QTorrentHandle&)));
  connect(QBtSession::instance(), SIGNAL(pausedTorrent(QTorrentHandle&)), SLOT(handleTorrentUpdate(QTorrentHandle&)));
  connect(QBtSession::instance(), SIGNAL(torrentFinishedChecking(QTorrentHandle&)), SLOT(handleTorrentUpdate(QTorrentHandle&)));
}

TorrentModel::~TorrentModel() {
  qDeleteAll(m_torrents);
  m_torrents.clear();
}

QVariant TorrentModel::headerData(int section, Qt::Orientation orientation,
                                  int role) const
{
  if (orientation == Qt::Horizontal) {
    if(role == Qt::DisplayRole) {
      switch(section) {
      case TorrentModelItem::TR_NAME: return tr("Name", "i.e: torrent name");
      case TorrentModelItem::TR_PRIORITY: return "#";
      case TorrentModelItem::TR_SIZE: return tr("Size", "i.e: torrent size");
      case TorrentModelItem::TR_PROGRESS: return tr("Done", "% Done");
      case TorrentModelItem::TR_STATUS: return tr("Status", "Torrent status (e.g. downloading, seeding, paused)");
      case TorrentModelItem::TR_SEEDS: return tr("Seeds", "i.e. full sources (often untranslated)");
      case TorrentModelItem::TR_PEERS: return tr("Peers", "i.e. partial sources (often untranslated)");
      case TorrentModelItem::TR_DLSPEED: return tr("Down Speed", "i.e: Download speed");
      case TorrentModelItem::TR_UPSPEED: return tr("Up Speed", "i.e: Upload speed");
      case TorrentModelItem::TR_RATIO: return tr("Ratio", "Share ratio");
      case TorrentModelItem::TR_ETA: return tr("ETA", "i.e: Estimated Time of Arrival / Time left");
      case TorrentModelItem::TR_LABEL: return tr("Label");
      case TorrentModelItem::TR_ADD_DATE: return tr("Added On", "Torrent was added to transfer list on 01/01/2010 08:00");
      case TorrentModelItem::TR_SEED_DATE: return tr("Completed On", "Torrent was completed on 01/01/2010 08:00");
      case TorrentModelItem::TR_TRACKER: return tr("Tracker");
      case TorrentModelItem::TR_DLLIMIT: return tr("Down Limit", "i.e: Download limit");
      case TorrentModelItem::TR_UPLIMIT: return tr("Up Limit", "i.e: Upload limit");
      default:
        return QVariant();
      }
    }
    if(role == Qt::TextAlignmentRole) {
      switch(section) {
      case TorrentModelItem::TR_PRIORITY:
      case TorrentModelItem::TR_SIZE:
      case TorrentModelItem::TR_SEEDS:
      case TorrentModelItem::TR_PEERS:
      case TorrentModelItem::TR_DLSPEED:
      case TorrentModelItem::TR_UPSPEED:
      case TorrentModelItem::TR_RATIO:
      case TorrentModelItem::TR_DLLIMIT:
      case TorrentModelItem::TR_UPLIMIT:
        return Qt::AlignRight;
      case TorrentModelItem::TR_PROGRESS:
        return Qt::AlignHCenter;
      default:
        return Qt::AlignLeft;
      }
    }
  }

  return QVariant();
}

QVariant TorrentModel::data(const QModelIndex &index, int role) const
{
  if(!index.isValid()) return QVariant();
  try {
    if(index.row() >= 0 && index.row() < rowCount() && index.column() >= 0 && index.column() < columnCount())
      return m_torrents[index.row()]->data(index.column(), role);
  } catch(invalid_handle&) {}
  return QVariant();
}

bool TorrentModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
  qDebug() << Q_FUNC_INFO << value;
  if(!index.isValid() || role != Qt::DisplayRole) return false;
  qDebug("Index is valid and role is DisplayRole");
  try {
    if(index.row() >= 0 && index.row() < rowCount() && index.column() >= 0 && index.column() < columnCount()) {
      bool change = m_torrents[index.row()]->setData(index.column(), value, role);
      if(change)
        notifyTorrentChanged(index.row());
      return change;
    }
  } catch(invalid_handle&) {}
  return false;
}

int TorrentModel::torrentRow(const QString &hash) const
{
  QList<TorrentModelItem*>::const_iterator it;
  int row;
  for(it = m_torrents.constBegin(); it != m_torrents.constEnd(); it++) {
    try {
      if((*it)->hash() == hash) return row;
    }catch(invalid_handle&) {}
    ++row;
  }
  return -1;
}

void TorrentModel::addTorrent(const QTorrentHandle &h)
{
  if(torrentRow(h.hash()) < 0) {
    beginInsertTorrent(m_torrents.size());
    TorrentModelItem *item = new TorrentModelItem(h);
    connect(item, SIGNAL(labelChanged(QString,QString)), SLOT(handleTorrentLabelChange(QString,QString)));
    m_torrents << item;
    emit torrentAdded(item);
    endInsertTorrent();
  }
}

void TorrentModel::removeTorrent(const QString &hash)
{
  const int row = torrentRow(hash);
  qDebug() << Q_FUNC_INFO << hash << row;
  if(row > 0) {
    beginRemoveTorrent(row);
    m_torrents.removeAt(row);
    endRemoveTorrent();
  }
}

void TorrentModel::beginInsertTorrent(int row)
{
  beginInsertRows(QModelIndex(), row, row);
}

void TorrentModel::endInsertTorrent()
{
  endInsertRows();
}

void TorrentModel::beginRemoveTorrent(int row)
{
  beginRemoveRows(QModelIndex(), row, row);
}

void TorrentModel::endRemoveTorrent()
{
  endRemoveRows();
}

void TorrentModel::handleTorrentUpdate(QTorrentHandle &h)
{
  const int row = torrentRow(h.hash());
  if(row >= 0) {
    notifyTorrentChanged(row);
  }
}

void TorrentModel::notifyTorrentChanged(int row)
{
  emit dataChanged(index(row, 0), index(row, columnCount()-1));
}

void TorrentModel::setRefreshInterval(int refreshInterval)
{
  if(m_refreshInterval != refreshInterval) {
    m_refreshInterval = refreshInterval;
    m_refreshTimer.stop();
    m_refreshTimer.start(m_refreshInterval);
  }
}

void TorrentModel::forceModelRefresh()
{
  emit dataChanged(index(0, 0), index(rowCount()-1, columnCount()-1));
}

TorrentStatusReport TorrentModel::getTorrentStatusReport() const
{
  TorrentStatusReport report;
  QList<TorrentModelItem*>::const_iterator it;
  for(it = m_torrents.constBegin(); it != m_torrents.constEnd(); it++) {
    switch((*it)->data(TorrentModelItem::TR_STATUS).toInt()) {
    case TorrentModelItem::STATE_DOWNLOADING:
      ++report.nb_active;
      ++report.nb_downloading;
      break;
    case TorrentModelItem::STATE_PAUSED_DL:
      ++report.nb_paused;
    case TorrentModelItem::STATE_STALLED_DL:
    case TorrentModelItem::STATE_CHECKING_DL:
    case TorrentModelItem::STATE_QUEUED_DL: {
      ++report.nb_inactive;
      ++report.nb_downloading;
      break;
    }
    case TorrentModelItem::STATE_SEEDING:
      ++report.nb_active;
      ++report.nb_seeding;
      break;
    case TorrentModelItem::STATE_PAUSED_UP:
      ++report.nb_paused;
    case TorrentModelItem::STATE_STALLED_UP:
    case TorrentModelItem::STATE_CHECKING_UP:
    case TorrentModelItem::STATE_QUEUED_UP: {
      ++report.nb_seeding;
      ++report.nb_inactive;
      break;
    }
    default:
      break;
    }
  }
  return report;
}

Qt::ItemFlags TorrentModel::flags(const QModelIndex &index) const
{
  if (!index.isValid())
    return 0;
  // Explicitely mark as editable
  return QAbstractListModel::flags(index) | Qt::ItemIsEditable;
}

void TorrentModel::handleTorrentLabelChange(QString previous, QString current)
{
  emit torrentChangedLabel(static_cast<TorrentModelItem*>(sender()), previous, current);
}

QString TorrentModel::torrentHash(int row) const
{
  if(row >= 0 && row < rowCount())
    return m_torrents.at(row)->hash();
  return QString();
}

void TorrentModel::handleTorrentAboutToBeRemoved(const QTorrentHandle &h)
{
  const int row = torrentRow(h.hash());
  if(row >= 0) {
    emit torrentAboutToBeRemoved(m_torrents.at(row));
  }
}
