#ifndef __OTC_REQUEST_VIEW_MODEL_H__
#define __OTC_REQUEST_VIEW_MODEL_H__

#include <QAbstractTableModel>
#include <QString>
#include <QTimer>

#include <vector>

#include "CommonTypes.h"

class OTCRequestViewModel : public QAbstractTableModel
{
Q_OBJECT
public:
   OTCRequestViewModel(QObject* parent);
   ~OTCRequestViewModel() noexcept override = default;

   int rowCount(const QModelIndex & parent) const override;
   int columnCount(const QModelIndex & parent) const override;
   QVariant data(const QModelIndex & index, int role = Qt::DisplayRole) const override;
   QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

   void clear();

public:
   void AddLiveOTCRequest(const bs::network::LiveOTCRequest& otc);
   bool RemoveOTCByID(const std::string& otc);

   bs::network::LiveOTCRequest GetOTCRequest(const QModelIndex& index);

private slots:
   // update time left only. do not remove anything
   void RefreshBoard();

private:
   enum Columns
   {
      ColumnSecurity = 0,
      ColumnType,
      ColumnProduct,
      ColumnSide,
      ColumnQuantity,
      ColumnDuration,
      ColumnCount
   };
private:
   QVariant getRowData(const int column, const bs::network::LiveOTCRequest& otc) const;

private:
   std::vector<bs::network::LiveOTCRequest>  currentRequests_;
   QTimer                                    refreshTicker_;
};

#endif