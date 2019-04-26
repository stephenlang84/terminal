#ifndef CHATCLIENTUSERSVIEWITEMDELEGATE_H
#define CHATCLIENTUSERSVIEWITEMDELEGATE_H

#include <QStyledItemDelegate>
#include "ChatUsersViewItemStyle.h"

class ChatClientUsersViewItemDelegate : public QStyledItemDelegate
{
   Q_OBJECT
public:
   explicit ChatClientUsersViewItemDelegate(QObject *parent = nullptr);

signals:

public slots:


   // QAbstractItemDelegate interface
public:
   void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
protected:
   void paintCategoryNode(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const;
   void paintRoomsElement(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const ;
   void paintContactsElement(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const;
   void paintUserElement(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const;

private:
   ChatUsersViewItemStyle itemStyle_;

};







#endif // CHATCLIENTUSERSVIEWITEMDELEGATE_H
